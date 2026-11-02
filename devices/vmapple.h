#ifndef HENGEVM_VMAPPLE_H
#define HENGEVM_VMAPPLE_H

#include <stdint.h>

#include "bdif.h"
#include "cfg.h"
#include "gicv3.h"
#include "pcie.h"
#include "pl011.h"
#include "pl031.h"
#include "virtio_blk.h"
#include "virtio_net.h"

/* The virtual Mac: QEMU's vmapple memory map and interrupt numbers (hw/vmapple/vmapple.c). */
#define VMAPPLE_FIRMWARE   0x00100000ull /* 1 MB: the boot ROM */
#define VMAPPLE_CONFIG     0x00400000ull
#define VMAPPLE_GIC_DIST   0x10000000ull
#define VMAPPLE_GIC_REDIST 0x10010000ull
#define VMAPPLE_UART       0x20010000ull
#define VMAPPLE_RTC        0x20050000ull
#define VMAPPLE_BDOOR      0x30000000ull
#define VMAPPLE_PCIE_ECAM  0x40000000ull
#define VMAPPLE_PCIE_MMIO  0x50000000ull
#define VMAPPLE_MEM        0x70000000ull

/* SPI numbers; the INTID is 32 more. */
#define VMAPPLE_IRQ_UART 1
#define VMAPPLE_IRQ_RTC 2
#define VMAPPLE_IRQ_PCIE 0x20

typedef struct HvVmapple {
    HvMachine *m;
    HvCpu *cpu;
    HvGic *gic;
    HvPl011 *uart;
    HvPl031 *rtc;
    HvPcie *pcie;
    HvBdif *bdif;
    HvVirtioBlk *aux_blk, *root_blk;
    HvVirtioNet *net;
    uint8_t *cfg, *rom, *ram;
    uint64_t ram_size;
} HvVmapple;

/* Builds the machine around m and cpu. Disks may be NULL (no device then).
   Guest RAM is reserved, not committed: pages are only used once touched. */
int hv_vmapple_create(HvVmapple *b, HvMachine *m, HvCpu *cpu, uint64_t ram_size, HvDisk *root, HvDisk *aux,
                      const HvCfgParams *cfg, const uint8_t mac[6]);
/* Copies a boot ROM image into the firmware region. */
int hv_vmapple_load_rom(HvVmapple *b, const char *path);
/* CPU reset: EL1, MMU off, interrupts masked, at the start of the ROM. */
void hv_vmapple_reset_cpu(HvVmapple *b);

#endif
