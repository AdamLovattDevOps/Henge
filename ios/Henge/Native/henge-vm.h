/* The engine (HengeVM, ../host/vm.h) as the Swift side sees it. */
#ifndef HENGE_VM_H
#define HENGE_VM_H

#include <stddef.h>
#include <stdint.h>

/* Builds the machine from imported files, on the JIT region the app holds
   (henge_ios_jit_prewarm), and starts it. 0, or -1 with a reason in err. */
int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                  int cpus, char *err, size_t errlen);
/* Installs macOS from an IPSW onto a new machine (blank disk and auxiliary
   storage): boots the ROM in DFU mode and runs the restore over its USB port,
   as Virtualization.framework's installer does. workdir is scratch space.
   0, or -1 with a reason in err; progress below. */
int henge_vm_install(const char *rom, const char *disk, const char *aux, uint64_t ecid, const char *ipsw,
                     const char *workdir, uint64_t ram_mb, int cpus, char *err, size_t errlen);
/* The restore's step (0 detect, 1 prepare, 2 system image, 3 verify, 4 firmware,
   6 FUD, 7 personalized images; 100 done, 101 failed) and how far it is (0..1). */
int henge_install_step(void);
double henge_install_progress(void);
/* Writes a blank auxiliary storage file (what VZMacAuxiliaryStorage makes). 0 or -1. */
int henge_make_aux(const char *path);

/* 0 not started, 1 running, 2 stopped. */
int henge_vm_state(void);
/* Why it stopped (an HV_EXIT_* name), once state is 2. */
const char *henge_vm_exit_reason(void);
void henge_vm_stop(void);

/* Progress, for the session screen. */
uint64_t henge_vm_blocks(void);            /* blocks the CPUs entered */
uint64_t henge_vm_net_frames(int sent);    /* Ethernet frames out (sent = 1) and in */
int henge_vm_cpus_on(void);                /* CPUs the guest has started */
int henge_vm_guest_el0(void);              /* any CPU running user code right now */

/* Input: Linux key codes, relative pointer, buttons 0 left 1 right 2 middle. */
void henge_vm_key(uint16_t code, int down);
void henge_vm_mouse_move(int dx, int dy);
void henge_vm_mouse_button(int button, int down);

/* The boot framebuffer (Apple logo and progress bar), 32-bit BGRA, or -1 if
   the kernel has not got that far. The pixels are the guest's live memory. */
/// Where the Mac's GPU frames go: a CAMetalLayer. Keep it alive while the machine runs.
void henge_vm_set_display_layer(void *layer);
/// How many frames have gone to it.
uint64_t henge_vm_gpu_frames(void);
int henge_vm_boot_display(const uint8_t **pixels, int *width, int *height, int *stride);

/* The largest single mapping the address space takes right now, in MB (the
   increased virtual addressing entitlement raises it). */
uint64_t henge_largest_mapping_mb(void);
/* Bytes this process may still allocate before jetsam (os_proc_available_memory). */
size_t henge_available_memory(void);

#endif
