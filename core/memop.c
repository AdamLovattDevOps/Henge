#include "memop.h"

int hv_mem_emulate(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    cpu->pc = pc;
    cpu->exit_insn = insn;
    cpu->exit_reason = HV_EXIT_UNSUPPORTED;
    return 1;
}
