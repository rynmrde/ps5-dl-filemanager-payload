/*
 * unrar_engine.h - multi-threaded UnRAR extraction for PS5 (Zen 2).
 *
 * Wraps libunrar's C ABI (RAROpenArchiveEx / RARProcessFile). Extraction is
 * parallelised at the *member* level: solid streams are decoded sequentially
 * by a reader thread while independent file writers drain a bounded queue,
 * overlapping archive decode with disk I/O. Caller picks thread count; the
 * engine caps it at the number of online cores minus one.
 */
#ifndef P5_UNRAR_ENGINE_H
#define P5_UNRAR_ENGINE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p5_unrar p5_unrar;

typedef struct {
    uint32_t threads;              /* 0 = auto (cores-1)                      */
    const char *password;          /* NULL if none                            */
    bool     overwrite;            /* overwrite existing files                */
    p5_progress_cb on_progress;
    void          *progress_user;
} p5_unrar_options;

p5_status p5_unrar_create(p5_unrar **out, const char *archive_path,
                          const char *dest_dir, const p5_unrar_options *opts);
void      p5_unrar_destroy(p5_unrar *u);

/* Blocking extraction of the whole archive. */
p5_status p5_unrar_extract_all(p5_unrar *u);
void      p5_unrar_cancel(p5_unrar *u);

/* List archive members without extracting. Frees with free(). */
typedef struct {
    char     path[P5_MAX_PATH];
    uint64_t unpacked_size;
    uint64_t packed_size;
    bool     is_dir;
    bool     encrypted;
} p5_unrar_member;

p5_status p5_unrar_list(p5_unrar *u, p5_unrar_member **members, size_t *count);

#ifdef __cplusplus
}
#endif
#endif /* P5_UNRAR_ENGINE_H */
