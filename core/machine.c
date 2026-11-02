#include "machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmu.h"

extern void hv_exit(void);
extern void hv_helper_entry(void);

int hv_machine_init(HvMachine *m, size_t jit_bytes) {
    memset(m, 0, sizeof *m);
    if (hv_jit_region_open(&m->jit, jit_bytes) != 0) return -1;
    m->blocks = calloc(HV_MAX_BLOCKS, sizeof(HvBlock));
    if (!m->blocks) return -1;
    m->stop_pc = ~0ull;
    return 0;
}

void hv_machine_free(HvMachine *m) {
    hv_jit_region_close(&m->jit);
    for (int i = 0; i < m->nram; i++) free(m->ram[i].code_pages);
    free(m->blocks);
}

int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int readonly) {
    if (m->nram == HV_MAX_RAM) return -1;
    HvRam *r = &m->ram[m->nram++];
    r->pa = pa;
    r->size = size;
    r->host = host;
    r->readonly = readonly;
    r->code_pages = calloc((size + (1u << HV_TLB_PAGE_BITS) - 1) >> HV_TLB_PAGE_BITS, 1);
    return r->code_pages ? 0 : -1;
}

int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    if (m->nmmio == HV_MAX_MMIO) return -1;
    m->mmio[m->nmmio++] = (HvMmio){pa, size, opaque, read, write, name};
    return 0;
}

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa) {
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (pa - r->pa < r->size) return r;
    }
    return NULL;
}

HvMmio *hv_mmio_lookup(HvMachine *m, uint64_t pa) {
    for (int i = 0; i < m->nmmio; i++) {
        HvMmio *d = &m->mmio[i];
        if (pa - d->pa < d->size) return d;
    }
    return NULL;
}

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (!r || pa + len - r->pa > r->size) return NULL;
    return r->host + (pa - r->pa);
}

/* MMIO is accessed in naturally sized pieces of at most 8 bytes. */
int hv_phys_read(HvMachine *m, uint64_t pa, void *buf, unsigned len) {
    void *p = hv_phys_ptr(m, pa, len);
    if (p) {
        memcpy(buf, p, len);
        return 0;
    }
    HvMmio *d = hv_mmio_lookup(m, pa);
    if (!d || len > 8) return -1;
    uint64_t v = d->read ? d->read(d->opaque, pa - d->pa, len) : 0;
    memcpy(buf, &v, len);
    if (m->log >= 3)
        fprintf(stderr, "mmio read  %s+%llx size %u = %llx pc %llx\n", d->name, (unsigned long long)(pa - d->pa), len,
                (unsigned long long)v, (unsigned long long)m->access_pc);
    return 0;
}

int hv_phys_write(HvMachine *m, uint64_t pa, const void *buf, unsigned len) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (r && pa + len - r->pa <= r->size) {
        if (r->readonly) return 0; /* ROM: writes are ignored */
        uint64_t off = pa - r->pa;
        uint64_t first = off >> HV_TLB_PAGE_BITS, last = (off + len - 1) >> HV_TLB_PAGE_BITS;
        for (uint64_t pg = first; pg <= last; pg++) {
            /* A write to a page we translated from: drop the translations. */
            if (r->code_pages[pg]) {
                memset(r->code_pages, 0, (r->size + (1u << HV_TLB_PAGE_BITS) - 1) >> HV_TLB_PAGE_BITS);
                m->flush_pending = 1;
            }
        }
        memcpy(r->host + off, buf, len);
        return 0;
    }
    HvMmio *d = hv_mmio_lookup(m, pa);
    if (!d || len > 8) return -1;
    uint64_t v = 0;
    memcpy(&v, buf, len);
    if (m->log >= 3)
        fprintf(stderr, "mmio write %s+%llx size %u = %llx pc %llx\n", d->name, (unsigned long long)(pa - d->pa), len,
                (unsigned long long)v, (unsigned long long)m->access_pc);
    if (d->write) d->write(d->opaque, pa - d->pa, v, len);
    return 0;
}

void hv_cpu_reset(HvCpu *cpu, HvMachine *m) {
    memset(cpu, 0, sizeof *cpu);
    cpu->m = m;
    cpu->exit_routine = (uint64_t)hv_exit;
    cpu->helper_routine = (uint64_t)hv_helper_entry;
    cpu->el = 1;
    cpu->spsel = 1;
    cpu->daif = HV_PSTATE_DAIF;
    cpu->excl_addr = ~0ull;
    /* Architectural reset value has the MMU and caches off; RES1 bits set. */
    cpu->sctlr_el1 = 0x30d00800;
    hv_tlb_flush(cpu);
}
