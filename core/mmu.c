#include "mmu.h"

#include <string.h>

#include "exception.h"
#include "machine.h"

/* VMSAv8-64 stage 1 translation for the EL1&0 regime, 4 KB, 16 KB and 64 KB
   granules, 48-bit VA and PA. There is no stage 2: HengeVM is the hypervisor. */

#define PAGE_MASK ((1ull << HV_TLB_PAGE_BITS) - 1)
#define OA_MASK 0x0000ffffffffffffull

static int granule_bits(uint32_t tg, int ttbr1) {
    if (ttbr1) return tg == 1 ? 14 : tg == 3 ? 16 : 12; /* TG1: 01 16K, 10 4K, 11 64K */
    return tg == 2 ? 14 : tg == 1 ? 16 : 12;            /* TG0: 00 4K, 01 64K, 10 16K */
}

void hv_mmu_walk(HvCpu *cpu, uint64_t va, HvAccess acc, int el, HvWalk *out) {
    memset(out, 0, sizeof *out);
    uint64_t sctlr = cpu->sctlr_el1;
    if (!(sctlr & 1)) {
        /* MMU off: flat, everything allowed. Data is Device-nGnRnE, fetches Normal. */
        out->pa = va;
        out->r = out->w = out->x = 1;
        out->block_size = 1ull << 30;
        out->attr = acc == HV_ACC_EXEC ? 0xff : 0x00;
        return;
    }

    uint64_t tcr = cpu->tcr_el1;
    int hi = (va >> 55) & 1;
    int tbi = hi ? (tcr >> 38) & 1 : (tcr >> 37) & 1;
    int tbid = hi ? (tcr >> 52) & 1 : (tcr >> 51) & 1;
    if (acc == HV_ACC_EXEC && tbid) tbi = 0;
    uint32_t tsz = hi ? (tcr >> 16) & 63 : tcr & 63;
    int epd = hi ? (tcr >> 23) & 1 : (tcr >> 7) & 1;
    int hpd = hi ? (tcr >> 42) & 1 : (tcr >> 41) & 1;
    int g = granule_bits(hi ? (tcr >> 30) & 3 : (tcr >> 14) & 3, hi);
    if (tsz < 16) tsz = 16;
    if (tsz > 39) tsz = 39;
    int inputsize = 64 - (int)tsz;

    /* Bits above the input size must all match bit 55 (or bit 63 without TBI). */
    int top = tbi ? 55 : 63;
    uint64_t mask = (top - inputsize + 1 >= 64 ? ~0ull : ((1ull << (top - inputsize + 1)) - 1)) << inputsize;
    if ((va & mask) != (hi ? mask : 0) || epd) {
        out->fsc = HV_FSC_TRANSLATION(0);
        return;
    }

    int stride = g - 3;
    int levels = (inputsize - g + stride - 1) / stride;
    int level = 4 - levels;
    uint64_t base = (hi ? cpu->ttbr1_el1 : cpu->ttbr0_el1) & OA_MASK & ~1ull;
    int ap_table = 0, pxn_table = 0, uxn_table = 0;

    for (;; level++) {
        int shift = g + stride * (3 - level);
        int idx_bits = level == 4 - levels ? inputsize - shift : stride;
        uint64_t idx = (va >> shift) & ((1ull << idx_bits) - 1);
        uint64_t desc;
        if (hv_phys_read(cpu->m, base + idx * 8, &desc, 8) != 0) {
            out->fsc = 0x14 + level; /* synchronous external abort on a table walk */
            out->s1ptw = 1;
            return;
        }
        if (!(desc & 1)) {
            out->fsc = HV_FSC_TRANSLATION(level);
            return;
        }
        if (level < 3 && (desc & 2)) { /* table */
            base = desc & OA_MASK & ~((1ull << g) - 1);
            if (!hpd) {
                pxn_table |= (desc >> 59) & 1;
                uxn_table |= (desc >> 60) & 1;
                ap_table |= (desc >> 61) & 3;
            }
            continue;
        }
        if (level == 3 && !(desc & 2)) {
            out->fsc = HV_FSC_TRANSLATION(level);
            return;
        }
        if (level < 3) { /* block: 4K at levels 1-2, 16K and 64K at level 2 */
            if (level == 0 || (g != 12 && level == 1)) {
                out->fsc = HV_FSC_TRANSLATION(level);
                return;
            }
        }
        uint64_t size = 1ull << shift;
        out->block_size = size;
        out->pa = (desc & OA_MASK & ~(size - 1)) | (va & (size - 1));
        out->attr = (cpu->mair_el1 >> (((desc >> 2) & 7) * 8)) & 0xff;
        if (!(desc & (1u << 10))) {
            out->fsc = HV_FSC_ACCESS(level);
            return;
        }
        int ap = (desc >> 6) & 3;
        int uxn = ((desc >> 54) & 1) | uxn_table;
        int pxn = ((desc >> 53) & 1) | pxn_table;
        int el0_access = (ap & 1) && !(ap_table & 1);
        int read_only = (ap & 2) || (ap_table & 2);
        if (el == 0) {
            out->r = el0_access;
            out->w = el0_access && !read_only;
            out->x = !uxn;
        } else {
            int pan = (cpu->pstate_extra & HV_PSTATE_PAN) && el0_access;
            out->r = !pan;
            out->w = !pan && !read_only;
            /* EL1 never executes what EL0 can write. */
            out->x = !pxn && !(el0_access && !read_only);
        }
        if ((sctlr >> 19) & 1 && !read_only && (el == 0 ? el0_access : 1)) out->x = 0; /* WXN */
        int ok = acc == HV_ACC_READ ? out->r : acc == HV_ACC_WRITE ? out->w : out->x;
        if (!ok) out->fsc = HV_FSC_PERMISSION(level);
        return;
    }
}

