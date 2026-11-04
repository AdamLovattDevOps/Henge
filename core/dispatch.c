#include "dispatch.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#include "exception.h"
#include "memop.h"
#include "mmu.h"
#include "sysreg.h"
#include "translate.h"

void hv_flush_blocks(HvMachine *m) {
    m->stats.block_flushes++;
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) {
            hv_ijc_reset(m->cpus[i], 0);
            hv_ijc_reset(m->cpus[i], 1);
            memset(m->percpu[i].jc, 0, sizeof m->percpu[i].jc);
        }
    memset(m->hash, 0, sizeof m->hash);
    m->nblocks = 0;
    m->nslots = 0;
    hv_jit_reset(&m->jit);
}

/* Parked: not running guest code, nor using its TLB, so other CPUs need not
   wait for it (TLBIs, a flush). It catches up with requests on the way back. */
static void park(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    __atomic_store_n(&p->tlb_live, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->active, 0, __ATOMIC_SEQ_CST);
}

static void unpark(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    __atomic_store_n(&p->active, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->tlb_live, 1, __ATOMIC_SEQ_CST);
    hv_cpu_process_requests(cpu); /* what came in while it was parked */
}

static uint32_t block_flags(HvCpu *cpu) {
    uint32_t flags = cpu->el ? HV_BLOCK_EL1 : 0;
    if (cpu->el && cpu->spsel) flags |= HV_BLOCK_SPX;
    if (cpu->el && (cpu->pstate_extra & HV_PSTATE_PAN)) flags |= HV_BLOCK_PAN;
    uint32_t fpen = (cpu->cpacr_el1 >> 20) & 3;
    if (fpen == 0 || fpen == 2 || (fpen == 1 && cpu->el == 0)) flags |= HV_BLOCK_FPOFF;
    return flags;
}

static uint32_t hash_of(uint64_t va, uint64_t pa, uint32_t flags) {
    uint64_t h = (va >> 2) ^ (pa >> 2) * 0x9e3779b97f4a7c15ull ^ flags;
    return (uint32_t)(h ^ (h >> 29)) & (HV_BLOCK_HASH - 1);
}

/* Finding a block is lock-free (blocks are only ever added, with a release
   store, until a flush, which stops every CPU); making one takes the
   translation lock. Returns NULL if the translations must all be dropped first
   (m->flush_pending is set: the dispatcher does that, then tries again). */
static HvBlock *find(HvMachine *m, uint32_t h, uint64_t va, uint64_t pa, uint32_t flags, uint32_t gen) {
    for (HvBlock *b = __atomic_load_n(&m->hash[h], __ATOMIC_ACQUIRE); b; b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE))
        if (b->va == va && b->pa == pa && b->flags == flags && b->gen == gen) return b;
    return NULL;
}

