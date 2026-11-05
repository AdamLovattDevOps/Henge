#include <time.h>
/* hengevm: boots the virtual Mac on the macOS host.
   hengevm --rom AVPBooter.vmapple2.bin --disk disk.img --aux auxiliary.bin --ecid N [--seed N] [--ram MB] [--log N] */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "exception.h"
#include "mmu.h"
#include "pvg.h"
#include "pvg_lite.h"
#include "vm.h"
#include "install.h"

/* The machine lives in vm.c; these name its parts here. */
#define m (*hv_vm_machine())
#define board (*hv_vm_board())
#define cpu (*hv_vm_cpu(0))
static int ncpus = 3; /* --cpus: 3 boots as fast as 4 on a 2+4-core Mac and leaves room for the window */
static volatile sig_atomic_t interrupted;

static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
    hv_vm_stop();
}

#define reason_name hv_vm_reason_name

typedef struct RunArgs {
    uint64_t brk, brk_hits, dump_pa, dump_len;
    const char *dump_path;
} RunArgs;
static RunArgs run_args;
static uint64_t translate_va;

/* Chained blocks run without returning to the dispatcher; this makes them leave
   every millisecond so timers, devices and interrupts are serviced. */
static mach_port_t cpu_port;
/* --profile: where the CPU thread is right now. */
static void sample_cpu(void) {
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_suspend(cpu_port) != KERN_SUCCESS) return;
    kern_return_t kr = thread_get_state(cpu_port, ARM_THREAD_STATE64, (thread_state_t)&st, &n);
    thread_resume(cpu_port);
    if (kr != KERN_SUCCESS) return;
    m.prof[m.prof_head & (HV_PROF_RING - 1)] = (uint64_t)arm_thread_state64_get_pc(st);
    __atomic_store_n(&m.prof_head, m.prof_head + 1, __ATOMIC_RELEASE);
}

static void *profile_thread(void *unused) {
    (void)unused;
    for (;;) {
        usleep(1000);
        if (cpu_port) sample_cpu();
    }
    return NULL;
}

/* Debugging: HV_CLICKS="seconds:x,y;seconds:x,y" walks the pointer to a spot on
   the screen (watching where the display says it is) and clicks, to drive a
   guest's UI without a window. */
static void *click_thread(void *unused) {
    (void)unused;
    const char *p = getenv("HV_CLICKS");
    time_t start = time(NULL);
    while (p && *p) {
        int when, x, y, n = 0;
        if (sscanf(p, "%d:%d,%d%n", &when, &x, &y, &n) != 3) break;
        p += n;
        if (*p == ';') p++;
        while (time(NULL) - start < when) sleep(1);
        const uint8_t *page = hv_pvg_lite_display_page(board.pvg);
        for (int i = 0; page && i < 1500; i++) {
            uint32_t at = *(const volatile uint32_t *)(page + 0xe00);
            int dx = x - (int)(at & 0xffff), dy = y - (int)(at >> 16);
            if (abs(dx) < 3 && abs(dy) < 3) break;
            /* Small steps, as a hand makes: macOS accelerates big ones past the target. */
            int sx = dx > 16 ? 16 : dx < -16 ? -16 : dx, sy = dy > 16 ? 16 : dy < -16 ? -16 : dy;
            hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, sx);
            hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, sy);
            hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
            usleep(30000);
        }
        uint32_t at = page ? *(const volatile uint32_t *)(page + 0xe00) : 0;
        fprintf(stderr, "hengevm: click at %u,%u (asked %d,%d) at %ld s\n", at & 0xffff, at >> 16, x, y, (long)(time(NULL) - start));
        hv_virtio_input_send(board.mouse, HV_EV_KEY, HV_BTN_LEFT, 1);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
        usleep(120000);
        hv_virtio_input_send(board.mouse, HV_EV_KEY, HV_BTN_LEFT, 0);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
    }
    return NULL;
}

