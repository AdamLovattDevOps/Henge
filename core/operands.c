#include "operands.h"

/* Follows the A64 encoding index. Only the copied groups are decoded here:
   data processing (immediate), data processing (register), and SIMD/FP. */

static uint32_t dp_immediate(uint32_t insn) {
    uint32_t op = (insn >> 23) & 7;
    switch (op) {
    case 2: { /* add/sub (immediate): Rd is SP unless flags are set; Rn is SP */
        uint32_t s = (insn >> 29) & 1;
        return HV_FIELD_RD | HV_FIELD_RN | HV_FIELD_RN_SP | (s ? 0 : HV_FIELD_RD_SP);
    }
    case 4: { /* logical (immediate): Rd is SP unless ANDS */
        uint32_t opc = (insn >> 29) & 3;
        return HV_FIELD_RD | HV_FIELD_RN | (opc == 3 ? 0 : HV_FIELD_RD_SP);
    }
    case 5: /* move wide */
        return HV_FIELD_RD;
    case 6: /* bitfield */
        return HV_FIELD_RD | HV_FIELD_RN;
    case 7: /* extract */
        return HV_FIELD_RD | HV_FIELD_RN | HV_FIELD_RM;
    default:
        return 0;
    }
}

static uint32_t dp_register(uint32_t insn) {
    uint32_t op1 = (insn >> 28) & 1;
    uint32_t op2 = (insn >> 21) & 0xf;
    if (op1 == 0) {
        if ((op2 & 0x9) == 0x9) { /* add/sub (extended register) */
            uint32_t s = (insn >> 29) & 1;
            return HV_FIELD_RD | HV_FIELD_RN | HV_FIELD_RM | HV_FIELD_RN_SP | (s ? 0 : HV_FIELD_RD_SP);
        }
        /* logical (shifted register), add/sub (shifted register) */
        return HV_FIELD_RD | HV_FIELD_RN | HV_FIELD_RM;
    }
    if (op2 == 0x2) { /* conditional compare: Rm is an immediate when bit 11 is set */
        return HV_FIELD_RN | (((insn >> 11) & 1) ? 0 : HV_FIELD_RM);
    }
    if (op2 == 0x6 && ((insn >> 30) & 1)) { /* data processing (1 source): Rm holds opcode2 */
        return HV_FIELD_RD | HV_FIELD_RN;
    }
    if (op2 & 0x8) { /* data processing (3 source) */
        return HV_FIELD_RD | HV_FIELD_RN | HV_FIELD_RM | HV_FIELD_RA;
    }
    if (op2 == 0x0 && ((insn >> 10) & 0x3f) != 0) {
        /* RMIF, SETF8, SETF16: only Rn is a register. RMIF keeps an immediate in the
           Rm bits and a flag mask in the Rd bits. */
        return HV_FIELD_RN;
    }
    /* add/sub with carry, flag manipulation, conditional select, 2 source */
    return HV_FIELD_RD | HV_FIELD_RN | HV_FIELD_RM;
}

static uint32_t simd_fp(uint32_t insn) {
    /* Conversion between floating-point and integer. */
    if ((insn & 0x5f20fc00) == 0x1e200000) {
        uint32_t opcode = (insn >> 16) & 7;
        if (opcode == 2 || opcode == 3 || opcode == 7) return HV_FIELD_RN; /* SCVTF, UCVTF, FMOV to FP */
        return HV_FIELD_RD;
    }
    /* Conversion between floating-point and fixed-point. */
    if ((insn & 0x5f200000) == 0x1e000000) {
        uint32_t opcode = (insn >> 16) & 7;
        if (opcode == 2 || opcode == 3) return HV_FIELD_RN;
        return HV_FIELD_RD;
    }
    /* Advanced SIMD copy: DUP (general), INS (general), SMOV, UMOV. */
    if ((insn & 0x9fe08400) == 0x0e000400) {
        uint32_t imm4 = (insn >> 11) & 0xf;
        uint32_t op = (insn >> 29) & 1;
        if (op == 0 && (imm4 == 1 || imm4 == 3)) return HV_FIELD_RN;
        if (op == 0 && (imm4 == 5 || imm4 == 7)) return HV_FIELD_RD;
        return 0;
    }
    return 0;
}

uint32_t hv_gpr_fields(uint32_t insn) {
    uint32_t op0 = (insn >> 25) & 0xf;
    if ((op0 & 0xe) == 0x8) return dp_immediate(insn);
    if ((op0 & 0x7) == 0x5) return dp_register(insn);
    if ((op0 & 0x7) == 0x7) return simd_fp(insn);
    return 0; /* hints: no registers */
}
