#include "machine.h"

#include <mach/mach_time.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mmu.h"
#include "sysreg.h"

extern void hv_exit(void);
extern void hv_helper_entry(void);

static int init_rest(HvMachine *m);

int hv_machine_init(HvMachine *m, size_t jit_bytes) {
    memset(m, 0, sizeof *m);
    if (hv_jit_region_open(&m->jit, jit_bytes) != 0) return -1;
    return init_rest(m);
}

int hv_machine_init_jit(HvMachine *m, void *rx, void *rw, size_t jit_bytes) {
    memset(m, 0, sizeof *m);
    if (hv_jit_region_attach(&m->jit, rx, rw, jit_bytes) != 0) return -1;
    return init_rest(m);
}

static int init_rest(HvMachine *m) {
    m->blocks = calloc(HV_MAX_BLOCKS, sizeof(HvBlock));
    if (!m->blocks) return -1;
    m->stop_pc = ~0ull;
    m->chain = 1;
    m->slots = calloc(HV_MAX_SLOTS, sizeof *m->slots);
    m->slot_site = calloc(HV_MAX_SLOTS, sizeof *m->slot_site);
    if (!m->slots || !m->slot_site) return -1;
    pthread_mutex_init(&m->big_mu, NULL);
    pthread_cond_init(&m->big_cv, NULL);
    pthread_mutexattr_t ra;
    pthread_mutexattr_init(&ra);
    pthread_mutexattr_settype(&ra, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->dev_mu, &ra);
    pthread_mutex_init(&m->tr_mu, NULL);
    pthread_mutex_init(&m->stw_mu, NULL);
    pthread_cond_init(&m->stw_cv, NULL);
    for (int i = 0; i < HV_MAX_CPUS; i++) { /* untouched pages cost nothing, so every CPU gets its tables now */
        HvPerCpu *p = &m->percpu[i];
        p->ijc = calloc(HV_IJC_TABLES * HV_IJC_TABLE_BYTES / 8, 8);
        p->ucode = calloc(1u << HV_UCODE_BITS, 8);
        if (!p->ijc || !p->ucode) return -1;
        pthread_mutex_init(&p->wake_mu, NULL);
        pthread_cond_init(&p->wake_cv, NULL);
        pthread_mutex_init(&p->req_mu, NULL);
    }
    return 0;
}

__thread HvCpu *hv_self;

void hv_dev_lock(HvMachine *m) {
    if (pthread_mutex_trylock(&m->dev_mu) == 0) return;
    uint64_t t0 = m->stats_interval ? mach_absolute_time() : 0;
    pthread_mutex_lock(&m->dev_mu);
    if (m->stats_interval) {
        m->stats.dev_waits++;
        m->stats.dev_wait_ticks += mach_absolute_time() - t0;
    }
}

void hv_dev_unlock(HvMachine *m) {
    pthread_mutex_unlock(&m->dev_mu);
}

static uint64_t post_locked_done(HvCpu *target, HvPerCpu *p) {
    uint64_t seq = __atomic_add_fetch(&p->req_seq, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&p->req_mu);
    __atomic_store_n(&target->exit_req, 1, __ATOMIC_SEQ_CST);
    return seq;
}

uint64_t hv_cpu_post_tlbi(HvCpu *target, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint64_t xt) {
    HvPerCpu *p = target->pc_state;
    pthread_mutex_lock(&p->req_mu);
    if (p->nreq < sizeof p->req / sizeof p->req[0]) {
        p->req[p->nreq].op1 = op1, p->req[p->nreq].crn = crn, p->req[p->nreq].crm = crm, p->req[p->nreq].op2 = op2;
        p->req[p->nreq].xt = xt;
        p->nreq++;
    } else {
        p->req_flags |= HV_REQ_TLB_ALL; /* too many queued: drop everything instead */
    }
    return post_locked_done(target, p);
}

uint64_t hv_cpu_post(HvCpu *target, uint32_t flags) {
    HvPerCpu *p = target->pc_state;
    pthread_mutex_lock(&p->req_mu);
    p->req_flags |= flags;
    return post_locked_done(target, p);
}