static HvBlock *lookup(HvCpu *cpu, uint64_t va, uint64_t pa, uint32_t flags, const uint32_t *code, uint32_t avail) {
    HvMachine *m = cpu->m;
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    HvBlock **jc = cpu->pc_state->jc;
    HvBlock *c = jc[j];
    if (c && c->va == va && c->pa == pa && c->flags == flags && c->ram->page_gen[c->pg] == c->gen) return c;
    uint32_t h = hash_of(va, pa, flags);
    HvRam *ram = hv_ram_lookup(m, pa);
    uint64_t pg = (pa - ram->pa) >> HV_TLB_PAGE_BITS;
    uint32_t gen = __atomic_load_n(&ram->page_gen[pg], __ATOMIC_ACQUIRE);
    HvBlock *b = find(m, h, va, pa, flags, gen);
    if (b) return jc[j] = b;

    if (m->flush_pending) goto flush_unlocked;
    /* Translate without the lock, into this thread's buffer... */
    HvBlock nb = {.va = va, .pa = pa, .flags = flags, .gen = gen, .ram = ram, .pg = pg};
    uint64_t t0 = m->stats_interval ? mach_absolute_time() : 0;
    if (hv_translate_emit(m, &nb, code, avail) != 0) goto flush_unlocked;
    if (m->stats_interval) {
        m->stats.translate_ticks += mach_absolute_time() - t0;
        m->stats.translated_insns += nb.ninsns;
    }
    /* ...then take it only to place and publish the block. */
    if (pthread_mutex_trylock(&m->tr_mu) != 0) {
        uint64_t w0 = m->stats_interval ? mach_absolute_time() : 0;
        pthread_mutex_lock(&m->tr_mu);
        if (m->stats_interval) {
            m->stats.tr_waits++;
            m->stats.tr_wait_ticks += mach_absolute_time() - w0;
        }
    }
    b = find(m, h, va, pa, flags, gen); /* another CPU may have just made it: ours is dropped (its space stays used until a flush) */
    if (b) {
        pthread_mutex_unlock(&m->tr_mu);
        return jc[j] = b;
    }
    if ((uint32_t)m->nblocks >= m->max_blocks || m->flush_pending) {
        m->stats.flush_full++;
        goto flush;
    }
    b = &m->blocks[m->nblocks];
    size_t used0 = m->jit.used;
    if (hv_translate_place(m, &nb) != 0) { /* JIT region full: start over */
        m->stats.flush_full++;
        goto flush;
    }
    *b = nb;
    m->stats.translations++;
    if (m->stats_interval) {
        m->stats.translated_words += m->jit.used - used0;
        static uint64_t seen_va[1 << 18], seen_vapa[1 << 18]; /* was this code translated before? (hashed, so approximate) */
        uint32_t hv = (uint32_t)(((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull >> 40);
        uint32_t hp = (uint32_t)((((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull ^ pa * 0xc2b2ae3d27d4eb4full) >> 40);
        if (seen_va[hv >> 6] >> (hv & 63) & 1) m->stats.retrans_va++;
        if (seen_vapa[hp >> 6] >> (hp & 63) & 1) m->stats.retrans_vapa++;
        seen_va[hv >> 6] |= 1ull << (hv & 63);
        seen_vapa[hp >> 6] |= 1ull << (hp & 63);
    }
    m->nblocks++;
    b->next = m->hash[h];
    __atomic_store_n(&m->hash[h], b, __ATOMIC_RELEASE);
    /* Writes to this page must now take the slow path so we notice them. */
    if (!ram->code_pages[pg]) {
        __atomic_store_n(&ram->code_pages[pg], 1, __ATOMIC_SEQ_CST); /* before the TLBs: see the end of tlb_fill */
        m->stats.code_protects++;
        for (int i = 0; i < m->ncpus; i++) /* in every CPU's TLB, or a write from another CPU goes unnoticed */
            if (m->cpus[i]) hv_tlb_protect_code_page(m->cpus[i], ram, pg);
    }
    pthread_mutex_unlock(&m->tr_mu);
    return b;
flush_unlocked:
    pthread_mutex_lock(&m->tr_mu);
flush:
    __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&m->tr_mu);
    hv_machine_kick_all(m);
    return NULL;
}

/* Blocks call this through hv_helper_entry. Returns 1 to leave the block. */
static uint64_t reg_hist[65536]; /* --stats: MRS/MSR/SYS by op0:op1:CRn:CRm:op2; reads low, writes high 32 bits */

uint32_t hv_helper(HvCpu *cpu, const HvHelperInfo *info) {
    if (info->op < 12) cpu->m->stats.helper[info->op]++;
    if (cpu->m->stats_interval && (info->op == HV_OP_MRS || info->op == HV_OP_MSR || info->op == HV_OP_SYS))
        reg_hist[(info->insn >> 5) & 0xffff] += info->op == HV_OP_MRS ? 1 : 1ull << 32;
    switch (info->op) {
    case HV_OP_MEM:
        cpu->m->stat_slow_mem++;
        if (cpu->m->stats_interval) { /* --stats: which kinds of access take the slow path */
            HvMemOp op;
            if (hv_mem_decode(info->insn, &op) == 0 && op.kind < 12) cpu->m->stats.slow_kind[op.kind]++;
        }
        return hv_mem_emulate(cpu, info->insn, info->pc);
    case HV_OP_MRS:
        return hv_sysreg_mrs(cpu, info->insn, info->pc);
    case HV_OP_MSR:
        return hv_sysreg_msr(cpu, info->insn, info->pc);
    case HV_OP_SYS:
        return hv_sysreg_sys(cpu, info->insn, info->pc);
    case HV_OP_EXCEPTION:
        return hv_exception_insn(cpu, info->insn, info->pc);
    case HV_OP_ERET:
        hv_eret(cpu);
        return 1;
    case HV_OP_FPTRAP:
        hv_raise_sync(cpu, HV_EC_FP_ACCESS, 1u << 24 | 0xeu << 20, 0, info->pc);
        return 1;
    case HV_OP_CLREX:
        cpu->excl_addr = ~0ull;
        return 0;
    case HV_OP_WFI:
        cpu->pc = info->pc + 4;
        cpu->exit_reason = HV_EXIT_WFI;
        return 1;
    case HV_OP_UNDEF:
    default:
        hv_raise_undef(cpu, info->pc);
        return 1;
    }
}

/* Debugging: EL0 is running a block translated from kernel code. Which block, and how did we get here? */
void hv_debug_el0_in_kernel(HvCpu *cpu, const char *what) {
    {
        {
            HvMachine *m = cpu->m;
            uint32_t *host = (uint32_t *)(uintptr_t)cpu->helper_ret; /* the block's blr into hv_helper_entry */
            int lo = 0, hi = m->nblocks - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (m->blocks[mid].code <= host) lo = mid;
                else hi = mid - 1;
            }
            HvBlock *b = m->nblocks ? &m->blocks[lo] : NULL;
            fprintf(stderr, "hengevm: el0 runs kernel code: block %llx flags %u (index %d of %d), flushes %llu, last stub %llx, "
                            "miss site %llx\n  recent:", b ? (unsigned long long)b->va : 0ull, b ? b->flags : 0, lo, m->nblocks,
                    (unsigned long long)m->stats.block_flushes, (unsigned long long)cpu->last_stub,
                    (unsigned long long)cpu->miss_site);
            for (int k = 15; k >= 0; k--) fprintf(stderr, " %llx", (unsigned long long)m->stats.recent[(m->stats.nrecent - 1 - k) & 15]);
            fprintf(stderr, "\n  (%s) el changes, oldest first:\n", what);
            for (int k = 15; k >= 0; k--) {
                uint32_t i = (m->stats.nelchg - 1 - k) & 15;
                if (m->stats.elchg[i].kind == 9)
                    fprintf(stderr, "    eret at %llx from el%u to %llx spsr %llx\n", (unsigned long long)m->stats.elchg[i].from,
                            m->stats.elchg[i].el, (unsigned long long)m->stats.elchg[i].to, (unsigned long long)m->stats.elchg[i].spsr);
                else
                    fprintf(stderr, "    exception type %u esr %llx from el%u pc %llx (spsr %llx)\n", m->stats.elchg[i].kind,
                            (unsigned long long)m->stats.elchg[i].to, m->stats.elchg[i].el,
                            (unsigned long long)m->stats.elchg[i].from, (unsigned long long)m->stats.elchg[i].spsr);
            }
        }
    }
}

/* WFI: sleep until an interrupt is pending (masked or not) or the next timer
   event, at most 10 ms. WFI may complete early, so the guest just runs again
   if nothing happened. */
static int wait_for_interrupt(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    uint64_t deadline = m->tick ? m->tick(m, cpu) : 0;
    HvPerCpu *p = cpu->pc_state;
    if (p->irq_line || p->fiq_line) return 0;
    if (!m->tick) return -1; /* nothing could ever wake us */
    uint64_t now = mach_absolute_time();
    uint64_t wait_ticks = deadline && deadline > now ? deadline - now : 240000; /* the counter runs at 24 MHz */
    if (wait_ticks > 240000) wait_ticks = 240000;
    uint64_t t0 = mach_absolute_time();
    /* Sleep until the next timer event or until a device thread has work for us. */
    hv_big_unlock(m); /* let the other CPUs run while this one sleeps */
    park(cpu);
    pthread_mutex_lock(&p->wake_mu);
    if (!p->wake_pending) {
        struct timespec ts = {(time_t)(wait_ticks / 24000000), (long)(wait_ticks % 24000000) * 1000 / 24};
        pthread_cond_timedwait_relative_np(&p->wake_cv, &p->wake_mu, &ts);
    }
    p->wake_pending = 0;
    pthread_mutex_unlock(&p->wake_mu);
    unpark(cpu);
    hv_big_lock(m);
    m->stats.wfi++;
    m->stats.wfi_ticks += mach_absolute_time() - t0;
    m->tick(m, cpu);
    return 0;
}

/* --profile: which guest blocks the sampled host PCs were in. Blocks are laid
   out in the JIT region in the order they were made, so a binary search over
   m->blocks finds the one holding a PC. */
static void print_profile(HvMachine *m) {
    enum { SLOTS = 4096 };
    static struct { uint64_t va; uint32_t flags, n, ninsns; } top[SLOTS];
    memset(top, 0, sizeof top);
    uint32_t total = 0, outside = 0;
    static struct { const char *name; uint32_t n; } fn[64];
    static struct { uint32_t off, n; } run_off[256];
    int nfn = 0, nrun = 0;
    uint32_t head = __atomic_load_n(&m->prof_head, __ATOMIC_ACQUIRE);
    if (head - m->prof_tail > HV_PROF_RING) m->prof_tail = head - HV_PROF_RING;
    for (; m->prof_tail != head; m->prof_tail++) {
        uint32_t *pc = (uint32_t *)(uintptr_t)m->prof[m->prof_tail & (HV_PROF_RING - 1)];
        total++;
        if (pc < m->jit.base || pc >= m->jit.base + m->jit.used || !m->nblocks) {
            outside++;
            Dl_info di;
            const char *name = dladdr(pc, &di) && di.dli_sname ? di.dli_sname : "?";
            static const char *detail;
            if (!detail) detail = getenv("HV_PROF_FN") ? getenv("HV_PROF_FN") : "run";
            if (!strcmp(name, detail)) { /* per-offset detail for one function (HV_PROF_FN, default the dispatcher loop) */
                uint32_t off = (uint32_t)((uint8_t *)pc - (uint8_t *)di.dli_saddr);
                int r = 0;
                while (r < nrun && run_off[r].off != off) r++;
                if (r == nrun && nrun < 256) { run_off[nrun].off = off; run_off[nrun++].n = 0; }
                if (r < nrun) run_off[r].n++;
            }
            int k = 0;
            while (k < nfn && strcmp(fn[k].name, name)) k++;
            if (k == nfn && nfn < 64) { fn[nfn].name = name; fn[nfn++].n = 0; }
            if (k < nfn) fn[k].n++;
            continue;
        }
        int lo = 0, hi = m->nblocks - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (m->blocks[mid].code <= pc) lo = mid;
            else hi = mid - 1;
        }
        HvBlock *b = &m->blocks[lo];
        uint32_t h = (uint32_t)((b->va >> 2) ^ b->flags) & (SLOTS - 1);
        int probes = 0;
        while (top[h].n && (top[h].va != b->va || top[h].flags != b->flags) && ++probes < 64) h = (h + 1) & (SLOTS - 1);
        if (probes == 64) continue; /* table full: the block is too cold to make the list anyway */
        top[h].va = b->va;
        top[h].flags = b->flags;
        top[h].ninsns = b->ninsns;
        top[h].n++;
    }
    if (!total) return;
    fprintf(stderr, "  profile: %u samples, %.0f%% outside translated code:", total, 100.0 * outside / total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < nfn; i++)
            if (fn[i].n && (best < 0 || fn[i].n > fn[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, " %s %.1f%%", fn[best].name, 100.0 * fn[best].n / total);
        fn[best].n = 0;
    }
    fprintf(stderr, "\n  %s+:", getenv("HV_PROF_FN") ? getenv("HV_PROF_FN") : "run");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < nrun; i++)
            if (run_off[i].n && (best < 0 || run_off[i].n > run_off[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, " %#x:%.1f%%", run_off[best].off, 100.0 * run_off[best].n / total);
        run_off[best].n = 0;
    }
    fprintf(stderr, "\n");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < SLOTS; i++)
            if (top[i].n && (best < 0 || top[i].n > top[best].n)) best = i;
        if (best < 0 || top[best].n * 200 < total) break;
        fprintf(stderr, "    %5.1f%%  %llx el%u (%u insns)\n", 100.0 * top[best].n / total,
                (unsigned long long)top[best].va, top[best].flags & HV_BLOCK_EL1, top[best].ninsns);
        top[best].n = 0;
    }
}

