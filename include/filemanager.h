/*
 * filemanager.h - asynchronous file manager core for PS5 mount points.
 *
 * A small worker pool executes copy/move/delete/rename jobs off the caller
 * thread so the UI/payload loop never blocks on slow media (HDD, /mnt/usb0).
 * Copy/move stream through an P5_FM_COPY_BUFFER bounce buffer; same-device
 * moves degrade to rename(2) when possible.
 */
#ifndef P5_FILEMANAGER_H
#define P5_FILEMANAGER_H

#include "common.h"
#include <sys/types.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    P5_FM_COPY,
    P5_FM_MOVE,
    P5_FM_DELETE,
    P5_FM_RENAME
} p5_fm_op;

typedef void (*p5_fm_done_cb)(p5_fm_op op, const char *src, const char *dst,
                              p5_status result, void *user);

typedef struct p5_fm p5_fm;

/* Start/stop the worker pool (workers = 0 -> P5_WORKER_THREADS). */
p5_status p5_fm_create(p5_fm **out, uint32_t workers);
void      p5_fm_destroy(p5_fm *fm);

/* Queue an operation; callback fires on completion (worker thread context). */
p5_status p5_fm_submit(p5_fm *fm, p5_fm_op op, const char *src, const char *dst,
                       p5_progress_cb on_progress, p5_fm_done_cb on_done,
                       void *user);

/* Synchronous helpers (used by tests and by the pool itself). */
p5_status p5_fm_copy_recursive(const char *src, const char *dst,
                               p5_progress_cb cb, void *user,
                               volatile bool *cancel_flag);
p5_status p5_fm_delete_recursive(const char *path, volatile bool *cancel_flag);

/* Directory listing for navigation. Caller frees entries with p5_fm_list_free. */
typedef struct {
    char     name[256];
    uint64_t size;
    bool     is_dir;
    int64_t  mtime;
} p5_fm_entry;

p5_status p5_fm_list(const char *path, p5_fm_entry **entries, size_t *count);
void      p5_fm_list_free(p5_fm_entry *entries);

#ifdef __cplusplus
}
#endif
#endif /* P5_FILEMANAGER_H */
