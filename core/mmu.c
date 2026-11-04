#include "mmu.h"

#include <stdlib.h>
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

/* This file is built with -mgeneral-regs-only, and the walk and the TLB fill
   call nothing in libc: hv_tlb_refill runs them from translated code without
   saving the guest's SIMD registers, which libc's memory routines would use. */
static HvRam *ram_of(HvMachine *m, uint64_t pa) {
    for (int i = 0; i < m->nram; i++)
        if (pa - m->ram[i].pa < m->ram[i].size) return &m->ram[i];
    return NULL;
}

/* A descriptor: RAM, 8 bytes, aligned. Device memory as a page table is not supported. */
static int read_desc(HvMachine *m, uint64_t pa, uint64_t *out) {
    HvRam *r = ram_of(m, pa);
    if (!r || pa + 8 - r->pa > r->size) return -1;
    *out = __atomic_load_n((const uint64_t *)(const void *)(r->host + (pa - r->pa)), __ATOMIC_RELAXED);
    return 0;
}

void hv_mmu_walk(HvCpu *cpu, uint64_t va, HvAccess acc, int el, HvWalk *out) {
    *out = (HvWalk){0};
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
        if (read_desc(cpu->m, base + idx * 8, &desc) != 0) {
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
    if (cpu->m) cpu->m->stats.reset_why[0]++;
    hv_ijc_reset(cpu, 0); /* mappings changed: branch targets cached by VA are no longer trusted */
    hv_ijc_reset(cpu, 1);
    memset(cpu->tlb, 0x80, sizeof cpu->tlb);
    cpu->nuser_fills = 0;
    hv_tlb_stash_drop(cpu, -1);
}

/* Clear the tables of one half (user tables are the even flags values, kernel ones odd). */
static void ijc_clear(HvCpu *cpu, int kernel) {
    if (!cpu->pc_state) return;
    for (int t = kernel; t < HV_IJC_TABLES; t += 2)
        memset(cpu->pc_state->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8), 0, HV_IJC_TABLE_BYTES);
}

/* ASIDs live in TTBR0 unless TCR.A1 puts them in TTBR1; then every process looks alike to us. */
static int current_asid(HvCpu *cpu) {
    return (cpu->tcr_el1 >> 22) & 1 ? -1 : (int)(cpu->ttbr0_el1 >> 48);
}

/* ---- the (user id, page) set ---- */

static uint64_t ucode_key(uint32_t cid, uint64_t va) {
    return ((uint64_t)cid << 40 | ((va >> HV_TLB_PAGE_BITS) & 0xffffffffffull)) + 1; /* never 0, the empty slot */
}

static uint32_t ucode_slot(uint64_t key) {
    return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> (64 - HV_UCODE_BITS));
}

static void ucode_clear(HvPerCpu *m) {
    if (m && m->ucode) memset(m->ucode, 0, (sizeof *m->ucode) << HV_UCODE_BITS);
    if (m) m->ucode_count = 0;
}

static int ucode_has(HvPerCpu *m, uint32_t cid, uint64_t va) {
    uint64_t key = ucode_key(cid, va);
    for (uint32_t i = ucode_slot(key);; i = (i + 1) & ((1u << HV_UCODE_BITS) - 1)) {
        if (m->ucode[i] == key) return 1;
        if (!m->ucode[i]) return 0;
    }
}

void hv_ucode_add(HvCpu *cpu, uint64_t va) {
    HvPerCpu *m = cpu->pc_state;
    uint32_t cid = (uint32_t)(cpu->ijc_salt[0] >> 48);
    if (!m || !cid) return;
    uint64_t key = ucode_key(cid, va);
    uint32_t i = ucode_slot(key);
    for (; m->ucode[i]; i = (i + 1) & ((1u << HV_UCODE_BITS) - 1))
        if (m->ucode[i] == key) return;
    if (m->ucode_count >= (1u << HV_UCODE_BITS) / 2) { /* half full: start the user half over */
        hv_ijc_reset(cpu, 0);
        hv_ijc_switch(cpu);
        cid = (uint32_t)(cpu->ijc_salt[0] >> 48);
        key = ucode_key(cid, va);
        i = ucode_slot(key);
    }
    m->ucode[i] = key;
    m->ucode_count++;
}

