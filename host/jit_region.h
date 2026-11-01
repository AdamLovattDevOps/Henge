#ifndef HENGEVM_JIT_REGION_H
#define HENGEVM_JIT_REGION_H

#include <stddef.h>
#include <stdint.h>

/* One executable region for translated blocks. Blocks are appended and never
   freed one by one; the whole region is reset when it fills. */
typedef struct HvJitRegion {
    uint32_t *base;
    size_t capacity;   /* in instructions */
    size_t used;       /* in instructions */
} HvJitRegion;

int hv_jit_region_open(HvJitRegion *region, size_t bytes);
void hv_jit_region_close(HvJitRegion *region);

/* Make the region writable on this thread, append code, then make it executable again. */
void hv_jit_begin_write(void);
void hv_jit_end_write(uint32_t *start, size_t words);

/* Space for `words` instructions, or NULL when the region is full. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words);
void hv_jit_reset(HvJitRegion *region);

#endif
