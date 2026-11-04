#include "vmapple.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

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
    /* The panic text, wherever the kernel formatted it (pram is not always
       where the log ends up, e.g. in the restore OS). */
    const char key[] = "panic(cpu ";
    for (uint8_t *p = b->ram, *end = b->ram + b->ram_size; p && p < end;) {
        p = memmem(p, (size_t)(end - p), key, sizeof key - 1);
        if (!p) break;
        size_t n = 0;
        while (p + n < end && n < 3000 && (p[n] == '\n' || p[n] == '\t' || (p[n] >= 32 && p[n] < 127))) n++;
        if (n > 80) {
            fprintf(stderr, "hengevm: panic text at PA %llx:\n%.*s\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)(p - b->ram)), (int)n, p);
            break;
        }
        p += sizeof key - 1;
    }
    if (v & 1)
        for (int i = 0; i < b->ncpus; i++) b->cpu[i].halted = 4;
}

/* Not in QEMU's vmapple, which targets macOS 12 guests. iBootStage2 (Sequoia)
   reads 0x30240040 and uses its PL031 driver ("pl031-rtc") if the value is
   0xf001, or an Apple virtual RTC ("avp-rtc") otherwise. HengeVM has a PL031,
   so it answers 0xf001. The kernel's AppleVirtualPlatformRTC uses the same
   window: +0 is a 64-bit microsecond counter, and wall time is the counter
   plus an offset the driver keeps in NVRAM ("AvpRtc", absent until the guest
   sets the clock), so the counter is Unix time in microseconds. +0x18 bit 0
   enables it, +0x20 is the interrupt control and +0x38 the alarm; they are
   plain storage here (the driver arms no alarm). Anything else is logged. */
static uint64_t rtc_select_read(void *opaque, uint64_t off, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x40) return 0xf001;
    if (off < 0x40 && !(off & 7) && off) return b->avp_rtc[off / 8];
    if (off == 0 || off == 4) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000;
        return off ? us >> 32 : size == 4 ? (uint32_t)us : us;
    }
    if (b->m->log) fprintf(stderr, "hengevm: read of unknown register 0x%llx+%llx (size %u)\n",
                           (unsigned long long)VMAPPLE_RTC_SELECT, (unsigned long long)off, size);
    return 0;
}

static void rtc_select_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvVmapple *b = opaque;
    if (off && off < 0x40 && !(off & 7)) {
        b->avp_rtc[off / 8] = v;
        return;
    }
    if (b->m->log) fprintf(stderr, "hengevm: write of unknown register 0x%llx+%llx = %llx (size %u)\n",
                           (unsigned long long)VMAPPLE_RTC_SELECT, (unsigned long long)off, (unsigned long long)v, size);
}

/* The guest's device tree gives the GIC a third region (gic,vmapple1), and
   the kernel's AppleVirtualPlatformPCIEMSIController asserts that the type
   register at +0x8 has bit 31 ("valid") set. It splits the rest into two
   13-bit fields and uses +0x40 as the doorbell: an MSI frame in the style of
   GICv2m, so it follows that layout: base INTID in bits 28..16, count in bits
   12..0, and a write of an INTID to +0x40 makes it pending. */
static uint64_t gic_msi_read(void *opaque, uint64_t off, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x8) return 1u << 31 | (uint32_t)VMAPPLE_MSI_BASE << 16 | VMAPPLE_MSI_COUNT;
    if (b->m->log) fprintf(stderr, "hengevm: gic msi frame read +%llx (size %u)\n", (unsigned long long)off, size);
    return 0;
}

static void gic_msi_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x40) {
        if (v >= VMAPPLE_MSI_BASE && v < VMAPPLE_MSI_BASE + VMAPPLE_MSI_COUNT) hv_gicv3_pend(b->gic, (int)v);
        else if (b->m->log) fprintf(stderr, "hengevm: msi doorbell for INTID %llu outside the frame\n", (unsigned long long)v);
        return;
    }
    if (b->m->log) fprintf(stderr, "hengevm: gic msi frame write +%llx = %llx (size %u)\n", (unsigned long long)off,
                           (unsigned long long)v, size);
}

/* Paravirtualized graphics (gfx, iosurface-mapper). QEMU's apple-gfx passes
   these to ParavirtualizedGraphics.framework, which iOS does not have, and the
   protocol is not documented. Until there is a display path the device is
   present but inert: reads give 0, writes are dropped, everything is logged. */
static uint64_t pvg_read(void *opaque, uint64_t off, unsigned size) {
    HvVmapple *b = opaque;
    if (b->m->log) fprintf(stderr, "hengevm: pvg read +%llx (size %u) pc %llx\n", (unsigned long long)off, size,
                           (unsigned long long)b->m->access_pc);
    /* Experiment (HV_GFX_ABSENT=1): read like a bus with nothing on it. */
    static int absent = -1;
    if (absent < 0) absent = getenv("HV_GFX_ABSENT") != NULL;
    return absent ? (size >= 8 ? ~0ull : (1ull << (size * 8)) - 1) : 0;
}