/* A user id no tag in the tables uses yet; when they run out, empty the user tables and start over. */
static uint16_t fresh_cid(HvCpu *cpu) {
    if (cpu->next_cid == 0 || cpu->next_cid > 0xffff) {
        ijc_clear(cpu, 0);
        ucode_clear(cpu->pc_state);
        memset(cpu->asid_cid, 0, sizeof cpu->asid_cid);
        cpu->next_cid = 1;
    }
    return (uint16_t)cpu->next_cid++;
}

void hv_ijc_switch(HvCpu *cpu) {
    int asid = current_asid(cpu);
    if (asid < 0) { /* no ASIDs to tell processes apart: a switch forgets the old one */
        cpu->ijc_salt[0] = (uint64_t)fresh_cid(cpu) << 48;
        return;
    }
    if (!cpu->asid_cid[asid]) {
        cpu->asid_cid[asid] = fresh_cid(cpu);
        cpu->cid_asid[cpu->asid_cid[asid]] = (uint16_t)asid;
    }
    cpu->ijc_salt[0] = (uint64_t)cpu->asid_cid[asid] << 48;
}

void hv_ijc_drop_asid(HvCpu *cpu, uint16_t asid) {
    if (cpu->m) cpu->m->stats.ijc_resets++;
    cpu->asid_cid[asid] = 0;
    int cur = current_asid(cpu);
    if (cur == asid || cur < 0) hv_ijc_switch(cpu); /* without ASIDs, any drop may be about the current process */
}

void hv_ijc_reset(HvCpu *cpu, int kernel) {
    if (cpu->m) {
        cpu->m->stats.ijc_resets++;
        memset(cpu->pc_state->ijc_pages[kernel], 0, sizeof cpu->pc_state->ijc_pages[kernel]);
    }
    if (kernel) {
        /* Kernel VAs have 0xffff in bits 63..48, so the salt must not make that 0 (the empty tag). */
        if (++cpu->kernel_gen >= 0xffff) {
            ijc_clear(cpu, 1);
            cpu->kernel_gen = 1;
        }
        cpu->ijc_salt[1] = (uint64_t)cpu->kernel_gen << 48;
        return;
    }
    memset(cpu->asid_cid, 0, sizeof cpu->asid_cid);
    ucode_clear(cpu->pc_state); /* every user id is new now */
    hv_ijc_switch(cpu);
}

/* TTBR0 changed or an ASID was dropped: only user-half (TTBR0) translations go;
   the kernel's TTBR1 entries stay. */
void hv_tlb_flush_user(HvCpu *cpu) {
    if (cpu->m) cpu->m->stats.tlb_flushes++;
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
            HvTlbEntry *e = &cpu->tlb[i][t];
            uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? e->exec_tag : e->write_tag + HV_TLB_ADJ;
            if (e->read_tag != HV_TLB_INVALID || e->exec_tag != HV_TLB_INVALID || e->write_tag != HV_TLB_INVALID) if (!((tag >> 55) & 1)) memset(e, 0x80, sizeof *e);
        }
}

/* A page just got translated code: writes to it must take the slow path from now
   on, so drop just the write permission of the entries that map it. */
static int maps_writable(const HvTlbEntry *e, const uint8_t *host_page) {
    return e->write_tag != HV_TLB_INVALID && (const uint8_t *)(uintptr_t)(e->write_tag + HV_TLB_ADJ + e->addend) == host_page;
}

/* RAM page pg just became code: drop write permission from cpu's entries for it. */
void hv_tlb_protect_code_page(HvCpu *cpu, HvRam *ram, uint64_t pg) {
    uint32_t *hint = &ram->whint[pg * HV_MAX_CPUS + cpu->index];
    uint32_t h = __atomic_exchange_n(hint, 0, __ATOMIC_SEQ_CST);
    const uint8_t *host_page = ram->host + (pg << HV_TLB_PAGE_BITS);
    if (h == HV_WHINT_MANY) {
        hv_tlb_protect_code(cpu, host_page);
    } else if (h) {
        HvTlbEntry *e = &cpu->tlb[0][0] + (h - 1);
        if (maps_writable(e, host_page)) e->write_tag = HV_TLB_INVALID;
    }
}

