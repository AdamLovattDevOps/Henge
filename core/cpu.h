#ifndef HENGEVM_CPU_H
#define HENGEVM_CPU_H

#include <stddef.h>
#include <stdint.h>

struct HvMachine;

/* One software TLB entry. A tag is the guest VA of a 16 KB page, or all ones
   when invalid (a page address never has its low bits set, so -1 never matches).
   host = guest VA + addend. */
typedef struct HvTlbEntry {
    uint64_t read_tag;
    uint64_t write_tag;
    uint64_t exec_tag;
    int64_t addend;
} HvTlbEntry;

#define HV_TLB_PAGE_BITS 14
#define HV_TLB_BITS 8
#define HV_TLB_SIZE (1u << HV_TLB_BITS)

/* Guest CPU state. While a translated block runs, host x28 points here and the
   guest's x0..x17, x19..x27, x29 and x30 live in the same host registers.
   Guest x18, x28 and SP stay in this struct: Darwin reserves host x18, and the
   translator owns host x28 and the host SP. */
typedef struct HvCpu {
    /* Fixed layout: translated code and host/enter.S address these by offset. */
    uint64_t x[31];          /* 0x000 */
    uint64_t sp;             /* 0x0f8: the guest SP for the current EL and SPSel */
    uint64_t pc;             /* 0x100 */
    uint64_t nzcv;           /* 0x108: N, Z, C, V in bits 31..28 */
    uint64_t exit_routine;   /* 0x110: host address blocks branch to when they leave */
    uint64_t spill[4];       /* 0x118: host scratch registers saved around a rewrite */
    uint32_t exit_reason;    /* 0x138 */
    uint32_t exit_insn;      /* 0x13c */
    __uint128_t v[32];       /* 0x140 */
    uint64_t fpcr;           /* 0x340 */
    uint64_t fpsr;           /* 0x348 */
    uint64_t helper_routine; /* 0x350: hv_helper_entry */
    uint64_t helper_ret;     /* 0x358: where hv_helper_entry returns into the block */
    uint64_t pad0[20];       /* keeps the TLB at 0x400 */
    HvTlbEntry tlb[2][HV_TLB_SIZE]; /* 0x400: [0] for EL0 and unprivileged accesses, [1] for EL1 */

    /* The rest is only touched by C. */
    struct HvMachine *m;
    uint32_t el;
    uint32_t spsel;
    uint64_t sp_el0;
    uint64_t sp_el1;
    uint64_t daif;           /* D, A, I, F in bits 9..6, as in SPSR */
    uint64_t pstate_extra;   /* PAN (bit 22), UAO (23), SSBS (12), DIT (24), TCO (25) */

    /* Exclusive monitor, emulated: STXR succeeds by compare-and-swap against the
       value LDXR saw, the way QEMU does it. */
    uint64_t excl_addr;      /* guest VA, or ~0 when clear */
    uint64_t excl_val[2];
    uint32_t excl_size;

    uint64_t sctlr_el1;
    uint64_t tcr_el1;
    uint64_t ttbr0_el1;
    uint64_t ttbr1_el1;
    uint64_t mair_el1;
    uint64_t amair_el1;
    uint64_t vbar_el1;
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t esr_el1;
    uint64_t far_el1;
    uint64_t par_el1;
    uint64_t afsr0_el1, afsr1_el1;
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    uint64_t tpidr_el1;
    uint64_t contextidr_el1;
    uint64_t cpacr_el1;
    uint64_t mdscr_el1;
    uint64_t csselr_el1;
    uint64_t cntkctl_el1;
    uint64_t actlr_el1;
    uint64_t sctlr_seen;     /* MMU/FP state the current TLB contents were filled under */

    /* Generic timer: physical and virtual. The counter is host time scaled to 24 MHz. */
    uint64_t cntv_ctl, cntv_cval;
    uint64_t cntp_ctl, cntp_cval;
    uint64_t cntvoff;

    uint64_t mpidr;
    uint64_t icount;         /* blocks entered, for tests and stats */
    int halted;              /* PSCI CPU_OFF / SYSTEM_OFF */
} HvCpu;

#define HV_CPU_X(n)        ((n) * 8)
#define HV_CPU_SP          0x0f8
#define HV_CPU_PC          0x100
#define HV_CPU_NZCV        0x108
#define HV_CPU_EXIT        0x110
#define HV_CPU_SPILL(n)    (0x118 + (n) * 8)
#define HV_CPU_REASON      0x138
#define HV_CPU_V           0x140
#define HV_CPU_FPCR        0x340
#define HV_CPU_FPSR        0x348
#define HV_CPU_HELPER      0x350
#define HV_CPU_HELPER_RET  0x358
#define HV_CPU_TLB         0x400
#define HV_CPU_TLB_EL      (HV_TLB_SIZE * 32)

_Static_assert(offsetof(HvCpu, sp) == HV_CPU_SP, "HvCpu layout");
_Static_assert(offsetof(HvCpu, pc) == HV_CPU_PC, "HvCpu layout");
_Static_assert(offsetof(HvCpu, nzcv) == HV_CPU_NZCV, "HvCpu layout");
_Static_assert(offsetof(HvCpu, exit_routine) == HV_CPU_EXIT, "HvCpu layout");
_Static_assert(offsetof(HvCpu, spill) == HV_CPU_SPILL(0), "HvCpu layout");
_Static_assert(offsetof(HvCpu, exit_reason) == HV_CPU_REASON, "HvCpu layout");
_Static_assert(offsetof(HvCpu, v) == HV_CPU_V, "HvCpu layout");
_Static_assert(offsetof(HvCpu, fpcr) == HV_CPU_FPCR, "HvCpu layout");
_Static_assert(offsetof(HvCpu, fpsr) == HV_CPU_FPSR, "HvCpu layout");
_Static_assert(offsetof(HvCpu, helper_routine) == HV_CPU_HELPER, "HvCpu layout");
_Static_assert(offsetof(HvCpu, helper_ret) == HV_CPU_HELPER_RET, "HvCpu layout");
_Static_assert(offsetof(HvCpu, tlb) == HV_CPU_TLB, "HvCpu layout");
_Static_assert(sizeof(HvTlbEntry) == 32, "TLB entries are indexed with lsl #5");

/* Why the last block returned to the dispatcher. */
enum {
    HV_EXIT_NONE = 0,
    HV_EXIT_BRANCH,      /* reached the end of a block; pc holds the next guest PC */
    HV_EXIT_SYSREG,      /* a system register write changed state blocks depend on */
    HV_EXIT_EXCEPTION,   /* an exception was taken; pc is the vector */
    HV_EXIT_WFI,
    HV_EXIT_UNDEF,
    HV_EXIT_UNSUPPORTED, /* the translator cannot handle this instruction yet */
    HV_EXIT_HALT,        /* PSCI off, or a test asked to stop */
    HV_EXIT_FLUSH,       /* translated code was invalidated (IC, self-modifying code) */
};

/* PSTATE bits as laid out in SPSR. */
#define HV_PSTATE_M_EL1H 0x5u
#define HV_PSTATE_DAIF   (0xfu << 6)
#define HV_PSTATE_I      (1u << 7)
#define HV_PSTATE_F      (1u << 6)
#define HV_PSTATE_PAN    (1u << 22)

#endif
