#include "dispatch.h"

#include <mach/mach_time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "exception.h"
#include "memop.h"
#include "mmu.h"
#include "sysreg.h"
#include "translate.h"

void hv_flush_blocks(HvMachine *m) {
    m->stats.block_flushes++;
    m->nchains = 0; /* the patched code goes away with the blocks */
    m->unchain_pending = 0;
    memset(m->hash, 0, sizeof m->hash);
    m->nblocks = 0;
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
    uint32_t h = hash_of(va, pa, flags);
    HvRam *ram = hv_ram_lookup(m, pa);
    uint64_t pg = (pa - ram->pa) >> HV_TLB_PAGE_BITS;
    uint32_t gen = ram->page_gen[pg];
    for (HvBlock *b = m->hash[h]; b; b = b->next)
        if (b->va == va && b->pa == pa && b->flags == flags && b->gen == gen) return b;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (m->nblocks == HV_MAX_BLOCKS) {
            m->stats.flush_full++;
            hv_flush_blocks(m);
        }
        HvBlock *b = &m->blocks[m->nblocks];
        *b = (HvBlock){.va = va, .pa = pa, .flags = flags, .gen = gen};
        m->stats.translations++;
        if (hv_translate_block(m, b, code, avail) != 0) {
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
            hv_tlb_flush(cpu);
        }
        return b;
    }
    return NULL;
}

/* Blocks call this through hv_helper_entry. Returns 1 to leave the block. */
static uint32_t reg_hist[65536]; /* --stats: MRS/MSR/SYS by op0:op1:CRn:CRm:op2 */

uint32_t hv_helper(HvCpu *cpu, const HvHelperInfo *info) {
    if (info->op < 12) cpu->m->stats.helper[info->op]++;
    if (cpu->m->stats_interval && (info->op == HV_OP_MRS || info->op == HV_OP_MSR || info->op == HV_OP_SYS))
        reg_hist[(info->insn >> 5) & 0xffff] += info->op == HV_OP_MRS ? 1 : 0x10000;
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
    usleep((useconds_t)(wait_ticks / 24));
    m->stats.wfi++;
    m->stats.wfi_ticks += mach_absolute_time() - t0;
    m->tick(m, cpu);
    return 0;
}

static void print_stats(HvCpu *cpu, int force) {
    static uint64_t last_t, start_t;
    static HvStats last;
    HvMachine *m = cpu->m;
    uint64_t now = mach_absolute_time();
    if (!start_t) start_t = last_t = now;
    if (!force && now - last_t < (uint64_t)m->stats_interval * 24000000ull) return;
    double dt = (double)(now - last_t) / 24e6;
    HvStats *s = &m->stats;
#define RATE(f) ((double)(s->f - last.f) / dt)
    fprintf(stderr,
            "stats %5.0fs: blocks %.2fM/s, translated %.0f/s, slow mem %.0fK/s, mrs %.0fK/s msr %.0fK/s sys %.0fK/s, "
            "exc %.0f/s irq %.0f/s, tlb flush %.0f/s, fetch walks %.0fK/s, wfi %.0f/s (%.0f%% idle), flushes %.0f/s (smc %.0f ic %.0f full %.0f), chained %.0f/s unchains %.0f/s, el%u pc %llx\n",
            (double)(now - start_t) / 24e6, RATE(blocks) / 1e6, RATE(translations), RATE(helper[HV_OP_MEM]) / 1e3,
            RATE(helper[HV_OP_MRS]) / 1e3, RATE(helper[HV_OP_MSR]) / 1e3, RATE(helper[HV_OP_SYS]) / 1e3,
            RATE(exceptions), RATE(interrupts), RATE(tlb_flushes), RATE(fetch_walks) / 1e3, RATE(wfi),
            100.0 * (double)(s->wfi_ticks - last.wfi_ticks) / (double)(now - last_t), RATE(block_flushes), RATE(flush_smc),
            RATE(flush_ic), RATE(flush_full), RATE(chained), RATE(unchains), cpu->el,
            (unsigned long long)cpu->pc);
#undef RATE
    /* The five busiest system registers or SYS operations (reads + writes) this interval. */
    for (int k = 0; k < 5; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t r = 0; r < 65536; r++) {
            uint32_t n = (reg_hist[r] & 0xffff) + (reg_hist[r] >> 16);
            if (n > best) best = n, bi = r;
        }
        if (!best) break;
        fprintf(stderr, "   S%u_%u_C%u_C%u_%u: %u reads %u writes/sys\n", bi >> 14, (bi >> 11) & 7, (bi >> 7) & 15,
                (bi >> 3) & 15, bi & 7, reg_hist[bi] & 0xffff, reg_hist[bi] >> 16);
        reg_hist[bi] = 0;
    }
    memset(reg_hist, 0, sizeof reg_hist);
    last = *s;
    last_t = now;
}

