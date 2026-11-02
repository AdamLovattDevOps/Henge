#ifndef HENGEVM_EXCEPTION_H
#define HENGEVM_EXCEPTION_H

#include <stdint.h>

#include "cpu.h"

/* Exception classes (ESR_ELx.EC). */
#define HV_EC_UNKNOWN     0x00
#define HV_EC_FP_ACCESS   0x07
#define HV_EC_ILLEGAL     0x0e
#define HV_EC_SVC64       0x15
#define HV_EC_HVC64       0x16
#define HV_EC_SMC64       0x17
#define HV_EC_SYSREG      0x18
#define HV_EC_IABT_LOWER  0x20
#define HV_EC_IABT_SAME   0x21
#define HV_EC_PC_ALIGN    0x22
#define HV_EC_DABT_LOWER  0x24
#define HV_EC_DABT_SAME   0x25
#define HV_EC_SP_ALIGN    0x26
#define HV_EC_BRK         0x3c

enum { HV_EXC_SYNC = 0, HV_EXC_IRQ = 1, HV_EXC_FIQ = 2, HV_EXC_SERROR = 3 };

/* PSTATE in SPSR layout, and setting it back (switches EL and SP bank). */
uint64_t hv_pstate_get(HvCpu *cpu);
void hv_pstate_set(HvCpu *cpu, uint64_t spsr);
/* Bank the current SP and select SP_EL0 or SP_ELx for (el, spsel). */
void hv_sp_switch(HvCpu *cpu, uint32_t el, uint32_t spsel);

/* Takes an exception to EL1. ret is the preferred return address (ELR_EL1). */
void hv_take_exception(HvCpu *cpu, int type, uint32_t esr, uint64_t far, int set_far, uint64_t ret);
void hv_raise_sync(HvCpu *cpu, uint32_t ec, uint32_t iss, uint64_t far, uint64_t pc);
void hv_raise_undef(HvCpu *cpu, uint64_t pc);
void hv_raise_abort(HvCpu *cpu, int insn, uint64_t va, uint32_t fsc, int wnr, int s1ptw, uint64_t pc);
void hv_eret(HvCpu *cpu);

/* SVC, HVC (PSCI), SMC, BRK, HLT at pc. Returns 1: the block always leaves. */
int hv_exception_insn(HvCpu *cpu, uint32_t insn, uint64_t pc);

/* Prints x0-x3 and the strings they point to (bring-up aid). */
void hv_debug_args(HvCpu *cpu);

/* Called by the dispatcher between blocks. Returns 1 if an interrupt was taken. */
int hv_check_interrupts(HvCpu *cpu);

#endif
