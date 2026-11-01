#ifndef HENGEVM_MEMOP_H
#define HENGEVM_MEMOP_H

#include <stdint.h>

#include "cpu.h"

/* Emulates one load/store instruction completely in C: address, translation,
   the access (RAM or MMIO), and base writeback. This is the slow path for TLB
   misses and device accesses, and the only path for exclusives.
   Returns 1 if the block must leave (an abort was taken). */
int hv_mem_emulate(HvCpu *cpu, uint32_t insn, uint64_t pc);

#endif
