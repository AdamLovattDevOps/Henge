#ifndef HENGEVM_TIMER_H
#define HENGEVM_TIMER_H

#include <stdint.h>

#include "cpu.h"
#include "gicv3.h"

/* The generic timer's outputs: virtual timer on PPI 27 (as vmapple wires it),
   physical timer on PPI 30. Level-triggered: asserted while enabled, unmasked
   and expired. */
#define HV_PPI_VTIMER 27
#define HV_PPI_PTIMER 30

/* Updates both lines; returns the counter value of the next expiry, or 0. */
uint64_t hv_timer_update(HvGic *g, HvCpu *cpu);

#endif
