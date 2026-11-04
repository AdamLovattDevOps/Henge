#ifndef HENGEVM_VM_H
#define HENGEVM_VM_H

#include <stddef.h>
#include <stdint.h>

#include "disk.h"
#include "vmapple.h"

/* The virtual Mac as a host app drives it: build it from files, run it, feed
   it input, stop it. The macOS command-line tool and the iOS app both use this;
   graphics are the host's own business (Mac: hv_pvg_attach before starting). */
typedef struct HvVmConfig {
    const char *rom;          /* AVPBooter.vmapple2.bin */
    const char *disk, *aux;   /* the root disk image and the auxiliary storage */
    uint64_t aux_offset;      /* where the aux image's data starts (16 KB) */
    uint64_t ecid, seed;      /* the machine identity the disk was installed with */
    uint64_t ram_mb;
    int ncpus;
    int log;                  /* m->log */
    int no_net, no_sound;
    /* JIT memory the host already has (iOS: granted by a debugger), or NULL to map it here. */
    void *jit_rx, *jit_rw;
    size_t jit_bytes;         /* 0: 768 MB */
} HvVmConfig;

/* Returns 0, or -1 with a reason in err. */
int hv_vm_create(const HvVmConfig *cfg, char *err, size_t errlen);
HvMachine *hv_vm_machine(void);
HvVmapple *hv_vm_board(void);
HvCpu *hv_vm_cpu(int index);
int hv_vm_ncpus(void);

/* Extra device work on CPU 0 each tick (Mac graphics). Set before hv_vm_start. */
void hv_vm_set_poll(void (*poll)(void));

/* Runs CPU 0 on a new thread (unless the caller runs it: cpu0 = 0), the other
   CPUs and the 1 ms kicker. on_exit is called on CPU 0's thread when the
   machine stops (powered off, a fatal error, or hv_vm_stop), after the disks
   are flushed; reason is an HV_EXIT_* value. */
void hv_vm_start(int cpu0, void (*on_exit)(int reason));
/* Call on a thread before it runs guest code (hv_vm_start's threads do): see vm.c. */
void hv_vm_prepare_cpu_thread(void);
/* Run CPU 0 on the calling thread (what hv_vm_start does on its own thread); returns its exit reason. */
int hv_vm_run_cpu0(void);
/* Stop at the next block; the CPUs' threads end. */
void hv_vm_stop(void);
void hv_vm_flush_disks(void);
const char *hv_vm_reason_name(int reason);

/* Input from the host UI, any thread. Keys are Linux input codes (KEY_A = 30). */
void hv_vm_key(uint16_t code, int down);
void hv_vm_mouse_move(int dx, int dy);
void hv_vm_mouse_button(int button, int down); /* 0 left, 1 right, 2 middle */
void hv_vm_mouse_wheel(int dy);

#endif