void hv_tlb_protect_code(HvCpu *cpu, const uint8_t *host_page) {
    for (int t = 0; t < 3; t++)
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++) {
            HvTlbEntry *e = &cpu->tlb[i][t];
            if (e->write_tag != HV_TLB_INVALID && (const uint8_t *)(uintptr_t)(e->write_tag + HV_TLB_ADJ + e->addend) == host_page)
                e->write_tag = HV_TLB_INVALID;
        }
}

/* Forget one salt's cached targets in one 16 KB page. A page's 4096 possible
   targets have 4096 known slots in each table, so only those are looked at. */
static void ijc_clear_page(HvCpu *cpu, uint64_t page, int kernel, uint64_t salt) {
    HvPerCpu *m = cpu->pc_state;
    for (int t = kernel; t < HV_IJC_TABLES; t += 2) {
        if (!(m->ijc_tables_used >> t & 1)) continue;
        uint64_t *tab = m->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8);
        for (uint32_t o = 0; o < (1u << HV_TLB_PAGE_BITS) / 4; o++) {
            uint64_t tag = (page | (uint64_t)o << 2) ^ salt;
            uint64_t *ent = tab + (size_t)hv_ijc_index(tag) * 2;
            if (ent[0] == tag) ent[0] = 0;
        }
    }
}

/* The page may hold cached branch targets even if its TLB entry is gone: forget
   them, if the filters say it has any. */
/* remote: another CPU doing it (hv_tlbi_drop_remote_va): returns 0 instead of
   starting the user half over, which only cpu itself may do. */
static int ijc_drop_va(HvCpu *cpu, uint64_t va, int asid, int remote) {
    int kernel = (va >> 55) & 1;
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    if (m && p) {
        int drop = 0;
        if (kernel) {
            uint32_t k = hv_ijc_key(va, cpu->ijc_salt[1]);
            drop = p->ijc_keyed[k >> 6] >> (k & 63) & 1;
        } else if (asid < 0) { /* every address space: any process's targets in the page count */
            uint32_t h = (uint32_t)(va >> HV_TLB_PAGE_BITS) & 0xffff;
            drop = p->ijc_pages[0][h >> 6] >> (h & 63) & 1;
        } else {
            uint64_t salt = current_asid(cpu) < 0 ? cpu->ijc_salt[0] : (uint64_t)cpu->asid_cid[asid] << 48;
            drop = (salt >> 48) && ucode_has(p, (uint32_t)(salt >> 48), va);
        }
        if (drop) {
            if (remote && !kernel && asid < 0) return 0;
            m->stats.reset_why[kernel ? 3 : 2]++;
            uint64_t page = va & ~((1ull << HV_TLB_PAGE_BITS) - 1);
            if (kernel) ijc_clear_page(cpu, page, 1, cpu->ijc_salt[1]);
            else if (asid < 0) m->stats.reset_why[5]++, hv_ijc_reset(cpu, 0); /* every address space: their slots differ, so start the user half over */
            else ijc_clear_page(cpu, page, 0, current_asid(cpu) < 0 ? cpu->ijc_salt[0] : (uint64_t)cpu->asid_cid[asid] << 48);
        }
    }
    return 1;
}

void hv_ijc_drop_va(HvCpu *cpu, uint64_t va, int asid) {
    ijc_drop_va(cpu, va, asid, 0);
}

