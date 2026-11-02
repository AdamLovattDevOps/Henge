#ifndef HENGEVM_MEMOP_H
#define HENGEVM_MEMOP_H

#include <stdint.h>

#include "cpu.h"

/* A decoded load/store, shared by the translator's fast path and the C slow path. */
typedef enum {
    HV_MEM_SINGLE,     /* LDR/STR and sign-extending loads, all addressing modes, GPR or SIMD&FP */
    HV_MEM_PAIR,       /* LDP/STP/LDNP/STNP/LDPSW */
    HV_MEM_LITERAL,    /* LDR (literal), LDRSW (literal) */
    HV_MEM_LDRA,       /* LDRAA/LDRAB: base + scaled imm10, optional pre-index writeback */
    HV_MEM_ORDERED,    /* LDAR/STLR/LDLAR/STLLR, and LDAPUR/STLUR (imm9) */
    HV_MEM_EXCL,       /* LDXR/STXR/LDAXR/STLXR and the pair forms */
    HV_MEM_CAS,        /* CAS, CASA, CASL, CASAL (all sizes) */
    HV_MEM_CASP,       /* CASP and its ordered forms */
    HV_MEM_ATOMIC,     /* LDADD..LDUMIN, SWP, LDAPR */
    HV_MEM_VEC_MULTI,  /* LD1-LD4 / ST1-ST4 (multiple structures) */
    HV_MEM_VEC_SINGLE, /* LD1-LD4 / ST1-ST4 (single structure) and LD1R-LD4R */
} HvMemKind;

typedef enum { HV_ADDR_BASE, HV_ADDR_OFFSET, HV_ADDR_PRE, HV_ADDR_POST, HV_ADDR_REG, HV_ADDR_LITERAL } HvAddrMode;

typedef struct HvMemOp {
    HvMemKind kind;
    HvAddrMode mode;
    uint8_t load, store;   /* both for atomics, CAS and SWP */
    uint8_t vec;           /* Rt (and Rt2) are SIMD&FP registers */
    uint8_t size;          /* log2 bytes of one register's access (0..4) */
    uint8_t sign;          /* 0: zero-extend, 32 or 64: sign-extend to that width */
    uint8_t unpriv;        /* LDTR/STTR: EL0 permissions */
    uint8_t align;         /* must be naturally aligned (atomics, exclusives, LDAR/STLR) */
    uint8_t rt, rt2, rn, rm, rs;
    uint8_t ext, shift;    /* register offset: option (UXTW 2, LSL 3, SXTW 6, SXTX 7) and shift */
    uint8_t atomic_op;     /* o3:opc for HV_MEM_ATOMIC */
    uint8_t rpt, selem;    /* vector structures */
    uint8_t vq;            /* vector: Q bit */
    uint8_t index;         /* single structure: element index; replicate if 0xff */
    int64_t imm;           /* byte offset, or post-index amount */
    uint32_t bytes;        /* total bytes touched */
} HvMemOp;

/* Returns 0, or -1 if the encoding is unallocated or a feature we do not expose. */
int hv_mem_decode(uint32_t insn, HvMemOp *op);

/* Emulates one load/store instruction completely in C: address, translation,
   the access (RAM or MMIO), and base writeback. This is the slow path for TLB
   misses and device accesses, and the only path for exclusives.
   Returns 1 if the block must leave (an abort was taken). */
int hv_mem_emulate(HvCpu *cpu, uint32_t insn, uint64_t pc);

/* Virtual-memory access from C (used by DC ZVA and tests). el selects the
   permissions. Returns 0, or 1 after taking a data abort. */
int hv_vmem_xfer(HvCpu *cpu, uint64_t va, void *buf, uint32_t len, int write, int el, uint64_t pc);

#endif
