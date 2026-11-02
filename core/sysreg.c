#include "sysreg.h"

#include <mach/mach_time.h>
#include <string.h>

#include "exception.h"
#include "machine.h"
#include "memop.h"
#include "mmu.h"

/* System registers are named by op0:op1:CRn:CRm:op2, which is bits 20..5 of MRS/MSR. */
#define R(op0, op1, crn, crm, op2) ((op0) << 14 | (op1) << 11 | (crn) << 7 | (crm) << 3 | (op2))

/* ID registers: only features the translator handles. Host features are a
   superset (Apple M1 and later), so copied instructions always exist. */
#define ID_AA64PFR0  0x1100000001110011ull /* EL0/EL1 AArch64, FP+AdvSIMD with FP16, GICv3 sysregs, CSV2, CSV3 */
#define ID_AA64ISAR0 0x0011100110212120ull /* AES+PMULL, SHA1, SHA2/512, CRC32, LSE, RDM, SHA3, DotProd, FHM, FlagM */
#define ID_AA64ISAR1 0x0000001110211011ull /* DPB, PAuth (no-op), JSCVT, FCMA, LRCPC2, PACGA, FRINTTS, SB */
#define ID_AA64MMFR0 0x0000000000100025ull /* 48-bit PA, 16-bit ASID, 4K/16K/64K granules */
#define ID_AA64MMFR1 0x0000000000111000ull /* HPDS, LOR, PAN */
#define ID_AA64DFR0  0x0000000000000006ull /* Armv8 debug, no PMU */
#define MIDR         0x611f0220ull         /* Apple; a configuration constant for now */
#define CTR_EL0      0x9444c004ull         /* 64-byte lines, PIPT I-cache, IDC */
#define CLIDR        0x0a200023ull         /* L1 split, L2 unified */
#define CNTFRQ       24000000ull

static uint64_t counter(void) {
    return mach_absolute_time(); /* 24 MHz on Apple silicon */
}

uint64_t hv_counter(void) {
    return counter();
}

static int undefined(HvCpu *cpu, uint64_t pc) {
    hv_raise_undef(cpu, pc);
    return 1;
}

/* Timer control: ISTATUS (bit 2) reflects the compare right now. */
static uint64_t timer_ctl(uint64_t ctl, uint64_t cval, uint64_t now) {
    ctl &= 3;
    if ((ctl & 1) && now >= cval) ctl |= 4;
    return ctl;
}

/* EL0 is allowed only a few registers, some gated by SCTLR_EL1 / CNTKCTL_EL1. */
static int el0_allowed(HvCpu *cpu, uint32_t reg, int write) {
    uint64_t k = cpu->cntkctl_el1;
    switch (reg) {
    case R(3, 3, 4, 2, 0): case R(3, 3, 4, 4, 0): case R(3, 3, 4, 4, 1): /* NZCV, FPCR, FPSR */
    case R(3, 3, 13, 0, 2):                                               /* TPIDR_EL0 */
    case R(3, 3, 4, 2, 5): case R(3, 3, 4, 2, 6):                         /* DIT, SSBS */
        return 1;
    case R(3, 3, 4, 2, 1): return (cpu->sctlr_el1 >> 9) & 1;            /* DAIF: UMA */
    case R(3, 3, 13, 0, 3): return !write;                               /* TPIDRRO_EL0 */
    case R(3, 3, 0, 0, 1): return !write && ((cpu->sctlr_el1 >> 15) & 1); /* CTR_EL0: UCT */
    case R(3, 3, 0, 0, 7): return !write;                                /* DCZID_EL0 */
    case R(3, 3, 14, 0, 0): return !write && (k & 3);                    /* CNTFRQ */
    case R(3, 3, 14, 0, 1): case R(3, 3, 14, 0, 5): return !write && (k & 1);       /* CNTPCT */
    case R(3, 3, 14, 0, 2): case R(3, 3, 14, 0, 6): return !write && ((k >> 1) & 1); /* CNTVCT */
    case R(3, 3, 14, 3, 0): case R(3, 3, 14, 3, 1): case R(3, 3, 14, 3, 2): return (k >> 8) & 1; /* CNTV_* */
    case R(3, 3, 14, 2, 0): case R(3, 3, 14, 2, 1): case R(3, 3, 14, 2, 2): return (k >> 9) & 1; /* CNTP_* */
    default: return 0;
    }
}

