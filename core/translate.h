#ifndef HENGEVM_TRANSLATE_H
#define HENGEVM_TRANSLATE_H

#include <stdint.h>

#include "machine.h"

/* What a block passes to hv_helper. Lives in the block's literal pool. */
typedef struct HvHelperInfo {
    uint64_t pc;
    uint32_t insn;
    uint32_t op;
} HvHelperInfo;

enum {
    HV_OP_MEM = 1,     /* a load or store the fast path did not handle: emulate it whole */
    HV_OP_MRS,
    HV_OP_MSR,         /* MSR (register) or MSR (immediate); always leaves the block */
    HV_OP_SYS,         /* DC, IC, TLBI, AT */
    HV_OP_EXCEPTION,   /* SVC, HVC, SMC, BRK, HLT */
    HV_OP_ERET,
    HV_OP_UNDEF,
    HV_OP_FPTRAP,      /* FP/SIMD while CPACR_EL1.FPEN traps it */
    HV_OP_CLREX,
    HV_OP_WFI,
    HV_OP_WFE,         /* waits for the exclusive monitor's word to change (or a request); the block carries on */
};

#define HV_MAX_BLOCK_INSNS 128

/* Translates guest code starting at b->va (b->pa, b->flags filled in) into the
   machine's JIT region. `code` is the host copy of the guest instructions and
   `avail` how many may be read before the end of the guest page.
   Returns -1 if the JIT region is full. */
int hv_translate_block(HvMachine *m, HvBlock *b, const uint32_t *code, uint32_t avail);
/* The same in two steps, so CPUs translate in parallel: emit into this
   thread's buffer (no lock needed), then place it in the JIT region (under the
   translation lock). place returns -1 if the region is full. */
int hv_translate_emit(HvMachine *m, HvBlock *b, const uint32_t *code, uint32_t avail);
int hv_translate_place(HvMachine *m, HvBlock *b);
uint32_t hv_translate_last_words(void);

int hv_branch_retarget(uint32_t *w, int64_t off);

#endif