void hv_tlb_flush_va_asid(HvCpu *cpu, uint64_t va, int asid) {
    /* Tagged addresses may sit in any slot's tag byte, so check by the low 56 bits. */
    uint32_t idx = (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    hv_ijc_drop_va(cpu, va, asid);
    if (!((va >> 55) & 1)) hv_tlb_stash_drop(cpu, asid);
    for (int el = 0; el < 3; el++) {
        HvTlbEntry *e = &cpu->tlb[idx][el];
        memset(e, 0x80, sizeof *e);
    }
}

/* A TLBI by range too big to do page by page in the TLB (which then drops its
   user half): forget cached branch targets only in the range's code pages. */
void hv_ijc_drop_range(HvCpu *cpu, uint64_t va, uint64_t len, int asid) {
    HvMachine *m = cpu->m;
    if (!m) return;
    if (asid < 0) { /* every address space */
        m->stats.reset_why[5]++;
        hv_ijc_reset(cpu, 0);
        return;
    }
    uint64_t salt = current_asid(cpu) < 0 ? cpu->ijc_salt[0] : (uint64_t)cpu->asid_cid[asid] << 48;
    uint32_t cid = (uint32_t)(salt >> 48);
    if (!cid) return; /* nothing cached for it */
    uint64_t first = va >> HV_TLB_PAGE_BITS, last = (va + len - 1) >> HV_TLB_PAGE_BITS;
    if (last - first >= 4096) { /* scanning the whole set costs more than the cold start: drop the address space */
        hv_ijc_drop_asid(cpu, (uint16_t)asid);
        return;
    }
    {
        for (uint64_t p = first; p <= last; p++)
            if (ucode_has(cpu->pc_state, cid, p << HV_TLB_PAGE_BITS)) {
                m->stats.reset_why[2]++;
                ijc_clear_page(cpu, p << HV_TLB_PAGE_BITS, 0, salt);
            }
        return;
    }
    for (uint32_t i = 0; i < 1u << HV_UCODE_BITS; i++) {
        uint64_t k = cpu->pc_state->ucode[i];
        if (!k || (k - 1) >> 40 != cid) continue;
        uint64_t p = (k - 1) & 0xffffffffffull;
        if (p >= (first & 0xffffffffffull) && p <= (last & 0xffffffffffull)) {
            m->stats.reset_why[2]++;
            ijc_clear_page(cpu, p << HV_TLB_PAGE_BITS, 0, salt);
        }
    }
}

void hv_tlb_flush_va(HvCpu *cpu, uint64_t va) {
    hv_tlb_flush_va_asid(cpu, va, -1);
}

/* A writable entry just went into the TLB at code (slot << 2 | table) for RAM
   page pg. Another CPU may have just translated code from the page, setting
   code_pages and then dropping write tags in every TLB: if that came before
   this entry was there, it must not stay writable. */
static void stash_drop(HvPerCpu *p, int asid);

static void write_guard(HvCpu *cpu, HvRam *ram, uint64_t pg, uint32_t code, HvTlbEntry *e) {
    /* Say where, so that the page becoming code drops just this entry (or, if
       this CPU now maps it writable twice, makes the protector scan). */
    uint32_t *hint = &ram->whint[pg * HV_MAX_CPUS + cpu->index];
    uint32_t mine = code + 1, old = __atomic_load_n(hint, __ATOMIC_RELAXED);
    const uint8_t *host_page = ram->host + (pg << HV_TLB_PAGE_BITS);
    if (old && old != mine && old != HV_WHINT_MANY && maps_writable(&cpu->tlb[0][0] + (old - 1), host_page)) mine = HV_WHINT_MANY;
    if (old != HV_WHINT_MANY) __atomic_store_n(hint, mine, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&ram->code_pages[pg], __ATOMIC_RELAXED)) e->write_tag = HV_TLB_INVALID;
}

