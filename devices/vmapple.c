#include "vmapple.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "timer.h"

#define ROM_SIZE 0x00100000ull

/* pvpanic (MMIO): reads give the events supported, as QEMU's default
   (panicked, crash loaded); a write reports one. */
static uint64_t pvpanic_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque, (void)off, (void)size;
    return 3;
}

static void pvpanic_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)off, (void)size;
    HvVmapple *b = opaque;
    b->panic_events = v;
    fprintf(stderr, "hengevm: guest reported a panic (pvpanic event %llx)\n", (unsigned long long)v);
    if (v & 1) b->cpu->halted = 4;
}

/* Not in QEMU's vmapple, which targets macOS 12 guests. iBootStage2 (Sequoia)
   reads 0x30240040 and uses its PL031 driver ("pl031-rtc") if the value is
   0xf001, or an Apple virtual RTC ("avp-rtc") otherwise. HengeVM has a PL031,
   so it answers 0xf001; everything else here is logged so unknown use shows up. */
static uint64_t rtc_select_read(void *opaque, uint64_t off, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x40) return 0xf001;
    if (b->m->log) fprintf(stderr, "hengevm: read of unknown register 0x%llx+%llx (size %u)\n",
                           (unsigned long long)VMAPPLE_RTC_SELECT, (unsigned long long)off, size);
    return 0;
}

static void rtc_select_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvVmapple *b = opaque;
    if (b->m->log) fprintf(stderr, "hengevm: write of unknown register 0x%llx+%llx = %llx (size %u)\n",
                           (unsigned long long)VMAPPLE_RTC_SELECT, (unsigned long long)off, (unsigned long long)v, size);
}

static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    HvVmapple *b = m->tick_opaque;
    uint64_t deadline = hv_timer_update(b->gic, cpu);
    hv_pl031_tick(b->rtc);
    hv_gicv3_update(b->gic);
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

    b->gic = hv_gicv3_create(m, VMAPPLE_GIC_DIST, VMAPPLE_GIC_REDIST);
    b->uart = hv_pl011_create(m, b->gic, VMAPPLE_UART, 32 + VMAPPLE_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, VMAPPLE_RTC, 32 + VMAPPLE_IRQ_RTC);
    b->gpio = hv_pl061_create(m, b->gic, VMAPPLE_GPIO, 32 + VMAPPLE_IRQ_GPIO);
    b->bdif = hv_bdif_create(m, VMAPPLE_BDOOR, root, aux);
    b->aes = hv_aes_create(m, b->gic, VMAPPLE_AES_1, VMAPPLE_AES_2, 32 + VMAPPLE_IRQ_AES);
    hv_machine_add_mmio(m, VMAPPLE_PVPANIC, 2, b, pvpanic_read, pvpanic_write, "pvpanic");
    hv_machine_add_mmio(m, VMAPPLE_RTC_SELECT, 0x4000, b, rtc_select_read, rtc_select_write, "rtc-select");
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
