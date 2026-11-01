// Entering and leaving translated code.
//
// hv_enter(HvCpu *cpu, void *block) saves the host's callee-saved registers,
// loads the guest registers, and jumps to the block. Every block starts with
// `ldr x0, [x28]`, because x0 carries the block address until then.
// The block leaves through cpu->exit_routine, which is hv_exit below: by then
// the block has stored guest x0, x1 and pc into the HvCpu.
//
// hv_helper_entry is how a block calls C without leaving: the block stores
// guest x0, x1 and x30, puts a HvHelperInfo pointer in x0 and does `blr` to it.
// Everything else is saved here, hv_helper(cpu, info) runs on the host stack,
// and either every guest register is reloaded and the block resumes (it then
// reloads x0 itself), or the helper asked to leave and we go out through hv_exit.

#define CPU_NZCV 0x108
#define CPU_V 0x140
#define CPU_FPCR 0x340
#define CPU_FPSR 0x348
#define CPU_HELPER_RET 0x358

    .text
    .p2align 2
    .globl _hv_enter
    .globl _hv_exit
    .globl _hv_helper_entry

.macro SAVE_X2_X29
    stp x2, x3, [x28, #16]
    stp x4, x5, [x28, #32]
    stp x6, x7, [x28, #48]
    stp x8, x9, [x28, #64]
    stp x10, x11, [x28, #80]
    stp x12, x13, [x28, #96]
    stp x14, x15, [x28, #112]
    stp x16, x17, [x28, #128]
    stp x19, x20, [x28, #152]
    stp x21, x22, [x28, #168]
    stp x23, x24, [x28, #184]
    stp x25, x26, [x28, #200]
    str x27, [x28, #216]
    str x29, [x28, #232]
.endm

// Uses x0 as a temporary, so x0 must already be saved.
.macro SAVE_FLAGS_FP
    mrs x0, nzcv
    str x0, [x28, #CPU_NZCV]
    mrs x0, fpcr
    str x0, [x28, #CPU_FPCR]
    mrs x0, fpsr
    str x0, [x28, #CPU_FPSR]
    stp q0, q1, [x28, #320]
    stp q2, q3, [x28, #352]
    stp q4, q5, [x28, #384]
    stp q6, q7, [x28, #416]
    stp q8, q9, [x28, #448]
    stp q10, q11, [x28, #480]
    stp q12, q13, [x28, #512]
    stp q14, q15, [x28, #544]
    stp q16, q17, [x28, #576]
    stp q18, q19, [x28, #608]
    stp q20, q21, [x28, #640]
    stp q22, q23, [x28, #672]
    stp q24, q25, [x28, #704]
    stp q26, q27, [x28, #736]
    stp q28, q29, [x28, #768]
    stp q30, q31, [x28, #800]
.endm

// Uses x0 and x1 as temporaries. FPCR writes are slow, so skip unchanged ones.
.macro LOAD_FLAGS_FP
    ldp q0, q1, [x28, #320]
    ldp q2, q3, [x28, #352]
    ldp q4, q5, [x28, #384]
    ldp q6, q7, [x28, #416]
    ldp q8, q9, [x28, #448]
    ldp q10, q11, [x28, #480]
    ldp q12, q13, [x28, #512]
    ldp q14, q15, [x28, #544]
    ldp q16, q17, [x28, #576]
    ldp q18, q19, [x28, #608]
    ldp q20, q21, [x28, #640]
    ldp q22, q23, [x28, #672]
    ldp q24, q25, [x28, #704]
    ldp q26, q27, [x28, #736]
    ldp q28, q29, [x28, #768]
    ldp q30, q31, [x28, #800]
    ldr x0, [x28, #CPU_FPCR]
    mrs x1, fpcr
    cmp x0, x1
    b.eq 1f
    msr fpcr, x0
1:
    ldr x0, [x28, #CPU_FPSR]
    msr fpsr, x0
    ldr x0, [x28, #CPU_NZCV]
    msr nzcv, x0
.endm

.macro LOAD_X1_X30
    ldr x1, [x28, #8]
    ldp x2, x3, [x28, #16]
    ldp x4, x5, [x28, #32]
    ldp x6, x7, [x28, #48]
    ldp x8, x9, [x28, #64]
    ldp x10, x11, [x28, #80]
    ldp x12, x13, [x28, #96]
    ldp x14, x15, [x28, #112]
    ldp x16, x17, [x28, #128]
    ldp x19, x20, [x28, #152]
    ldp x21, x22, [x28, #168]
    ldp x23, x24, [x28, #184]
    ldp x25, x26, [x28, #200]
    ldr x27, [x28, #216]
    ldp x29, x30, [x28, #232]
.endm

_hv_enter:
    stp x29, x30, [sp, #-176]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    str x1, [sp, #160]

    mov x28, x0
    LOAD_FLAGS_FP
    LOAD_X1_X30
    ldr x0, [sp, #160]
    br x0

_hv_exit:
    // x0, x1 and pc are already in the HvCpu.
    SAVE_X2_X29
    str x30, [x28, #240]
    SAVE_FLAGS_FP
Lrestore_host:
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8, d9, [sp, #96]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #176
    ret

_hv_helper_entry:
    // Guest x0, x1 and x30 are saved; x0 = HvHelperInfo *, x30 = resume address.
    str x30, [x28, #CPU_HELPER_RET]
    mov x1, x0
    SAVE_X2_X29
    SAVE_FLAGS_FP
    mov x0, x28
    bl _hv_helper
    cbnz w0, Lrestore_host
    LOAD_FLAGS_FP
    LOAD_X1_X30
    ldr x0, [x28, #CPU_HELPER_RET]
    br x0