static void print_stats(HvCpu *cpu, int force) {
    static uint64_t last_t, start_t;
    /* This uses floating point, and the host FP registers hold the guest's state. */
    hv_fp_leave(cpu);
    uint64_t zero = 0;
    __asm__ volatile("msr fpcr, %0" ::"r"(zero));
    static HvStats last;
    HvMachine *m = cpu->m;
    uint64_t now = mach_absolute_time();
    if (!start_t) start_t = last_t = now;
    if (!force && now - last_t < (uint64_t)m->stats_interval * 24000000ull) {
        hv_fp_enter(cpu);
        return;
    }
    double dt = (double)(now - last_t) / 24e6;
    HvStats *s = &m->stats;
#define RATE(f) ((double)(s->f - last.f) / dt)
    fprintf(stderr,
            "stats %5.0fs: blocks %.2fM/s, translated %.0f/s (again: va %.0f%% va+pa %.0f%%; %.0f%% of time, %.1f insns, %.1fx), slow mem %.0fK/s, mrs %.0fK/s msr %.0fK/s sys %.0fK/s, "
            "exc %.0f/s irq %.0f/s, tlb flush %.0f/s, fetch walks %.0fK/s, wfi %.0f/s (%.0f%% idle), flushes %.0f/s (smc %.0f ic %.0f full %.0f), blocks held %d (%.0f MB, %llu full flushes), chained %.0f/s ijc fills %.0fK/s resets %.0f/s, el%u pc %llx\n",
            (double)(now - start_t) / 24e6, RATE(blocks) / 1e6, RATE(translations), 100.0 * RATE(retrans_va) / (RATE(translations) + 1e-9), 100.0 * RATE(retrans_vapa) / (RATE(translations) + 1e-9), 100.0 * (double)(s->translate_ticks - last.translate_ticks) / (double)(now - last_t), RATE(translated_insns) / (RATE(translations) + 1e-9), RATE(translated_words) / (RATE(translated_insns) + 1e-9), RATE(helper[HV_OP_MEM]) / 1e3,
            RATE(helper[HV_OP_MRS]) / 1e3, RATE(helper[HV_OP_MSR]) / 1e3, RATE(helper[HV_OP_SYS]) / 1e3,
            RATE(exceptions), RATE(interrupts), RATE(tlb_flushes), RATE(fetch_walks) / 1e3, RATE(wfi),
            100.0 * (double)(s->wfi_ticks - last.wfi_ticks) / (double)(now - last_t), RATE(block_flushes), RATE(flush_smc),
            RATE(flush_ic), RATE(flush_full), m->nblocks, (double)m->jit.used * 4 / (1 << 20), (unsigned long long)s->flush_full, RATE(chained), RATE(ijc_fills) / 1e3, RATE(ijc_resets), cpu->el,
            (unsigned long long)cpu->pc);
    {
        double tot = 0;
        for (int k = 0; k < 6; k++) tot += RATE(why[k]);
        /* How much of the interval the CPU thread actually ran: below 100% means the host took it away. */
        static double last_busy;
        thread_basic_info_data_t ti;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        mach_port_t self = mach_thread_self();
        if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&ti, &cnt) == KERN_SUCCESS) {
            double busy = ti.user_time.seconds + ti.user_time.microseconds / 1e6 + ti.system_time.seconds +
                          ti.system_time.microseconds / 1e6;
            fprintf(stderr, "  cpu thread ran %.0f%% of the interval\n", 100 * (busy - last_busy) / dt);
            if (m->ncpus > 1) {
                fprintf(stderr, "  cpus:");
                for (int i = 0; i < m->ncpus; i++)
                    if (m->cpus[i])
                        fprintf(stderr, " [%d] %s el%u pc %llx", i, m->cpus[i]->off ? "off" : "on", m->cpus[i]->el,
                                (unsigned long long)m->cpus[i]->pc);
                fprintf(stderr, "\n");
            }
            last_busy = busy;
        }
        mach_port_deallocate(mach_task_self(), self);
        fprintf(stderr, "  cache drops/s: full %.0f, ASID %.0f, user VA %.0f, kernel VA %.0f, code %.0f (whole user half %.0f, filter full %.0f, big user range %.0f)\n", RATE(reset_why[0]),
                RATE(reset_why[1]), RATE(reset_why[2]), RATE(reset_why[3]), RATE(reset_why[4]), RATE(reset_why[5]), RATE(reset_why[6]), RATE(reset_why[7]));
        { /* block hash health: chain lengths and how much of it is stale */
            uint64_t used = 0, stale = 0, maxlen = 0, sumsq = 0;
            uint32_t worst = 0;
            for (uint32_t h = 0; h < HV_BLOCK_HASH; h++) {
                uint64_t len = 0;
                for (HvBlock *b = m->hash[h]; b; b = b->next) {
                    len++;
                    if (b->ram->page_gen[b->pg] != b->gen) stale++;
                }
                if (len) used++;
                sumsq += len * len;
                if (len > maxlen) maxlen = len, worst = h;
            }
            fprintf(stderr, "  block hash: %llu buckets used, %llu stale blocks, max chain %llu, mean walk %.1f\n",
                    (unsigned long long)used, (unsigned long long)stale, (unsigned long long)maxlen,
                    m->nblocks ? (double)sumsq / m->nblocks : 0.0);
            int k = 0;
            for (HvBlock *b = m->hash[worst]; b && k < 6; b = b->next, k++)
                fprintf(stderr, "    worst: va %llx pa %llx flags %x gen %u/%u\n", (unsigned long long)b->va,
                        (unsigned long long)b->pa, b->flags, b->gen, b->ram->page_gen[b->pg]);
        }
        {
            double tw = RATE(words_inline) + RATE(words_ool) + RATE(words_pool) + 1e-9;
            fprintf(stderr, "  code words: inline %.0f%% out of line %.0f%% literals %.0f%%\n", 100 * RATE(words_inline) / tw,
                    100 * RATE(words_ool) / tw, 100 * RATE(words_pool) / tw);
        }
        fprintf(stderr, "  new code pages %.0f/s; waits: tlbi acks %.0f/s %.1f%%, device lock %.0f/s %.1f%%, translation lock %.0f/s %.1f%% (of one CPU)\n",
                RATE(code_protects), RATE(syncs), RATE(sync_ticks) / 240000.0, RATE(dev_waits), RATE(dev_wait_ticks) / 240000.0,
                RATE(tr_waits), RATE(tr_wait_ticks) / 240000.0);
        fprintf(stderr, "  tlb misses K/s: el0 empty %.0f el0 conflict %.0f el1 empty %.0f el1 conflict %.0f\n",
                RATE(tlb_miss_kind[0]) / 1e3, RATE(tlb_miss_kind[1]) / 1e3, RATE(tlb_miss_kind[2]) / 1e3, RATE(tlb_miss_kind[3]) / 1e3);
        fprintf(stderr, "  slow mem causes K/s: mmio %.0f code-page write %.0f page end %.0f tlb miss %.0f other %.0f entry-but-tag %.0f\n",
                RATE(slow_why[0]) / 1e3, RATE(slow_why[1]) / 1e3, RATE(slow_why[2]) / 1e3, RATE(slow_why[3]) / 1e3,
                RATE(slow_why[4]) / 1e3, RATE(slow_why[5]) / 1e3);
        fprintf(stderr, "  slow mem K/s: single %.0f pair %.0f literal %.0f ldra %.0f ordered %.0f excl %.0f cas %.0f casp %.0f atomic %.0f vec %.0f/%.0f\n",
                RATE(slow_kind[0]) / 1e3, RATE(slow_kind[1]) / 1e3, RATE(slow_kind[2]) / 1e3, RATE(slow_kind[3]) / 1e3,
                RATE(slow_kind[4]) / 1e3, RATE(slow_kind[5]) / 1e3, RATE(slow_kind[6]) / 1e3, RATE(slow_kind[7]) / 1e3,
                RATE(slow_kind[8]) / 1e3, RATE(slow_kind[9]) / 1e3, RATE(slow_kind[10]) / 1e3);
        fprintf(stderr, "  ijc fills into: empty %.0fK/s, live other salt %.0fK/s, dead salt %.0fK/s, same salt %.0fK/s\n",
                RATE(fill_kind[0]) / 1e3, RATE(fill_kind[1]) / 1e3, RATE(fill_kind[3]) / 1e3, RATE(fill_kind[2]) / 1e3);
        fprintf(stderr, "  misses at: RET %.0fK/s, BR/BLR %.0fK/s, B/BL %.0fK/s, conditional %.0fK/s, MSR %.0fK/s, other %.0fK/s, unknown %.0fK/s\n",
                RATE(miss_kind[0]) / 1e3, RATE(miss_kind[1]) / 1e3, RATE(miss_kind[2]) / 1e3, RATE(miss_kind[3]) / 1e3,
                RATE(miss_kind[4]) / 1e3, RATE(miss_kind[5]) / 1e3, RATE(miss_kind[6]) / 1e3);
        fprintf(stderr, "  entries %.2fM/s: chain fill %.0f%%, kick %.0f%%, exception/sysreg %.0f%%, interrupt %.0f%%, cached target %.0f%%, cache miss %.0f%%\n",
                tot / 1e6, 100 * RATE(why[0]) / tot, 100 * RATE(why[1]) / tot, 100 * RATE(why[2]) / tot,
                100 * RATE(why[3]) / tot, 100 * RATE(why[4]) / tot, 100 * RATE(why[5]) / tot);
    }
