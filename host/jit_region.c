#include "jit_region.h"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

/* macOS host: MAP_JIT with per-thread write protection. The iOS host will
   provide the same functions over the region StikDebug makes available. */

int hv_jit_region_open(HvJitRegion *region, size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->capacity = bytes / sizeof(uint32_t);
    region->used = 0;
    return 0;
}

void hv_jit_region_close(HvJitRegion *region) {
    if (region->base) munmap(region->base, region->capacity * sizeof(uint32_t));
    region->base = NULL;
    region->capacity = region->used = 0;
}

void hv_jit_begin_write(void) {
    pthread_jit_write_protect_np(0);
}

void hv_jit_end_write(uint32_t *start, size_t words) {
    pthread_jit_write_protect_np(1);
    if (start && words) sys_icache_invalidate(start, words * sizeof(uint32_t));
}

uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words) {
    if (region->used + words > region->capacity) return NULL;
    uint32_t *p = region->base + region->used;
    region->used += words;
    return p;
}

void hv_jit_reset(HvJitRegion *region) {
    region->used = 0;
}