void hv_cpu_process_requests(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    uint64_t seq = __atomic_load_n(&p->req_seq, __ATOMIC_ACQUIRE);
    if (seq == __atomic_load_n(&p->ack_seq, __ATOMIC_RELAXED)) return;
    static __thread struct { uint32_t op1, crn, crm, op2; uint64_t xt; } q[64];
    pthread_mutex_lock(&p->req_mu);
    seq = p->req_seq;
    uint32_t flags = p->req_flags, n = p->nreq;
    memcpy(q, p->req, n * sizeof q[0]);
    p->req_flags = 0;
    p->nreq = 0;
    pthread_mutex_unlock(&p->req_mu);
    if (flags & HV_REQ_TLB_ALL) {
        hv_tlb_flush(cpu);
    } else {
        for (uint32_t i = 0; i < n; i++) hv_tlbi_apply(cpu, q[i].op1, q[i].crn, q[i].crm, q[i].op2, q[i].xt);
    }
    if (flags & HV_REQ_IJC_RESET) {
        hv_ijc_reset(cpu, 0);
        hv_ijc_reset(cpu, 1);
    }
    __atomic_store_n(&p->ack_seq, seq, __ATOMIC_RELEASE);
}

void hv_cpu_sync(HvCpu *self, HvCpu *target, uint64_t seq) {
    HvPerCpu *t = target->pc_state, *s = self ? self->pc_state : NULL;
    HvMachine *mm = target->m;
    uint64_t t0 = mm->stats_interval ? mach_absolute_time() : 0;
    if (s) __atomic_store_n(&s->tlb_live, 0, __ATOMIC_SEQ_CST); /* others need not wait for this one meanwhile */
    for (uint32_t spins = 0;; spins++) {
        if (__atomic_load_n(&t->ack_seq, __ATOMIC_ACQUIRE) >= seq) break;
        if (!__atomic_load_n(&t->tlb_live, __ATOMIC_SEQ_CST)) break; /* parked: it does the work before it runs again */
        if (self) hv_cpu_process_requests(self);
        if (spins > 200) sched_yield();
    }
    if (s) {
        __atomic_store_n(&s->tlb_live, 1, __ATOMIC_SEQ_CST);
        hv_cpu_process_requests(self); /* anything that came in while we were not live */
    }
    if (mm->stats_interval) {
        mm->stats.syncs++;
        mm->stats.sync_ticks += mach_absolute_time() - t0;
    }
}

void hv_big_lock(HvMachine *m) {
    if (!m->big_lock_on) return;
    pthread_mutex_lock(&m->big_mu);
    uint64_t me = m->big_next++;
    while (m->big_serving != me) pthread_cond_wait(&m->big_cv, &m->big_mu);
    pthread_mutex_unlock(&m->big_mu);
}

void hv_big_unlock(HvMachine *m) {
    if (!m->big_lock_on) return;
    pthread_mutex_lock(&m->big_mu);
    m->big_serving++;
    pthread_cond_broadcast(&m->big_cv);
    pthread_mutex_unlock(&m->big_mu);
}

void hv_big_yield(HvMachine *m) {
    if (!m->big_lock_on) return;
    pthread_mutex_lock(&m->big_mu);
    int waiting = m->big_next - m->big_serving > 1;
    pthread_mutex_unlock(&m->big_mu);
    if (!waiting) return;
    hv_big_unlock(m);
    hv_big_lock(m);
}

void hv_cpu_wake(HvCpu *cpu) {
    __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_RELAXED);
    HvPerCpu *p = cpu->pc_state;
    pthread_mutex_lock(&p->wake_mu);
    p->wake_pending = 1;
    pthread_cond_signal(&p->wake_cv);
    pthread_mutex_unlock(&p->wake_mu);
}

void hv_machine_kick_all(HvMachine *m) {
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) __atomic_store_n(&m->cpus[i]->exit_req, 1, __ATOMIC_RELAXED);
}

/* Device work is for CPU 0, which polls the devices. */
void hv_machine_wake(HvMachine *m) {
    if (m->cpu) hv_cpu_wake(m->cpu);
}

void hv_machine_free(HvMachine *m) {
    hv_jit_region_close(&m->jit);
    for (int i = 0; i < m->nram; i++) {
        free(m->ram[i].code_pages);
        free(m->ram[i].page_gen);
    }
    free(m->blocks);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        free(m->percpu[i].ijc);
        free(m->percpu[i].ucode);
    }
    free(m->slots);
    free(m->slot_site);
}

int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int readonly) {
    if (m->nram == HV_MAX_RAM) return -1;
    HvRam *r = &m->ram[m->nram++];
    r->pa = pa;
    r->size = size;
    r->host = host;
    r->readonly = readonly;
    size_t pages = (size + (1u << HV_TLB_PAGE_BITS) - 1) >> HV_TLB_PAGE_BITS;
    r->code_pages = calloc(pages, 1);
    r->page_gen = calloc(pages, sizeof *r->page_gen);
    r->whint = calloc(pages * HV_MAX_CPUS, sizeof *r->whint);
    return r->code_pages && r->page_gen && r->whint ? 0 : -1;
}