/* Returns 0 and sets *v, or -1 if the register does not exist. */
static int read_reg(HvCpu *cpu, uint32_t reg, uint64_t *v) {
    uint64_t now = counter();
    HvMachine *m = cpu->m;
    switch (reg) {
    case R(3, 3, 4, 2, 0): *v = cpu->nzcv & 0xf0000000; return 0;
    case R(3, 3, 4, 2, 1): *v = cpu->daif; return 0;
    case R(3, 0, 4, 2, 2): *v = (uint64_t)cpu->el << 2; return 0;      /* CurrentEL */
    case R(3, 0, 4, 2, 0): *v = cpu->spsel; return 0;
    case R(3, 0, 4, 2, 3): *v = cpu->pstate_extra & (1u << 22); return 0; /* PAN */
    case R(3, 0, 4, 2, 4): *v = cpu->pstate_extra & (1u << 23); return 0; /* UAO */
    case R(3, 3, 4, 2, 5): *v = cpu->pstate_extra & (1u << 24); return 0; /* DIT */
    case R(3, 3, 4, 2, 6): *v = cpu->pstate_extra & (1u << 12); return 0; /* SSBS */
    case R(3, 3, 4, 4, 0): *v = cpu->fpcr; return 0;
    case R(3, 3, 4, 4, 1): *v = cpu->fpsr; return 0;
    case R(3, 0, 4, 1, 0): *v = cpu->spsel ? cpu->sp_el0 : cpu->sp; return 0; /* SP_EL0 */
    case R(3, 0, 4, 0, 0): *v = cpu->spsr_el1; return 0;
    case R(3, 0, 4, 0, 1): *v = cpu->elr_el1; return 0;
    case R(3, 0, 1, 0, 0): *v = cpu->sctlr_el1; return 0;
    case R(3, 0, 1, 0, 1): *v = cpu->actlr_el1; return 0;
    case R(3, 0, 1, 0, 2): *v = cpu->cpacr_el1; return 0;
    case R(3, 0, 2, 0, 0): *v = cpu->ttbr0_el1; return 0;
    case R(3, 0, 2, 0, 1): *v = cpu->ttbr1_el1; return 0;
    case R(3, 0, 2, 0, 2): *v = cpu->tcr_el1; return 0;
    case R(3, 0, 5, 1, 0): *v = cpu->afsr0_el1; return 0;
    case R(3, 0, 5, 1, 1): *v = cpu->afsr1_el1; return 0;
    case R(3, 0, 5, 2, 0): *v = cpu->esr_el1; return 0;
    case R(3, 0, 6, 0, 0): *v = cpu->far_el1; return 0;
    case R(3, 0, 7, 4, 0): *v = cpu->par_el1; return 0;
    case R(3, 0, 10, 2, 0): *v = cpu->mair_el1; return 0;
    case R(3, 0, 10, 3, 0): *v = cpu->amair_el1; return 0;
    case R(3, 0, 12, 0, 0): *v = cpu->vbar_el1; return 0;
    case R(3, 0, 13, 0, 1): *v = cpu->contextidr_el1; return 0;
    case R(3, 0, 13, 0, 4): *v = cpu->tpidr_el1; return 0;
    case R(3, 3, 13, 0, 2): *v = cpu->tpidr_el0; return 0;
    case R(3, 3, 13, 0, 3): *v = cpu->tpidrro_el0; return 0;
    case R(3, 0, 14, 1, 0): *v = cpu->cntkctl_el1; return 0;
    case R(2, 0, 0, 2, 2): *v = cpu->mdscr_el1; return 0;
    case R(2, 0, 1, 1, 4): *v = 0x8; return 0;                           /* OSLSR_EL1: OS lock implemented, unlocked */
    case R(3, 2, 0, 0, 0): *v = cpu->csselr_el1; return 0;

    /* Identification */
    case R(3, 0, 0, 0, 0): *v = MIDR; return 0;
    case R(3, 0, 0, 0, 5): *v = cpu->mpidr; return 0;
    case R(3, 0, 0, 0, 6): *v = 0; return 0;                             /* REVIDR */
    case R(3, 0, 0, 4, 0): *v = ID_AA64PFR0; return 0;
    case R(3, 0, 0, 5, 0): *v = ID_AA64DFR0; return 0;
    case R(3, 0, 0, 6, 0): *v = ID_AA64ISAR0; return 0;
    case R(3, 0, 0, 6, 1): *v = ID_AA64ISAR1; return 0;
    case R(3, 0, 0, 7, 0): *v = ID_AA64MMFR0; return 0;
    case R(3, 0, 0, 7, 1): *v = ID_AA64MMFR1; return 0;
    case R(3, 3, 0, 0, 1): *v = CTR_EL0; return 0;
    case R(3, 3, 0, 0, 7): *v = 4; return 0;                             /* DCZID: 64-byte DC ZVA */
    case R(3, 1, 0, 0, 1): *v = CLIDR; return 0;
    case R(3, 1, 0, 0, 7): *v = 0; return 0;                             /* AIDR */
    case R(3, 1, 0, 0, 0):                                               /* CCSIDR for CSSELR's level */
        *v = (cpu->csselr_el1 >> 1) == 0 ? (127u << 13 | 7u << 3 | 2) : (2047u << 13 | 15u << 3 | 2);
        return 0;

    /* Generic timer */
    case R(3, 3, 14, 0, 0): *v = CNTFRQ; return 0;
    case R(3, 3, 14, 0, 1): case R(3, 3, 14, 0, 5): *v = now; return 0;
    case R(3, 3, 14, 0, 2): case R(3, 3, 14, 0, 6): *v = now - cpu->cntvoff; return 0;
    case R(3, 3, 14, 2, 0): *v = (uint32_t)(cpu->cntp_cval - now); return 0;
    case R(3, 3, 14, 2, 1): *v = timer_ctl(cpu->cntp_ctl, cpu->cntp_cval, now); return 0;
    case R(3, 3, 14, 2, 2): *v = cpu->cntp_cval; return 0;
    case R(3, 3, 14, 3, 0): *v = (uint32_t)(cpu->cntv_cval - (now - cpu->cntvoff)); return 0;
    case R(3, 3, 14, 3, 1): *v = timer_ctl(cpu->cntv_ctl, cpu->cntv_cval, now - cpu->cntvoff); return 0;
    case R(3, 3, 14, 3, 2): *v = cpu->cntv_cval; return 0;
    }
    /* GIC CPU interface (ICC_*_EL1): CRn 12, CRm 8..12. */
    if ((reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 12 && ((reg >> 3) & 15) >= 8) {
        *v = m->icc_read ? m->icc_read(m, cpu, reg) : 0;
        return 0;
    }
    /* The rest of the ID space reads as zero; so do debug and PMU registers. */
    if ((reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 0 && ((reg >> 3) & 15) >= 1) {
        *v = 0;
        return 0;
    }
    if ((reg >> 14) == 2 || (((reg >> 7) & 15) == 9 && (reg >> 14) == 3)) {
        *v = 0;
        return 0;
    }
    return -1;
}

static int write_reg(HvCpu *cpu, uint32_t reg, uint64_t v) {
    HvMachine *m = cpu->m;
    switch (reg) {
    case R(3, 3, 4, 2, 0): cpu->nzcv = v & 0xf0000000; return 0;
    case R(3, 3, 4, 2, 1): cpu->daif = v & HV_PSTATE_DAIF; return 0;
    case R(3, 0, 4, 2, 0): hv_sp_switch(cpu, cpu->el, v & 1); return 0;
    case R(3, 0, 4, 2, 3): hv_pstate_set(cpu, (hv_pstate_get(cpu) & ~(1ull << 22)) | (v & (1u << 22))); return 0;
    case R(3, 0, 4, 2, 4): cpu->pstate_extra = (cpu->pstate_extra & ~(1ull << 23)) | (v & (1u << 23)); return 0;
    case R(3, 3, 4, 2, 5): cpu->pstate_extra = (cpu->pstate_extra & ~(1ull << 24)) | (v & (1u << 24)); return 0;
    case R(3, 3, 4, 2, 6): cpu->pstate_extra = (cpu->pstate_extra & ~(1ull << 12)) | (v & (1u << 12)); return 0;
    case R(3, 3, 4, 4, 0): cpu->fpcr = v & 0x07ff9f00; return 0;
    case R(3, 3, 4, 4, 1): cpu->fpsr = v & 0xf800009f; return 0;
    case R(3, 0, 4, 1, 0):
        if (!cpu->spsel) return -1; /* SP_EL0 is not accessible as a sysreg while it is the SP */
        cpu->sp_el0 = v;
        return 0;
    case R(3, 0, 4, 0, 0): cpu->spsr_el1 = v; return 0;
    case R(3, 0, 4, 0, 1): cpu->elr_el1 = v; return 0;
    case R(3, 0, 1, 0, 0): cpu->sctlr_el1 = v; hv_tlb_flush(cpu); return 0;
    case R(3, 0, 1, 0, 1): cpu->actlr_el1 = v; return 0;
    case R(3, 0, 1, 0, 2): cpu->cpacr_el1 = v; return 0;
    case R(3, 0, 2, 0, 0): cpu->ttbr0_el1 = v; hv_tlb_flush(cpu); return 0;
    case R(3, 0, 2, 0, 1): cpu->ttbr1_el1 = v; hv_tlb_flush(cpu); return 0;
    case R(3, 0, 2, 0, 2): cpu->tcr_el1 = v; hv_tlb_flush(cpu); return 0;
    case R(3, 0, 5, 1, 0): cpu->afsr0_el1 = v; return 0;
    case R(3, 0, 5, 1, 1): cpu->afsr1_el1 = v; return 0;
    case R(3, 0, 5, 2, 0): cpu->esr_el1 = v; return 0;
    case R(3, 0, 6, 0, 0): cpu->far_el1 = v; return 0;
    case R(3, 0, 7, 4, 0): cpu->par_el1 = v; return 0;
    case R(3, 0, 10, 2, 0): cpu->mair_el1 = v; hv_tlb_flush(cpu); return 0;
    case R(3, 0, 10, 3, 0): cpu->amair_el1 = v; return 0;
    case R(3, 0, 12, 0, 0): cpu->vbar_el1 = v & ~0x7ffull; return 0;
    case R(3, 0, 13, 0, 1): cpu->contextidr_el1 = v; return 0;
    case R(3, 0, 13, 0, 4): cpu->tpidr_el1 = v; return 0;
    case R(3, 3, 13, 0, 2): cpu->tpidr_el0 = v; return 0;
    case R(3, 3, 13, 0, 3): cpu->tpidrro_el0 = v; return 0;
    case R(3, 0, 14, 1, 0): cpu->cntkctl_el1 = v; return 0;
    case R(2, 0, 0, 2, 2): cpu->mdscr_el1 = v; return 0;
    case R(3, 2, 0, 0, 0): cpu->csselr_el1 = v & 0xf; return 0;
    case R(3, 3, 14, 2, 0): cpu->cntp_cval = counter() + (uint64_t)(int64_t)(int32_t)v; return 0;
    case R(3, 3, 14, 2, 1): cpu->cntp_ctl = v & 3; return 0;
    case R(3, 3, 14, 2, 2): cpu->cntp_cval = v; return 0;
    case R(3, 3, 14, 3, 0): cpu->cntv_cval = counter() - cpu->cntvoff + (uint64_t)(int64_t)(int32_t)v; return 0;
    case R(3, 3, 14, 3, 1): cpu->cntv_ctl = v & 3; return 0;
    case R(3, 3, 14, 3, 2): cpu->cntv_cval = v; return 0;
    }
    /* PAuth key registers (CRn 2, CRm 1..3): kept, never used, since signing is a no-op. */
    if ((reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 2 && ((reg >> 3) & 15) >= 1 &&
        ((reg >> 3) & 15) <= 3)
        return 0;
    if ((reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 12 && ((reg >> 3) & 15) >= 8) {
        if (m->icc_write) m->icc_write(m, cpu, reg, v);
        return 0;
    }
    /* Debug and PMU registers: ignored. */
    if ((reg >> 14) == 2 || (((reg >> 7) & 15) == 9 && (reg >> 14) == 3)) return 0;
    return -1;
}

/* PAuth keys read back as zero. */
static int is_pauth_key(uint32_t reg) {
    return (reg >> 14) == 3 && ((reg >> 11) & 7) == 0 && ((reg >> 7) & 15) == 2 && ((reg >> 3) & 15) >= 1 &&
           ((reg >> 3) & 15) <= 3;
}

int hv_sysreg_mrs(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t reg = (insn >> 5) & 0xffff;
    int rt = insn & 0x1f;
    if (cpu->el == 0 && !el0_allowed(cpu, reg, 0)) return undefined(cpu, pc);
    uint64_t v;
    if (is_pauth_key(reg)) v = 0;
    else if (read_reg(cpu, reg, &v) != 0) return undefined(cpu, pc);
    if (rt != 31) cpu->x[rt] = v;
    return 0;
}

int hv_sysreg_msr(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    if (((insn >> 19) & 3) == 0) { /* MSR (immediate) to a PSTATE field */
        uint32_t op1 = (insn >> 16) & 7, op2 = (insn >> 5) & 7, imm = (insn >> 8) & 0xf;
        uint32_t field = op1 << 3 | op2;
        if (cpu->el == 0 && field != 036 && field != 037 && field != 031 && field != 032) return undefined(cpu, pc);
        if (cpu->el == 0 && (field == 036 || field == 037) && !((cpu->sctlr_el1 >> 9) & 1)) return undefined(cpu, pc);
        switch (field) {
        case 005: hv_sp_switch(cpu, cpu->el, imm & 1); break;                       /* SPSel */
        case 036: cpu->daif |= (uint64_t)imm << 6; break;                           /* DAIFSet */
        case 037: cpu->daif &= ~((uint64_t)imm << 6); break;                        /* DAIFClr */
        case 004: write_reg(cpu, R(3, 0, 4, 2, 3), (uint64_t)(imm & 1) << 22); break; /* PAN */
        case 003: write_reg(cpu, R(3, 0, 4, 2, 4), (uint64_t)(imm & 1) << 23); break; /* UAO */
        case 032: write_reg(cpu, R(3, 3, 4, 2, 5), (uint64_t)(imm & 1) << 24); break; /* DIT */
        case 031: write_reg(cpu, R(3, 3, 4, 2, 6), (uint64_t)(imm & 1) << 12); break; /* SSBS */
        default: return undefined(cpu, pc);
        }
        return 0;
    }
    uint32_t reg = (insn >> 5) & 0xffff;
    int rt = insn & 0x1f;
    if (cpu->el == 0 && !el0_allowed(cpu, reg, 1)) return undefined(cpu, pc);
    if (write_reg(cpu, reg, rt == 31 ? 0 : cpu->x[rt]) != 0) return undefined(cpu, pc);
    return 0;
}

int hv_sysreg_sys(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t op1 = (insn >> 16) & 7, crn = (insn >> 12) & 0xf, crm = (insn >> 8) & 0xf, op2 = (insn >> 5) & 7;
    int rt = insn & 0x1f;
    uint64_t xt = rt == 31 ? 0 : cpu->x[rt];
    if (crn == 7 && op1 == 3 && crm == 4 && op2 == 1) { /* DC ZVA */
        if (cpu->el == 0 && !((cpu->sctlr_el1 >> 14) & 1)) return undefined(cpu, pc);
        static const uint8_t zero[64];
        return hv_vmem_xfer(cpu, xt & ~63ull, (void *)zero, 64, 1, cpu->el, pc);
    }
    if (crn == 7 && (crm == 1 || crm == 5)) { /* IC IALLUIS, IALLU, IVAU: drop translations */
        cpu->m->flush_pending = 1;
        return 0;
    }
    if (crn == 7 && crm == 8 && op1 == 0 && op2 <= 3) { /* AT S1E1R, S1E1W, S1E0R, S1E0W */
        if (cpu->el == 0) return undefined(cpu, pc);
        HvWalk w;
        hv_mmu_walk(cpu, xt, (op2 & 1) ? HV_ACC_WRITE : HV_ACC_READ, op2 >= 2 ? 0 : 1, &w);
        if (w.fsc) cpu->par_el1 = 1 | (uint64_t)w.fsc << 1 | (uint64_t)w.s1ptw << 9 | 1ull << 11;
        else cpu->par_el1 = (w.pa & 0x0000fffffffff000ull) | (uint64_t)w.attr << 56 | 3u << 7;
        return 0;
    }
    if (crn == 8 || crn == 9) { /* TLBI: everything, for now */
        if (cpu->el == 0) return undefined(cpu, pc);
        hv_tlb_flush(cpu);
        return 0;
    }
    if (crn == 7) return 0; /* other cache maintenance: caches are coherent here */
    return undefined(cpu, pc);
}
