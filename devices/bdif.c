#include "bdif.h"

#include <stdlib.h>
#include <string.h>

#define DEVID_MASK 0xffff0000u
#define DEVID_ROOT 0x00000000u
#define DEVID_AUX 0x00010000u

#define REG_STATUS 0x0
#define REG_CFG 0x4
#define REG_UNK1 0x8
#define REG_BUSY 0x10
#define REG_UNK2 0x400
#define REG_CMD 0x408
#define REG_NEXT_DEVICE 0x420
#define REG_UNK3 0x434

#define DATA_FLAGS_READ 0x00030001u
#define MAX_LEN (128u << 20)

/* A request, in guest memory: three (addr, len, flags) commands. */
typedef struct {
    uint64_t addr;
    uint32_t len, flags;
} ReqCmd;

static uint64_t bdif_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque, (void)size;
    uint32_t devid = (uint32_t)off & DEVID_MASK;
    switch (off & ~(uint64_t)DEVID_MASK) {
    case REG_STATUS: return 1;    /* active */
    case REG_CFG: return 2;       /* active */
    case REG_UNK1: return 0x420;
    case REG_BUSY: return 1;      /* ready */
    case REG_UNK2: return 1;
    case REG_UNK3: return 0;
    case REG_NEXT_DEVICE:
        if (devid == DEVID_ROOT) return 0x8000000;
        if (devid == DEVID_AUX) return 0x10000;
        return ~0ull;
    default: return ~0ull;
    }
}

static void command(HvBdif *b, HvDisk *disk, uint64_t req_pa) {
    uint8_t ret = 1; /* failed */
    ReqCmd req[3];   /* sector, data, retval */
    if (hv_phys_read(b->m, req_pa, req, sizeof req) != 0) return;
    if (disk && req[0].len == 16) {
        uint32_t sector[4];
        if (hv_phys_read(b->m, req[0].addr, sector, sizeof sector) == 0 && req[1].len <= MAX_LEN &&
            req[1].flags == DATA_FLAGS_READ) {
            void *dst = hv_phys_ptr(b->m, req[1].addr, req[1].len);
            if (dst && hv_disk_read(disk, (uint64_t)sector[2] * 512, dst, req[1].len) == 0) ret = 0;
        }
    }
    hv_phys_write(b->m, req[2].addr, &ret, 1);
}

static void bdif_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvBdif *b = opaque;
    if ((off & ~(uint64_t)DEVID_MASK) != REG_CMD) return;
    uint32_t devid = (uint32_t)off & DEVID_MASK;
    if (devid == DEVID_ROOT) command(b, b->root, v);
    else if (devid == DEVID_AUX) command(b, b->aux, v);
}

HvBdif *hv_bdif_create(HvMachine *m, uint64_t pa, HvDisk *root, HvDisk *aux) {
    HvBdif *b = calloc(1, sizeof *b);
    b->m = m;
    b->root = root;
    b->aux = aux;
    hv_machine_add_mmio(m, pa, 0x200000, b, bdif_read, bdif_write, "bdif");
    return b;
}
