#include "exception.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "dispatch.h"
#include "mmu.h"

/* Only EL0 and EL1 exist for the guest; HengeVM plays EL2 (PSCI over HVC, as
   QEMU's vmapple machine sets up) and there is no EL3. */

#define PSTATE_EXTRA_MASK ((1ull << 22) | (1ull << 23) | (1ull << 12) | (1ull << 24) | (1ull << 25))

uint64_t hv_pstate_get(HvCpu *cpu) {
    return (cpu->nzcv & 0xf0000000) | (cpu->daif & HV_PSTATE_DAIF) | (cpu->pstate_extra & PSTATE_EXTRA_MASK) |
           (uint64_t)cpu->el << 2 | (cpu->el ? cpu->spsel : 0);
}

void hv_sp_switch(HvCpu *cpu, uint32_t el, uint32_t spsel) {
    if (cpu->el == 0 || cpu->spsel == 0) cpu->sp_el0 = cpu->sp;
    else cpu->sp_el1 = cpu->sp;
    cpu->el = el;
    cpu->spsel = el ? spsel : 0;
    cpu->sp = cpu->spsel ? cpu->sp_el1 : cpu->sp_el0;
}

static void set_extra(HvCpu *cpu, uint64_t extra) {
    /* PAN selects a different TLB table (hv_tlb_table), so nothing to flush. */
    cpu->pstate_extra = extra;
}

void hv_pstate_set(HvCpu *cpu, uint64_t spsr) {
    uint32_t m = spsr & 0x1f;
    uint32_t el = (m >> 2) & 3;
    if ((m & 0x10) || el > 1) el = 1; /* AArch32 and EL2/EL3 do not exist: an illegal return, kept at EL1 */
    cpu->nzcv = spsr & 0xf0000000;
    cpu->daif = spsr & HV_PSTATE_DAIF;
    set_extra(cpu, spsr & PSTATE_EXTRA_MASK);
    hv_sp_switch(cpu, el, m & 1);
}

void hv_take_exception(HvCpu *cpu, int type, uint32_t esr, uint64_t far, int set_far, uint64_t ret) {
    if (type == HV_EXC_SYNC) cpu->m->stats.exceptions++;
    else cpu->m->stats.interrupts++;
    uint64_t spsr = hv_pstate_get(cpu);
    {
        HvStats *st = &cpu->m->stats;
        uint32_t k = st->nelchg++ & 15;
        st->elchg[k].from = ret, st->elchg[k].spsr = spsr, st->elchg[k].kind = (uint32_t)type, st->elchg[k].el = cpu->el;
        st->elchg[k].to = esr;
    }
    if (cpu->m->trace || cpu->m->log >= 2)
        fprintf(stderr, "exception type %d esr %08x far %llx from pc %llx el%u\n", type, esr, (unsigned long long)far,
                (unsigned long long)ret, cpu->el);
    uint64_t offset = cpu->el == 0 ? 0x400 : cpu->spsel ? 0x200 : 0x000;
    cpu->spsr_el1 = spsr;
    cpu->elr_el1 = ret;
    if (type == HV_EXC_SYNC || type == HV_EXC_SERROR) cpu->esr_el1 = esr;
    if (set_far) cpu->far_el1 = far;
    hv_sp_switch(cpu, 1, 1);
    cpu->daif = HV_PSTATE_DAIF;
    uint64_t extra = cpu->pstate_extra;
    if (!((cpu->sctlr_el1 >> 23) & 1)) extra |= HV_PSTATE_PAN; /* SCTLR_EL1.SPAN == 0 */
    extra &= ~(1ull << 12);
    if ((cpu->sctlr_el1 >> 44) & 1) extra |= 1ull << 12;      /* SSBS from DSSBS */
    set_extra(cpu, extra);
    cpu->excl_addr = ~0ull;
    cpu->pc = (cpu->vbar_el1 & ~0x7ffull) + offset + (uint64_t)type * 0x80;
    cpu->exit_reason = HV_EXIT_EXCEPTION;
}

