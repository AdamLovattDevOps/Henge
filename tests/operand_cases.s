// Pairs of words: an instruction, then the hv_gpr_fields mask it should give.
// RD=1 RN=2 RM=4 RA=8 RD_SP=16 RN_SP=32
    .text
    .p2align 2
    .globl _hv_operand_cases
    .globl _hv_operand_cases_end
_hv_operand_cases:
    add x0, sp, #16
    .word 1|2|16|32
    adds x0, sp, #16
    .word 1|2|32
    and sp, x1, #0xff
    .word 1|2|16
    ands x1, x2, #0xff
    .word 1|2
    movz x18, #1
    .word 1
    movk w28, #1, lsl #16
    .word 1
    ubfx x1, x18, #3, #4
    .word 1|2
    extr x1, x2, x3, #7
    .word 1|2|4
    add x1, x2, x3, lsl #2
    .word 1|2|4
    orr x1, x2, x28
    .word 1|2|4
    add x1, sp, w3, uxtw #2
    .word 1|2|4|16|32
    adds x1, sp, x3, uxtx
    .word 1|2|4|32
    ccmp x18, #3, #4, ne
    .word 2
    ccmp x18, x28, #4, ne
    .word 2|4
    csel x1, x2, x3, eq
    .word 1|2|4
    adc x1, x2, x3
    .word 1|2|4
    udiv x1, x2, x28
    .word 1|2|4
    clz x18, x1
    .word 1|2
    rev x1, x2
    .word 1|2
    madd x1, x2, x3, x28
    .word 1|2|4|8
    umulh x1, x2, x3
    .word 1|2|4|8
    fmov x18, d1
    .word 1
    fmov d1, x28
    .word 2
    scvtf d0, x18
    .word 2
    fcvtzs w18, s3
    .word 1
    fcvtzs x1, d2, #3
    .word 1
    ucvtf s1, w28, #5
    .word 2
    dup v0.4s, w18
    .word 2
    ins v0.s[1], w28
    .word 2
    umov w18, v1.s[2]
    .word 1
    smov x28, v1.h[2]
    .word 1
    fadd d0, d1, d2
    .word 0
    add v0.4s, v1.4s, v2.4s
    .word 0
    nop
    .word 0
    .arch armv8.4-a
    rmif x18, #36, #4
    .word 2
    setf8 w28
    .word 2
_hv_operand_cases_end:
