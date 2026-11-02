#include "virtio_blk.h"

#include <stdlib.h>
#include <string.h>

#define F_SEG_MAX (1ull << 2)
#define F_RO (1ull << 5)
#define F_BLK_SIZE (1ull << 6)
#define F_FLUSH (1ull << 9)

#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4
#define T_GET_ID 8
#define T_APPLE_BARRIER 0x10000

#define S_OK 0
#define S_IOERR 1
#define S_UNSUPP 2

#define CHUNK (1u << 20)

static int handle(HvVirtioBlk *b, HvVirtqElem *e, uint32_t *written) {
    HvVirtio *v = &b->vio;
    struct { uint32_t type, reserved; uint64_t sector; } hdr;
    uint32_t wlen = hv_virtq_writable(e);
    *written = 0;
    if (wlen < 1 || hv_virtq_read(v, e, 0, &hdr, sizeof hdr) != sizeof hdr) return -1;
    uint32_t data = wlen - 1; /* the status byte is the last writable one */
    uint8_t status = S_OK;
    static uint8_t buf[CHUNK];
    switch (hdr.type) {
    case T_IN:
        for (uint32_t done = 0; done < data && status == S_OK;) {
            uint32_t n = data - done < CHUNK ? data - done : CHUNK;
            if (hv_disk_read(b->disk, hdr.sector * 512 + done, buf, n) != 0) status = S_IOERR;
            else hv_virtq_write(v, e, done, buf, n);
            done += n;
        }
        if (status == S_OK) *written = data;
        break;
    case T_OUT: {
        uint32_t off = sizeof hdr;
        for (;;) {
            uint32_t n = hv_virtq_read(v, e, off, buf, CHUNK);
            if (!n) break;
            if (hv_disk_write(b->disk, hdr.sector * 512 + (off - sizeof hdr), buf, n) != 0) {
                status = S_IOERR;
                break;
            }
            off += n;
        }
        break;
    }
    case T_FLUSH:
        if (hv_disk_flush(b->disk) != 0) status = S_IOERR;
        break;
    case T_GET_ID: {
        char id[20] = "hengevm-disk";
        uint32_t n = data < 20 ? data : 20;
        hv_virtq_write(v, e, 0, id, n);
        *written = n;
        break;
    }
    case T_APPLE_BARRIER: /* a no-op here, as in QEMU: requests complete in order */
        break;
    default:
        status = S_UNSUPP;
        break;
    }
    if (status != S_OK) b->errors++;
    hv_virtq_write(v, e, data, &status, 1);
    *written += 1;
    return 0;
}

static void notify(HvVirtio *v, int q) {
    HvVirtioBlk *b = v->dev;
    static HvVirtqElem e;
    int any = 0;
    while (hv_virtq_pop(v, q, &e) > 0) {
        uint32_t written;
        if (handle(b, &e, &written) != 0) written = 0;
        hv_virtq_push(v, q, &e, written);
        b->requests++;
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

HvVirtioBlk *hv_virtio_blk_create(HvMachine *m, HvPcie *bus, HvDisk *disk, int apple_variant) {
    HvVirtioBlk *b = calloc(1, sizeof *b);
    HvVirtio *v = &b->vio;
    hv_virtio_init(v, m, 2, 0x010000, 1, F_SEG_MAX | F_BLK_SIZE | F_FLUSH | (disk->readonly ? F_RO : 0));
    v->dev = b;
    v->notify = notify;
    b->disk = disk;
    uint64_t capacity = disk->size / 512;
    uint32_t seg_max = HV_VIRTQ_MAX_SEGS - 2, blk_size = 512, apple_type = (uint32_t)apple_variant;
    uint16_t num_queues = 1;
    memcpy(v->config + 0, &capacity, 8);
    memcpy(v->config + 12, &seg_max, 4);
    memcpy(v->config + 20, &blk_size, 4);
    memcpy(v->config + 34, &num_queues, 2);
    memcpy(v->config + 60, &apple_type, 4);
    v->config_len = 96;
    if (apple_variant) {
        /* Apple's identifiers, SCSI storage class, as vmapple-virtio-blk-pci sets them. */
        uint16_t vendor = 0x106b, device = 0x1a00;
        memcpy(v->pci.config + 0, &vendor, 2);
        memcpy(v->pci.config + 2, &device, 2);
        /* Apple vendor data (a virtio 1.2 PCI vendor-data capability, cfg_type
           9, vendor 0x106b). AppleVirtIOStorageDevice reads 8 bytes of it: if
           bit 0 of the first byte is clear, or the capability is missing, it
           reports the disk as AES-XTS inline-encrypted, and APFS then refuses
           an unencrypted Data volume. These disks are plain files, so bit 0
           is set. QEMU's model has no such capability. */
        uint8_t *cap = v->pci.config + 0x88;
        v->pci.config[0x71] = 0x88; /* chain it after the notify capability */
        cap[0] = 0x09;
        cap[1] = 0x00;
        cap[2] = 6 + 8;
        cap[3] = 9;
        memcpy(cap + 4, &vendor, 2);
        memset(cap + 6, 0, 8);
        cap[6] = 1; /* not inline-encrypted */
    }
    hv_pcie_attach(bus, &v->pci);
    return b;
}
