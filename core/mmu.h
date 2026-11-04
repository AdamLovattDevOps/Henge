#ifndef HENGEVM_MMU_H
#define HENGEVM_MMU_H

#include <stdint.h>

#include "cpu.h"

struct HvRam;

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
void hv_tlb_switch_user(HvCpu *cpu, uint64_t old_ttbr0, uint64_t new_ttbr0); /* a process switch */
void hv_tlb_stash_drop(HvCpu *cpu, int asid); /* asid -1: all */
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
void hv_ijc_drop_va(HvCpu *cpu, uint64_t va, int asid);
/* Translated code's TLB miss: walk and fill without the full helper (see mmu.c). */
int hv_tlb_refill(HvCpu *cpu, uint64_t va, uint32_t flags);
struct HvBlock;
void hv_ijc_fill(HvCpu *cpu, struct HvBlock *b);
void *hv_ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags);
/* Another CPU's TLBI of one page: drop target's entries for it from here. 1 if
   that is enough (no page walk of target's raced it), 0 if target must do it
   itself and be waited for. Its branch cache is still target's own business. */
int hv_tlbi_drop_remote_va(HvCpu *target, uint64_t va, int asid); /* asid -1: all */       /* just the cached branch targets in the page */
void hv_ijc_drop_range(HvCpu *cpu, uint64_t va, uint64_t len, int asid);
void hv_ucode_add(HvCpu *cpu, uint64_t va); /* a user target was cached in this page (current address space) */
void hv_tlb_protect_code(HvCpu *cpu, const uint8_t *host_page);
void hv_tlb_protect_code_page(HvCpu *cpu, struct HvRam *ram, uint64_t pg); /* the same, through the page's write hints */

/* Translates for a C access of `len` bytes that stays inside one 16 KB page,
   filling the TLB when the page is ordinary RAM. Returns 0 and sets *pa, or the
   fault status code. */
uint32_t hv_mmu_access(HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t *pa);

/* For the dispatcher: host pointer to the guest instructions at va and how many
   can be read before the page ends. On a fault, the instruction abort is taken
   and NULL is returned. */
const uint32_t *hv_mmu_fetch(HvCpu *cpu, uint64_t va, uint64_t *pa, uint32_t *avail);

#endif
