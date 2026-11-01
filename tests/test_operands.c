#include "operands.h"

#include <stdio.h>

extern const uint32_t hv_operand_cases[];
extern const uint32_t hv_operand_cases_end[];

int main(void) {
    size_t count = (size_t)(hv_operand_cases_end - hv_operand_cases) / 2;
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = hv_operand_cases[i * 2], want = hv_operand_cases[i * 2 + 1];
        uint32_t got = hv_gpr_fields(insn);
        if (got != want) {
            printf("case %zu: %08x gives fields %02x, expected %02x\n", i, insn, got, want);
            failures++;
        }
    }
    printf("%zu cases, %d failures\n", count, failures);
    return failures != 0;
}