#undef RATE
    if (m->prof) print_profile(m);
    /* The five busiest system registers or SYS operations (reads + writes) this interval. */
    for (int k = 0; k < 14; k++) {
        uint64_t best = 0;
        uint32_t bi = 0;
        for (uint32_t r = 0; r < 65536; r++) {
            uint64_t n = (reg_hist[r] & 0xffffffff) + (reg_hist[r] >> 32);
            if (n > best) best = n, bi = r;
        }
        if (!best) break;
        fprintf(stderr, "   S%u_%u_C%u_C%u_%u: %.0f reads %.0f writes/sys per s\n", bi >> 14, (bi >> 11) & 7, (bi >> 7) & 15,
                (bi >> 3) & 15, bi & 7, (double)(reg_hist[bi] & 0xffffffff) / dt, (double)(reg_hist[bi] >> 32) / dt);
        reg_hist[bi] = 0;
    }
    memset(reg_hist, 0, sizeof reg_hist);
    last = *s;
    last_t = now;
    hv_fp_enter(cpu);
}

/* Fill the slot of the chainable exit a block just left through, so it jumps
   straight to b next time. Chains never need undoing: both blocks are in the
   same 16 KB page, so when that page's code is invalidated the stale blocks are
   only reachable from each other, and the exit_req check before every chained
   jump gets a block that just wrote its own page back to the dispatcher. */