void hv_raise_sync(HvCpu *cpu, uint32_t ec, uint32_t iss, uint64_t far, uint64_t pc) {
    int set_far = ec == HV_EC_PC_ALIGN || ec == HV_EC_IABT_LOWER || ec == HV_EC_IABT_SAME ||
                  ec == HV_EC_DABT_LOWER || ec == HV_EC_DABT_SAME;
    hv_take_exception(cpu, HV_EXC_SYNC, ec << 26 | 1u << 25 | (iss & 0x1ffffff), far, set_far, pc);
}

void hv_raise_undef(HvCpu *cpu, uint64_t pc) {
    static int reported;
    if (cpu->el == 0 && ((pc >> 55) & 1) && reported++ < 3) hv_debug_el0_in_kernel(cpu, "undef");
    if (cpu->m->log) {
        uint32_t insn = 0;
        uint64_t pa;
        if (!hv_mmu_access(cpu, pc, HV_ACC_EXEC, cpu->el, &pa)) hv_phys_read(cpu->m, pa, &insn, 4);
        fprintf(stderr, "undefined: pc %llx insn %08x el%u\n", (unsigned long long)pc, insn, cpu->el);
    }
    hv_raise_sync(cpu, HV_EC_UNKNOWN, 0, 0, pc);
}

void hv_raise_abort(HvCpu *cpu, int insn, uint64_t va, uint32_t fsc, int wnr, int s1ptw, uint64_t pc) {
    uint32_t ec = insn ? (cpu->el ? HV_EC_IABT_SAME : HV_EC_IABT_LOWER)
                       : (cpu->el ? HV_EC_DABT_SAME : HV_EC_DABT_LOWER);
    uint32_t iss = fsc | (uint32_t)(wnr && !insn) << 6 | (uint32_t)s1ptw << 7;
    hv_raise_sync(cpu, ec, iss, va, pc);
}

void hv_eret(HvCpu *cpu) {
    if (cpu->m) {
        HvStats *st = &cpu->m->stats;
        uint32_t k = st->nelchg++ & 15;
        st->elchg[k].from = cpu->pc, st->elchg[k].to = cpu->elr_el1, st->elchg[k].spsr = cpu->spsr_el1;
        st->elchg[k].kind = 9, st->elchg[k].el = cpu->el;
    }
    cpu->pc = cpu->elr_el1;
    hv_pstate_set(cpu, cpu->spsr_el1);
    cpu->excl_addr = ~0ull;
}

/* PSCI 1.0 for one CPU. */
static uint64_t psci(HvCpu *cpu, uint64_t fn) {
    static int log = -1;
    if (log < 0) log = getenv("HV_PSCI_LOG") != NULL;
    if (log && ((uint32_t)fn >> 24 == 0x84 || (uint32_t)fn >> 24 == 0xc4))
        fprintf(stderr, "psci %#llx x1 %#llx x2 %#llx x3 %#llx pc %llx\n", (unsigned long long)fn, (unsigned long long)cpu->x[1],
                (unsigned long long)cpu->x[2], (unsigned long long)cpu->x[3], (unsigned long long)cpu->pc);
    switch ((uint32_t)fn) {
    case 0x84000000: return 0x10000;                         /* PSCI_VERSION: 1.0 */
    case 0x84000006: return 2;                               /* MIGRATE_INFO_TYPE: no trusted OS */
    case 0x84000008:                                         /* SYSTEM_OFF */
    case 0x84000009:                                         /* SYSTEM_RESET */
        cpu->halted = (uint32_t)fn == 0x84000008 ? 1 : 2;
        return 0;
    case 0x8400000a: {                                       /* PSCI_FEATURES */
        uint32_t q = (uint32_t)cpu->x[1];
        if (q == 0x84000000 || q == 0x84000006 || q == 0x84000008 || q == 0x84000009 ||
            q == 0x8400000a || q == 0xc4000003 || q == 0x84000003 || q == 0xc4000004 || q == 0x84000004)
            return 0;
        return (uint64_t)-1;
    }
    case 0x84000003: case 0xc4000003:                        /* CPU_ON: no other CPUs yet */
        return (uint64_t)-2;                                 /* INVALID_PARAMETERS */
    case 0x84000004: case 0xc4000004:                        /* AFFINITY_INFO */
        return (cpu->x[1] & 0xffffff) == (cpu->mpidr & 0xffffff) ? 0 : 1;
    /* vmapple's paravirtualized PAC (XNU osfmk/arm64/hv_hvc.h): the guest
       cannot program PAC keys itself, so the hypervisor does. HengeVM never
       signs pointers, so there is nothing to program; each call only has to
       succeed (XNU spins on a nonzero x0). */
    case 0xc1000000: /* VMAPPLE_PAC_SET_INITIAL_STATE */
    case 0xc1000002: /* VMAPPLE_PAC_SET_A_KEYS */
    case 0xc1000003: /* VMAPPLE_PAC_SET_B_KEYS */
    case 0xc1000004: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER */
    case 0xc1000005: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER_AT_EL1 */
    case 0xc1000006: /* VMAPPLE_PAC_SET_G_KEY */
    case 0xc10000f0: /* VMAPPLE_PAC_NOP */
        return 0;
    case 0xc1000001: /* VMAPPLE_PAC_GET_DEFAULT_KEYS: x2 = default ROP id, x3 = default JOP id */
        cpu->x[2] = 0x524f50; /* the values only name keys, and no key is ever used */
        cpu->x[3] = 0x4a4f50;
        return 0;
    default:
        if (cpu->m->log) fprintf(stderr, "hengevm: unknown hypervisor call %#llx (x1 %#llx) at el%u\n",
                                 (unsigned long long)fn, (unsigned long long)cpu->x[1], cpu->el);
        return (uint64_t)-1;                                 /* NOT_SUPPORTED */
    }
}

