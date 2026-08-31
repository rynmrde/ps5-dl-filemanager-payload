/*
 * common.h - shared definitions for the unified PS5 payload.
 *
 * Constraints honoured throughout the codebase:
 *  - PS5 payload environment: FreeBSD 12 (Prospero), AMD Zen 2, 16GB unified
 *    memory of which a payload should stay well under a ~256MB footprint.
 *  - No dynamic loader tricks; everything links statically into one ELF.
 */
#ifndef P5_COMMON_H
#define P5_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#define P5_VERSION          "1.0.0"

/* Memory safety limits (per module, bytes) */
#define P5_DL_BUFFER_CAP    (32u * 1024u * 1024u)   /* per-connection write buffer */
#define P5_FM_COPY_BUFFER   (8u  * 1024u * 1024u)   /* file manager copy chunk   */
#define P5_UNRAR_IO_BUFFER  (4u  * 1024u * 1024u)   /* unrar per-file io buffer  */
#define P5_FFPFSC_WR_BUFFER (16u * 1024u * 1024u)   /* ffpfsc pack write buffer  */

#define P5_MAX_PATH         1024
#define P5_MAX_URL          2048
#define P5_DL_MAX_CONN      8u                       /* Zen 2: 8c/16t, keep headroom */
#define P5_WORKER_THREADS   4u

typedef enum {
    P5_OK = 0,
    P5_ERR_INVALID_ARG   = -1,
    P5_ERR_NOMEM         = -2,
    P5_ERR_IO            = -3,
    P5_ERR_NET           = -4,
    P5_ERR_HTTP          = -5,
    P5_ERR_STATE         = -6,
    P5_ERR_CANCELLED     = -7,
    P5_ERR_ARCHIVE       = -8,
    P5_ERR_FORMAT        = -9
} p5_status;

static inline uint64_t p5_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* Lightweight logger; payload builds route stdout over klog/socket. */
#define P5_LOG(fmt, ...)  do { fprintf(stdout, "[payload] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)
#define P5_ERRL(fmt, ...) do { fprintf(stderr, "[payload:err] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

/* Progress callback shared by all engines. percent = 0..100, -1 if unknown. */
typedef void (*p5_progress_cb)(const char *tag, int percent, uint64_t done,
                               uint64_t total, void *user);

#endif /* P5_COMMON_H */