static void chain(HvMachine *m, uint64_t *slot, HvBlock *b) {
    __atomic_store_n(slot, (uint64_t)(uintptr_t)b->code, __ATOMIC_RELEASE);
    /* And the stub's B, when the target is in its range: a direct branch, no load. */
    uint32_t *site = m->slot_site[slot - m->slots];
    int64_t off = site ? b->code - site : 0;
    if (site && off >= -(1 << 25) && off < (1 << 25)) {
        uint32_t insn = 0x14000000u | (uint32_t)(off & 0x3ffffff);
        hv_jit_write(&m->jit, site, &insn, 4);
        hv_jit_finish(&m->jit, site, 1);
    }
    m->stats.chained++;
}

static int run_loop(HvCpu *cpu, int one);

static int run(HvCpu *cpu, int one) {
    int r = run_loop(cpu, one);
    park(cpu);
    hv_self = NULL;
    return r;
}

/* A powered-off CPU waits here, without the big lock, for PSCI CPU_ON. */
static void wait_while_off(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    hv_big_unlock(m);
    park(cpu);
    pthread_mutex_lock(&p->wake_mu);
    __atomic_store_n(&cpu->parked_off, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&cpu->off, __ATOMIC_ACQUIRE) && !cpu->halted) {
        p->wake_pending = 0;
        pthread_cond_wait(&p->wake_cv, &p->wake_mu);
    }
    __atomic_store_n(&cpu->parked_off, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&p->wake_mu);
    unpark(cpu);
    hv_big_lock(m);
}

