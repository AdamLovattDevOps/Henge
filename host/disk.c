#include "disk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly) {
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    if (d->fd < 0) return -1;
    struct stat st;
    if (fstat(d->fd, &st) != 0 || (uint64_t)st.st_size < offset) {
        close(d->fd);
        return -1;
    }
    d->offset = offset;
    d->size = (uint64_t)st.st_size - offset;
    d->readonly = readonly;
    return 0;
}

void hv_disk_close(HvDisk *d) {
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
}

int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len) {
    if (off > d->size || len > d->size - off) return -1;
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(d->fd, p, len, (off_t)(d->offset + off));
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len) {
    if (d->readonly || off > d->size || len > d->size - off) return -1;
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(d->fd, p, len, (off_t)(d->offset + off));
        if (n <= 0) return -1;
        p += n;
        off += (uint64_t)n;
        len -= (uint64_t)n;
    }
    return 0;
}

int hv_disk_flush(HvDisk *d) {
    return d->readonly ? 0 : fsync(d->fd);
}
