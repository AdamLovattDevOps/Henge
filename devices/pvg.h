#ifndef HENGEVM_PVG_H
#define HENGEVM_PVG_H

#include "vmapple.h"

/* Mac-only: backs the vmapple gfx and iosurface-mapper devices with
   ParavirtualizedGraphics.framework, as QEMU's apple-gfx does. iOS has no such
   framework; this exists so the boot can proceed on the Mac, and so the
   device's behaviour can be observed for HengeVM's own implementation later.
   screenshot: PNG path written every few seconds while frames arrive (or NULL).
   Call before the main thread enters its run loop: the framework delivers
   display events on the main queue, so the CPU must run on another thread.
   Returns 0 on success. */
int hv_pvg_attach(HvVmapple *b, const char *screenshot);

/* Called from the CPU thread: delivers interrupts the framework raised. */
void hv_pvg_poll(void);

#endif