void hv_tlb_flush(HvCpu *cpu) {
    if (cpu->m) cpu->m->stats.tlb_flushes++;
    hv_ijc_reset(cpu, 0); /* mappings changed: branch targets cached by VA are no longer trusted */
    hv_ijc_reset(cpu, 1);
    memset(cpu->tlb, 0x80, sizeof cpu->tlb);
    cpu->nuser_fills = 0;
}

void hv_ijc_reset(HvCpu *cpu, int kernel) {
    cpu->ijc_salt[kernel]++;
    if (cpu->m) {
        cpu->m->stats.ijc_resets++;
        memset(cpu->m->ijc_pages[kernel], 0, sizeof cpu->m->ijc_pages[kernel]);
    }
}

/* TTBR0 changed or an ASID was dropped: only user-half (TTBR0) translations go;
   the kernel's TTBR1 entries stay. */
void hv_tlb_flush_user(HvCpu *cpu) {
    if (cpu->m) cpu->m->stats.tlb_flushes++;
    hv_ijc_reset(cpu, 0); /* user VAs now name another process's code; the kernel's entries stay */
    uint32_t n = cpu->nuser_fills;
    cpu->nuser_fills = 0;
    if (n <= HV_USER_FILLS) {
        for (uint32_t k = 0; k < n; k++) {
            HvTlbEntry *e = &cpu->tlb[0][0] + cpu->user_fills[k];
            uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? e->exec_tag : e->write_tag + HV_TLB_ADJ;
            if (e->read_tag != HV_TLB_INVALID || e->exec_tag != HV_TLB_INVALID || e->write_tag != HV_TLB_INVALID) if (!((tag >> 55) & 1)) memset(e, 0x80, sizeof *e); /* the slot may hold a kernel page by now */
        }
        return;
    }
    for (int t = 0; t < 3; t++)
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++) {
            HvTlbEntry *e = &cpu->tlb[t][i];
            uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? e->exec_tag : e->write_tag + HV_TLB_ADJ;
            if (e->read_tag != HV_TLB_INVALID || e->exec_tag != HV_TLB_INVALID || e->write_tag != HV_TLB_INVALID) if (!((tag >> 55) & 1)) memset(e, 0x80, sizeof *e);
        }
}

/* A page just got translated code: writes to it must take the slow path from now
   on, so drop just the write permission of the entries that map it. */
void hv_tlb_protect_code(HvCpu *cpu, const uint8_t *host_page) {
    for (int t = 0; t < 3; t++)
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++) {
            HvTlbEntry *e = &cpu->tlb[t][i];
            if (e->write_tag != HV_TLB_INVALID && (const uint8_t *)(uintptr_t)(e->write_tag + HV_TLB_ADJ + e->addend) == host_page)
                e->write_tag = HV_TLB_INVALID;
        }
}

