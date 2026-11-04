#ifndef HENGEVM_VIRTIO_H
#define HENGEVM_VIRTIO_H

#include <stdint.h>

#include "pcie.h"

/* virtio 1.x over PCI (modern only, as vmapple sets disable-legacy), split
   virtqueues, INTx interrupts. Requests are handled synchronously when the
   driver notifies. */
#define HV_VIRTQ_MAX 4
#define HV_VIRTQ_MAX_SIZE 1024
#define HV_VIRTQ_MAX_SEGS 128

#define HV_VIRTIO_F_VERSION_1 (1ull << 32)

typedef struct HvVirtq {
    uint16_t size, enabled, last_avail;
    uint64_t desc, avail, used;
    uint8_t out[HV_VIRTQ_MAX_SIZE]; /* heads taken and not yet completed (a completion twice corrupts the driver) */
} HvVirtq;

typedef struct HvVirtqSeg {
    uint64_t addr;
    uint32_t len;
    int write;     /* device writes (driver reads) */
} HvVirtqSeg;

typedef struct HvVirtqElem {
    uint16_t head;
    int nseg;
    HvVirtqSeg seg[HV_VIRTQ_MAX_SEGS];
} HvVirtqElem;

typedef struct HvVirtio {
    HvPciDev pci;
    HvMachine *m;
    uint64_t features, driver_features;
    uint32_t dfsel, gfsel;
    uint8_t status, isr, config_gen;
    uint16_t qsel, nq;
    HvVirtq q[HV_VIRTQ_MAX];
    uint8_t config[256];
    uint32_t config_len;
    void (*notify)(struct HvVirtio *v, int q);
    void (*config_write)(struct HvVirtio *v, uint32_t off, uint64_t val, unsigned size);
    void *dev;
} HvVirtio;

/* device_id is the virtio device type (1 net, 2 block); PCI IDs default to the
   modern 0x1af4:0x1040+id and may be changed afterwards. */
void hv_virtio_init(HvVirtio *v, HvMachine *m, int device_id, uint32_t class_code, int nq, uint64_t features);

/* 1 and an element if the driver made one available, 0 if not, -1 if the ring is broken. */
int hv_virtq_pop(HvVirtio *v, int q, HvVirtqElem *e);
void hv_virtq_push(HvVirtio *v, int q, const HvVirtqElem *e, uint32_t written);
void hv_virtio_interrupt(HvVirtio *v);

/* Copy between an element's buffers and host memory, skipping `off` bytes of
   the readable (to) or writable (from) segments. Return bytes copied. */
uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t off, void *buf, uint32_t len);
uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t off, const void *buf, uint32_t len);
uint32_t hv_virtq_writable(const HvVirtqElem *e);

#endif