int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    if (m->nmmio == HV_MAX_MMIO) return -1;
    m->mmio[m->nmmio++] = (HvMmio){pa, size, opaque, read, write, name, 0};
    return 0;
}

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa) {
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (pa - r->pa < r->size) return r;
    }
    return NULL;
}

void hv_invalidate_code(HvMachine *m, uint64_t pa) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (!r) return;
    uint64_t pg = (pa - r->pa) >> HV_TLB_PAGE_BITS;
    if (!r->code_pages[pg]) return;
    /* Blocks remember the generation they were made under; a stale one is
       retranslated the next time it is looked up. The block running now may
       finish with the old code, as it would with a real instruction pipeline. */
    r->code_pages[pg] = 0;
    __atomic_add_fetch(&r->page_gen[pg], 1, __ATOMIC_SEQ_CST);
    m->stats.flush_smc++;
    /* The indirect-branch cache may lead into the stale blocks: start it over,
       here at once and on the other CPUs before they next leave a block (their
       cache lookups and chained exits check exit_req first). */
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (!c) continue;
        if (!hv_self || c == hv_self) {
            hv_ijc_reset(c, 0);
            hv_ijc_reset(c, 1);
        } else {
            hv_cpu_post(c, HV_REQ_IJC_RESET);
        }
    }
    m->stats.reset_why[4]++;
    /* Make a running chain leave (chains stay patched: see chain() in dispatch.c). */
    hv_machine_kick_all(m);
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
    if (!d->self_locked) hv_dev_lock(m);
    uint64_t v = d->read ? d->read(d->opaque, pa - d->pa, len) : 0;
    if (!d->self_locked) hv_dev_unlock(m);
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
        for (uint64_t pg = first; pg <= last; pg++)
            if (r->code_pages[pg]) hv_invalidate_code(m, r->pa + (pg << HV_TLB_PAGE_BITS)); /* a write to code */
        if (m->watch_pa && m->watch_pa - pa < len) {
            uint64_t v = 0;
            memcpy(&v, (const uint8_t *)buf + (m->watch_pa - pa), len - (m->watch_pa - pa) < 8 ? len - (m->watch_pa - pa) : 8);
            fprintf(stderr, "hengevm: watch: write to pa %llx = %llx (%u bytes at pa %llx) by pc %llx\n",
                    (unsigned long long)m->watch_pa, (unsigned long long)v, len, (unsigned long long)pa,
                    (unsigned long long)m->access_pc);
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
    if (!d->self_locked) hv_dev_lock(m);
    if (d->write) d->write(d->opaque, pa - d->pa, v, len);
    if (!d->self_locked) hv_dev_unlock(m);
    return 0;
}

void hv_cpu_reset(HvCpu *cpu, HvMachine *m) {
    hv_cpu_reset_index(cpu, m, 0);
}

void hv_cpu_reset_index(HvCpu *cpu, HvMachine *m, uint32_t index) {
    memset(cpu, 0, sizeof *cpu);
    cpu->m = m;
    cpu->index = index;
    cpu->pc_state = &m->percpu[index];
    m->cpus[index] = cpu;
    if (m->ncpus <= (int)index) m->ncpus = (int)index + 1;
    if (index == 0) m->cpu = cpu;
    HvPerCpu *p = cpu->pc_state;
    /* Ids start over, so entries tagged with the old ones must go. */
    for (int t = 0; t < HV_IJC_TABLES; t++)
        if (p->ijc_tables_used >> t & 1) memset(p->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8), 0, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used = 0;
    memset(p->ijc_keyed, 0, sizeof p->ijc_keyed);
    p->ijc_keyed_count = 0;
    memset(p->ijc_pages, 0, sizeof p->ijc_pages);
    memset(p->jc, 0, sizeof p->jc);
    cpu->ijc = (uint64_t)(uintptr_t)p->ijc;
    cpu->lines_ptr = (uint64_t)(uintptr_t)&p->irq_line;
    for (int t = 0; t < HV_IJC_TABLES; t++) cpu->ijc_tab[t] = (uint64_t)(uintptr_t)(p->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8));
    cpu->next_cid = 1;
    cpu->kernel_gen = 1;
    cpu->ijc_salt[1] = 1ull << 48; /* tags of an all-zero table never match */
    hv_ijc_switch(cpu);
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
