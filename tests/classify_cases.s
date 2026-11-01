// One instruction per case, in the same order as the expectations in test_classify.c.
// The assembler produces the encodings, so the test does not trust hand-written hex.
    .text
    .p2align 2
    .globl _hv_classify_cases
    .globl _hv_classify_cases_end
_hv_classify_cases:
    add x0, x1, x2              // copy
    madd x3, x4, x5, x6         // copy
    fadd d0, d1, d2             // copy
    movz x7, #0x1234            // copy
    nop                         // copy
    bti c                       // copy
    adr x0, .                   // pcrel
    adrp x1, _hv_classify_cases@PAGE // pcrel
    b .                         // branch
    bl .                        // branch
    b.ne .                      // branch
    cbz x2, .                   // branch
    tbnz w3, #5, .              // branch
    br x4                       // branch-reg
    blr x5                      // branch-reg
    ret                         // branch-reg
    retaa                       // branch-reg
    eret                        // eret
    ldr x6, .                   // load-literal
    ldrsw x7, .                 // load-literal
    ldr x8, [x9, #16]           // memory
    stp x10, x11, [sp, #-16]!   // memory
    ldxr x12, [x13]             // memory
    ldadd x14, x15, [x16]       // memory
    ld1 {v0.16b}, [x17]         // memory
    prfm pldl1keep, [x18]       // prefetch
    svc #0x80                   // exception
    hvc #0                      // exception
    brk #1                      // exception
    mrs x0, sctlr_el1           // sysreg
    msr ttbr0_el1, x1           // sysreg
    msr daifset, #2             // sysreg
    dc civac, x2                // sys
    tlbi vmalle1                // sys
    dsb sy                      // barrier
    isb                         // barrier
    wfi                         // wait
    paciasp                     // pac
    autiasp                     // pac
    pacia x0, x1                // pac
    xpaci x2                    // pac
    pacga x3, x4, x5            // pac
    udf #0                      // undefined
_hv_classify_cases_end:
