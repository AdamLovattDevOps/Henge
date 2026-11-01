#include "classify.h"

#include <stdio.h>
#include <string.h>

extern const uint32_t hv_classify_cases[];
extern const uint32_t hv_classify_cases_end[];

static const HvInsnKind expected[] = {
    HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY, HV_INSN_COPY,
    HV_INSN_PCREL, HV_INSN_PCREL,
    HV_INSN_BRANCH, HV_INSN_BRANCH, HV_INSN_BRANCH, HV_INSN_BRANCH, HV_INSN_BRANCH,
    HV_INSN_BRANCH_REG, HV_INSN_BRANCH_REG, HV_INSN_BRANCH_REG, HV_INSN_BRANCH_REG,
    HV_INSN_ERET,
    HV_INSN_LOAD_LITERAL, HV_INSN_LOAD_LITERAL,
    HV_INSN_MEMORY, HV_INSN_MEMORY, HV_INSN_MEMORY, HV_INSN_MEMORY, HV_INSN_MEMORY,
    HV_INSN_PREFETCH,
    HV_INSN_EXCEPTION, HV_INSN_EXCEPTION, HV_INSN_EXCEPTION,
    HV_INSN_SYSREG, HV_INSN_SYSREG, HV_INSN_SYSREG,
    HV_INSN_SYS, HV_INSN_SYS,
    HV_INSN_BARRIER, HV_INSN_BARRIER,
    HV_INSN_WAIT,
    HV_INSN_PAC, HV_INSN_PAC, HV_INSN_PAC, HV_INSN_PAC, HV_INSN_PAC,
    HV_INSN_UNDEFINED,
};

int main(void) {
    size_t count = (size_t)(hv_classify_cases_end - hv_classify_cases);
    size_t want = sizeof expected / sizeof expected[0];
    if (count != want) {
        printf("case count %zu does not match %zu expectations\n", count, want);
        return 1;
    }
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        HvInsnKind got = hv_classify(hv_classify_cases[i]);
        if (got != expected[i]) {
            printf("case %zu: %08x is %s, expected %s\n", i, hv_classify_cases[i],
                   hv_insn_kind_name(got), hv_insn_kind_name(expected[i]));
            failures++;
        }
    }
    printf("%zu cases, %d failures\n", count, failures);
    return failures != 0;
}
