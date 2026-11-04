#include "jit_region.h"

#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The split shape on a host that can make it itself: plain RW memory, an
   alias, and the original turned R-X. Works for macOS binaries without the
   hardened runtime; on iOS the executable half has to come from a debugger. */
static int open_split(HvJitRegion *region, size_t bytes) {
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur, &max,
                 VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, (void *)rw, bytes) != 0) return -1;
    region->owned = 1;
    return 0;
}

int hv_jit_region_open(HvJitRegion *region, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (getenv("HV_JIT_SPLIT")) return open_split(region, bytes);
#if TARGET_OS_OSX
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->capacity = bytes / sizeof(uint32_t);
    region->owned = 1;
    return 0;
#else
    return open_split(region, bytes); /* works only where nothing enforces code signing (tests on a jailbroken device) */
#endif
}

int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (!rx || !rw || bytes < 4096) return -1;
    region->base = rx;
    region->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    region->capacity = bytes / sizeof(uint32_t);
    return 0;
}

void hv_jit_region_close(HvJitRegion *region) {
    if (region->owned && region->base) {
        size_t bytes = region->capacity * sizeof(uint32_t);
        if (region->rw_diff) munmap((uint8_t *)region->base + region->rw_diff, bytes);
        munmap(region->base, bytes);
    }
    memset(region, 0, sizeof *region);
}

void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes) {
    if (region->rw_diff) {
        memcpy((uint8_t *)start + region->rw_diff, src, bytes);
        return;
    }
    pthread_jit_write_protect_np(0); /* MAP_JIT: writable on this thread only, until hv_jit_finish */
    memcpy(start, src, bytes);
}

void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words) {
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
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
