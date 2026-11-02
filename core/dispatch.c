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
    memset(m->hash, 0, sizeof m->hash);
    m->nblocks = 0;
    hv_jit_reset(&m->jit);
}

static uint32_t block_flags(HvCpu *cpu) {
    uint32_t flags = cpu->el ? HV_BLOCK_EL1 : 0;
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
    for (HvBlock *b = m->hash[h]; b; b = b->next)
        if (b->va == va && b->pa == pa && b->flags == flags) return b;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (m->nblocks == HV_MAX_BLOCKS) hv_flush_blocks(m);
        HvBlock *b = &m->blocks[m->nblocks];
        *b = (HvBlock){.va = va, .pa = pa, .flags = flags};
        if (hv_translate_block(m, b, code, avail) != 0) {
            hv_flush_blocks(m); /* JIT region full: start over */
            h = hash_of(va, pa, flags);
            continue;
        }
        m->nblocks++;
        b->next = m->hash[h];
        m->hash[h] = b;
        /* Writes to this page must now take the slow path so we notice them. */
        HvRam *ram = hv_ram_lookup(m, pa);
        uint64_t pg = (pa - ram->pa) >> HV_TLB_PAGE_BITS;
        if (!ram->code_pages[pg]) {
            ram->code_pages[pg] = 1;
            hv_tlb_flush(cpu);
        }
        return b;
    }
    return NULL;
}

/* Blocks call this through hv_helper_entry. Returns 1 to leave the block. */
uint32_t hv_helper(HvCpu *cpu, const HvHelperInfo *info) {
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
    usleep((useconds_t)(wait_ticks / 24));
    m->tick(m, cpu);
    return 0;
}

int hv_run(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    uint32_t since_tick = 0;
    uint64_t last_fault = ~0ull;
    for (;;) {
        if (m->flush_pending) {
            m->flush_pending = 0;
            hv_flush_blocks(m);
        }
        if (cpu->halted) return HV_EXIT_HALT;
        if (m->tick && ++since_tick >= 64) {
            since_tick = 0;
            m->tick(m, cpu);
        }
        hv_check_interrupts(cpu);
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
        if (m->trace) fprintf(stderr, "block %llx el%u\n", (unsigned long long)cpu->pc, cpu->el);
        cpu->exit_reason = HV_EXIT_BRANCH;
        cpu->icount++;
        hv_enter(cpu, b->code);

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
    }
}
