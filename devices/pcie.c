#include "pcie.h"

#include <stdlib.h>
#include <string.h>

#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define CMD_MEMORY 0x2
#define CMD_INTX_DISABLE 0x400
#define STATUS_INTX 0x8
#define STATUS_CAPS 0x10

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

void hv_pci_init(HvPciDev *d, uint16_t vendor, uint16_t device, uint32_t class_rev, uint16_t sub_vendor,
                 uint16_t sub_id) {
    memset(d->config, 0, sizeof d->config);
    memset(d->wmask, 0, sizeof d->wmask);
    put16(d->config + 0x00, vendor);
    put16(d->config + 0x02, device);
    put32(d->config + 0x08, class_rev);
    put16(d->config + 0x2c, sub_vendor);
    put16(d->config + 0x2e, sub_id);
    d->config[0x3d] = 1;                          /* INTA */
    put16(d->wmask + PCI_COMMAND, 0x0407);        /* I/O, memory, bus master, INTx disable */
    d->wmask[0x0c] = 0xff;                        /* cache line size */
    d->wmask[0x0d] = 0xff;                        /* latency timer */
    d->wmask[0x3c] = 0xff;                        /* interrupt line */
}

void hv_pci_add_bar(HvPciDev *d, int bar, uint64_t size, int is64) {
    d->bar_size[bar] = size;
    d->bar64[bar] = (uint8_t)is64;
    put32(d->config + 0x10 + bar * 4, is64 ? 0x4 : 0x0); /* memory, 32/64-bit, not prefetchable */
    put32(d->wmask + 0x10 + bar * 4, (uint32_t)~(size - 1) & ~0xfu);
    if (is64) put32(d->wmask + 0x14 + bar * 4, (uint32_t)(~(size - 1) >> 32));
}

uint64_t hv_pci_bar_addr(HvPciDev *d, int bar) {
    uint64_t lo = get32(d->config + 0x10 + bar * 4) & ~0xfu;
    if (d->bar64[bar]) lo |= (uint64_t)get32(d->config + 0x14 + bar * 4) << 32;
    return lo;
}

static void update_line(HvPcie *p, int line) {
    hv_gicv3_set_irq(p->gic, p->intx_base + line, p->line_count[line] > 0);
}

void hv_pci_set_intx(HvPciDev *d, int level) {
    HvPcie *p = d->bus;
    uint16_t cmd = get16(d->config + PCI_COMMAND);
    int eff = level && !(cmd & CMD_INTX_DISABLE);
    uint16_t status = get16(d->config + PCI_STATUS);
    put16(d->config + PCI_STATUS, level ? status | STATUS_INTX : status & ~STATUS_INTX);
    if (eff == d->intx) return;
    d->intx = eff;
    /* gpex swizzle: INTA of slot s is line s % 4 */
    int line = d->slot % 4;
    p->line_count[line] += eff ? 1 : -1;
    update_line(p, line);
}

static uint64_t ecam_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    uint32_t bus = (uint32_t)(off >> 20), slot = (off >> 15) & 31, fn = (off >> 12) & 7, reg = off & 0xfff;
    HvPciDev *d = bus == 0 && fn == 0 ? p->dev[slot] : NULL;
    if (!d) return size == 8 ? ~0ull : (1ull << (size * 8)) - 1;
    uint64_t v = 0;
    memcpy(&v, d->config + reg, size > 4096 - reg ? 4096 - reg : size);
    return v;
}

static void ecam_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    uint32_t bus = (uint32_t)(off >> 20), slot = (off >> 15) & 31, fn = (off >> 12) & 7, reg = off & 0xfff;
    HvPciDev *d = bus == 0 && fn == 0 ? p->dev[slot] : NULL;
    if (!d) return;
    uint16_t old_cmd = get16(d->config + PCI_COMMAND);
    for (unsigned i = 0; i < size && reg + i < 256; i++) {
        uint8_t w = d->wmask[reg + i], b = (uint8_t)(v >> (i * 8));
        d->config[reg + i] = (uint8_t)((d->config[reg + i] & ~w) | (b & w));
    }
    if ((old_cmd ^ get16(d->config + PCI_COMMAND)) & CMD_INTX_DISABLE) {
        int level = (get16(d->config + PCI_STATUS) & STATUS_INTX) != 0;
        hv_pci_set_intx(d, level);
    }
}

/* The MMIO window: find the BAR that claims the address. */
static HvPciDev *bar_lookup(HvPcie *p, uint64_t pa, int *bar, uint64_t *off) {
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        HvPciDev *d = p->dev[s];
        if (!d || !(get16(d->config + PCI_COMMAND) & CMD_MEMORY)) continue;
        for (int b = 0; b < 6; b++) {
            if (!d->bar_size[b]) continue;
            uint64_t base = hv_pci_bar_addr(d, b);
            if (base && pa - base < d->bar_size[b]) {
                *bar = b;
                *off = pa - base;
                return d;
            }
        }
    }
    return NULL;
}

static uint64_t window_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    int bar;
    uint64_t boff;
    HvPciDev *d = bar_lookup(p, p->mmio_base + off, &bar, &boff);
    if (!d || !d->bar_read) return ~0ull;
    return d->bar_read(d, bar, boff, size);
}

static void window_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    int bar;
    uint64_t boff;
    HvPciDev *d = bar_lookup(p, p->mmio_base + off, &bar, &boff);
    if (d && d->bar_write) d->bar_write(d, bar, boff, v, size);
}

int hv_pcie_attach(HvPcie *p, HvPciDev *d) {
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        if (p->dev[s]) continue;
        p->dev[s] = d;
        d->bus = p;
        d->slot = s;
        return s;
    }
    return -1;
}

HvPcie *hv_pcie_create(HvMachine *m, HvGic *gic, uint64_t ecam_pa, uint64_t ecam_size, uint64_t mmio_pa,
                       uint64_t mmio_size, int intx_intid) {
    HvPcie *p = calloc(1, sizeof *p);
    p->m = m;
    p->gic = gic;
    p->intx_base = intx_intid;
    p->mmio_base = mmio_pa;
    p->mmio_size = mmio_size;
    hv_pci_init(&p->bridge, 0x1b36, 0x0008, 0x06000000, 0x1af4, 0x1100); /* QEMU PCIe host bridge */
    hv_pcie_attach(p, &p->bridge);
    hv_machine_add_mmio(m, ecam_pa, ecam_size, p, ecam_read, ecam_write, "pcie-ecam");
    hv_machine_add_mmio(m, mmio_pa, mmio_size, p, window_read, window_write, "pcie-mmio");
    return p;
}