/* Bring-up aid: x0-x3, and any string they point to (read with the current EL's mapping). */
void hv_debug_args(HvCpu *cpu) {
    for (int i = 0; i < 4; i++) fprintf(stderr, " x%d=%llx", i, (unsigned long long)cpu->x[i]);
    fprintf(stderr, "\n");
    for (int i = 0; i < 4; i++) {
        char str[161];
        int n = 0;
        for (; n < 160; n++) {
            uint64_t pa;
            if (hv_mmu_access(cpu, cpu->x[i] + n, HV_ACC_READ, cpu->el, &pa) || hv_phys_read(cpu->m, pa, &str[n], 1) ||
                str[n] < 9 || (unsigned char)str[n] > 126)
                break;
        }
        str[n] = 0;
        if (n >= 4) fprintf(stderr, "    x%d -> \"%s\"\n", i, str);
    }
}

int hv_exception_insn(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t imm16 = (insn >> 5) & 0xffff;
    uint32_t opc = (insn >> 21) & 7, ll = insn & 3;
    if (opc == 0 && ll == 1 && cpu->m->log >= 3) {
        fprintf(stderr, "svc #%x at %llx el%u:", imm16, (unsigned long long)pc, cpu->el);
        hv_debug_args(cpu);
    }
    if (opc == 0 && ll == 1) { /* SVC: ELR is the next instruction */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_SVC64 << 26 | 1u << 25 | imm16, 0, 0, pc + 4);
        return 1;
    }
    if (opc == 0 && ll == 2 && cpu->el == 1) { /* HVC: the hypervisor's PSCI and vmapple's PAC calls */
        cpu->x[0] = psci(cpu, cpu->x[0]);
        if (!cpu->halted) return 0; /* like a call: the block carries on (xnu makes one per context switch) */
        cpu->pc = pc + 4;
        cpu->exit_reason = HV_EXIT_HALT;
        return 1;
    }
    if (opc == 1 && ll == 0) { /* BRK */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_BRK << 26 | 1u << 25 | imm16, 0, 0, pc);
        return 1;
    }
    /* SMC with no EL3, HVC from EL0, HLT without halting debug, DCPS: undefined. */
    hv_raise_undef(cpu, pc);
    return 1;
}

int hv_check_interrupts(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    (void)m;
    if (p->fiq_line && !(cpu->daif & HV_PSTATE_F)) {
        hv_take_exception(cpu, HV_EXC_FIQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    if (p->irq_line && !(cpu->daif & HV_PSTATE_I)) {
        hv_take_exception(cpu, HV_EXC_IRQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    return 0;
}
