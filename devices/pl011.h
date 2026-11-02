#ifndef HENGEVM_PL011_H
#define HENGEVM_PL011_H

#include <stdint.h>

#include "gicv3.h"

/* PL011 UART. Transmit is immediate, so the TX FIFO always reads empty. */
typedef struct HvPl011 {
    HvGic *gic;
    int intid;
    uint32_t cr, lcr, ibrd, fbrd, ifls, imsc, ris, dmacr;
    uint8_t rx[64];
    int rx_head, rx_count;
    void (*output)(void *opaque, uint8_t c);
    void *output_opaque;
} HvPl011;

HvPl011 *hv_pl011_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid);
/* Host side input; returns 0 if the RX FIFO was full. */
int hv_pl011_receive(HvPl011 *u, uint8_t c);

#endif
