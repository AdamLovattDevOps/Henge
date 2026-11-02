// Exceptions and system registers, run through the JIT only (they need EL1).
// The vector table records ESR, FAR, ELR, SPSR in x20..x23, counts exceptions
// in x24 and puts the vector offset in x25. Synchronous faults resume at
// ELR+4 (SVC already points past itself), an FP trap enables FP and retries,
// and an IRQ is acknowledged by a store to the test device at x19.
#include "harness.h"

    .arch armv8.4-a
    .text
    .p2align 14
    .globl _exc_programs_begin
    .globl _exc_programs_finish
_exc_programs_begin:

.macro VECTOR off
    .p2align 7
    mov x25, #\off
    b exc_common
.endm

    .p2align 11
vectors:
    VECTOR 0x000
    VECTOR 0x080
    VECTOR 0x100
    VECTOR 0x180
    VECTOR 0x200
    VECTOR 0x280
    VECTOR 0x300
    VECTOR 0x380
    VECTOR 0x400
    VECTOR 0x480
    VECTOR 0x500
    VECTOR 0x580
    VECTOR 0x600
    VECTOR 0x680
    VECTOR 0x700
    VECTOR 0x780

exc_common:
    mrs x20, esr_el1
    mrs x21, far_el1
    mrs x22, elr_el1
    mrs x23, spsr_el1
    add x24, x24, #1
    lsr x26, x20, #26
    cmp x25, #0x280
    b.eq 3f
    cmp x25, #0x480
    b.eq 3f
    cmp x26, #0x15
    b.eq 2f
    cmp x26, #0x07
    b.ne 1f
    mov x26, #(3 << 20)
    msr cpacr_el1, x26
    eret
1:  add x26, x22, #4
    msr elr_el1, x26
2:  eret
3:  str wzr, [x19]
    eret

.macro SETUP
    mov x24, #0
    adr x26, vectors
    msr vbar_el1, x26
.endm

PROGRAM p_svc
    SETUP
    mov x5, #0
    cmp x5, #1             // N set, C clear
    svc #0x42
1:  mov x1, x20
    mov x2, x22
    mov x3, x23
    mrs x4, nzcv
    adr x6, 1b
    mov x7, x25
END p_svc

PROGRAM p_brk_undef
    SETUP
1:  brk #7
    mov x1, x20
    mov x2, x22
    adr x3, 1b
2:  .word 0             // udf #0
    mov x4, x20
    mov x5, x22
    adr x6, 2b
END p_brk_undef

// External aborts: nothing is mapped at PA 0x10000. The faulting load's
// post-index must not happen.
PROGRAM p_dabt
    SETUP
    mov x8, #0x10000
1:  ldr x7, [x8], #8
    mov x1, x20
    mov x2, x21
    mov x3, x22
    adr x4, 1b
    str x9, [x8, #16]
    mov x5, x20
    mov x6, x21
    mov x10, x8
END p_dabt

// Down to EL0 and back up through SVC and an undefined MRS.
PROGRAM p_el0
    SETUP
    msr sp_el0, x9
    adr x0, 1f
    msr elr_el1, x0
    msr spsr_el1, xzr       // EL0t, interrupts unmasked
    eret
1:  mov x10, sp
    svc #1
    mov x13, x25
    mrs x12, sctlr_el1
    mov x14, x20
    mov x15, x25
    mrs x16, tpidr_el0
    mov x17, #0x55
    msr tpidr_el0, x17
    mrs x18, tpidr_el0
END p_el0

PROGRAM p_sysregs
    SETUP
    msr tpidr_el1, x1
    mrs x2, tpidr_el1
    mrs x3, CurrentEL
    msr daifset, #0xf
    mrs x4, daif
    msr daifclr, #0x3
    mrs x5, daif
    mov x6, #0x60000000
    msr nzcv, x6
    mrs x7, nzcv
    mrs x8, cntvct_el0
    mov x0, #2000
1:  subs x0, x0, #1
    b.ne 1b
    mrs x9, cntvct_el0
    mrs x10, cntfrq_el0
    mrs x11, id_aa64mmfr0_el1
    msr spsel, #0
    mov x12, sp
    msr spsel, #1
    mov x13, sp
    mrs x14, midr_el1
    mrs x15, ctr_el0
    mov x16, #(1 << 24)
    msr fpcr, x16
    mrs x17, fpcr
    msr fpcr, xzr
    mrs x18, vbar_el1
    mov x19, #0x123
    msr cntv_cval_el0, x19
    mrs x21, cntv_cval_el0
    mov x22, #1
    msr cntv_ctl_el0, x22
    mrs x23, cntv_ctl_el0      // enabled and already expired: ISTATUS set
END p_sysregs

// The test raises the IRQ line before running; it is masked until DAIFClr.
PROGRAM p_irq
    SETUP
    wfi                     // a pending interrupt wakes WFI even while masked
    mov x1, x24             // still 0
    msr daifclr, #2
1:  mov x2, x24
    mov x3, x25
    mov x4, x22
    adr x5, 1b
END p_irq

PROGRAM p_fptrap
    SETUP
    msr cpacr_el1, xzr
    fadd d0, d1, d2
    mov x1, x20
    mov x2, x24
    fmov x3, d0
END p_fptrap

PROGRAM p_psci
    SETUP
    movz x0, #0x8400, lsl #16
    hvc #0
    mov x3, x0
    movz x0, #0x8400, lsl #16
    movk x0, #0x000a
    movz x1, #0xc400, lsl #16
    movk x1, #0x0003
    hvc #0
    mov x2, x0
END p_psci

// Copied into writable RAM by the test: rewrites its own subroutine.
PROGRAM p_smc
    mov x1, #0
    mov x4, #0
1:  bl 3f
    add x1, x1, x0
    adr x2, 3f
    ldr w3, 4f
    str w3, [x2]
    dc cvau, x2
    dsb ish
    ic ivau, x2
    dsb ish
    isb
    add x4, x4, #1
    cmp x4, #2
    b.lt 1b
    b 5f
3:  mov x0, #1
    ret
4:  mov x0, #2
5:
END p_smc

// The same without any cache maintenance: the write itself must be noticed.
PROGRAM p_smc_nomaint
    mov x1, #0
    mov x4, #0
1:  bl 3f
    add x1, x1, x0
    adr x2, 3f
    ldr w3, 4f
    str w3, [x2]
    add x4, x4, #1
    cmp x4, #2
    b.lt 1b
    b 5f
3:  mov x0, #1
    ret
4:  mov x0, #2
5:
END p_smc_nomaint

    .p2align 14
_exc_programs_finish:
