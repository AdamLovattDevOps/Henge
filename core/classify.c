#include "classify.h"

/* Decoding follows the A64 top-level encoding table in the Arm ARM:
   bits 28..25 pick the group, then each group is narrowed by mask and value. */

static HvInsnKind classify_hint(uint32_t insn) {
    /* HINT #imm: CRm in 11..8, op2 in 7..5. */
    uint32_t crm = (insn >> 8) & 0xf;
    uint32_t op2 = (insn >> 5) & 0x7;
    uint32_t imm = (crm << 3) | op2;
    switch (imm) {
    case 2:  /* WFE */
    case 3:  /* WFI */
        return HV_INSN_WAIT;
    case 7:  /* XPACLRI */
    case 8: case 10: case 12: case 14:   /* PACIA1716, PACIB1716, AUTIA1716, AUTIB1716 */
    case 24: case 25: case 26: case 27:  /* PACIAZ, PACIASP, PACIBZ, PACIBSP */
    case 28: case 29: case 30: case 31:  /* AUTIAZ, AUTIASP, AUTIBZ, AUTIBSP */
        return HV_INSN_PAC;
    default:
        /* NOP, YIELD, SEV, SEVL, ESB, PSB, CSDB, BTI and unallocated hints run as NOPs. */
        return HV_INSN_COPY;
    }
}

static HvInsnKind classify_system(uint32_t insn) {
    uint32_t l = (insn >> 21) & 1;
    uint32_t op0 = (insn >> 19) & 3;
    uint32_t op1 = (insn >> 16) & 7;
    uint32_t crn = (insn >> 12) & 0xf;

    if (op0 == 0) {
        if (l == 0 && crn == 2 && op1 == 3 && (insn & 0x1f) == 0x1f) return classify_hint(insn);
        if (l == 0 && crn == 3 && op1 == 3 && (insn & 0x1f) == 0x1f) {
            uint32_t op2 = (insn >> 5) & 7;
            /* CLREX, DSB, DMB, ISB, SB and the DSB nXS forms. */
            (void)op2;
            return HV_INSN_BARRIER;
        }
        if (l == 0 && crn == 4) return HV_INSN_SYSREG; /* MSR (immediate) to PSTATE */
        if (l == 0 && crn == 1 && op1 == 3 && (insn & 0x1f) == 0x1f) {
            /* WFET, WFIT (FEAT_WFxT) live in CRn=1. */
            return HV_INSN_WAIT;
        }
        return HV_INSN_UNDEFINED;
    }
    if (op0 == 1) return HV_INSN_SYS;     /* SYS, SYSL */
    return HV_INSN_SYSREG;                /* MRS, MSR (register) */
}

static HvInsnKind classify_branch_group(uint32_t insn) {
    if ((insn & 0x7c000000) == 0x14000000) return HV_INSN_BRANCH;   /* B, BL */
    if ((insn & 0x7e000000) == 0x34000000) return HV_INSN_BRANCH;   /* CBZ, CBNZ */
    if ((insn & 0x7e000000) == 0x36000000) return HV_INSN_BRANCH;   /* TBZ, TBNZ */
    if ((insn & 0xff000000) == 0x54000000) return HV_INSN_BRANCH;   /* B.cond, BC.cond */
    if ((insn & 0xff000000) == 0xd4000000) return HV_INSN_EXCEPTION;
    if ((insn & 0xffc00000) == 0xd5000000) return classify_system(insn);
    if ((insn & 0xfe000000) == 0xd6000000) {
        uint32_t opc = (insn >> 21) & 0xf;
        if (opc == 4) return HV_INSN_ERET;                          /* ERET, ERETAA, ERETAB */
        if (opc == 5) return HV_INSN_UNDEFINED;                     /* DRPS: not at EL0/EL1 */
        return HV_INSN_BRANCH_REG;                                  /* BR, BLR, RET and PAC forms */
    }
    return HV_INSN_UNDEFINED;
}

static HvInsnKind classify_memory(uint32_t insn) {
    /* Load register (literal): bits 29..27 = 011, bit 24 = 0. */
    if ((insn & 0x3b000000) == 0x18000000) {
        uint32_t opc = insn >> 30;
        uint32_t v = (insn >> 26) & 1;
        if (opc == 3 && v == 0) return HV_INSN_PREFETCH;            /* PRFM (literal) */
        return HV_INSN_LOAD_LITERAL;
    }
    /* PRFM (immediate) and PRFM (register), PRFUM: size=11, V=0, opc=10. */
    if ((insn & 0xffc00000) == 0xf9800000) return HV_INSN_PREFETCH;            /* PRFM (unsigned offset) */
    if ((insn & 0xffe00c00) == 0xf8a00800) return HV_INSN_PREFETCH;            /* PRFM (register) */
    if ((insn & 0xffe00c00) == 0xf8800000) return HV_INSN_PREFETCH;            /* PRFUM */
    return HV_INSN_MEMORY;
}

static HvInsnKind classify_dp_register(uint32_t insn) {
    /* Data-processing (1 source) with opcode2 = 00001: PAC*, AUT*, XPAC*. */
    if ((insn & 0xffff0000) == 0xdac10000) return HV_INSN_PAC;
    /* PACGA: Data-processing (2 source), opcode 001100. */
    if ((insn & 0xffe0fc00) == 0x9ac03000) return HV_INSN_PAC;
    return HV_INSN_COPY;
}

HvInsnKind hv_classify(uint32_t insn) {
    uint32_t op0 = (insn >> 25) & 0xf;
    /* 00xx: UDF and SME (0000), SVE (0010), unallocated. None are exposed to the guest. */
    if ((op0 & 0xc) == 0x0) return HV_INSN_UNDEFINED;
    if ((op0 & 0xe) == 0x8) {                   /* 100x: data processing, immediate */
        if ((insn & 0x1f000000) == 0x10000000) return HV_INSN_PCREL;
        return HV_INSN_COPY;
    }
    if ((op0 & 0xe) == 0xa) return classify_branch_group(insn); /* 101x */
    if ((op0 & 0x5) == 0x4) return classify_memory(insn);       /* x1x0 */
    if ((op0 & 0x7) == 0x5) return classify_dp_register(insn);  /* x101 */
    if ((op0 & 0x7) == 0x7) return HV_INSN_COPY;                /* x111: SIMD and FP */
    return HV_INSN_UNDEFINED;
}

const char *hv_insn_kind_name(HvInsnKind kind) {
    switch (kind) {
    case HV_INSN_COPY: return "copy";
    case HV_INSN_PCREL: return "pcrel";
    case HV_INSN_BRANCH: return "branch";
    case HV_INSN_BRANCH_REG: return "branch-reg";
    case HV_INSN_ERET: return "eret";
    case HV_INSN_LOAD_LITERAL: return "load-literal";
    case HV_INSN_MEMORY: return "memory";
    case HV_INSN_PREFETCH: return "prefetch";
    case HV_INSN_EXCEPTION: return "exception";
    case HV_INSN_SYSREG: return "sysreg";
    case HV_INSN_SYS: return "sys";
    case HV_INSN_BARRIER: return "barrier";
    case HV_INSN_WAIT: return "wait";
    case HV_INSN_PAC: return "pac";
    case HV_INSN_UNDEFINED: return "undefined";
    }
    return "?";
}
