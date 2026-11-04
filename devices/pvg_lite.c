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

   Display: channel 4 op 1 names a shared state page the device fills in
   (modes, seq at +200, ack pending at +fa9, state at +fc0); the guest reads
   +100 once it has set its mask at +104, acks with op 2. Swaps are op 8:
   {port, 0, u64 mapping id, u64 gamma offset, u64 gamma length, count, sum},
   and the device counts them at +f98 with the mapping id at +fb0.

   A command with bit 16 of its op set first carries a barrier: {channel, stamp}.

   iosurface mapper: +1028 queue count, +1030 enable, +1000 ring, +1010
   descriptor table, +1008 size, +1018 submitted index, +1020 completed index.
   Ring entries are {op (1 map, 2 unmap, 3 test), mapping id, result, 0}. */

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
    uint64_t display_pa;
    uint64_t swaps;
};

/* What the guest's GetDeviceInfo asks for: key, value pairs, as the framework
   answers on an Apple GPU. */
static const uint32_t device_info[][2] = {
    {0x01, 4},      {0x02, 0},     {0x03, 0x400},  {0x04, 0x400},  {0x05, 0x400},  {0x06, 0x8000}, {0x07, 1},
    {0x08, 1},      {0x09, 1},     {0x0a, 8},      {0x0b, 0x1ff},  {0x0c, 1},      {0x0d, 0x10},   {0x0e, 1},
    {0x0f, 0x100},  {0x10, 1},     {0x11, 1},      {0x12, 0x20007}, {0x13, 1},     {0x14, 2},      {0x15, 1},
    {0x16, 2},      {0x17, 1},     {0x18, 1},      {0x19, 1},      {0x1a, 1},      {0x1b, 1},      {0x1c, 7},
    {0x21, 0xfff},  {0x24, 0xffffffff}, {0x1d, 0x400}, {0x1e, 0x1f}, {0x1f, 0x10000}, {0x20, 0x10}, {0x22, 5},
    {0x29, 7},      {0x23, 0x800}, {0x25, 0x3f1},  {0x28, 0x10},
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
        if ((hdr[0] & 0xffff) == 0x3a && hdr[1] >= 24) {
            uint32_t *count = guest32(p, buf + off + (at + 12) % ring), *page = guest32(p, buf + off + (at + 20) % ring);
            uint32_t *dst = page ? hv_phys_ptr(p->m, (uint64_t)*page << 14, sizeof device_info) : NULL;
            if (dst && count) {
                uint32_t n = sizeof device_info / sizeof device_info[0];
                memcpy(dst, device_info, sizeof device_info);
                if (p->log) fprintf(stderr, "hengevm: pvg-lite: device info: %u keys (asked %u)\n", n, *count);
            }
        }
        p->root_rp += hdr[1];
        uint32_t *stamp = guest32(p, buf);
        if (stamp) __atomic_store_n(stamp, hdr[2], __ATOMIC_RELEASE);
    }
}

/* Reads a command's bytes out of a ring made of scattered pages. */
static void ring_read(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t at, void *to, uint32_t n) {
    uint8_t *d = to;
    while (n) {
        uint32_t o = at % size, c = 0x4000 - (o & 0x3fff);
        if (c > n) c = n;
        uint8_t *q = hv_phys_ptr(p->m, ((uint64_t)table[o >> 14] << 14) + (o & 0x3fff), c);
        if (q) memcpy(d, q, c);
        else memset(d, 0, c);
        d += c, at += c, n -= c;
    }
}

static void display_set_page(HvPvgLite *p, uint64_t pa) {
    uint8_t *d = hv_phys_ptr(p->m, pa, 0x4000);
    if (!d) return;
    p->display_pa = pa;
#define PUT32(off, v) (*(uint32_t *)(d + (off)) = (uint32_t)(v))
#define PUTF(off, v) (*(float *)(d + (off)) = (v))
    PUT32(0x00, 1);
    PUT32(0x14, 300u << 16 | 400); /* physical size in mm */
    PUT32(0x18, 0x01000100);
    PUT32(0x1c, 0x400);
    PUT32(0x20, 3);
    PUTF(0x24, 400.0f);
    PUTF(0x28, 300.0f);
    PUT32(0x208, 1); /* one mode: */
    PUT32(0x210, 1080u << 16 | 1920);
    PUT32(0x214, 60u << 16);
    PUTF(0xa10, 1.0f); /* identity colour matrix */
    PUTF(0xa24, 1.0f);
    PUTF(0xa38, 1.0f);
    d[0xa73] = 1;
    PUT32(0x200, 1); /* the mode list's sequence number, which the guest acks */
    d[0xfa9] = 1;    /* ack pending */
    PUT32(0xfc0, 1); /* state: told the guest, waiting for the ack */
    __atomic_store_n((uint32_t *)(d + 0x100), 4, __ATOMIC_RELEASE); /* mode change: read when the guest sets its mask */
#undef PUT32
#undef PUTF
}

static void display_ack(HvPvgLite *p, uint32_t seq) {
    uint8_t *d = p->display_pa ? hv_phys_ptr(p->m, p->display_pa, 0x4000) : NULL;
    if (!d) return;
    *(uint32_t *)(d + 0xfbc) = seq;
    d[0xfa9] = 0;
    *(uint32_t *)(d + 0xfc0) = 2;
}

static void display_swap(HvPvgLite *p, uint64_t mapping, const uint32_t *args) {
    uint8_t *d = p->display_pa ? hv_phys_ptr(p->m, p->display_pa, 0x4000) : NULL;
    if (!d) return;
    memcpy(d + 0xf80, args + 4, 16); /* gamma table offset, length */
    memcpy(d + 0xf90, args + 8, 8);  /* gamma entries, sum */
    p->swaps++;
    *(uint32_t *)(d + 0xf98) = (uint32_t)p->swaps;
    *(uint32_t *)(d + 0xfb0) = (uint32_t)mapping;
    __atomic_fetch_or((uint32_t *)(d + 0x100), 2, __ATOMIC_RELEASE);
    if (p->log) fprintf(stderr, "hengevm: pvg-lite: swap %llu: mapping %llu\n", (unsigned long long)p->swaps, (unsigned long long)mapping);
}

/* Commands that do something even without a GPU to run them. */
static void channel_command(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t rd, const uint32_t *hdr) {
    uint32_t op = hdr[0] & 0xffff, args[32], skip = hdr[0] >> 16 ? 8 : 0; /* a barrier comes first */
    uint32_t n = hdr[1] > 12 + skip ? hdr[1] - 12 - skip : 0;
    if (n > sizeof args) n = sizeof args;
    if (op != 1 && op != 2 && op != 8) return;
    memset(args, 0, sizeof args);
    ring_read(p, table, size, rd + 12 + skip, args, n);
    if (op == 1) display_set_page(p, (uint64_t)args[1] << 14);
    else if (op == 2) display_ack(p, args[1]);
    else display_swap(p, (uint64_t)args[3] << 32 | args[2], args);
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
        channel_command(p, table, size, rd, hdr);
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
