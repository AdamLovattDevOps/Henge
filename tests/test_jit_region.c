#include "jit_region.h"

#include <stdio.h>

int main(void) {
    HvJitRegion region;
    if (hv_jit_region_open(&region, 1 << 20) != 0) {
        printf("could not map the JIT region\n");
        return 1;
    }
    hv_jit_begin_write();
    uint32_t *code = hv_jit_reserve(&region, 2);
    code[0] = 0xd2800540; /* mov x0, #42 */
    code[1] = 0xd65f03c0; /* ret */
    hv_jit_end_write(code, 2);
    long (*fn)(void) = (long (*)(void))code;
    long got = fn();
    printf("jit region returned %ld\n", got);
    hv_jit_region_close(&region);
    return got != 42;
}
