#ifndef HENGEVM_CLASSIFY_H
#define HENGEVM_CLASSIFY_H

#include <stdint.h>

/* What the translator has to do with one A64 instruction. */
typedef enum {
    HV_INSN_COPY,        /* runs unchanged (ALU, SIMD/FP, most hints) */
    HV_INSN_PCREL,       /* ADR, ADRP: guest PC goes into the result */
    HV_INSN_BRANCH,      /* B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ: direct, target known */
    HV_INSN_BRANCH_REG,  /* BR, BLR, RET and their PAC forms: exit to the dispatcher */
    HV_INSN_ERET,        /* ERET and its PAC forms */
    HV_INSN_LOAD_LITERAL,/* LDR (literal), LDRSW (literal), PRFM (literal) */
    HV_INSN_MEMORY,      /* every other load, store, atomic, or exclusive */
    HV_INSN_PREFETCH,    /* PRFM / PRFUM: dropped */
    HV_INSN_EXCEPTION,   /* SVC, HVC, SMC, BRK, HLT, DCPS */
    HV_INSN_SYSREG,      /* MRS, MSR (register), MSR (immediate) to PSTATE */
    HV_INSN_SYS,         /* SYS/SYSL: DC, IC, TLBI, AT */
    HV_INSN_BARRIER,     /* DSB, DMB, ISB, CLREX, SB */
    HV_INSN_WAIT,        /* WFI, WFE, WFIT, WFET */
    HV_INSN_PAC,         /* PAC*, AUT*, XPAC*, PACGA: strip or no-op */
    HV_INSN_UNDEFINED,
} HvInsnKind;

HvInsnKind hv_classify(uint32_t insn);
const char *hv_insn_kind_name(HvInsnKind kind);

#endif
