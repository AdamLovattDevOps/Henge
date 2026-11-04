#include "henge-vm.h"

#include <os/proc.h>
#include <stdio.h>
#include <string.h>

#include "dispatch.h"
#include "henge-ios-jit.h"
#include "net_slirp.h"
#include "vm.h"

HENGE_EXPORT HengeDualMapping *henge_ios_jit_get_mapping(void);

static volatile int state;
static volatile int exit_reason;

static void on_exit(int reason) {
    exit_reason = reason;
    state = 2;
    fprintf(stderr, "henge: the machine stopped (%s)\n", hv_vm_reason_name(reason));
}

int henge_vm_boot(const char *rom, const char *disk, const char *aux, uint64_t ecid, uint64_t seed, uint64_t ram_mb,
                  int cpus, char *err, size_t errlen) {
    if (state) {
        snprintf(err, errlen, "A machine has already run in this session. Quit Henge and open it again to start another.");
        return -1;
    }
    HengeDualMapping *jit = henge_ios_jit_get_mapping();
    if (!jit || !jit->rw_addr) {
        snprintf(err, errlen, "Henge has no JIT region. Turn on JIT first.");
        return -1;
    }
    HvVmConfig c = {.rom = rom, .disk = disk, .aux = aux, .ecid = ecid, .seed = seed, .ram_mb = ram_mb, .ncpus = cpus, .log = 1,
                    .jit_rx = jit->rx_addr, .jit_rw = jit->rw_addr, .jit_bytes = jit->size};
    fprintf(stderr, "henge: booting with %d CPUs, %llu MB RAM, a %zu MB JIT region\n", cpus, (unsigned long long)ram_mb,
            jit->size >> 20);
    if (hv_vm_create(&c, err, errlen) != 0) return -1;
    state = 1;
    hv_vm_start(1, on_exit);
    return 0;
}

int henge_vm_state(void) { return state; }
const char *henge_vm_exit_reason(void) { return hv_vm_reason_name(exit_reason); }
void henge_vm_stop(void) { if (state == 1) hv_vm_stop(); }

uint64_t henge_vm_blocks(void) { return state ? hv_vm_machine()->stats.blocks : 0; }
uint64_t henge_vm_net_frames(int sent) { return sent ? hv_net_tx_frames : hv_net_rx_frames; }

int henge_vm_cpus_on(void) {
    int n = 0;
    for (int i = 0; i < hv_vm_ncpus(); i++)
        if (!hv_vm_cpu(i)->off) n++;
    return state ? n : 0;
}

int henge_vm_guest_el0(void) {
    for (int i = 0; state && i < hv_vm_ncpus(); i++)
        if (!hv_vm_cpu(i)->off && hv_vm_cpu(i)->el == 0) return 1;
    return 0;
}

void henge_vm_key(uint16_t code, int down) { if (state == 1) hv_vm_key(code, down); }
void henge_vm_mouse_move(int dx, int dy) { if (state == 1) hv_vm_mouse_move(dx, dy); }
void henge_vm_mouse_button(int button, int down) { if (state == 1) hv_vm_mouse_button(button, down); }

size_t henge_available_memory(void) { return os_proc_available_memory(); }
