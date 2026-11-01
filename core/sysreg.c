#include "sysreg.h"

static int unsupported(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    cpu->pc = pc;
    cpu->exit_insn = insn;
    cpu->exit_reason = HV_EXIT_UNSUPPORTED;
    return 1;
}

int hv_sysreg_mrs(HvCpu *cpu, uint32_t insn, uint64_t pc) { return unsupported(cpu, insn, pc); }
int hv_sysreg_msr(HvCpu *cpu, uint32_t insn, uint64_t pc) { return unsupported(cpu, insn, pc); }
int hv_sysreg_sys(HvCpu *cpu, uint32_t insn, uint64_t pc) { return unsupported(cpu, insn, pc); }
