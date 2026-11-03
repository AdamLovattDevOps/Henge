#ifndef HENGEVM_VIRTIO_BLK_H
#define HENGEVM_VIRTIO_BLK_H

#include "disk.h"
#include "virtio.h"

#include <pthread.h>

/* virtio-blk. The vmapple variant (QEMU's vmapple-virtio-blk-pci) uses Apple's
   PCI IDs and puts its role in the max_secure_erase_sectors config field. */
enum { HV_VBLK_PLAIN = 0, HV_VBLK_ROOT = 1, HV_VBLK_AUX = 2 };

/* Requests run on a worker thread, like a real controller doing DMA, so the
   CPU thread never waits on the host disk. The CPU thread takes requests off
   the virtqueue (notify) and publishes completions and the interrupt
   (hv_virtio_blk_poll, from the board's tick); the worker only touches the
   disk, guest buffers and these indices. */
#define HV_VBLK_RING 256

typedef struct HvVblkReq {
    HvVirtqElem e;
    int q;
    uint32_t written;
} HvVblkReq;

typedef struct HvVirtioBlk {
    HvVirtio vio;
    HvDisk *disk;
    uint64_t requests, errors;
    HvVblkReq *ring;
    uint32_t submitted;  /* CPU thread: requests handed to the worker */
    uint32_t completed;  /* worker: requests done */
    uint32_t published;  /* CPU thread: completions pushed to the used ring */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t worker;
} HvVirtioBlk;

HvVirtioBlk *hv_virtio_blk_create(HvMachine *m, HvPcie *bus, HvDisk *disk, int apple_variant);
/* CPU thread: publish finished requests and raise the interrupt. */
void hv_virtio_blk_poll(HvVirtioBlk *b);
/* Tests: wait for the worker to finish everything submitted, then publish it. */
void hv_virtio_blk_drain(HvVirtioBlk *b);

#endif
