#ifndef HENGEVM_VMAPPLE_H
#define HENGEVM_VMAPPLE_H

#include <stdint.h>

#include "aes.h"
#include "bdif.h"
#include "cfg.h"
#include "gicv3.h"
#include "pcie.h"
#include "pl011.h"
#include "pl061.h"
#include "pl031.h"
#include "virtio_blk.h"
#include "virtio_input.h"
#include "xhci.h"
#include "virtio_net.h"

/* The virtual Mac: QEMU's vmapple memory map and interrupt numbers (hw/vmapple/vmapple.c). */
#define VMAPPLE_FIRMWARE   0x00100000ull /* 1 MB: the boot ROM */
#define VMAPPLE_CONFIG     0x00400000ull
#define VMAPPLE_GIC_DIST   0x10000000ull
#define VMAPPLE_GIC_REDIST 0x10010000ull
#define VMAPPLE_GIC_MSI    0x1fff0000ull /* third gic reg in the guest's device tree; not in QEMU */
#define VMAPPLE_UART       0x20010000ull
#define VMAPPLE_RTC        0x20050000ull
#define VMAPPLE_GPIO       0x20060000ull
#define VMAPPLE_PVPANIC    0x20070000ull
#define VMAPPLE_BDOOR      0x30000000ull
#define VMAPPLE_GFX        0x30200000ull /* paravirtualized graphics: present but inert */
#define VMAPPLE_IOSFC      0x30210000ull
#define VMAPPLE_AES_1      0x30220000ull
#define VMAPPLE_AES_2      0x30230000ull
#define VMAPPLE_RTC_SELECT 0x30240000ull /* not in QEMU's map; see vmapple.c */
#define VMAPPLE_PCIE_ECAM  0x40000000ull
#define VMAPPLE_PCIE_MMIO  0x50000000ull
#define VMAPPLE_MEM        0x70000000ull

/* SPI numbers; the INTID is 32 more. */
#define VMAPPLE_IRQ_UART 1
#define VMAPPLE_IRQ_RTC 2
#define VMAPPLE_IRQ_GPIO 5
#define VMAPPLE_IRQ_AES 0x12
#define VMAPPLE_IRQ_PCIE 0x20
#define VMAPPLE_MSI_BASE 96   /* INTIDs handed out through the MSI frame */
#define VMAPPLE_MSI_COUNT 64

typedef struct HvVmapple {
    uint64_t avp_rtc[8];     /* Apple virtual RTC control registers +0x00..+0x38 (only +0x18, +0x20, +0x38 are written) */
    HvMachine *m;
    HvCpu *cpu;
    HvGic *gic;
    HvPl011 *uart;
    HvPl031 *rtc;
    HvPl061 *gpio;
    HvPcie *pcie;
    HvBdif *bdif;
    HvAes *aes;
    HvVirtioBlk *aux_blk, *root_blk;
    HvVirtioNet *net;
    HvVirtioInput *keyboard, *mouse;
    HvXhci *usb;             /* USB controller with the keyboard macOS actually uses */
    uint8_t *cfg, *rom, *ram;
    uint64_t ram_size;
    uint64_t panic_events;   /* last pvpanic event the guest reported */
    void (*poll)(void);      /* extra device work on the CPU thread each tick (Mac PVG) */
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