void hv_tlb_flush_va(HvCpu *cpu, uint64_t va) {
    /* Tagged addresses may sit in any slot's tag byte, so check by the low 56 bits. */
    uint32_t idx = (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    /* The page may hold cached branch targets even if its TLB entry is gone. */
    int kernel = (va >> 55) & 1;
    uint32_t h = (uint32_t)(va >> HV_TLB_PAGE_BITS) & 0xffff;
    if (cpu->m && (cpu->m->ijc_pages[kernel][h >> 6] >> (h & 63) & 1)) hv_ijc_reset(cpu, kernel);
    for (int el = 0; el < 3; el++) {
        HvTlbEntry *e = &cpu->tlb[el][idx];
        memset(e, 0x80, sizeof *e);
    }
}

static void tlb_fill(HvCpu *cpu, uint64_t va, int el, const HvWalk *w) {
    if (w->block_size < (1ull << HV_TLB_PAGE_BITS)) return; /* smaller guest pages: slow path only */
    HvRam *ram = hv_ram_lookup(cpu->m, w->pa);
    if (!ram) return;
    uint64_t page_pa = w->pa & ~PAGE_MASK;
    if (page_pa < ram->pa || page_pa + PAGE_MASK + 1 - ram->pa > ram->size) return;
    uint64_t pg = (page_pa - ram->pa) >> HV_TLB_PAGE_BITS;
    uint64_t page_va = va & ~PAGE_MASK;
    int table = hv_tlb_table(cpu, el);
    uint32_t slot = (uint32_t)(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    HvTlbEntry *e = &cpu->tlb[table][slot];
    if (!((page_va >> 55) & 1)) {
        if (cpu->nuser_fills < HV_USER_FILLS) cpu->user_fills[cpu->nuser_fills] = (uint16_t)(table << HV_TLB_BITS | slot);
        if (cpu->nuser_fills <= HV_USER_FILLS) cpu->nuser_fills++;
    }
    e->addend = (int64_t)((uint64_t)(uintptr_t)(ram->host + (page_pa - ram->pa)) - page_va);
    e->read_tag = w->r ? page_va - HV_TLB_ADJ : HV_TLB_INVALID;
    /* Writes to ROM or to pages we translated from take the slow path. */
    e->write_tag = w->w && !ram->readonly && !ram->code_pages[pg] ? page_va - HV_TLB_ADJ : HV_TLB_INVALID;
    /* A watched page takes the slow path for every write. */
    if (cpu->m->watch_va && (cpu->m->watch_va & ~PAGE_MASK & 0x00ffffffffffffffull) == (page_va & 0x00ffffffffffffffull))
        e->write_tag = HV_TLB_INVALID;
    if (cpu->m->watch_pa && (cpu->m->watch_pa & ~PAGE_MASK) == page_pa) e->write_tag = HV_TLB_INVALID;
    e->exec_tag = w->x ? page_va : HV_TLB_INVALID;
}

uint32_t hv_mmu_access(HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t *pa) {
    HvWalk w;
    hv_mmu_walk(cpu, va, acc, el, &w);
    if (w.fsc) return w.fsc;
    tlb_fill(cpu, va, el, &w);
    *pa = w.pa;
    return 0;
}

const uint32_t *hv_mmu_fetch(HvCpu *cpu, uint64_t va, uint64_t *pa, uint32_t *avail) {
    if (va & 3) {
        hv_raise_sync(cpu, HV_EC_PC_ALIGN, 0, va, va);
        return NULL;
    }
    HvTlbEntry *t = &cpu->tlb[hv_tlb_table(cpu, cpu->el)][(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1)];
    if (t->exec_tag == (va & ~PAGE_MASK)) {
        uint8_t *host = (uint8_t *)(uintptr_t)(va + t->addend);
        for (int i = 0; i < cpu->m->nram; i++) {
            HvRam *r = &cpu->m->ram[i];
            if ((uint64_t)(host - r->host) < r->size) {
                *pa = r->pa + (uint64_t)(host - r->host);
                *avail = (uint32_t)(((1ull << HV_TLB_PAGE_BITS) - (va & PAGE_MASK)) / 4);
                return (const uint32_t *)host;
            }
        }
    }
    HvWalk w;
    cpu->m->stats.fetch_walks++;
    hv_mmu_walk(cpu, va, HV_ACC_EXEC, cpu->el, &w);
    if (w.fsc) {
        hv_raise_abort(cpu, 1, va, w.fsc, 0, w.s1ptw, va);
        return NULL;
    }
    tlb_fill(cpu, va, cpu->el, &w);
    uint64_t chunk = w.block_size < (1ull << HV_TLB_PAGE_BITS) ? w.block_size : 1ull << HV_TLB_PAGE_BITS;
    uint64_t left = chunk - (va & (chunk - 1));
    const uint32_t *p = hv_phys_ptr(cpu->m, w.pa, 4);
    if (!p) {
        hv_raise_abort(cpu, 1, va, HV_FSC_EXTERNAL, 0, 0, va);
        return NULL;
    }
    HvRam *ram = hv_ram_lookup(cpu->m, w.pa);
    uint64_t ram_left = ram->size - (w.pa - ram->pa);
    if (left > ram_left) left = ram_left;
    *pa = w.pa;
    *avail = (uint32_t)(left / 4);
    return p;
}
