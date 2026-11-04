#include "hvsparse.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (8u << 20)

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w <= 0) return -1;
        b += w, n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r <= 0) return -1;
        b += r, n -= (size_t)r;
    }
    return 0;
}

static int all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

int hv_sparse_pack_progress(const char *src, const char *dst, void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct stat st;
    uint8_t *buf = malloc(CHUNK);
    int rc = -1;
    if (in < 0 || out < 0 || fstat(in, &st) != 0 || !buf) goto done;
    uint64_t size = (uint64_t)st.st_size;
    if (write_all(out, "HVSPARSE", 8) || write_all(out, &size, 8)) goto done;
    /* Walk the data runs (SEEK_DATA/SEEK_HOLE), and skip zero chunks inside them too. */
    off_t pos = 0;
    while ((uint64_t)pos < size) {
        off_t data = lseek(in, pos, SEEK_DATA);
        if (data < 0) break; /* no more data */
        off_t hole = lseek(in, data, SEEK_HOLE);
        if (hole < 0) hole = (off_t)size;
        for (off_t at = data; at < hole;) {
            size_t n = (size_t)(hole - at < CHUNK ? hole - at : CHUNK);
            if (pread(in, buf, n, at) != (ssize_t)n) goto done;
            if (!all_zero(buf, n)) {
                uint64_t rec[2] = {(uint64_t)at, n};
                if (write_all(out, rec, 16) || write_all(out, buf, n)) goto done;
            }
            at += (off_t)n;
            if (progress) progress((uint64_t)at, size, opaque);
        }
        pos = hole;
    }
    uint64_t end[2] = {0, 0};
    if (write_all(out, end, 16)) goto done;
    if (fsync(out) != 0) goto done;
    rc = 0;
done:
    free(buf);
    if (in >= 0) close(in);
    if (out >= 0 && close(out) != 0) rc = -1;
    return rc;
}

int hv_sparse_pack(const char *src, const char *dst) { return hv_sparse_pack_progress(src, dst, NULL, NULL); }

int hv_sparse_unpack(const char *src, const char *dst, void (*progress)(uint64_t, uint64_t, void *), void *opaque) {
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    uint8_t *buf = malloc(CHUNK);
    int rc = -1;
    char magic[8];
    uint64_t size;
    if (in < 0 || out < 0 || !buf || read_all(in, magic, 8) || memcmp(magic, "HVSPARSE", 8) || read_all(in, &size, 8)) goto done;
    if (ftruncate(out, (off_t)size) != 0) goto done; /* the holes stay holes */
    for (;;) {
        uint64_t rec[2];
        if (read_all(in, rec, 16)) goto done;
        if (!rec[1]) break;
        if (rec[1] > CHUNK || rec[0] + rec[1] > size) goto done;
        if (read_all(in, buf, rec[1]) || pwrite(out, buf, rec[1], (off_t)rec[0]) != (ssize_t)rec[1]) goto done;
        if (progress) progress(rec[0] + rec[1], size, opaque);
    }
    rc = fsync(out);
done:
    free(buf);
    if (in >= 0) close(in);
    if (out >= 0 && close(out) != 0) rc = -1;
    return rc;
}
