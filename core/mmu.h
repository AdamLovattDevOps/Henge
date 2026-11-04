#ifndef HENGEVM_MMU_H
#define HENGEVM_MMU_H

#include <stdint.h>

#include "cpu.h"

typedef enum { HV_ACC_READ = 0, HV_ACC_WRITE = 1, HV_ACC_EXEC = 2 } HvAccess;

/* Result of a stage 1 translation for the EL1&0 regime. */
typedef struct HvWalk {
    uint64_t pa;
    uint32_t fsc;        /* 0 on success, otherwise the abort's fault status code */
    int s1ptw;           /* the fault happened reading a table */
    uint64_t block_size; /* size of the mapping that was found */
    uint8_t r, w, x;     /* permissions at the EL that was asked about */
    uint8_t attr;        /* MAIR attribute byte */
} HvWalk;

/* Fault status codes (ESR ISS DFSC/IFSC). */
#define HV_FSC_ADDR_SIZE(l)   (0x00 + (l))
#define HV_FSC_TRANSLATION(l) (0x04 + (l))
#define HV_FSC_ACCESS(l)      (0x08 + (l))
#define HV_FSC_PERMISSION(l)  (0x0c + (l))
#define HV_FSC_EXTERNAL       0x10
#define HV_FSC_ALIGNMENT      0x21

/* el is the EL whose permissions apply (0 for LDTR/STTR at EL1). */
void hv_mmu_walk(HvCpu *cpu, uint64_t va, HvAccess acc, int el, HvWalk *out);

void hv_tlb_flush(HvCpu *cpu);
void hv_tlb_flush_va(HvCpu *cpu, uint64_t va);
void hv_tlb_flush_user(HvCpu *cpu);
/* Empty the indirect-branch cache for all user (0) or for kernel (1) VAs. */
void hv_ijc_reset(HvCpu *cpu, int kernel);
/* The indirect-branch cache slot of a tag (VA ^ salt); translated code computes the same. */
static inline uint32_t hv_ijc_index(uint64_t tag) {
    return (uint32_t)(((tag ^ tag >> 16 ^ tag >> 46) >> 2) & ((1u << HV_IJC_BITS) - 1));
}
static inline uint32_t hv_ijc_key(uint64_t va, uint64_t salt) {
    return (uint32_t)(((va >> 14) * 0x9e3779b97f4a7c15ull ^ (salt >> 48) * 0xc2b2ae3d27d4eb4full) >> 44);
}
/* Forget one address space's cached user targets. */
void hv_ijc_drop_asid(HvCpu *cpu, uint16_t asid);
/* TTBR0 changed: use the new address space's user entries. */
void hv_ijc_switch(HvCpu *cpu);
void hv_tlb_flush_va_asid(HvCpu *cpu, uint64_t va, int asid); /* asid -1: every address space */
void hv_ijc_drop_va(HvCpu *cpu, uint64_t va, int asid);       /* just the cached branch targets in the page */
void hv_tlb_protect_code(HvCpu *cpu, const uint8_t *host_page);

/* Translates for a C access of `len` bytes that stays inside one 16 KB page,
   filling the TLB when the page is ordinary RAM. Returns 0 and sets *pa, or the
   fault status code. */
uint32_t hv_mmu_access(HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t *pa);

/* For the dispatcher: host pointer to the guest instructions at va and how many
   can be read before the page ends. On a fault, the instruction abort is taken
   and NULL is returned. */
const uint32_t *hv_mmu_fetch(HvCpu *cpu, uint64_t va, uint64_t *pa, uint32_t *avail);

#endif
