#include "dispatch.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
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
    if (m->cpu) {
        hv_ijc_reset(m->cpu, 0);
        hv_ijc_reset(m->cpu, 1);
    }
    memset(m->jc, 0, sizeof m->jc);
    memset(m->hash, 0, sizeof m->hash);
    m->nblocks = 0;
    m->nslots = 0;
    hv_jit_reset(&m->jit);
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

static HvBlock *lookup(HvCpu *cpu, uint64_t va, uint64_t pa, uint32_t flags, const uint32_t *code, uint32_t avail) {
    HvMachine *m = cpu->m;
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    HvBlock *c = m->jc[j];
    if (c && c->va == va && c->pa == pa && c->flags == flags && c->ram->page_gen[c->pg] == c->gen) return c;
    uint32_t h = hash_of(va, pa, flags);
    HvRam *ram = hv_ram_lookup(m, pa);
    uint64_t pg = (pa - ram->pa) >> HV_TLB_PAGE_BITS;
    uint32_t gen = ram->page_gen[pg];
    for (HvBlock *b = m->hash[h]; b; b = b->next)
        if (b->va == va && b->pa == pa && b->flags == flags && b->gen == gen) return m->jc[j] = b;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (m->nblocks == HV_MAX_BLOCKS) {
            m->stats.flush_full++;
            hv_flush_blocks(m);
        }
        HvBlock *b = &m->blocks[m->nblocks];
        *b = (HvBlock){.va = va, .pa = pa, .flags = flags, .gen = gen, .ram = ram, .pg = pg};
        m->stats.translations++;
        if (m->stats_interval) { /* diagnostics: was this code translated before? (hashed bitmaps, so approximate) */
            static uint64_t seen_va[1 << 18], seen_vapa[1 << 18];
            uint32_t hv = (uint32_t)(((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull >> 40);
            uint32_t hp = (uint32_t)((((va >> 2) ^ flags) * 0x9e3779b97f4a7c15ull ^ pa * 0xc2b2ae3d27d4eb4full) >> 40);
            if (seen_va[hv >> 6] >> (hv & 63) & 1) m->stats.retrans_va++;
            if (seen_vapa[hp >> 6] >> (hp & 63) & 1) m->stats.retrans_vapa++;
            seen_va[hv >> 6] |= 1ull << (hv & 63);
            seen_vapa[hp >> 6] |= 1ull << (hp & 63);
        }
        uint64_t t0 = m->stats_interval ? mach_absolute_time() : 0;
        size_t used0 = m->jit.used;
        int tr = hv_translate_block(m, b, code, avail);
        if (m->stats_interval) {
            m->stats.translate_ticks += mach_absolute_time() - t0;
            m->stats.translated_insns += b->ninsns;
            m->stats.translated_words += m->jit.used - used0;
        }
        if (tr != 0) {
            m->stats.flush_full++;
            hv_flush_blocks(m); /* JIT region full: start over */
            h = hash_of(va, pa, flags);
            continue;
        }
        m->nblocks++;
        b->next = m->hash[h];
        m->hash[h] = b;
        /* Writes to this page must now take the slow path so we notice them. */
        if (!ram->code_pages[pg]) {
            ram->code_pages[pg] = 1;
            hv_tlb_protect_code(cpu, ram->host + (pg << HV_TLB_PAGE_BITS));
        }
        return b;
    }
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
        if (!cpu->el && ((info->pc >> 55) & 1)) { /* EL0 in kernel code: which block is this? */
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
            fprintf(stderr, "\n");
        }
        hv_raise_undef(cpu, info->pc);
        return 1;
    }
}

/* WFI: sleep until an interrupt is pending (masked or not) or the next timer
   event, at most 10 ms. WFI may complete early, so the guest just runs again
   if nothing happened. */
static int wait_for_interrupt(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    uint64_t deadline = m->tick ? m->tick(m, cpu) : 0;
    if (m->irq_line || m->fiq_line) return 0;
    if (!m->tick) return -1; /* nothing could ever wake us */
    uint64_t now = mach_absolute_time();
    uint64_t wait_ticks = deadline && deadline > now ? deadline - now : 240000; /* the counter runs at 24 MHz */
    if (wait_ticks > 240000) wait_ticks = 240000;
    uint64_t t0 = mach_absolute_time();
    /* Sleep until the next timer event or until a device thread has work for us. */
    pthread_mutex_lock(&m->wake_mu);
    if (!m->wake_pending) {
        struct timespec ts = {(time_t)(wait_ticks / 24000000), (long)(wait_ticks % 24000000) * 1000 / 24};
        pthread_cond_timedwait_relative_np(&m->wake_cv, &m->wake_mu, &ts);
    }
    m->wake_pending = 0;
    pthread_mutex_unlock(&m->wake_mu);
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
            last_busy = busy;
        }
        mach_port_deallocate(mach_task_self(), self);
        fprintf(stderr, "  cache drops/s: full %.0f, ASID %.0f, user VA %.0f, kernel VA %.0f, code %.0f\n", RATE(reset_why[0]),
                RATE(reset_why[1]), RATE(reset_why[2]), RATE(reset_why[3]), RATE(reset_why[4]));
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
    for (int k = 0; k < 5; k++) {
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
    *slot = (uint64_t)(uintptr_t)b->code;
    m->stats.chained++;
}

