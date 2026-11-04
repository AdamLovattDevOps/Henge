#ifndef HENGEVM_INSTALL_H
#define HENGEVM_INSTALL_H

#include <stdint.h>

#include "virtio_usb.h"

/* Progress steps are idevicerestore's RESTORE_STEP_* (0 detect, 1 prepare,
   2 upload filesystem, 3 verify, 4 flash firmware, ...), then one of these. */
#define HV_INSTALL_DONE 100
#define HV_INSTALL_FAILED 101
typedef void (*HvInstallProgress)(int step, double progress, void *user);

/* Restore the IPSW onto the machine whose USB port is `usb` (an installer
   boot: the ROM in DFU mode). workdir holds the restore's scratch files.
   Runs on its own threads; returns at once (0, or -1). */
int hv_install_start(HvVirtioUsb *usb, const char *ipsw, const char *workdir, uint64_t ecid, HvInstallProgress cb,
                     void *user);
/* 1 while running, 0 when it succeeded, -1 when it failed. */
int hv_install_result(void);

#endif
