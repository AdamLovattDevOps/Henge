#ifndef HENGEVM_JIT_REGION_H
#define HENGEVM_JIT_REGION_H

#include <stddef.h>
#include <stdint.h>

/* One executable region for translated blocks. Blocks are appended and never
   freed one by one; the whole region is reset when it fills.

   Two shapes. macOS: one MAP_JIT mapping, made writable per thread while code
   is copied in (rw_diff is 0). iOS: an executable mapping a debugger granted
   (StikDebug or the built-in helper) plus a writable alias of the same pages
   made with vm_remap; code is written through base + rw_diff and runs at base.
   Everything outside this file and the final copy in the translator sees only
   the executable addresses. HV_JIT_SPLIT=1 makes the macOS build use the iOS
   shape, to test it. */
typedef struct HvJitRegion {
    uint32_t *base;     /* executable addresses */
    intptr_t rw_diff;   /* writable alias = base + rw_diff (bytes), or 0 */
    size_t capacity;    /* in instructions */
    size_t used;        /* in instructions */
    int owned;          /* mapped here (closed here) rather than handed in */
} HvJitRegion;

int hv_jit_region_open(HvJitRegion *region, size_t bytes);
/* Use memory the host already has: rx executable, rw a writable alias of it (or rx itself on macOS MAP_JIT). */
int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes);
void hv_jit_region_close(HvJitRegion *region);

/* Copy `words` instructions to start (an executable address) and make them runnable. */
void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes);
void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words);

/* Space for `words` instructions, or NULL when the region is full. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words);
void hv_jit_reset(HvJitRegion *region);

#endif
