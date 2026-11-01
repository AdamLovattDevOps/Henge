#ifndef HENGEVM_SYSREG_H
#define HENGEVM_SYSREG_H

#include <stdint.h>

#include "cpu.h"

/* MRS, MSR (register and immediate), and SYS (DC, IC, TLBI, AT).
   Each returns 1 if the block must leave. */
int hv_sysreg_mrs(HvCpu *cpu, uint32_t insn, uint64_t pc);
int hv_sysreg_msr(HvCpu *cpu, uint32_t insn, uint64_t pc);
int hv_sysreg_sys(HvCpu *cpu, uint32_t insn, uint64_t pc);

#endif
