/*
 * ffpfsc.h - Folder-to-FFPFSC converter.
 *
 * FFPFSC is a simple flat container this payload writes at maximum disk
 * throughput:
 *
 *   [Header "FFPFSC1\\0" | entry_count | index_offset]
 *   [blob of file payloads, each 4K-aligned]
 *   [index: fixed-size records {path, offset, size, fnv1a64}]
 *
 * Writes stream through a P5_FFPFSC_WR_BUFFER bounce buffer so large trees
 * serialise with a handful of big write(2) calls instead of thousands of
 * small ones. The index records carry FNV-1a-64 checksums for integrity
 * verification on unpack.
 */
#ifndef P5_FFPFSC_H
#define P5_FFPFSC_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P5_FFPFSC_MAGIC     "FFPFSC1\0"
#define P5_FFPFSC_ALIGN     4096u

typedef struct {
    p5_progress_cb on_progress;
    void          *progress_user;
} p5_ffpfsc_options;

/* Pack src_dir into out_path (.ffpfsc). Blocking. */
p5_status p5_ffpfsc_pack(const char *src_dir, const char *out_path,
                         const p5_ffpfsc_options *opts);

/* Verify an .ffpfsc container's structure + checksums without extracting. */
p5_status p5_ffpfsc_verify(const char *container_path, bool *ok);

/* FNV-1a 64 over a buffer (exposed for tooling/tests). */
uint64_t p5_fnv1a64(const void *data, size_t len, uint64_t seed);

#ifdef __cplusplus
}
#endif
#endif /* P5_FFPFSC_H */
