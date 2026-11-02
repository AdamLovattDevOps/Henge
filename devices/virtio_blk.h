#ifndef HENGEVM_VIRTIO_BLK_H
#define HENGEVM_VIRTIO_BLK_H

#include "disk.h"
#include "virtio.h"

/* virtio-blk. The vmapple variant (QEMU's vmapple-virtio-blk-pci) uses Apple's
   PCI IDs and puts its role in the max_secure_erase_sectors config field. */
enum { HV_VBLK_PLAIN = 0, HV_VBLK_ROOT = 1, HV_VBLK_AUX = 2 };

typedef struct HvVirtioBlk {
    HvVirtio vio;
    HvDisk *disk;
    uint64_t requests, errors;
} HvVirtioBlk;

HvVirtioBlk *hv_virtio_blk_create(HvMachine *m, HvPcie *bus, HvDisk *disk, int apple_variant);

#endif
