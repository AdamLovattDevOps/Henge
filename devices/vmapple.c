#include "vmapple.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "timer.h"

#define ROM_SIZE 0x00100000ull

static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    HvVmapple *b = m->tick_opaque;
    uint64_t deadline = hv_timer_update(b->gic, cpu);
    hv_pl031_tick(b->rtc);
    hv_gic_update(b->gic);
    return deadline;
}

int hv_vmapple_create(HvVmapple *b, HvMachine *m, HvCpu *cpu, uint64_t ram_size, HvDisk *root, HvDisk *aux,
                      const HvCfgParams *cfg, const uint8_t mac[6]) {
    memset(b, 0, sizeof *b);
    b->m = m;
    b->cpu = cpu;
    b->ram_size = ram_size;
    b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED) return -1;
    b->rom = mmap(NULL, ROM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->rom == MAP_FAILED) return -1;
    /* QEMU maps the firmware as RAM, and so do we. */
    hv_machine_add_ram(m, VMAPPLE_FIRMWARE, ROM_SIZE, b->rom, 0);
    hv_machine_add_ram(m, VMAPPLE_MEM, ram_size, b->ram, 0);

    b->gic = hv_gic_create(m, VMAPPLE_GIC_DIST, VMAPPLE_GIC_REDIST);
    b->uart = hv_pl011_create(m, b->gic, VMAPPLE_UART, 32 + VMAPPLE_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, VMAPPLE_RTC, 32 + VMAPPLE_IRQ_RTC);
    b->bdif = hv_bdif_create(m, VMAPPLE_BDOOR, root, aux);
    HvCfgParams p = *cfg;
    p.ram_size = ram_size;
    b->cfg = hv_cfg_create(m, VMAPPLE_CONFIG, &p);
    b->pcie = hv_pcie_create(m, b->gic, VMAPPLE_PCIE_ECAM, 0x10000000, VMAPPLE_PCIE_MMIO, 0x1fff0000,
                             32 + VMAPPLE_IRQ_PCIE);
    /* Same order as QEMU's documented command line: aux, root, then the NIC. */
    if (aux) b->aux_blk = hv_virtio_blk_create(m, b->pcie, aux, HV_VBLK_AUX);
    if (root) b->root_blk = hv_virtio_blk_create(m, b->pcie, root, HV_VBLK_ROOT);
    if (mac) b->net = hv_virtio_net_create(m, b->pcie, mac);

    m->tick = tick;
    m->tick_opaque = b;
    cpu->m = m;
    return 0;
}

int hv_vmapple_load_rom(HvVmapple *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(b->rom, 1, ROM_SIZE, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return n == 0 || more ? -1 : 0;
}

void hv_vmapple_reset_cpu(HvVmapple *b) {
    hv_cpu_reset(b->cpu, b->m);
    b->cpu->mpidr = 0x80000000; /* Aff0 = 0, bit 31 RES1 */
    b->cpu->pc = VMAPPLE_FIRMWARE;
}