static void tlb_fill(HvCpu *cpu, uint64_t va, int el, const HvWalk *w) {
    if (w->block_size < (1ull << HV_TLB_PAGE_BITS)) return; /* smaller guest pages: slow path only */
    HvRam *ram = ram_of(cpu->m, w->pa);
    if (!ram) return;
    uint64_t page_pa = w->pa & ~PAGE_MASK;
    if (page_pa < ram->pa || page_pa + PAGE_MASK + 1 - ram->pa > ram->size) return;
    uint64_t pg = (page_pa - ram->pa) >> HV_TLB_PAGE_BITS;
    uint64_t page_va = va & ~PAGE_MASK;
    int table = hv_tlb_table(cpu, el);
    uint32_t slot = (uint32_t)(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    HvTlbEntry *e = &cpu->tlb[slot][table];
    if (!((page_va >> 55) & 1)) {
        if (cpu->nuser_fills < HV_USER_FILLS) cpu->user_fills[cpu->nuser_fills] = (uint16_t)(slot << 2 | table);
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
    if (e->write_tag != HV_TLB_INVALID) write_guard(cpu, ram, pg, slot << 2 | table, e);
}

/* Walks that may fill the TLB are bracketed by the CPU's walk_seq (odd while
   one runs), so another CPU's TLBI can drop entries here itself and know no
   fill from an old page table raced it (hv_tlbi_drop_remote_va). */
static void walk_begin(HvCpu *cpu) {
    if (cpu->pc_state) __atomic_add_fetch(&cpu->pc_state->walk_seq, 1, __ATOMIC_SEQ_CST);
}

static void walk_end(HvCpu *cpu) {
    if (cpu->pc_state) __atomic_add_fetch(&cpu->pc_state->walk_seq, 1, __ATOMIC_RELEASE);
}

uint32_t hv_mmu_access(HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t *pa) {
    HvWalk w;
    walk_begin(cpu);
    hv_mmu_walk(cpu, va, acc, el, &w);
    if (!w.fsc) tlb_fill(cpu, va, el, &w);
    walk_end(cpu);
    if (w.fsc) return w.fsc;
    *pa = w.pa;
    return 0;
}

int hv_tlbi_drop_remote_va(HvCpu *target, uint64_t va, int asid) {
    HvPerCpu *p = target->pc_state;
    uint32_t s1 = __atomic_load_n(&p->walk_seq, __ATOMIC_SEQ_CST);
    if (s1 & 1) return 0; /* mid-walk: it may be about to fill from the old table */
    uint32_t c1 = __atomic_load_n(&p->ijc_seq, __ATOMIC_SEQ_CST);
    if (c1 & 1) return 0; /* caching a block it may have looked up through the old mapping */
    if (!((va >> 55) & 1)) stash_drop(p, asid);
    uint32_t idx = (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    for (int t = 0; t < 3; t++) { /* the slot whatever its tag byte, as hv_tlb_flush_va_asid does */
        HvTlbEntry *e = &target->tlb[idx][t];
        __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Its cached branch targets in the page too: a cache fill checks the TLB
       entry first (hv_ijc_fill_checked), so one after this sees it gone, and one
       during it changed ijc_seq. */
    if (!ijc_drop_va(target, va, asid, 1)) return 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&p->walk_seq, __ATOMIC_SEQ_CST) == s1 && /* no fill in between */
           __atomic_load_n(&p->ijc_seq, __ATOMIC_SEQ_CST) == c1;
}

const uint32_t *hv_mmu_fetch(HvCpu *cpu, uint64_t va, uint64_t *pa, uint32_t *avail) {
    if (va & 3) {
        hv_raise_sync(cpu, HV_EC_PC_ALIGN, 0, va, va);
        return NULL;
    }
    HvTlbEntry *t = &cpu->tlb[(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1)][hv_tlb_table(cpu, cpu->el)];
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
    walk_begin(cpu);
    hv_mmu_walk(cpu, va, HV_ACC_EXEC, cpu->el, &w);
    if (!w.fsc) tlb_fill(cpu, va, cpu->el, &w);
    walk_end(cpu);
    if (w.fsc) {
        hv_raise_abort(cpu, 1, va, w.fsc, 0, w.s1ptw, va);
        return NULL;
    }
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

/* A TLB miss in translated code, before the full helper: walk and fill, and
   say whether the fast path will now succeed (0) or the helper must do the
   access (a fault, device memory, a write to a code page, an unusual spot in
   the page). Called by hv_helper_thunk with the guest's SIMD registers live,
   so nothing here may use them (see read_desc). flags: bit 0 write, bits 2..1
   the TLB table (0 EL0 or unprivileged, 1 EL1, 2 EL1 with PAN), bits 8..3 bytes - 1. */
int hv_tlb_refill(HvCpu *cpu, uint64_t va, uint32_t flags) {
    int write = flags & 1, table = (flags >> 1) & 3;
    uint32_t bytes = ((flags >> 3) & 63) + 1;
    int el = table ? 1 : 0;
    uint64_t off = va & PAGE_MASK;
    if (off + bytes > PAGE_MASK + 1) return 1; /* crosses into the next page: the helper */
    HvWalk w;
    walk_begin(cpu);
    hv_mmu_walk(cpu, va, write ? HV_ACC_WRITE : HV_ACC_READ, el, &w);
    if (!w.fsc) tlb_fill(cpu, va, el, &w);
    walk_end(cpu);
    if (w.fsc) return 1;
    HvTlbEntry *e = &cpu->tlb[(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1)][table];
    uint64_t tag = write ? e->write_tag : e->read_tag;
    if (cpu->m->stats_interval) cpu->m->stats.refills++;
    return tag != HV_TLB_INVALID && tag + HV_TLB_ADJ == (va & ~(uint64_t)PAGE_MASK) ? 0 : 1;
}

/* Put block b in this CPU's indirect-branch cache under its VA (b->va must be in
   the half its flags say: kernel blocks, kernel VAs). */
void hv_ijc_fill(HvCpu *cpu, HvBlock *b) {
    HvMachine *m = cpu->m;
    HvPerCpu *pcs = cpu->pc_state;
    int kernel = b->flags & HV_BLOCK_EL1;
    uint64_t *t = pcs->ijc + (size_t)(b->flags & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8);
    uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
    uint64_t *ent = t + (size_t)hv_ijc_index(tag) * 2;
    ent[0] = tag;
    ent[1] = (uint64_t)(uintptr_t)b->code;
    pcs->ijc_tables_used |= 1u << (b->flags & (HV_IJC_TABLES - 1));
    if (!kernel) {
        hv_ucode_add(cpu, b->va);
        uint32_t h = (uint32_t)(b->va >> HV_TLB_PAGE_BITS) & 0xffff;
        pcs->ijc_pages[0][h >> 6] |= 1ull << (h & 63);
    }
    uint32_t k = hv_ijc_key(b->va, cpu->ijc_salt[kernel]);
    if (!(pcs->ijc_keyed[k >> 6] >> (k & 63) & 1)) {
        pcs->ijc_keyed[k >> 6] |= 1ull << (k & 63);
        if (++pcs->ijc_keyed_count > (1u << 17)) { /* an eighth full: start the cache and the filter over */
            m->stats.reset_why[6]++;
            hv_ijc_reset(cpu, 0);
            hv_ijc_reset(cpu, 1);
            memset(pcs->ijc_keyed, 0, sizeof pcs->ijc_keyed);
            pcs->ijc_keyed_count = 0;
        }
    }
}

/* An indirect-branch cache miss in translated code, before the dispatcher: if
   the target's block already exists (found through this CPU's TLB and the
   block hash), cache it and return its code for the thunk to jump to; else 0
   and the dispatcher does it. Called by hv_ijc_thunk with the guest's SIMD
   registers live (see read_desc), so nothing that could reach libc runs: a
   fill that would reset the cache's filters goes to the dispatcher instead.
   flags: the block flags the branch was translated under (the target's too). */
/* Does this CPU's TLB (the table for flags) map b's page where b came from? */
static int tlb_maps_block(HvCpu *cpu, const HvBlock *b) {
    int table = !(b->flags & HV_BLOCK_EL1) ? 0 : (b->flags & HV_BLOCK_PAN) ? 2 : 1;
    const HvTlbEntry *e = &cpu->tlb[(b->va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1)][table];
    uint64_t page_va = b->va & ~(uint64_t)PAGE_MASK, page_pa = b->pa & ~(uint64_t)PAGE_MASK;
    return __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED) == page_va &&
           (uint64_t)e->addend == (uint64_t)(uintptr_t)(b->ram->host + (page_pa - b->ram->pa)) - page_va;
}

/* The dispatcher caching a block it found for va: only while the TLB still
   says va is there, inside ijc_seq, so another CPU's TLBI can drop this CPU's
   cached targets itself (hv_tlbi_drop_remote_va). */
int hv_ijc_fill_checked(HvCpu *cpu, HvBlock *b) {
    HvPerCpu *p = cpu->pc_state;
    if (!p) {
        hv_ijc_fill(cpu, b);
        return 1;
    }
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_SEQ_CST);
    int ok = tlb_maps_block(cpu, b);
    if (ok) hv_ijc_fill(cpu, b);
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_RELEASE);
    return ok;
}

static void *ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags);

