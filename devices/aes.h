#ifndef HENGEVM_AES_H
#define HENGEVM_AES_H

#include <stdint.h>

#include "gicv3.h"

/* The vmapple AES engine (QEMU's hw/vmapple/aes.c): commands are pushed one
   word at a time into a FIFO; DATA commands DMA through AES-ECB or AES-CBC.
   Two 16 KB pages: the engine, and a second page that reads as zero. */
typedef struct HvAesKey {
    uint32_t len;
    uint8_t key[32];
} HvAesKey;

typedef struct HvAes {
    HvMachine *m;
    HvGic *gic;
    int intid;
    uint32_t status, q_status, irq_status, irq_enable, watermark, flag_info;
    uint32_t fifo[9];
    uint32_t fifo_idx;
    HvAesKey key[2];
    uint32_t iv[4][4];
    int encrypt, cbc;
} HvAes;

HvAes *hv_aes_create(HvMachine *m, HvGic *gic, uint64_t pa1, uint64_t pa2, int intid);

#endif
