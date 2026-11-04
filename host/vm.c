#include "vm.h"

#include <TargetConditionals.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdio.h>

#include "usb_host.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_mac.h"
#include "dispatch.h"
#include "net_slirp.h"

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS]; /* the board takes them as an array */
static int ncpus = 1;
static HvVmapple board;
static HvDisk disk, aux;
static int net_on;
static void (*extra_poll)(void);
static void (*exit_cb)(int);

HvMachine *hv_vm_machine(void) { return &m; }
HvVmapple *hv_vm_board(void) { return &board; }
HvCpu *hv_vm_cpu(int index) { return index >= 0 && index < ncpus ? &cpus[index] : NULL; }
int hv_vm_ncpus(void) { return ncpus; }
void hv_vm_set_poll(void (*poll)(void)) { extra_poll = poll; }

const char *hv_vm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_FAULT_LOOP: return "fault on the exception vector";
    default: return "other";
    }
}

/* Host-side device work on CPU 0's thread each tick. */
static void board_poll(void) {
    if (extra_poll) extra_poll();
    if (net_on) hv_net_poll();
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

int hv_vm_create(const HvVmConfig *c, char *err, size_t errlen) {
    if (!c->rom || !c->disk || !c->aux) return fail(err, errlen, "The ROM, disk and auxiliary storage are all needed.");
    if (hv_disk_open(&disk, c->disk, 0, 0) != 0) return fail(err, errlen, "Cannot open the disk image.");
    if (hv_disk_open(&aux, c->aux, c->aux_offset ? c->aux_offset : 16384, 0) != 0)
        return fail(err, errlen, "Cannot open the auxiliary storage.");
    size_t jit = c->jit_bytes ? c->jit_bytes : (size_t)768 << 20;
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot set up the JIT region.");
    m.log = c->log;
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK"); /* debugging: one CPU in guest code at a time */
    HvCfgParams cfg = {.nr_cpus = (uint32_t)ncpus, .ecid = c->ecid, .seed = c->seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    if (c->installer) cfg.run_installer1 = cfg.run_installer2 = 1;
    if (getenv("HV_RUN_INSTALLER")) { /* experiment: "a,b" for the two installer words */
        unsigned a = 0, b2 = 0;
        sscanf(getenv("HV_RUN_INSTALLER"), "%u,%u", &a, &b2);
        cfg.run_installer1 = a, cfg.run_installer2 = b2;
    }
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    if (hv_vmapple_create(&board, &m, cpus, (c->ram_mb ? c->ram_mb : 2048) << 20, &disk, &aux, &cfg, mac) != 0)
        return fail(err, errlen, "Cannot build the machine (not enough memory for the guest's RAM?).");
    if (hv_vmapple_load_rom(&board, c->rom) != 0) return fail(err, errlen, "Cannot read the ROM (AVPBooter.vmapple2.bin).");
    hv_vmapple_reset_cpu(&board);
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) != 0) fprintf(stderr, "hengevm: user-mode network unavailable\n");
        else net_on = 1;
    }
    if (board.snd && !c->no_sound) hv_audio_attach(board.snd);
    board.poll = board_poll;
    if (board.vusb && getenv("HV_VUSB_PROBE")) hv_usb_probe_start(board.vusb);
    return 0;
}

static void *kicker_thread(void *unused) {
    (void)unused;
    for (;;) {
        usleep(1000);
        hv_machine_kick_all(&m);
    }
    return NULL;
}

/* A thread that runs guest code. While a block runs its SP register holds the
   guest's SP, a guest address, so the kernel must never push a signal frame
   there: other threads take the asynchronous signals, and a handler that does
   run here (a fault) gets a stack of its own. */
void hv_vm_prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGTRAP), sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* iOS parks default-QoS threads on efficiency cores readily; the guest's CPUs are what the user waits on. */
void hv_vm_cpu_qos(void) {
    hv_vm_prepare_cpu_thread();
    /* HV_QOS=ui|init|default (testing): the class the guest's CPUs run at. */
    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_DEFAULT;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
}

/* CPUs 1 and up: run whenever PSCI CPU_ON starts them; the machine stops with CPU 0. */
static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    hv_vm_cpu_qos();
    int r = hv_run(c);
    if (!c->halted)
        fprintf(stderr, "hengevm: cpu %u stopped (%s) at pc %llx el%u\n", c->index, hv_vm_reason_name(r),
                (unsigned long long)c->pc, c->el);
    return NULL;
}

void hv_vm_flush_disks(void) {
    hv_disk_flush(&disk);
    hv_disk_flush(&aux);
}

int hv_vm_run_cpu0(void) {
    return hv_run(&cpus[0]);
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    hv_vm_cpu_qos();
    int r = hv_vm_run_cpu0();
    hv_vm_flush_disks();
    if (exit_cb) exit_cb(r);
    return NULL;
}

void hv_vm_start(int cpu0, void (*on_exit)(int reason)) {
    exit_cb = on_exit;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t t;
    if (cpu0) pthread_create(&t, &attr, cpu0_thread, NULL);
    for (int i = 1; i < ncpus; i++) pthread_create(&t, &attr, secondary_thread, &cpus[i]);
    pthread_create(&t, NULL, kicker_thread, NULL);
}

void hv_vm_stop(void) {
    for (int i = 0; i < ncpus; i++) cpus[i].halted = 3; /* the dispatchers stop at the next block */
    hv_machine_kick_all(&m);
    for (int i = 0; i < ncpus; i++) hv_cpu_wake(&cpus[i]);
}

void hv_vm_key(uint16_t code, int down) {
    hv_xhci_key_linux(board.usb, code, down);
}

void hv_vm_mouse_move(int dx, int dy) {
    if (dx) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, dx);
    if (dy) hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, dy);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

void hv_vm_mouse_button(int button, int down) {
    static const uint16_t codes[3] = {HV_BTN_LEFT, HV_BTN_RIGHT, HV_BTN_MIDDLE};
    if (button < 0 || button > 2) return;
    hv_virtio_input_send(board.mouse, HV_EV_KEY, codes[button], down);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

void hv_vm_mouse_wheel(int dy) {
    hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_WHEEL, dy);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}