/* Debugging: HV_TYPE_AT=seconds types "abc" then, to check input end to end without a window. */
static void *type_thread(void *unused) {
    (void)unused;
    sleep((unsigned)atoi(getenv("HV_TYPE_AT")));
    for (int i = 0; i < 60; i++) { /* shake it: macOS enlarges the pointer, so new glyphs get sent */
        hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, i & 1 ? 120 : -120);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
        usleep(15000);
    }
    for (int i = 0; i < 40; i++) { /* the pointer: up and to the left, so it shows */
        hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, -10);
        hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, -6);
        hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
        usleep(20000);
    }
    if (getenv("HV_TYPE_NO_KEYS")) { /* out into the open, for a screenshot */
        for (int i = 0; i < 60; i++) {
            hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, 12);
            hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, 8);
            hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
            usleep(20000);
        }
        return NULL;
    }
    static const uint16_t keys[3] = {30, 48, 46}; /* a, b, c */
    for (int i = 0; i < 3; i++) {
        hv_xhci_key_linux(board.usb, keys[i], 1);
        hv_virtio_input_send(board.keyboard, HV_EV_KEY, keys[i], 1);
        hv_virtio_input_send(board.keyboard, HV_EV_SYN, 0, 0);
        usleep(100000);
        hv_xhci_key_linux(board.usb, keys[i], 0);
        hv_virtio_input_send(board.keyboard, HV_EV_KEY, keys[i], 0);
        hv_virtio_input_send(board.keyboard, HV_EV_SYN, 0, 0);
        usleep(700000);
    }
    for (int i = 0; i < 10; i++) {
        sleep(2);
        fprintf(stderr, "hengevm: typed abc: usb delivered %llu reports, keyboard delivered %llu of 12 events, mouse %llu\n",
                (unsigned long long)board.usb->delivered, (unsigned long long)board.keyboard->delivered, board.mouse ? (unsigned long long)board.mouse->delivered : 0ull);
    }
    return NULL;
}

static void *cpu_thread(void *unused) {
    (void)unused;
    RunArgs *a = &run_args;
    cpu_port = mach_thread_self();
    hv_vm_cpu_qos();
    /* --break PC: stop the brk_hits-th time execution reaches PC. */
    m.stop_pc = a->brk;
    int r;
    for (;;) {
        r = hv_vm_run_cpu0();
        if (r != HV_EXIT_HALT || cpu.halted || cpu.pc != a->brk || --a->brk_hits == 0) break;
        m.stop_pc = ~0ull; /* step past it: run one block, then re-arm */
        uint64_t at = cpu.pc;
        while (cpu.pc == at && !cpu.halted) {
            m.stop_pc = ~0ull;
            hv_run_one(&cpu);
        }
        m.stop_pc = a->brk;
    }
    if (cpu.pc == a->brk) {
        fprintf(stderr, "hengevm: breakpoint %llx, x30 %llx:", (unsigned long long)a->brk, (unsigned long long)cpu.x[30]);
        hv_debug_args(&cpu);
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 8; i++) {
            uint64_t v = 0, pa;
            if (!hv_mmu_access(&cpu, cpu.sp + 8 * i, HV_ACC_READ, cpu.el, &pa)) hv_phys_read(&m, pa, &v, 8);
            fprintf(stderr, " %llx", (unsigned long long)v);
        }
        fprintf(stderr, "\n");
        if (translate_va) {
            uint64_t pa = 0;
            uint32_t fsc = hv_mmu_access(&cpu, translate_va, HV_ACC_READ, cpu.el, &pa);
            fprintf(stderr, "  translate %llx -> pa %llx (fsc %x)\n", (unsigned long long)translate_va,
                    (unsigned long long)pa, fsc);
        }
    }
    fprintf(stderr, "\nhengevm: stopped (%s) after %llu blocks, %d translated\n", interrupted ? "interrupted" : reason_name(r),
            (unsigned long long)cpu.icount, m.nblocks);
    fprintf(stderr, "  pc %016llx el%u sp %016llx insn %08x\n", (unsigned long long)cpu.pc, cpu.el,
            (unsigned long long)cpu.sp, cpu.exit_insn);
    fprintf(stderr, "  esr %08llx far %016llx elr %016llx vbar %016llx sctlr %08llx\n", (unsigned long long)cpu.esr_el1,
            (unsigned long long)cpu.far_el1, (unsigned long long)cpu.elr_el1, (unsigned long long)cpu.vbar_el1,
            (unsigned long long)cpu.sctlr_el1);
    for (int i = 0; i < 31; i += 4)
        fprintf(stderr, "  x%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)cpu.x[i],
                (unsigned long long)cpu.x[i + 1], (unsigned long long)cpu.x[i + 2],
                (unsigned long long)(i + 3 < 31 ? cpu.x[i + 3] : 0));
    if (a->dump_path) { /* --dump PA LEN FILE: guest physical memory after the run, for reading code */
        FILE *f = fopen(a->dump_path, "wb");
        void *p = hv_phys_ptr(&m, a->dump_pa, a->dump_len);
        if (f && p) fwrite(p, 1, a->dump_len, f);
        if (f) fclose(f);
        fprintf(stderr, "hengevm: %s %llu bytes at %llx to %s\n", p ? "wrote" : "cannot dump", (unsigned long long)a->dump_len,
                (unsigned long long)a->dump_pa, a->dump_path);
    }
    if (getenv("HV_RAM_GREP")) { /* debugging: text lines in guest RAM with any of these words (the kernel's log buffer) */
        HvVmapple *bd = hv_vm_board();
        char words[512];
        snprintf(words, sizeof words, "%s", getenv("HV_RAM_GREP"));
        const char *w[16];
        int nw = 0;
        for (char *t = strtok(words, ","); t && nw < 16; t = strtok(NULL, ",")) w[nw++] = t;
        FILE *out = fopen(getenv("HV_RAM_GREP_OUT") ? getenv("HV_RAM_GREP_OUT") : "/dev/stderr", "w");
        uint64_t hits = 0;
        for (uint64_t i = 0; i < bd->ram_size && out && hits < 20000;) {
            /* a run of printable bytes ending in a newline or NUL */
            uint64_t j = i;
            while (j < bd->ram_size && j - i < 1024 && (bd->ram[j] >= 32 && bd->ram[j] < 127)) j++;
            if (j - i >= 12) {
                for (int k = 0; k < nw; k++)
                    if (memmem(bd->ram + i, j - i, w[k], strlen(w[k]))) {
                        fprintf(out, "%llx: %.*s\n", (unsigned long long)(0x70000000ull + i), (int)(j - i), bd->ram + i);
                        hits++;
                        break;
                    }
            }
            i = j + 1;
        }
        if (out) fclose(out);
        fprintf(stderr, "hengevm: RAM grep: %llu lines\n", (unsigned long long)hits);
    }
    if (getenv("HV_BOOT_DISPLAY_DUMP")) { /* debugging: the boot framebuffer, raw BGRA, for the iOS view */
        HvBootDisplay bd;
        if (hv_vm_boot_display(&bd) == 0) {
            FILE *f = fopen(getenv("HV_BOOT_DISPLAY_DUMP"), "wb");
            if (f) fwrite(bd.pixels, 1, (size_t)bd.stride * bd.height, f), fclose(f);
            fprintf(stderr, "hengevm: boot display %ux%u stride %u written\n", bd.width, bd.height, bd.stride);
        } else {
            fprintf(stderr, "hengevm: no boot display found\n");
        }
    }
    hv_vm_flush_disks();
    exit(0);
    return NULL;
}

