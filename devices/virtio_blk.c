#include "virtio_blk.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* Disk I/O straight between the file and the guest's buffers: the
   segments of one direction, skipping `off` bytes of them, `len` bytes in all.
   Returns 0, or -1 on a host error or a buffer outside guest RAM. */
static int seg_io(HvVirtioBlk *b, const HvVirtqElem *e, int writable, uint32_t off, uint64_t disk_off, uint32_t len) {
    for (int s = 0; s < e->nseg && len; s++) {
        const HvVirtqSeg *g = &e->seg[s];
        if (g->write != writable) continue;
        if (off >= g->len) {
            off -= g->len;
            continue;
        }
        uint32_t n = g->len - off;
        if (n > len) n = len;
        void *p = hv_phys_ptr(b->vio.m, g->addr + off, n);
        if (!p) return -1;
        int r = writable ? hv_disk_read(b->disk, disk_off, p, n) : hv_disk_write(b->disk, disk_off, p, n);
        if (r != 0) return -1;
        disk_off += n;
        len -= n;
        off = 0;
    }
    return len ? -1 : 0;
}

static uint32_t readable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (!e->seg[s].write) n += e->seg[s].len;
    return n;
}

/* Worker thread. */
static int handle(HvVirtioBlk *b, HvVirtqElem *e, uint32_t *written) {
    HvVirtio *v = &b->vio;
    struct { uint32_t type, reserved; uint64_t sector; } hdr;
    uint32_t wlen = hv_virtq_writable(e);
    *written = 0;
    if (wlen < 1 || hv_virtq_read(v, e, 0, &hdr, sizeof hdr) != sizeof hdr) return -1;
    uint32_t data = wlen - 1; /* the status byte is the last writable one */
    uint8_t status = S_OK;
    switch (hdr.type) {
    case T_IN:
        if (seg_io(b, e, 1, 0, hdr.sector * 512, data) != 0) status = S_IOERR;
        else *written = data;
        break;
    case T_OUT: {
        uint32_t rlen = readable(e);
        if (rlen < sizeof hdr || seg_io(b, e, 0, sizeof hdr, hdr.sector * 512, rlen - (uint32_t)sizeof hdr) != 0)
            status = S_IOERR;
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
    if (status != S_OK) __atomic_fetch_add(&b->errors, 1, __ATOMIC_RELAXED);
    hv_virtq_write(v, e, data, &status, 1);
    *written += 1;
    return 0;
}

static void *worker(void *opaque) {
    HvVirtioBlk *b = opaque;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        while (b->completed == __atomic_load_n(&b->submitted, __ATOMIC_ACQUIRE)) pthread_cond_wait(&b->cv, &b->mu);
        pthread_mutex_unlock(&b->mu);
        HvVblkReq *r = &b->ring[b->completed % HV_VBLK_RING];
        if (handle(b, &r->e, &r->written) != 0) r->written = 0;
        __atomic_store_n(&b->completed, b->completed + 1, __ATOMIC_RELEASE); /* in order: a single worker */
        hv_machine_wake(b->vio.m);
    }
    return NULL;
}

/* CPU thread: hand everything the driver made available to the worker, while the ring has room. */
static void notify(HvVirtio *v, int q) {
    HvVirtioBlk *b = v->dev;
    int any = 0;
    while (b->submitted - b->published < HV_VBLK_RING) {
        HvVblkReq *r = &b->ring[b->submitted % HV_VBLK_RING];
        if (hv_virtq_pop(v, q, &r->e) <= 0) break;
        r->q = q;
        pthread_mutex_lock(&b->mu);
        __atomic_store_n(&b->submitted, b->submitted + 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&b->mu);
        any = 1;
    }
    if (any) pthread_cond_signal(&b->cv);
}

void hv_virtio_blk_drain(HvVirtioBlk *b) {
    while (__atomic_load_n(&b->completed, __ATOMIC_ACQUIRE) != b->submitted) usleep(100);
    hv_virtio_blk_poll(b);
}

void hv_virtio_blk_poll(HvVirtioBlk *b) {
    if (!b) return;
    uint32_t done = __atomic_load_n(&b->completed, __ATOMIC_ACQUIRE);
    if (done == b->published) return;
    int q = 0;
    for (; b->published != done; b->published++) {
        HvVblkReq *r = &b->ring[b->published % HV_VBLK_RING];
        hv_virtq_push(&b->vio, r->q, &r->e, r->written);
        q = r->q;
        b->requests++;
    }
    hv_virtio_interrupt(&b->vio);
    notify(&b->vio, q); /* requests that waited for room in the ring */
}

HvVirtioBlk *hv_virtio_blk_create(HvMachine *m, HvPcie *bus, HvDisk *disk, int apple_variant) {
    HvVirtioBlk *b = calloc(1, sizeof *b);
    HvVirtio *v = &b->vio;
    hv_virtio_init(v, m, 2, 0x010000, 1, F_SEG_MAX | F_BLK_SIZE | F_FLUSH | (disk->readonly ? F_RO : 0));
    v->dev = b;
    v->notify = notify;
    b->disk = disk;
    b->ring = calloc(HV_VBLK_RING, sizeof *b->ring);
    pthread_mutex_init(&b->mu, NULL);
    pthread_cond_init(&b->cv, NULL);
    pthread_create(&b->worker, NULL, worker, b);
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