void *hv_ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags) {
    HvPerCpu *p = cpu->pc_state;
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_SEQ_CST);
    void *r = ijc_resolve(cpu, va, flags);
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_RELEASE);
    return r;
}

static void *ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags) {
    HvMachine *m = cpu->m;
    HvPerCpu *pcs = cpu->pc_state;
    int kernel = flags & HV_BLOCK_EL1;
#define FAIL(k) do { if (m->stats_interval) m->stats.light_fail[k]++; return 0; } while (0)
    if (!m->chain || va == m->stop_pc || (int)((va >> 55) & 1) != kernel || (va & 3)) FAIL(0);
    if (pcs->ijc_keyed_count >= (1u << 17) || pcs->ucode_count + 1 >= (1u << HV_UCODE_BITS) / 2) FAIL(1);
    int table = !kernel ? 0 : (flags & HV_BLOCK_PAN) ? 2 : 1;
    HvTlbEntry *e = &cpu->tlb[(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1)][table];
    if (e->exec_tag != (va & ~(uint64_t)PAGE_MASK)) {
        /* Not in the TLB (a process switch, or another page took the slot):
           walk for it here, as hv_tlb_refill does for data. A fault is for the
           dispatcher to take. */
        HvWalk w;
        walk_begin(cpu);
        hv_mmu_walk(cpu, va, HV_ACC_EXEC, kernel ? 1 : 0, &w);
        if (!w.fsc) tlb_fill(cpu, va, kernel ? 1 : 0, &w);
        walk_end(cpu);
        if (w.fsc || e->exec_tag != (va & ~(uint64_t)PAGE_MASK)) FAIL(2);
        if (m->stats_interval) m->stats.light_walks++;
    }
    const uint8_t *host = (const uint8_t *)(uintptr_t)(va + (uint64_t)e->addend);
    HvRam *ram = NULL;
    for (int i = 0; i < m->nram; i++)
        if ((uint64_t)(host - m->ram[i].host) < m->ram[i].size) ram = &m->ram[i];
    if (!ram) return 0;
    uint64_t pa = ram->pa + (uint64_t)(host - ram->host);
    uint32_t gen = __atomic_load_n(&ram->page_gen[(pa - ram->pa) >> HV_TLB_PAGE_BITS], __ATOMIC_ACQUIRE);
    /* This CPU's small cache in front of the block hash first (as the dispatcher's lookup). */
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    HvBlock *b = pcs->jc[j];
    if (!(b && b->va == va && b->pa == pa && b->flags == flags && b->gen == gen)) {
        b = hv_block_find(m, hv_block_hash(va, pa, flags), va, pa, flags, gen);
        if (!b) FAIL(3);
#undef FAIL
        pcs->jc[j] = b;
    }
    if (m->stats_interval) { /* what the fill replaces, as the dispatcher counts it */
        uint64_t *t = pcs->ijc + (size_t)(b->flags & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8);
        uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
        uint64_t old = t[(size_t)hv_ijc_index(tag) * 2];
        int k = !old ? 0 : (old >> 48) != (tag >> 48) ? 1 : 2;
        if (k == 1) {
            uint32_t c = (uint32_t)(old >> 48);
            if (!(kernel ? (c ^ 0xffff) == cpu->kernel_gen : cpu->asid_cid[cpu->cid_asid[c]] == c)) k = 3;
        }
        m->stats.light_kind[k]++;
        m->stats.ijc_light++;
    }
    hv_ijc_fill(cpu, b);
    return b->code;
}