/* Dropping every translation: the CPU that finds a flush pending waits until
   all the others have parked (at this barrier, asleep or off), flushes, and
   lets them go. Nobody holds a lock while waiting here. */
static void stop_the_world(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    pthread_mutex_lock(&m->stw_mu);
    if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST) && !m->stw_active) {
        __atomic_store_n(&m->stw_active, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&m->flush_pending, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&m->stw_mu);
        hv_machine_kick_all(m);
        for (uint32_t spins = 0;; spins++) {
            int busy = 0;
            for (int i = 0; i < m->ncpus; i++)
                if (m->cpus[i] && m->cpus[i] != cpu && __atomic_load_n(&m->cpus[i]->pc_state->active, __ATOMIC_SEQ_CST)) busy = 1;
            if (!busy) break;
            hv_cpu_process_requests(cpu); /* one of them may be waiting on a TLBI of ours */
            if (spins > 100) sched_yield();
        }
        pthread_mutex_lock(&m->tr_mu);
        hv_flush_blocks(m);
        pthread_mutex_unlock(&m->tr_mu);
        pthread_mutex_lock(&m->stw_mu);
        __atomic_store_n(&m->stw_active, 0, __ATOMIC_SEQ_CST);
        pthread_cond_broadcast(&m->stw_cv);
        pthread_mutex_unlock(&m->stw_mu);
    } else if (m->stw_active) {
        park(cpu);
        while (m->stw_active) pthread_cond_wait(&m->stw_cv, &m->stw_mu);
        pthread_mutex_unlock(&m->stw_mu);
        unpark(cpu);
    } else {
        pthread_mutex_unlock(&m->stw_mu);
    }
}

int hv_run(HvCpu *cpu) {
    hv_big_lock(cpu->m);
    hv_fp_enter(cpu);
    int r = run(cpu, 0);
    hv_fp_leave(cpu);
    hv_big_unlock(cpu->m);
    return r;
}

int hv_run_one(HvCpu *cpu) {
    hv_big_lock(cpu->m);
    hv_fp_enter(cpu);
    int r = run(cpu, 1);
    hv_fp_leave(cpu);
    hv_big_unlock(cpu->m);
    return r;
}

/* Bring-up aid (log >= 4): report the first block entered with a host address
   in a guest register, which means the translator leaked one. */
static int host_pointer_check(HvCpu *cpu, uint64_t prev_pc) {
    HvMachine *m = cpu->m;
    for (int r = 0; r < 31; r++) {
        uint64_t v = cpu->x[r];
        const char *what = NULL;
        for (int i = 0; i < m->nram && !what; i++)
            if (v - (uint64_t)(uintptr_t)m->ram[i].host < m->ram[i].size && m->ram[i].pa != (uint64_t)(uintptr_t)m->ram[i].host)
                what = "host copy of guest RAM";
        if (v - (uint64_t)(uintptr_t)m->jit.base < m->jit.capacity * 4) what = "JIT region";
        if (v - (uint64_t)(uintptr_t)cpu < sizeof *cpu) what = "HvCpu";
        if (what) {
            fprintf(stderr, "host pointer: x%d = %llx (%s) entering %llx, previous block %llx\n", r,
                    (unsigned long long)v, what, (unsigned long long)cpu->pc, (unsigned long long)prev_pc);
            return 1;
        }
    }
    return 0;
}