static int run(HvCpu *cpu, int one);

int hv_run(HvCpu *cpu) {
    hv_fp_enter(cpu);
    int r = run(cpu, 0);
    hv_fp_leave(cpu);
    return r;
}

int hv_run_one(HvCpu *cpu) {
    hv_fp_enter(cpu);
    int r = run(cpu, 1);
    hv_fp_leave(cpu);
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

static int run(HvCpu *cpu, int one) {
    HvMachine *m = cpu->m;
    uint32_t since_tick = 0;
    uint64_t last_fault = ~0ull, prev_pc = 0;
    uint64_t *pending_chain = NULL;
    uint32_t last_reason = HV_EXIT_BRANCH;
    for (;;) {
        if (m->flush_pending) {
            m->flush_pending = 0;
            hv_flush_blocks(m);
            pending_chain = NULL;
        }
        if (cpu->halted) return HV_EXIT_HALT;
        /* Someone asked the CPU to leave (the 1 ms kicker, a device, an invalidation): service devices now. */
        int kicked = __atomic_exchange_n(&cpu->exit_req, 0, __ATOMIC_RELAXED) != 0;
        if (m->tick && (kicked || ++since_tick >= 64)) {
            since_tick = 0;
            m->tick(m, cpu);
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
            fprintf(stderr, "hengevm: cannot translate block at %llx\n", (unsigned long long)cpu->pc);
            return HV_EXIT_UNSUPPORTED;
        }
        /* Remember it for indirect branches (not the tests' stop PC: that must reach us). */
        /* EL0 tables only hold user VAs and EL1 tables kernel VAs, so each half has its own salt. */
        int kernel = b->flags & HV_BLOCK_EL1;
        if (m->chain && b->va != m->stop_pc && (int)((b->va >> 55) & 1) == kernel) {
            uint64_t *t = m->ijc + (size_t)(b->flags & (HV_IJC_TABLES - 1)) * (HV_IJC_TABLE_BYTES / 8);
            uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
            uint64_t *ent = t + (size_t)hv_ijc_index(tag) * 2;
            if (why == 5 && ent[0] == tag) why = 4; /* it was cached: not a miss */
            ent[0] = tag;
            ent[1] = (uint64_t)(uintptr_t)b->code;
            m->ijc_tables_used |= 1u << (b->flags & (HV_IJC_TABLES - 1));
            if (!kernel) {
                uint32_t h = (uint32_t)(b->va >> HV_TLB_PAGE_BITS) & 0xffff;
                m->ijc_pages[0][h >> 6] |= 1ull << (h & 63);
            }
            uint32_t k = hv_ijc_key(b->va, cpu->ijc_salt[kernel]);
            if (!(m->ijc_keyed[k >> 6] >> (k & 63) & 1)) {
                m->ijc_keyed[k >> 6] |= 1ull << (k & 63);
                if (++m->ijc_keyed_count > (1u << 17)) { /* an eighth full: start the cache and the filter over */
                    hv_ijc_reset(cpu, 0);
                    hv_ijc_reset(cpu, 1);
                    memset(m->ijc_keyed, 0, sizeof m->ijc_keyed);
                    m->ijc_keyed_count = 0;
                }
            }
            m->stats.ijc_fills++;
        }
        /* The block before left through a chainable direct branch to this one: link them. */
        if (pending_chain && !m->trace) chain(m, pending_chain, b);
        pending_chain = NULL;
        if (m->trace) fprintf(stderr, "block %llx el%u\n", (unsigned long long)cpu->pc, cpu->el);
        if (m->log >= 4 && host_pointer_check(cpu, prev_pc)) return HV_EXIT_UNSUPPORTED;
        prev_pc = cpu->pc;
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
        if (m->stats_interval && (m->stats.blocks & 0xffff) == 0) print_stats(cpu, 0);
        hv_enter(cpu, b->code);

        if (cpu->exit_reason == HV_EXIT_BRANCH && cpu->last_stub && m->chain) pending_chain = (uint64_t *)cpu->last_stub;
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
