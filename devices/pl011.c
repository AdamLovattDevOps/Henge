#include "pl011.h"

#include <stdio.h>
#include <stdlib.h>

/* Registers and ID values as in QEMU's hw/char/pl011.c (Arm PrimeCell PL011). */

#define FLAG_TXFE 0x80
#define FLAG_RXFF 0x40
#define FLAG_RXFE 0x10
#define INT_RT (1u << 6)
#define INT_TX (1u << 5)
#define INT_RX (1u << 4)

static const uint8_t pl011_id[8] = {0x11, 0x10, 0x14, 0x00, 0x0d, 0xf0, 0x05, 0xb1};

static void update(HvPl011 *u) {
    hv_gic_set_irq(u->gic, u->intid, (u->ris & u->imsc) != 0);
}

static void default_output(void *opaque, uint8_t c) {
    (void)opaque;
    fputc(c, stdout);
    if (c == '\n') fflush(stdout);
}

static uint64_t pl011_read(void *opaque, uint64_t off, unsigned size) {
    (void)size;
    HvPl011 *u = opaque;
    switch (off) {
    case 0x00: { /* DR */
        uint32_t c = 0;
        if (u->rx_count) {
            c = u->rx[u->rx_head];
            u->rx_head = (u->rx_head + 1) % (int)sizeof u->rx;
            u->rx_count--;
        }
        if (!u->rx_count) u->ris &= ~(INT_RX | INT_RT);
        update(u);
        return c;
    }
    case 0x04: return 0;                                   /* RSR */
    case 0x18: return FLAG_TXFE | (u->rx_count ? 0 : FLAG_RXFE) | (u->rx_count == (int)sizeof u->rx ? FLAG_RXFF : 0);
    case 0x24: return u->ibrd;
    case 0x28: return u->fbrd;
    case 0x2c: return u->lcr;
    case 0x30: return u->cr;
    case 0x34: return u->ifls;
    case 0x38: return u->imsc;
    case 0x3c: return u->ris;
    case 0x40: return u->ris & u->imsc;
    case 0x48: return u->dmacr;
    default:
        if (off >= 0xfe0 && off < 0x1000) return pl011_id[(off - 0xfe0) >> 2];
        return 0;
    }
}

static void pl011_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvPl011 *u = opaque;
    switch (off) {
    case 0x00:
        u->output(u->output_opaque, (uint8_t)v);
        u->ris |= INT_TX; /* the FIFO drained at once */
        break;
    case 0x04: break;
    case 0x24: u->ibrd = (uint32_t)v & 0xffff; break;
    case 0x28: u->fbrd = (uint32_t)v & 0x3f; break;
    case 0x2c: u->lcr = (uint32_t)v & 0xff; break;
    case 0x30: u->cr = (uint32_t)v & 0xffff; break;
    case 0x34: u->ifls = (uint32_t)v & 0x3f; break;
    case 0x38: u->imsc = (uint32_t)v & 0x7ff; break;
    case 0x44: u->ris &= ~(uint32_t)v; break;              /* ICR */
    case 0x48: u->dmacr = (uint32_t)v & 7; break;
    default: break;
    }
    update(u);
}

int hv_pl011_receive(HvPl011 *u, uint8_t c) {
    if (u->rx_count == (int)sizeof u->rx) return 0;
    u->rx[(u->rx_head + u->rx_count) % (int)sizeof u->rx] = c;
    u->rx_count++;
    u->ris |= INT_RX | INT_RT;
    update(u);
    return 1;
}

HvPl011 *hv_pl011_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid) {
    HvPl011 *u = calloc(1, sizeof *u);
    u->gic = gic;
    u->intid = intid;
    u->cr = 0x300;  /* TXE, RXE: QEMU's reset value */
    u->ifls = 0x12;
    u->ris = INT_TX;
    u->output = default_output;
    hv_machine_add_mmio(m, pa, 0x1000, u, pl011_read, pl011_write, "pl011");
    return u;
}