static void usage(void) {
    fprintf(stderr, "usage: hengevm --rom FILE --disk FILE --aux FILE --ecid N [--seed N] [--ram MB] [--log N] [--trace 1] [--screenshot PNG] [--watch VA] [--stats SECONDS [--profile 1]] [--cpus N] [--window] [--no-pvg] [--no-chain] [--break PC [--break-hits N]] [--dump PA LEN FILE] [--aux-offset BYTES] [--install IPSW]\n");
    exit(2);
}

static void install_progress(int step, double p, void *user) {
    (void)user;
    static int last_step = -1, last_pct = -1;
    int pct = (int)(p * 100);
    if (step == last_step && pct / 5 == last_pct / 5) return;
    last_step = step, last_pct = pct;
    if (step == HV_INSTALL_DONE) fprintf(stderr, "hengevm: install: done\n");
    else if (step == HV_INSTALL_FAILED) fprintf(stderr, "hengevm: install: failed\n");
    if ((step == HV_INSTALL_DONE || step == HV_INSTALL_FAILED) && getenv("HV_INSTALL_STOP_AT_END")) {
        sleep(3); /* debugging: stop the machine once the restore has a result (the exit dumps then run) */
        hv_vm_stop();
    }
    else fprintf(stderr, "hengevm: install: step %d %d%%\n", step, pct);
}

