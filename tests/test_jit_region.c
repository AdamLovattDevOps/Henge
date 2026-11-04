#include "jit_region.h"

#include <stdio.h>
#include <stdlib.h>

/* Both shapes: macOS MAP_JIT, and the iOS split mapping (RX plus an RW alias). */
static int one(const char *what) {
    HvJitRegion region;
    if (hv_jit_region_open(&region, 1 << 20) != 0) {
        printf("%s: could not map the JIT region\n", what);
        return 1;
    }
    uint32_t *code = hv_jit_reserve(&region, 2);
    static const uint32_t body[2] = {0xd2800540, 0xd65f03c0}; /* mov x0, #42; ret */
    hv_jit_write(&region, code, body, sizeof body);
    hv_jit_finish(&region, code, 2);
    long (*fn)(void) = (long (*)(void))code;
    long got = fn();
    printf("%s: jit region returned %ld\n", what, got);
    hv_jit_region_close(&region);
    return got != 42;
}

int main(void) {
    int bad = one("MAP_JIT");
    setenv("HV_JIT_SPLIT", "1", 1);
    bad |= one("split");
    return bad;
}
