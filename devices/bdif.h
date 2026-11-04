#ifndef HENGEVM_BDIF_H
#define HENGEVM_BDIF_H

#include "disk.h"
#include "machine.h"
#include "virtio_usb.h"

/* The vmapple "backdoor" block interface the boot ROM reads its disks
   through, as in QEMU's hw/vmapple/bdif.c. Reads only, like iBoot. */
/* Channel 0x100000 is the USB device port (device type 0x1a01) on installer
   boots: Apple's MMIO transport, two queues, the virtio-usb messages. A
   submission is a chain of up to 5 descriptors {u64 addr, u32 len, u16 1,
   u16 flags: bit 0 next, bit 1 device writes}, its address written to +0x408
   for the queue selected at +0x410; +0x10 reads 1 once that queue's last
   submission is done (and when nothing was submitted: the queue is idle). */
typedef struct HvBdifUsbQ {
    uint64_t pending;   /* a submitted chain not yet done (queue 0 waits for a request) */
    int done;
} HvBdifUsbQ;

typedef struct HvBdif {
    HvMachine *m;
    HvDisk *root, *aux;
    HvVirtioUsb *usb;   /* NULL: no USB channel */
    uint32_t usb_qsel, usb_irq;
    HvBdifUsbQ usb_q[2];
} HvBdif;

/* CPU thread (board tick): hand a queued USB request to the waiting buffer. */
void hv_bdif_poll(HvBdif *b);

HvBdif *hv_bdif_create(HvMachine *m, uint64_t pa, HvDisk *root, HvDisk *aux);

#endif
