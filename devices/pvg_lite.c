/* The guest driver's view of the graphics device, worked out from its symbols
   and from tracing ParavirtualizedGraphics.framework (HV_PVG_TRACE=2):

   gfx registers
     +1000 enable, +1004 FIFO buffer size, +1010 ring offset in it (0x4000),
     +1030 FIFO buffer page (<< 14), +101c root page (<< 14)
     +1008 root ring write pointer (doorbell), +100c read pointer
     +1020 channel doorbell (the value is the channel)
     +1018 stamps-updated bits (1 << channel), +1014 display bits, +102c faults
     +1034 protocol version (the driver offers one, the device answers),
     +122c number of displays

   FIFO buffer: a control area where stamp[channel] is at +4 * channel, then
   the root ring. Root page: channel records at 0x400 + 0x14 * (channel - 1)
   of {write, read, -1, id, page table page}; the table lists the ring's 16 KB
   pages as 32-bit page numbers, zero after the last.

   Every command, on every ring, starts {op, length, stamp}. It is complete
   when the device has stored its stamp and moved the read pointer.

   iosurface mapper: +1028 queue count, +1030 enable, +1000 ring, +1010
   completions, +1008 size, +1018 submitted index, +1020 completed index. */

#include "pvg_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/machine.h"

#define MAX_CHANNELS 16

struct HvPvgLite {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    int log;
    uint32_t regs[0x1000 / 4]; /* +1000.. */
    uint64_t io_regs[0x40 / 8];
    uint32_t root_rp, status, chan_pending;
    int root_pending, iosfc_pending;
};

static uint32_t *guest32(HvPvgLite *p, uint64_t pa) { return hv_phys_ptr(p->m, pa, 4); }

static uint32_t reg(HvPvgLite *p, uint32_t off) { return p->regs[(off - 0x1000) / 4]; }

static void do_root(HvPvgLite *p) {
    uint64_t buf = (uint64_t)reg(p, 0x1030) << 14;
    uint32_t off = reg(p, 0x1010), size = reg(p, 0x1004), wp = reg(p, 0x1008);
    if (!buf || size <= off) return;
    uint32_t ring = size - off;
    while (p->root_rp != wp) {
        uint32_t at = p->root_rp % ring, hdr[3];
        for (int i = 0; i < 3; i++) {
            uint32_t *w = guest32(p, buf + off + (at + 4u * i) % ring);
            hdr[i] = w ? *w : 0;
        }
        if (hdr[1] < 12 || hdr[1] > ring) {
            fprintf(stderr, "hengevm: pvg-lite: bad root command %x length %x\n", hdr[0], hdr[1]);
            p->root_rp = wp;
            break;
        }
        if (p->log) fprintf(stderr, "hengevm: pvg-lite: root op %x len %x stamp %x\n", hdr[0], hdr[1], hdr[2]);
        p->root_rp += hdr[1];
        uint32_t *stamp = guest32(p, buf);
        if (stamp) __atomic_store_n(stamp, hdr[2], __ATOMIC_RELEASE);
    }
}