int main(int argc, char **argv) {
    const char *rom = NULL, *disk_path = NULL, *aux_path = NULL;
    uint64_t seed = 0, ecid = 0, ram_mb = 4096, aux_offset = 16384;
    const char *install_ipsw = NULL;
    int log = 1, trace = 0, use_pvg = 1, no_chain = 0;
    const char *screenshot = NULL;
    uint64_t watch = 0, watch_pa = 0;
    int stats = 0, profile = 0, window = 0;
    uint64_t brk = ~0ull, brk_hits = 1, dump_pa = 0, dump_len = 0;
    const char *dump_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--window")) {
            window = 1;
            continue;
        }
        if (!strcmp(argv[i], "--phone")) {
            /* Test iOS's setup on the Mac: no graphics framework, split JIT mapping. */
            use_pvg = 0;
            setenv("HV_JIT_SPLIT", "1", 1);
            memmove(&argv[i], &argv[i + 1], (size_t)(argc - i - 1) * sizeof *argv);
            argc--, i--;
            continue;
        }
        if (!strcmp(argv[i], "--no-pvg")) {
            use_pvg = 0;
            continue;
        }
        if (!strcmp(argv[i], "--no-chain")) {
            no_chain = 1;
            continue;
        }
        if (i + 1 >= argc) usage();
        if (!strcmp(argv[i], "--rom")) rom = argv[++i];
        else if (!strcmp(argv[i], "--disk")) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--aux")) aux_path = argv[++i];
        else if (!strcmp(argv[i], "--ecid")) ecid = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ram")) ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--log")) log = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")) trace = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--screenshot")) screenshot = argv[++i];
        else if (!strcmp(argv[i], "--watch")) watch = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--stats")) stats = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile")) profile = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpus")) ncpus = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch-pa")) watch_pa = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--translate")) translate_va = strtoull(argv[++i], NULL, 0);

        else if (!strcmp(argv[i], "--break")) brk = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--break-hits")) brk_hits = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dump") && i + 3 < argc) {
            dump_pa = strtoull(argv[++i], NULL, 0);
            dump_len = strtoull(argv[++i], NULL, 0);
            dump_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--aux-offset")) aux_offset = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--install")) install_ipsw = argv[++i];
        else usage();
    }
    if (!rom || !disk_path || !aux_path) usage();
    setvbuf(stderr, NULL, _IONBF, 0);

    HvVmConfig vc = {.rom = rom, .disk = disk_path, .aux = aux_path, .aux_offset = aux_offset, .ecid = ecid, .seed = seed,
                     .ram_mb = ram_mb, .ncpus = ncpus, .log = log, .no_net = getenv("HV_NO_NET") != NULL,
                     .installer = install_ipsw != NULL};
    if (getenv("HV_JIT_MB")) vc.jit_bytes = (size_t)atoi(getenv("HV_JIT_MB")) << 20; /* debugging: a small region flushes often */
    char err[256];
    if (hv_vm_create(&vc, err, sizeof err) != 0) {
        fprintf(stderr, "hengevm: %s\n", err);
        return 1;
    }
    m.trace = trace;
    m.watch_va = watch;
    m.watch_pa = watch_pa;
    m.stats_interval = stats; /* set after the machine is made, which clears it */
    if (profile) m.prof = calloc(HV_PROF_RING, sizeof *m.prof);
    m.chain = !no_chain && brk == ~0ull; /* breakpoints need every block to come back */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (install_ipsw) {
        char work[1024];
        snprintf(work, sizeof work, "%s.restore", disk_path);
        mkdir(work, 0755);
        if (hv_install_start(board.vusb, install_ipsw, work, ecid, install_progress, NULL) != 0) {
            fprintf(stderr, "hengevm: cannot start the installer\n");
            return 1;
        }
        /* usbmuxd installs its own handlers; take SIGINT and SIGTERM back so
           the machine still stops on them. */
        usleep(500000);
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
    }
    int pvg_on = 0;
    if (use_pvg) {
        if (hv_pvg_attach(&board, screenshot) != 0) fprintf(stderr, "hengevm: ParavirtualizedGraphics device unavailable; graphics stays inert\n");
        else pvg_on = 1;
    }
    if (pvg_on) hv_vm_set_poll(hv_pvg_poll);
    run_args = (RunArgs){brk, brk_hits, dump_pa, dump_len, dump_path};
    /* The CPU runs on its own thread: the graphics framework needs the main queue. */
    hv_vm_start(0, NULL); /* the other CPUs and the kicker; CPU 0 is cpu_thread, for --break */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(&t, &attr, cpu_thread, NULL);
    if (profile) pthread_create(&t, NULL, profile_thread, NULL);
    if (getenv("HV_CLICKS")) {
        pthread_t cl;
        pthread_create(&cl, NULL, click_thread, NULL);
    }
    if (getenv("HV_TYPE_AT")) {
        pthread_t ty;
        pthread_create(&ty, NULL, type_thread, NULL);
    }
    if (window && use_pvg) hv_pvg_window_run();
    CFRunLoopRun();
    return 0;
}
