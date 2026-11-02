#ifndef HENGEVM_MACHINE_H
#define HENGEVM_MACHINE_H

#include <stdint.h>

#include "cpu.h"
#include "jit_region.h"

/* Guest physical memory: RAM regions backed by host memory, and MMIO regions
   handled by device callbacks. The fast path only ever sees RAM. */
typedef struct HvRam {
    uint64_t pa, size;
    uint8_t *host;
    int readonly;
    uint8_t *code_pages; /* one byte per 16 KB page: a translated block came from here */
} HvRam;

typedef struct HvMmio {
    uint64_t pa, size;
    void *opaque;
    uint64_t (*read)(void *opaque, uint64_t off, unsigned size);
    void (*write)(void *opaque, uint64_t off, uint64_t val, unsigned size);
    const char *name;
} HvMmio;

typedef struct HvBlock {
    uint64_t va, pa;
    uint32_t flags;      /* HV_BLOCK_* state the translation depends on */
    uint32_t ninsns;
    uint32_t *code;
    struct HvBlock *next;
} HvBlock;

#define HV_BLOCK_EL1   1u        /* translated for EL1 (selects tlb[1]) */
#define HV_BLOCK_FPOFF 2u        /* CPACR_EL1.FPEN traps FP/SIMD at this EL */

#define HV_MAX_RAM 8
#define HV_MAX_MMIO 48
#define HV_BLOCK_HASH 4096
#define HV_MAX_BLOCKS 65536

typedef struct HvMachine {
    HvRam ram[HV_MAX_RAM];
    int nram;
    HvMmio mmio[HV_MAX_MMIO];
    int nmmio;

    HvJitRegion jit;
    HvBlock *hash[HV_BLOCK_HASH];
    HvBlock *blocks;
    int nblocks;
    int flush_pending;   /* set by helpers; the dispatcher flushes once the block has left */

    /* Interrupt lines into the CPU, driven by the GIC. */
    int irq_line, fiq_line;
    void *gic;
    /* Devices: called between blocks every so often and while the CPU waits.
       Returns the host time (mach_absolute_time units) of the next timer event, or 0. */
    uint64_t (*tick)(struct HvMachine *m, HvCpu *cpu);
    void *tick_opaque;
    /* GIC CPU interface system registers (ICC_*_EL1), reg = op0:op1:CRn:CRm:op2. */
    uint64_t (*icc_read)(struct HvMachine *m, HvCpu *cpu, uint32_t reg);
    void (*icc_write)(struct HvMachine *m, HvCpu *cpu, uint32_t reg, uint64_t v);

    uint64_t stop_pc;    /* tests: leave the dispatcher when pc reaches this */
    int trace;           /* print every block entered */
    int log;             /* 1: unknown sysregs, undefined instructions, unmapped accesses; 2: also every exception */
    int slow_mem;        /* tests: send every load and store through hv_mem_emulate */
    uint64_t stat_slow_mem; /* hv_mem_emulate calls */
    uint64_t access_pc;  /* guest PC of the load/store being emulated, for logs */
    uint64_t watch_va;   /* debugging: log every write that touches this guest VA (0: off) */
    uint64_t watch_pa;   /* debugging: the same by physical address */
} HvMachine;

int hv_machine_init(HvMachine *m, size_t jit_bytes);
void hv_machine_free(HvMachine *m);
int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int readonly);
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name);

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa);
HvMmio *hv_mmio_lookup(HvMachine *m, uint64_t pa);
/* Host pointer for `len` bytes of guest RAM at pa, or NULL. */
void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len);

/* Physical accesses from C (page-table walks, devices doing DMA, slow paths). */
int hv_phys_read(HvMachine *m, uint64_t pa, void *buf, unsigned len);
int hv_phys_write(HvMachine *m, uint64_t pa, const void *buf, unsigned len);

void hv_cpu_reset(HvCpu *cpu, HvMachine *m);

#endif