static void do_channel(HvPvgLite *p, uint32_t ch) {
    uint64_t root = (uint64_t)reg(p, 0x101c) << 14, buf = (uint64_t)reg(p, 0x1030) << 14;
    if (!root || !buf || !ch) return;
    uint32_t *rec = hv_phys_ptr(p->m, root + 0x400 + 0x14 * (ch - 1), 0x14);
    if (!rec) return;
    uint32_t *table = hv_phys_ptr(p->m, (uint64_t)rec[4] << 14, 0x4000);
    if (!table) return;
    uint32_t pages = 0;
    while (pages < 0x1000 && table[pages]) pages++;
    if (!pages) return;
    uint32_t size = pages << 14, wr = __atomic_load_n(&rec[0], __ATOMIC_ACQUIRE), rd = rec[1], last = 0;
    int any = 0;
    while (rd != wr) {
        uint32_t hdr[3];
        for (int i = 0; i < 3; i++) {
            uint32_t at = (rd + 4u * i) % size;
            uint32_t *w = guest32(p, ((uint64_t)table[at >> 14] << 14) + (at & 0x3fff));
            hdr[i] = w ? *w : 0;
        }
        if (hdr[1] < 12 || hdr[1] > size) {
            fprintf(stderr, "hengevm: pvg-lite: channel %u: bad command %x length %x at %x\n", ch, hdr[0], hdr[1], rd);
            rd = wr;
            break;
        }
        if (p->log) fprintf(stderr, "hengevm: pvg-lite: ch %u op %x len %x stamp %x\n", ch, hdr[0], hdr[1], hdr[2]);
        last = hdr[2], any = 1;
        rd += hdr[1];
    }
    __atomic_store_n(&rec[1], rd, __ATOMIC_RELEASE);
    if (any) {
        uint32_t *stamp = guest32(p, buf + 4 * ch);
        if (stamp) __atomic_store_n(stamp, last, __ATOMIC_RELEASE);
        p->status |= 1u << ch;
    }
}

void hv_pvg_lite_poll(HvPvgLite *p) {
    if (!p) return;
    int irq = 0;
    if (p->root_pending) {
        p->root_pending = 0;
        do_root(p);
        irq = 1;
    }
    uint32_t chans = p->chan_pending;
    if (chans) {
        p->chan_pending = 0;
        for (uint32_t ch = 1; ch < MAX_CHANNELS; ch++)
            if (chans & (1u << ch)) do_channel(p, ch);
        irq = 1;
    }
    if (irq) hv_gicv3_pend(p->gic, p->gfx_irq);
    if (p->iosfc_pending) {
        p->iosfc_pending = 0;
        p->io_regs[0x20 / 8] = p->io_regs[0x18 / 8];
        hv_gicv3_pend(p->gic, p->iosfc_irq);
    }
}

static uint64_t gfx_read(void *opaque, uint64_t off, unsigned size) {
    HvPvgLite *p = opaque;
    uint64_t v = 0;
    switch (off) {
    case 0x100c:
        do_root(p); /* the driver spins on this waiting for ring space */
        v = p->root_rp;
        break;
    case 0x1018:
        v = p->status;
        p->status = 0;
        break;
    case 0x122c: v = 1; break;
    default:
        if (off >= 0x1000 && off < 0x2000) v = p->regs[(off - 0x1000) / 4];
        break;
    }
    (void)size;
    return v;
}

static void gfx_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off >= 0x1000 && off < 0x2000) p->regs[(off - 0x1000) / 4] = (uint32_t)v;
    if (off == 0x1008) p->root_pending = 1;
    else if (off == 0x1020 && v < MAX_CHANNELS) p->chan_pending |= 1u << v;
    else if (p->log && off != 0x1020) fprintf(stderr, "hengevm: pvg-lite: write +%llx = %llx\n", (unsigned long long)off, (unsigned long long)v);
    if (off == 0x1008 || off == 0x1020) hv_machine_kick_all(p->m);
}

static uint64_t iosfc_read(void *opaque, uint64_t off, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off == 0x1028) return 1;
    return off >= 0x1000 && off < 0x1040 ? p->io_regs[(off - 0x1000) / 8] : 0;
}

static void iosfc_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off >= 0x1000 && off < 0x1040) p->io_regs[(off - 0x1000) / 8] = v;
    if (off == 0x1018) {
        p->iosfc_pending = 1;
        hv_machine_kick_all(p->m);
    }
}

HvPvgLite *hv_pvg_lite_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    HvPvgLite *p = calloc(1, sizeof *p);
    p->m = m;
    p->gic = gic;
    p->gfx_irq = gfx_irq;
    p->iosfc_irq = iosfc_irq;
    p->log = getenv("HV_PVG_LITE_LOG") != NULL;
    hv_machine_add_mmio(m, gfx_pa, 0x10000, p, gfx_read, gfx_write, "gfx");
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, p, iosfc_read, iosfc_write, "iosurface");
    return p;
}
