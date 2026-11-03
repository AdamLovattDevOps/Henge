/* hengevm: boots the virtual Mac on the macOS host.
   hengevm --rom AVPBooter.vmapple2.bin --disk disk.img --aux auxiliary.bin --ecid N [--seed N] [--ram MB] [--log N] */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "exception.h"
#include "mmu.h"
#include "pvg.h"
#include "vmapple.h"

static HvMachine m;
static HvCpu cpu;
static HvVmapple board;
static volatile sig_atomic_t interrupted;

static void on_signal(int sig) {
    (void)sig;
    interrupted = 1;
    cpu.halted = 3; /* the dispatcher stops at the next block */
}

static const char *reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_FAULT_LOOP: return "fault on the exception vector";
    default: return "other";
    }
}

typedef struct RunArgs {
    uint64_t brk, brk_hits, dump_pa, dump_len;
    const char *dump_path;
    HvDisk *disk, *aux;
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

static void *kicker_thread(void *unused) {
    (void)unused;
    for (;;) {
        usleep(1000);
        __atomic_store_n(&cpu.exit_req, 1, __ATOMIC_RELAXED);
        if (m.prof && cpu_port) sample_cpu();
    }
    return NULL;
}

static void *cpu_thread(void *unused) {
    (void)unused;
    RunArgs *a = &run_args;
    cpu_port = mach_thread_self();
    /* --break PC: stop the brk_hits-th time execution reaches PC. */
    m.stop_pc = a->brk;
    int r;
    for (;;) {
        r = hv_run(&cpu);
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
    hv_disk_flush(a->disk);
    hv_disk_flush(a->aux);
    exit(0);
    return NULL;
}

static void usage(void) {
    fprintf(stderr, "usage: hengevm --rom FILE --disk FILE --aux FILE --ecid N [--seed N] [--ram MB] [--log N] [--trace 1] [--screenshot PNG] [--watch VA] [--stats SECONDS [--profile 1]] [--no-pvg] [--no-chain] [--break PC [--break-hits N]] [--dump PA LEN FILE] [--aux-offset BYTES]\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *rom = NULL, *disk_path = NULL, *aux_path = NULL;
    uint64_t seed = 0, ecid = 0, ram_mb = 4096, aux_offset = 16384;
    int log = 1, trace = 0, use_pvg = 1, no_chain = 0;
    const char *screenshot = NULL;
    uint64_t watch = 0, watch_pa = 0;
    int stats = 0, profile = 0;
    uint64_t brk = ~0ull, brk_hits = 1, dump_pa = 0, dump_len = 0;
    const char *dump_path = NULL;
    for (int i = 1; i < argc; i++) {
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
        else usage();
    }
    if (!rom || !disk_path || !aux_path) usage();
    setvbuf(stderr, NULL, _IONBF, 0);

    static HvDisk disk, aux;
    if (hv_disk_open(&disk, disk_path, 0, 0) != 0 || hv_disk_open(&aux, aux_path, aux_offset, 0) != 0) {
        fprintf(stderr, "hengevm: cannot open the disks\n");
        return 1;
    }
    size_t jit_mb = getenv("HV_JIT_MB") ? (size_t)atoi(getenv("HV_JIT_MB")) : 768; /* debugging: a small region flushes often */
    if (hv_machine_init(&m, jit_mb << 20) != 0) {
        fprintf(stderr, "hengevm: cannot map the JIT region\n");
        return 1;
    }
    m.log = log;
    m.trace = trace;
    m.watch_va = watch;
    m.watch_pa = watch_pa;
    m.stats_interval = stats; /* set after hv_machine_init, which clears the machine */
    if (profile) m.prof = calloc(HV_PROF_RING, sizeof *m.prof);
    m.chain = !no_chain && brk == ~0ull; /* breakpoints need every block to come back */
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = ecid, .seed = seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    if (hv_vmapple_create(&board, &m, &cpu, ram_mb << 20, &disk, &aux, &cfg, mac) != 0 ||
        hv_vmapple_load_rom(&board, rom) != 0) {
        fprintf(stderr, "hengevm: cannot build the machine or load the ROM\n");
        return 1;
    }
    hv_vmapple_reset_cpu(&board);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (use_pvg) {
        if (hv_pvg_attach(&board, screenshot) != 0) {
            fprintf(stderr, "hengevm: ParavirtualizedGraphics device unavailable; graphics stays inert\n");
        } else {
            board.poll = hv_pvg_poll;
        }
    }
    run_args = (RunArgs){brk, brk_hits, dump_pa, dump_len, dump_path, &disk, &aux};
    /* The CPU runs on its own thread: the graphics framework needs the main queue. */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(&t, &attr, cpu_thread, NULL);
    pthread_t k;
    pthread_create(&k, NULL, kicker_thread, NULL);
    CFRunLoopRun();
    return 0;
}
