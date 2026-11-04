#ifndef HENGEVM_MACHINE_H
#define HENGEVM_MACHINE_H

#include <pthread.h>
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
    uint32_t *page_gen;  /* per page: bumped to invalidate the blocks translated from it */
    /* Per page and CPU: the TLB slot (table << HV_TLB_BITS | index, plus 1) that
       last mapped the page writable, or HV_WHINT_MANY when it has had more than
       one. When the page becomes code only those are dropped, not a whole TLB scanned. */
    uint32_t *whint;
} HvRam;

typedef struct HvMmio {
    uint64_t pa, size;
    void *opaque;
    uint64_t (*read)(void *opaque, uint64_t off, unsigned size);
    void (*write)(void *opaque, uint64_t off, uint64_t val, unsigned size);
    const char *name;
    int self_locked;     /* the device takes its own lock (the GIC): no device lock around it */
} HvMmio;

typedef struct HvBlock {
    uint64_t va, pa;
    uint32_t flags;      /* HV_BLOCK_* state the translation depends on */
    uint32_t ninsns;
    uint32_t gen;        /* page_gen of its page when translated */
    struct HvRam *ram;   /* where its page lives, for a quick staleness check */
    uint64_t pg;
    uint32_t *code;
    struct HvBlock *next;
} HvBlock;

#define HV_BLOCK_EL1   1u        /* translated for EL1 (selects tlb[1]) */
#define HV_BLOCK_FPOFF 2u        /* CPACR_EL1.FPEN traps FP/SIMD at this EL */
#define HV_BLOCK_SPX   4u        /* EL1 with SPSel = 1 (SP_EL1 in use, SP_EL0 banked) */
#define HV_BLOCK_PAN   8u        /* EL1 with PSTATE.PAN set (selects TLB table 2) */

#define HV_MAX_RAM 8
#define HV_MAX_MMIO 48
#define HV_BLOCK_HASH (1 << 21) /* one bucket per block: chains are walked on every front-cache miss */
#define HV_JC_BITS 16
#define HV_MAX_BLOCKS (3 << 20) /* a macOS boot runs well over a million distinct blocks; a full flush retranslates them all */
#define HV_PROF_RING (1 << 16)
#define HV_MAX_SLOTS (6 << 20)  /* chainable exits: about two per block */

/* Counters for --stats: where the time goes. */
typedef struct HvStats {
    uint64_t blocks, translations, block_flushes;
    uint64_t flush_smc, flush_ic, flush_full;  /* why translations were dropped */
    uint64_t chained, ijc_fills, ijc_resets;
    uint64_t syncs, sync_ticks;        /* TLBI acks waited for, and host ticks spent waiting */
    uint64_t dev_waits, dev_wait_ticks; /* device lock taken while another CPU held it */
    uint64_t tr_waits, tr_wait_ticks;   /* translation lock likewise */
    uint64_t code_protects;   /* pages that became code (every CPU's TLB scanned for writable entries) */
    uint64_t tlb_miss_kind[4]; /* TLB misses: EL0 empty, EL0 other page there, EL1 empty, EL1 other page there */
    uint64_t slow_why[6];     /* slow accesses by cause: MMIO, code-page write, page end, TLB miss, other, TLB entry there but mismatched */
    uint64_t slow_kind[12];   /* hv_mem_emulate calls by HV_MEM_* kind */
    uint64_t fill_kind[4];    /* ijc fills into an empty slot, over another live salt, over the same salt, over a dead salt */
    uint64_t reset_why[8];    /* cache drops: full flush, TLBI by ASID, TLBI user VA, TLBI kernel VA, code change, user VA in every ASID (whole user half), keyed filter full, big user range */
    uint64_t why[6];          /* dispatcher entries by cause: chain fill, kick, exception/sysreg, interrupt, cached target, cache miss */
    uint64_t miss_kind[7];    /* cache misses by the branch that missed: RET, BR/BLR, B/BL, conditional, MSR, other, unknown */
    uint64_t recent[16];      /* debugging: the last blocks the dispatcher entered (pc | el) */
    struct { uint64_t from, to, spsr; uint32_t kind, el; } elchg[16]; /* debugging: last exception entries (kind = type) and ERETs (kind 9) */
    uint32_t nelchg;
    uint64_t nrecent;
    uint64_t translate_ticks, translated_insns, translated_words; /* time spent translating, guest and host code size */
    uint64_t retrans_va, retrans_vapa; /* translations of a VA (and VA+PA) translated before: diagnostics */
    uint64_t helper[12];      /* by HV_OP_* */
    uint64_t tlb_flushes, fetch_walks, exceptions, interrupts;
    uint64_t wfi, wfi_ticks;  /* WFI exits, host ticks spent asleep */
} HvStats;

#define HV_MAX_CPUS 8
#define HV_WHINT_MANY 0xffffffffu

/* What each CPU has to itself and keeps across hv_cpu_reset (tests copy HvCpu
   structs around, so it lives here, by CPU index): its indirect-branch cache and
   the filters for dropping entries from it, the front cache of blocks, its
   interrupt lines and its wakeup. */
