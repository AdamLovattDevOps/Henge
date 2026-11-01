#ifndef HENGEVM_OPERANDS_H
#define HENGEVM_OPERANDS_H

#include <stdint.h>

/* Which register fields of a copied instruction name general-purpose registers.
   Rd is bits 4..0, Rn 9..5, Rm 20..16, Ra 14..10. */
enum {
    HV_FIELD_RD = 1u << 0,
    HV_FIELD_RN = 1u << 1,
    HV_FIELD_RM = 1u << 2,
    HV_FIELD_RA = 1u << 3,
    /* Register 31 in this field is SP rather than the zero register. */
    HV_FIELD_RD_SP = 1u << 4,
    HV_FIELD_RN_SP = 1u << 5,
};

/* For instructions hv_classify calls HV_INSN_COPY. */
uint32_t hv_gpr_fields(uint32_t insn);

static inline uint32_t hv_field_reg(uint32_t insn, int field) {
    switch (field) {
    case 0: return insn & 0x1f;
    case 1: return (insn >> 5) & 0x1f;
    case 2: return (insn >> 16) & 0x1f;
    default: return (insn >> 10) & 0x1f;
    }
}

static inline uint32_t hv_field_set(uint32_t insn, int field, uint32_t reg) {
    static const int shift[4] = {0, 5, 16, 10};
    return (insn & ~(0x1fu << shift[field])) | ((reg & 0x1f) << shift[field]);
}

#endif
