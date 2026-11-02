#ifndef HENGEVM_PCIE_H
#define HENGEVM_PCIE_H

#include <stdint.h>

#include "gicv3.h"

/* A PCIe host bridge like QEMU's gpex, as vmapple uses it: ECAM config space,
   one MMIO window, legacy INTx on four SPIs (no MSI: vmapple has no ITS).
   Bus 0, function 0 only. */
#define HV_PCI_SLOTS 32

struct HvPcie;

typedef struct HvPciDev {
    struct HvPcie *bus;
    int slot;
    uint8_t config[4096];
    uint8_t wmask[256];       /* writable bits of the standard header */
    uint64_t bar_size[6];     /* 0: no BAR; a 64-bit BAR's size sits in its low half */
    uint8_t bar64[6];
    int intx;                 /* current INTA level */
    void *opaque;
    uint64_t (*bar_read)(struct HvPciDev *d, int bar, uint64_t off, unsigned size);
    void (*bar_write)(struct HvPciDev *d, int bar, uint64_t off, uint64_t v, unsigned size);
} HvPciDev;

typedef struct HvPcie {
    HvMachine *m;
    HvGic *gic;
    int intx_base;            /* INTID of INTx line 0 */
    int line_count[4];
    HvPciDev *dev[HV_PCI_SLOTS];
    HvPciDev bridge;
    uint64_t mmio_base, mmio_size;
} HvPcie;

HvPcie *hv_pcie_create(HvMachine *m, HvGic *gic, uint64_t ecam_pa, uint64_t ecam_size, uint64_t mmio_pa,
                       uint64_t mmio_size, int intx_intid);
/* Fills in the header; the caller then adds BARs and capabilities. */
void hv_pci_init(HvPciDev *d, uint16_t vendor, uint16_t device, uint32_t class_rev, uint16_t sub_vendor,
                 uint16_t sub_id);
void hv_pci_add_bar(HvPciDev *d, int bar, uint64_t size, int is64);
int hv_pcie_attach(HvPcie *p, HvPciDev *d); /* returns the slot, or -1 */
uint64_t hv_pci_bar_addr(HvPciDev *d, int bar);
void hv_pci_set_intx(HvPciDev *d, int level);

#endif
