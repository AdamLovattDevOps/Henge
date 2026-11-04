#ifndef HENGEVM_HVSPARSE_H
#define HENGEVM_HVSPARSE_H

#include <stdint.h>

/* A disk image with its holes left out, for moving one to an iPhone: Finder's
   file sharing copies every byte of a sparse file, holes included.
     "HVSPARSE" magic, u64 logical size, then records {u64 offset, u64 length, data}
     until a record with length 0. Little-endian. */

/* Mac: pack src (a sparse raw image) into dst. Returns 0 or -1. */
int hv_sparse_pack(const char *src, const char *dst);
/* Both: unpack into a sparse file. progress(done, total, opaque) may be NULL. Returns 0 or -1. */
int hv_sparse_unpack(const char *src, const char *dst, void (*progress)(uint64_t done, uint64_t total, void *opaque), void *opaque);

#endif