/* Undo every chain: the patch slots go back to "fall through to the exit". */
static void unchain_all(HvMachine *m) {
    if (!m->nchains) return;
    hv_jit_begin_write();
    for (uint32_t i = 0; i < m->nchains; i++) *m->chains[i] = 0x14000001; /* b +1 */
    for (uint32_t i = 0; i < m->nchains; i++) hv_jit_end_write(m->chains[i], 1);
    m->nchains = 0;
    m->stats.unchains++;
}

/* Patch the exit slot a block just left through so it jumps straight to b. */
static void chain(HvMachine *m, uint32_t *slot, HvBlock *b) {
    int64_t off = (int64_t)(b->code - slot);
    if (off < -(1 << 25) || off >= (1 << 25) || m->nchains == HV_MAX_CHAINS) return;
    hv_jit_begin_write();
    *slot = 0x14000000u | (uint32_t)(off & 0x3ffffff);
    hv_jit_end_write(slot, 1);
    m->chains[m->nchains++] = slot;
    m->stats.chained++;
}

static int run(HvCpu *cpu, int one);

int hv_run(HvCpu *cpu) {
    return run(cpu, 0);
}

int hv_run_one(HvCpu *cpu) {
    return run(cpu, 1);
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
    uint32_t *pending_chain = NULL;
    for (;;) {
        if (m->flush_pending) {
            m->flush_pending = 0;
            hv_flush_blocks(m);
            pending_chain = NULL;
        }
        if (m->unchain_pending) {
            m->unchain_pending = 0;
            unchain_all(m);
            pending_chain = NULL;
        }
        if (cpu->halted) return HV_EXIT_HALT;
        /* Someone asked the CPU to leave (the 1 ms kicker, a device, an invalidation): service devices now. */
        int kicked = __atomic_exchange_n(&cpu->exit_req, 0, __ATOMIC_RELAXED) != 0;
        if (m->tick && (kicked || ++since_tick >= 64)) {
            since_tick = 0;
            m->tick(m, cpu);
        }
        if (hv_check_interrupts(cpu)) pending_chain = NULL;
        if (cpu->pc == m->stop_pc) return HV_EXIT_HALT;

        uint64_t pa;
        uint32_t avail;
        const uint32_t *code = hv_mmu_fetch(cpu, cpu->pc, &pa, &avail);
        if (!code) { /* the instruction abort was taken */
            if (cpu->pc == last_fault) return HV_EXIT_FAULT_LOOP;
            last_fault = cpu->pc;
            continue;
        }
        last_fault = ~0ull;
        HvBlock *b = lookup(cpu, cpu->pc, pa, block_flags(cpu), code, avail);
        if (!b) {
            fprintf(stderr, "hengevm: cannot translate block at %llx\n", (unsigned long long)cpu->pc);
            return HV_EXIT_UNSUPPORTED;
        }
        /* The block before left through a chainable direct branch to this one: link them. */
        if (pending_chain && !m->trace) chain(m, pending_chain, b);
        pending_chain = NULL;
        if (m->trace) fprintf(stderr, "block %llx el%u\n", (unsigned long long)cpu->pc, cpu->el);
        if (m->log >= 4 && host_pointer_check(cpu, prev_pc)) return HV_EXIT_UNSUPPORTED;
        prev_pc = cpu->pc;
        cpu->exit_reason = HV_EXIT_BRANCH;
        cpu->last_stub = 0;
        cpu->icount++;
        m->stats.blocks++;
        if (m->stats_interval && (m->stats.blocks & 0xffff) == 0) print_stats(cpu, 0);
        hv_enter(cpu, b->code);

        if (cpu->exit_reason == HV_EXIT_BRANCH && cpu->last_stub && m->chain) pending_chain = (uint32_t *)cpu->last_stub;
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
