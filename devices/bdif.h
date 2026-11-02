#ifndef HENGEVM_BDIF_H
#define HENGEVM_BDIF_H

#include "disk.h"
#include "machine.h"

/* The vmapple "backdoor" block interface the boot ROM reads its disks
   through, as in QEMU's hw/vmapple/bdif.c. Reads only, like iBoot. */
typedef struct HvBdif {
    HvMachine *m;
    HvDisk *root, *aux;
} HvBdif;

HvBdif *hv_bdif_create(HvMachine *m, uint64_t pa, HvDisk *root, HvDisk *aux);

#endif