static int run_loop(HvCpu *cpu, int one) {
    HvMachine *m = cpu->m;
    uint32_t since_tick = 0;
    uint64_t next_due = 0; /* host time of this CPU's next timer event, from its last tick (0: none armed) */
    uint64_t last_fault = ~0ull, prev_pc = 0;
    HvBlock *prev_b = NULL;
    uint64_t *pending_chain = NULL;
    uint64_t chain_pc = 0;    /* where that exit was going, and in which state */
    uint32_t chain_flags = 0;
    uint32_t last_reason = HV_EXIT_BRANCH;
    hv_self = cpu;
    unpark(cpu);
    for (;;) {
        if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST) || __atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST)) {
            stop_the_world(cpu);
            pending_chain = NULL;
            prev_b = NULL;
            continue;
        }
        hv_cpu_process_requests(cpu);
        if (cpu->halted) return HV_EXIT_HALT;
        if (cpu->off) {
            wait_while_off(cpu); /* comes back after CPU_ON, reset to its entry point */
            pending_chain = NULL;
            last_reason = HV_EXIT_EXCEPTION;
            continue;
        }
        /* Someone asked the CPU to leave (the 1 ms kicker, a device, an invalidation): service devices now. */
        int kicked = __atomic_exchange_n(&cpu->exit_req, 0, __ATOMIC_RELAXED) != 0;
        if (kicked && m->big_lock_on) {
            /* Give another CPU its turn. Whatever it does in between may touch
               what this CPU's exclusive monitor watches, so the monitor clears. */
            cpu->excl_addr = ~0ull;
            hv_big_yield(m);
        }
        /* Ticks take the device lock, so not every few blocks: on a kick (the 1 ms
           kicker, a device with work for CPU 0, an interrupt raised for this
           CPU) or once this CPU's own timer is due. */
        if (m->tick && (kicked || (++since_tick >= 64 && (since_tick = 0, next_due && mach_absolute_time() >= next_due)))) {
            since_tick = 0;
            next_due = m->tick(m, cpu);
        }
        int why = pending_chain ? 0 : kicked ? 1 : last_reason != HV_EXIT_BRANCH ? 2 : 5;
        if (hv_check_interrupts(cpu)) {
            pending_chain = NULL;
            why = 3;
        }
        if (cpu->pc == m->stop_pc) return HV_EXIT_HALT;

        if (m->log && !cpu->el && ((cpu->pc >> 55) & 1)) { /* EL0 can never run kernel code: say how we got here */
            static int reported;
            if (reported++ < 4)
                fprintf(stderr, "hengevm: el0 at kernel pc %llx: previous block %llx, exit reason %u, why %d, miss site %llx, "
                                "elr %llx spsr %llx x30 %llx\n",
                        (unsigned long long)cpu->pc, (unsigned long long)prev_pc, last_reason, why,
                        (unsigned long long)cpu->miss_site, (unsigned long long)cpu->elr_el1,
                        (unsigned long long)cpu->spsr_el1, (unsigned long long)cpu->x[30]);
        }
        uint64_t pa;
        uint32_t avail;
        const uint32_t *code = hv_mmu_fetch(cpu, cpu->pc, &pa, &avail);
        if (!code) { /* the instruction abort was taken */
            /* The pc is the vector now, not the branch target: the exit that got
               here must not be chained to whatever runs next (that put EL0 code
               on a path straight into the kernel's vector code). */
            pending_chain = NULL;
            if (cpu->pc == last_fault) return HV_EXIT_FAULT_LOOP;
            last_fault = cpu->pc;
            continue;
        }
        last_fault = ~0ull;
        uint64_t flushes = m->stats.block_flushes;
        HvBlock *b = lookup(cpu, cpu->pc, pa, block_flags(cpu), code, avail);
        /* A full flush while translating freed the slot the last exit came through,
           and new blocks may have it now: chaining it would rewrite one of theirs. */
        if (m->stats.block_flushes != flushes) pending_chain = NULL;
        if (!b) {
            if (m->flush_pending) continue; /* everything is about to be dropped: try again after */
            fprintf(stderr, "hengevm: cannot translate block at %llx\n", (unsigned long long)cpu->pc);
            park(cpu);
            return HV_EXIT_UNSUPPORTED;
        }
        /* Remember it for indirect branches (not the tests' stop PC: that must reach us). */
        /* EL0 tables only hold user VAs and EL1 tables kernel VAs, so each half has its own salt. */
        int kernel = b->flags & HV_BLOCK_EL1;
        if (m->chain && b->va != m->stop_pc && (int)((b->va >> 55) & 1) == kernel) {
            HvPerCpu *pcs = cpu->pc_state;
            uint64_t *t = pcs->ijc + (size_t)(b->flags & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8);
            uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
            uint64_t *ent = t + (size_t)hv_ijc_index(tag) * 2;
            if (why == 5 && ent[0] == tag) {
                why = 4; /* it was cached: not a miss */
                static int dbg = -1, shown;
                if (dbg < 0) dbg = getenv("HV_IJC_DEBUG") != NULL;
                if (dbg) { /* what instruction came just before the target (put_exit leaves with pc + 4) */
                    static uint64_t hist[4], total;
                    uint32_t before = 0;
                    uint64_t off = b->pa - b->ram->pa;
                    if (off & 0x3fff) memcpy(&before, b->ram->host + off - 4, 4);
                    int k = cpu->miss_site ? 0 : before == 0xd5033fdf ? 1 : (before & 0xfff00000u) == 0xd5100000u ? 2 : 3;
                    hist[k]++;
                    if (++total % 200000 == 0)
                        fprintf(stderr, "ijc cached-miss kinds: ijc site %llu, after ISB %llu, after MSR %llu, other %llu\n",
                                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                                (unsigned long long)hist[3]);
                    static uint64_t msrs;
                    if (k == 2 && ++msrs % 20000 == 0)
                        fprintf(stderr, "ijc cached-miss msr: S%u_%u_C%u_C%u_%u\n", 2 + ((before >> 19) & 1), (before >> 16) & 7,
                                (before >> 12) & 15, (before >> 8) & 15, (before >> 5) & 7);
                    static uint64_t others;
                    if (k == 3 && ++others % 50000 == 0 && shown < 60) {
                        shown++;
                        fprintf(stderr, "ijc cached-miss other: target %llx el%u reason %u, insn before %08x\n", (unsigned long long)b->va, cpu->el, last_reason, before);
                    }
                }
                if (0) {
                    shown++;
                    uint32_t last = 0;
                    if (prev_b && prev_b->ninsns)
                        memcpy(&last, prev_b->ram->host + (prev_b->pa - prev_b->ram->pa) + 4 * (prev_b->ninsns - 1), 4);
                    fprintf(stderr, "ijc cached-miss: target %llx flags %x from site %llx (prev block %llx flags %x, %u insns, last %08x, reason %u)\n",
                            (unsigned long long)b->va, b->flags, (unsigned long long)cpu->miss_site,
                            (unsigned long long)prev_pc, prev_b ? prev_b->flags : 0, prev_b ? prev_b->ninsns : 0, last, last_reason);
                }
            }
            if (m->stats_interval) { /* what the fill replaces: nothing (cold), another salt's entry, or another target */
                uint64_t old = ent[0];
                int kind = !old ? 0 : (old >> 48) != (tag >> 48) ? 1 : 2;
                if (kind == 1) { /* another salt: is it still in use, or dead (dropped address space, reset)? */
                    uint32_t c = (uint32_t)(old >> 48);
                    int alive = kernel ? (c ^ 0xffff) == cpu->kernel_gen : cpu->asid_cid[cpu->cid_asid[c]] == c;
                    if (!alive) kind = 3;
                }
                m->stats.fill_kind[kind]++;
            }
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
            m->stats.ijc_fills++;
        }
        /* The block before left through a chainable direct branch to this one: link them. */
        /* Only if nothing in between (an abort, an interrupt) sent the CPU elsewhere. */
        if (pending_chain && !m->trace && b->va == chain_pc && b->flags == chain_flags) chain(m, pending_chain, b);
        pending_chain = NULL;
        if (m->trace) fprintf(stderr, "block %llx el%u\n", (unsigned long long)cpu->pc, cpu->el);
        if (m->log >= 4 && host_pointer_check(cpu, prev_pc)) return HV_EXIT_UNSUPPORTED;
        prev_pc = cpu->pc;
        prev_b = b;
        m->stats.why[why]++;
        m->stats.recent[m->stats.nrecent++ & 15] = cpu->pc | cpu->el;
        if (why == 5 && m->stats_interval) {
            int kind = 6;
            /* A plain walk: fetching could raise an abort in the guest. */
            HvWalk w;
            uint32_t i = 0;
            if (cpu->miss_site) hv_mmu_walk(cpu, cpu->miss_site, HV_ACC_EXEC, cpu->el, &w);
            if (cpu->miss_site && !w.fsc && hv_phys_read(m, w.pa, &i, 4) == 0) {
                if ((i & 0xfffffc1fu) == 0xd65f0000u || (i & 0xfffffbffu) == 0xd65f0bffu) kind = 0;         /* RET, RETAA/AB */
                else if ((i & 0xfe9f0000u) == 0xd61f0000u) kind = 1; /* BR, BLR and their PAC forms (bit 24) */
                else if ((i & 0x7c000000u) == 0x14000000u) kind = 2;                                         /* B, BL */
                else if ((i & 0xff000010u) == 0x54000000u || (i & 0x7e000000u) == 0x34000000u ||
                         (i & 0x7e000000u) == 0x36000000u) kind = 3;                                        /* B.cond, CB(N)Z, TB(N)Z */
                else if ((i & 0xfff8f01fu) == 0xd500401fu) kind = 4;                                         /* MSR (immediate) */
                else kind = 5;
            }
            m->stats.miss_kind[kind]++;
            cpu->miss_site = 0;
        }
        cpu->exit_reason = HV_EXIT_BRANCH;
        cpu->last_stub = 0;
        cpu->icount++;
        m->stats.blocks++;
        if (m->stats_interval && cpu->index == 0 && (m->stats.blocks & 0xffff) == 0) print_stats(cpu, 0);
        hv_enter(cpu, b->code);

        if (cpu->exit_reason == HV_EXIT_BRANCH && cpu->last_stub && m->chain) {
            pending_chain = (uint64_t *)cpu->last_stub;
            chain_pc = cpu->pc;
            chain_flags = block_flags(cpu); /* the exiting block's state (a block ends where its state changes) */
        }
        last_reason = cpu->exit_reason;
        switch (cpu->exit_reason) {
        case HV_EXIT_BRANCH:
        case HV_EXIT_EXCEPTION:
        case HV_EXIT_SYSREG:
        case HV_EXIT_FLUSH:
            break;
        case HV_EXIT_WFI:
            if (wait_for_interrupt(cpu) != 0) return HV_EXIT_WFI;
            break;
        default:
            return (int)cpu->exit_reason;
        }
        if (one) return HV_EXIT_BRANCH;
    }
}