static void pvg_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvVmapple *b = opaque;
    if (b->m->log) fprintf(stderr, "hengevm: pvg write +%llx = %llx (size %u)\n", (unsigned long long)off,
                           (unsigned long long)v, size);
}

static uint64_t tick_locked(HvMachine *m, HvCpu *cpu) {
    HvVmapple *b = m->tick_opaque;
    uint64_t deadline = hv_timer_update(b->gic, cpu);
    if (cpu->index != 0) return deadline; /* the others only have their own timers to look after */
    hv_pl031_tick(b->rtc);
    if (b->poll) b->poll();
    hv_virtio_blk_poll(b->root_blk);
    hv_virtio_blk_poll(b->aux_blk);
    hv_xhci_poll(b->usb);
    hv_virtio_input_poll(b->keyboard);
    hv_virtio_input_poll(b->mouse);
    hv_virtio_snd_poll(b->snd);
    hv_virtio_usb_poll(b->vusb);
    hv_bdif_poll(b->bdif);
    return deadline; /* every GIC state change already updated the CPU's lines */
}

/* Devices and the GIC are shared by the CPUs: one at a time. */
static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    if (cpu->index != 0) return hv_timer_update(((HvVmapple *)m->tick_opaque)->gic, cpu); /* its own timer: the GIC's lock only */
    hv_dev_lock(m);
    uint64_t deadline = tick_locked(m, cpu);
    hv_dev_unlock(m);
    return deadline;
}

int hv_vmapple_create(HvVmapple *b, HvMachine *m, HvCpu *cpu, uint64_t ram_size, HvDisk *root, HvDisk *aux,
                      const HvCfgParams *cfg, const uint8_t mac[6]) {
    memset(b, 0, sizeof *b);
    b->m = m;
    b->cpu = cpu;
    b->ncpus = cfg->nr_cpus ? (int)cfg->nr_cpus : 1;
    if (b->ncpus > HV_MAX_CPUS) b->ncpus = HV_MAX_CPUS;
    b->ram_size = ram_size;
    b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED) return -1;
    b->rom = mmap(NULL, ROM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->rom == MAP_FAILED) return -1;
    /* QEMU maps the firmware as RAM, and so do we. */
    hv_machine_add_ram(m, VMAPPLE_FIRMWARE, ROM_SIZE, b->rom, 0);
    hv_machine_add_ram(m, VMAPPLE_MEM, ram_size, b->ram, 0);

    b->gic = hv_gicv3_create(m, VMAPPLE_GIC_DIST, VMAPPLE_GIC_REDIST);
    hv_machine_add_mmio(m, VMAPPLE_GIC_MSI, 0x10000, b, gic_msi_read, gic_msi_write, "gic-msi");
    b->uart = hv_pl011_create(m, b->gic, VMAPPLE_UART, 32 + VMAPPLE_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, VMAPPLE_RTC, 32 + VMAPPLE_IRQ_RTC);
    b->gpio = hv_pl061_create(m, b->gic, VMAPPLE_GPIO, 32 + VMAPPLE_IRQ_GPIO);
    b->bdif = hv_bdif_create(m, VMAPPLE_BDOOR, root, aux);
    b->aes = hv_aes_create(m, b->gic, VMAPPLE_AES_1, VMAPPLE_AES_2, 32 + VMAPPLE_IRQ_AES);
    hv_machine_add_mmio(m, VMAPPLE_PVPANIC, 2, b, pvpanic_read, pvpanic_write, "pvpanic");
    hv_machine_add_mmio(m, VMAPPLE_GFX, 0x10000, b, pvg_read, pvg_write, "gfx");
    hv_machine_add_mmio(m, VMAPPLE_IOSFC, 0x10000, b, pvg_read, pvg_write, "iosurface");
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
    b->usb = hv_xhci_create(m, b->pcie);
    /* HV_INPUT_ORDER (experiment): "keyboard" = keyboard only, "swap" = mouse first. */
    const char *order = getenv("HV_INPUT_ORDER");
    if (order && !strcmp(order, "swap")) b->mouse = hv_virtio_input_create(m, b->pcie, HV_VINPUT_MOUSE);
    b->keyboard = hv_virtio_input_create(m, b->pcie, HV_VINPUT_KEYBOARD);
    if (!order) b->mouse = hv_virtio_input_create(m, b->pcie, HV_VINPUT_MOUSE);
    if (!getenv("HV_NO_SOUND")) b->snd = hv_virtio_snd_create(m, b->pcie);
    /* Installer boots (the ROM goes to DFU) get the USB device port, as VZ gives it. */
    if (cfg->run_installer1 || cfg->run_installer2) {
        b->vusb = hv_virtio_usb_create(m, b->pcie);
        b->bdif->usb = b->vusb;
    }

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
    for (int i = 0; i < b->ncpus; i++) {
        HvCpu *c = &b->cpu[i];
        hv_cpu_reset_index(c, b->m, (uint32_t)i);
        c->mpidr = 0x80000000u | (uint32_t)i; /* Aff0 = the config's cpu_ids[i], bit 31 RES1 */
        c->pc = VMAPPLE_FIRMWARE;
        c->off = i != 0; /* secondaries wait for PSCI CPU_ON */
    }
}