typedef struct HvPerCpu {
    uint64_t *ijc;           /* indirect-branch cache tables (cpu->ijc points here) */
    uint64_t ijc_pages[2][1024]; /* hashed bitmap of the user pages any process cached targets in (for all-ASID TLBIs) */
    /* Hashed bitmap of (page, salt) pairs with cached targets: a TLBI for one
       address space drops its entries only if that address space cached
       something in the page. A new salt makes old bits meaningless, so this is
       only cleared (with the whole cache) when it gets an eighth full. */
    uint64_t ijc_keyed[1 << 14];
    uint32_t ijc_keyed_count;
    uint32_t ijc_tables_used; /* bit per flags value whose table was ever filled */
    /* Exactly which (user id, user page) pairs have cached targets: a hash set,
       so unmapping a big range of one address space drops only its code pages'
       entries instead of the whole address space's. Emptied with the user half. */
    uint64_t *ucode;
    uint32_t ucode_count;
    struct HvBlock *jc[1 << HV_JC_BITS]; /* direct-mapped cache in front of the hash */
    /* Interrupt lines into this CPU, driven by the GIC. */
    int irq_line, fiq_line;
    /* WFI sleeps on this; device threads, the GIC and other CPUs signal it. */
    pthread_mutex_t wake_mu;
    pthread_cond_t wake_cv;
    int wake_pending;
    /* Work other CPUs ask of this one (only the owner may touch its TLB and
       branch cache while it runs). Posting bumps req_seq; the owner does the
       work at its next dispatcher entry and publishes ack_seq. */
    pthread_mutex_t req_mu;
    uint32_t req_flags;          /* HV_REQ_* */
    uint32_t nreq;
    struct { uint32_t op1, crn, crm, op2; uint64_t xt; } req[64]; /* TLBIs to apply */
    uint64_t req_seq, ack_seq;
    /* active: inside the dispatcher loop (0 while parked: WFI, powered off, at a
       flush barrier). tlb_live: active and not itself waiting for acks, i.e. it
       may be using TLB entries right now. */
    int active, tlb_live;
    uint32_t walk_seq;           /* odd while it walks page tables and fills its TLB (see hv_tlbi_drop_remote_va) */
} HvPerCpu;

#define HV_REQ_TLB_ALL 1u   /* drop the whole TLB and branch cache */
#define HV_REQ_IJC_RESET 2u /* drop the branch cache (code changed) */

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
    HvStats stats;
    int chain;               /* block chaining and the indirect-branch cache are enabled */
    uint64_t *slots;         /* chainable exits' targets (0: not chained) */
    uint32_t **slot_site;    /* the B in each exit's stub that chaining rewrites to jump to the target */
    uint32_t nslots;
    HvCpu *cpu;              /* CPU 0: the one that polls devices */
    HvCpu *cpus[HV_MAX_CPUS]; /* every CPU, so device and invalidation code can ask them to leave */
    int ncpus;
    HvPerCpu percpu[HV_MAX_CPUS];
    /* With more than one CPU, only the holder of this runs guest code (a fair
       ticket lock, handed over at every kick and while a CPU sleeps). */
    int big_lock_on;
    /* Devices, the GIC and the ICC registers: one CPU at a time (recursive). */
    pthread_mutex_t dev_mu;
    /* Translating and inserting blocks, the JIT region and the slots. */
    pthread_mutex_t tr_mu;
    /* Stopping every CPU to flush all translations. */
    pthread_mutex_t stw_mu;
    pthread_cond_t stw_cv;
    int stw_active;
    pthread_mutex_t big_mu;
    pthread_cond_t big_cv;
    uint64_t big_next, big_serving;
    int stats_interval;  /* seconds between --stats lines, 0 = off */
    /* --profile: host PCs of the CPU thread sampled every millisecond, drained by the stats line. */
    uint64_t *prof;
    uint32_t prof_head, prof_tail;
} HvMachine;

int hv_machine_init(HvMachine *m, size_t jit_bytes);
/* The same with JIT memory the host provides (iOS: the region a debugger granted and its writable alias). */
int hv_machine_init_jit(HvMachine *m, void *rx, void *rw, size_t jit_bytes);
void hv_machine_free(HvMachine *m);
int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int readonly);
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name);

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa);
/* Drop the translations made from the RAM page holding pa (it was written, or the guest invalidated it). */
void hv_invalidate_code(HvMachine *m, uint64_t pa);
HvMmio *hv_mmio_lookup(HvMachine *m, uint64_t pa);
/* Host pointer for `len` bytes of guest RAM at pa, or NULL. */
void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len);

/* Physical accesses from C (page-table walks, devices doing DMA, slow paths). */
int hv_phys_read(HvMachine *m, uint64_t pa, void *buf, unsigned len);
int hv_phys_write(HvMachine *m, uint64_t pa, const void *buf, unsigned len);

void hv_cpu_reset(HvCpu *cpu, HvMachine *m); /* CPU 0 */
void hv_cpu_reset_index(HvCpu *cpu, HvMachine *m, uint32_t index);
/* The CPU this thread runs (NULL outside the dispatcher: tests, devices). */
extern __thread HvCpu *hv_self;
void hv_dev_lock(HvMachine *m);
void hv_dev_unlock(HvMachine *m);
/* Ask another CPU to apply a TLBI (or HV_REQ_* work); returns the sequence to wait for. */
uint64_t hv_cpu_post_tlbi(HvCpu *target, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint64_t xt);
uint64_t hv_cpu_post(HvCpu *target, uint32_t flags);
/* Wait until target has done everything up to seq (or is parked, and will before it runs). */
void hv_cpu_sync(HvCpu *self, HvCpu *target, uint64_t seq);
/* The owner: do what other CPUs asked. */
void hv_cpu_process_requests(HvCpu *cpu);
void hv_big_lock(HvMachine *m);
void hv_big_unlock(HvMachine *m);
/* Let a waiting CPU run (the lock is held again on return). */
void hv_big_yield(HvMachine *m);
/* Make every CPU leave its block at the next exit. */
void hv_machine_kick_all(HvMachine *m);
/* Wake one CPU out of WFI (and make it leave its block). */
void hv_cpu_wake(HvCpu *cpu);

/* From any thread: device work is waiting for the CPU thread's tick. Makes a
   running block leave and ends a WFI sleep. */
void hv_machine_wake(HvMachine *m);

#endif
