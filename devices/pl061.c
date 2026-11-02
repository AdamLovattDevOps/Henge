#include "pl061.h"

#include <stdlib.h>

/* Register behaviour from QEMU's hw/gpio/pl061.c (Arm PrimeCell PL061). */

static const uint8_t pl061_id[12] = {0x00, 0x00, 0x00, 0x00, 0x61, 0x10, 0x04, 0x00, 0x0d, 0xf0, 0x05, 0xb1};

static void update(HvPl061 *g) {
    /* Input pins follow the host's levels (pulled down when not driven). */
    g->data = (uint8_t)((g->data & g->dir) | (g->input & ~g->dir));
    uint8_t changed = (uint8_t)((g->old_in ^ g->data) & ~g->dir);
    g->old_in = g->data;
    for (int i = 0; i < 8; i++) {
        uint8_t mask = (uint8_t)(1u << i);
        if (!(changed & mask) || (g->isense & mask)) continue;
        if (g->ibe & mask) g->istate |= mask;                            /* both edges */
        else g->istate |= (uint8_t)(~(g->data ^ g->iev) & mask);          /* edge chosen by IEV */
    }
    g->istate |= (uint8_t)(~(g->data ^ g->iev) & g->isense);             /* level */
    hv_gicv3_set_irq(g->gic, g->intid, (g->istate & g->im) != 0);
}

static uint64_t pl061_read(void *opaque, uint64_t off, unsigned size) {
    (void)size;
    HvPl061 *g = opaque;
    if (off < 0x400) return g->data & (off >> 2); /* address bits 9..2 mask the data */
    switch (off) {
    case 0x400: return g->dir;
    case 0x404: return g->isense;
    case 0x408: return g->ibe;
    case 0x40c: return g->iev;
    case 0x410: return g->im;
    case 0x414: return g->istate;
    case 0x418: return g->istate & g->im;
    case 0x420: return g->afsel;
    default:
        if (off >= 0xfd0 && off < 0x1000) return pl061_id[(off - 0xfd0) >> 2];
        return 0;
    }
}

static void pl061_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvPl061 *g = opaque;
    if (off < 0x400) {
        uint8_t mask = (uint8_t)((off >> 2) & g->dir);
        g->data = (uint8_t)((g->data & ~mask) | (v & mask));
        update(g);
        return;
    }
    switch (off) {
    case 0x400: g->dir = (uint8_t)v; break;
    case 0x404: g->isense = (uint8_t)v; break;
    case 0x408: g->ibe = (uint8_t)v; break;
    case 0x40c: g->iev = (uint8_t)v; break;
    case 0x410: g->im = (uint8_t)v; break;
    case 0x41c: g->istate &= (uint8_t)~v; break;
    case 0x420: g->afsel = (uint8_t)v; break;
    default: return;
    }
    update(g);
}

void hv_pl061_set_input(HvPl061 *g, int pin, int level) {
    if (level) g->input |= (uint8_t)(1u << pin);
    else g->input &= (uint8_t)~(1u << pin);
    update(g);
}

HvPl061 *hv_pl061_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid) {
    HvPl061 *g = calloc(1, sizeof *g);
    g->gic = gic;
    g->intid = intid;
    hv_machine_add_mmio(m, pa, 0x1000, g, pl061_read, pl061_write, "pl061");
    return g;
}