/* ---- user TLB entries kept across process switches (HvTlbStash) ---- */

/* Forget the stashed entries of address space asid (-1: all). Other CPUs call
   this on the target's stashes too, between reading and rechecking its
   walk_seq: saves and restores run inside walk_begin/walk_end, so a TLBI that
   races one of them falls back to waiting for the target. */
static void stash_drop(HvPerCpu *p, int asid) {
    for (int i = 0; i < HV_STASHES; i++) {
        uint64_t t = __atomic_load_n(&p->stash[i].ttbr0, __ATOMIC_SEQ_CST);
        if (t && (asid < 0 || (int)(t >> 48) == asid)) __atomic_add_fetch(&p->stash[i].gen, 1, __ATOMIC_SEQ_CST);
    }
}

void hv_tlb_stash_drop(HvCpu *cpu, int asid) {
    if (cpu->pc_state) stash_drop(cpu->pc_state, asid);
}

static int user_entry(const HvTlbEntry *e) {
    if (e->read_tag == HV_TLB_INVALID && e->exec_tag == HV_TLB_INVALID && e->write_tag == HV_TLB_INVALID) return 0;
    uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? e->exec_tag : e->write_tag + HV_TLB_ADJ;
    return !((tag >> 55) & 1);
}

static HvRam *ram_of_host(HvMachine *m, const uint8_t *host) {
    for (int i = 0; i < m->nram; i++)
        if ((uint64_t)(host - m->ram[i].host) < m->ram[i].size) return &m->ram[i];
    return NULL;
}

