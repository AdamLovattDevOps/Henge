#ifndef HENGEVM_PL061_H
#define HENGEVM_PL061_H

#include <stdint.h>

#include "gicv3.h"

/* PL061 GPIO, as vmapple has it: inputs pulled low unless the host drives
   them; QEMU wires the power button to pin 3. */
typedef struct HvPl061 {
    HvGic *gic;
    int intid;
    uint8_t data, dir, isense, ibe, iev, im, istate, afsel;
    uint8_t input;      /* levels the host drives on input pins */
    uint8_t old_in;
} HvPl061;

HvPl061 *hv_pl061_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid);
void hv_pl061_set_input(HvPl061 *g, int pin, int level);

#endif
