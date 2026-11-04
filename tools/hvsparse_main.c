/* hvsparse pack SRC DST | hvsparse unpack SRC DST: see host/hvsparse.h. */
#include <stdio.h>
#include <string.h>

#include "hvsparse.h"

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "pack")) return hv_sparse_pack(argv[2], argv[3]) ? 1 : 0;
    if (argc == 4 && !strcmp(argv[1], "unpack")) return hv_sparse_unpack(argv[2], argv[3], NULL, NULL) ? 1 : 0;
    fprintf(stderr, "usage: hvsparse pack|unpack SRC DST\n");
    return 2;
}