/* TTBR0 went from old to now (a process switch): keep old's user entries
   aside, drop them from the TLB, and bring back now's if they were kept and no
   TLBI touched them since. */
void hv_tlb_switch_user(HvCpu *cpu, uint64_t old, uint64_t now) {
    HvPerCpu *p = cpu->pc_state;
    HvMachine *m = cpu->m;
    if (old == now) return; /* the same address space: a TLB tagged by ASID would keep everything too */
    static int no_stash = -1; /* HV_NO_STASH: a switch just drops the user entries */
    if (no_stash < 0) no_stash = getenv("HV_NO_STASH") != NULL;
    if (!p || current_asid(cpu) < 0 || m->watch_va || m->watch_pa || no_stash) {
        hv_tlb_flush_user(cpu);
        return;
    }
    walk_begin(cpu);
    m->stats.tlb_flushes++;
    uint32_t n = cpu->nuser_fills;
    cpu->nuser_fills = 0;
    HvTlbStash *s = NULL;
    if (old && n <= HV_STASH_CAP) {
        for (int i = 0; i < HV_STASHES && !s; i++)
            if (p->stash[i].ttbr0 == old) s = &p->stash[i];
        for (int i = 0; i < HV_STASHES && !s; i++)
            if (!p->stash[i].ttbr0) s = &p->stash[i];
        if (!s) {
            s = &p->stash[0];
            for (int i = 1; i < HV_STASHES; i++)
                if (p->stash[i].used < s->used) s = &p->stash[i];
        }
        __atomic_store_n(&s->ttbr0, old, __ATOMIC_SEQ_CST);
        s->saved_gen = __atomic_load_n(&s->gen, __ATOMIC_SEQ_CST);
        s->used = ++p->stash_clock;
        m->stats.stash_saves++;
    }
    uint32_t k = 0;
    if (n <= HV_USER_FILLS) {
        for (uint32_t i = 0; i < n; i++) {
            uint16_t code = cpu->user_fills[i];
            HvTlbEntry *e = &cpu->tlb[0][0] + code;
            if (!user_entry(e)) continue; /* the slot may hold a kernel page by now */
            if (s) {
                s->slot[k] = code;
                s->ent[k++] = *e;
            }
            memset(e, 0x80, sizeof *e);
        }
    } else {
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++)
            for (int t = 0; t < 3; t++)
                if (user_entry(&cpu->tlb[i][t])) memset(&cpu->tlb[i][t], 0x80, sizeof cpu->tlb[i][t]);
    }
    if (s) s->n = k;

    for (int i = 0; i < HV_STASHES; i++) {
        HvTlbStash *r = &p->stash[i];
        if (r->ttbr0 != now) continue;
        int fresh = __atomic_load_n(&r->gen, __ATOMIC_SEQ_CST) == r->saved_gen;
        __atomic_store_n(&r->ttbr0, 0, __ATOMIC_SEQ_CST); /* used up either way: the live TLB has them now */
        if (!fresh) break;
        for (uint32_t j = 0; j < r->n; j++) {
            HvTlbEntry *e = &cpu->tlb[0][0] + r->slot[j];
            *e = r->ent[j];
            cpu->user_fills[cpu->nuser_fills++] = r->slot[j];
            if (e->write_tag != HV_TLB_INVALID) {
                const uint8_t *host = (const uint8_t *)(uintptr_t)(e->write_tag + HV_TLB_ADJ + e->addend);
                HvRam *ram = ram_of_host(m, host);
                if (!ram) e->write_tag = HV_TLB_INVALID;
                else write_guard(cpu, ram, (uint64_t)(host - ram->host) >> HV_TLB_PAGE_BITS, r->slot[j], e);
            }
        }
        m->stats.stash_hits++;
        m->stats.stash_entries += r->n;
        break;
    }
    walk_end(cpu);
}
