/*
 * downloader.h - high-speed multi-threaded HTTP download engine.
 *
 * Design:
 *  - N connections (default 4, max P5_DL_MAX_CONN), each owning one HTTP/1.1
 *    Range request over a non-blocking socket drained with poll().
 *  - Chunked on-disk layout: file preallocated, each worker writes into its
 *    own [start,end) region via pwrite() from a memory buffer flushed at
 *    P5_DL_BUFFER_CAP boundaries.
 *  - Resume: a <file>.p5part sidecar records chunk progress (magic-verified);
 *    pause = cooperative cancellation that leaves a resumable state.
 *  - HTTPS: the engine is transport-pluggable; p5_dl_set_tls_hook() installs a
 *    TLS read/write pair (e.g. backed by mbedTLS or libcurl) without touching
 *    the scheduler.
 */
#ifndef P5_DOWNLOADER_H
#define P5_DOWNLOADER_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p5_dl p5_dl;

typedef struct {
    int   (*send_all)(void *ctx, const void *buf, size_t len);
    int   (*recv_some)(void *ctx, void *buf, size_t cap, int timeout_ms);
    void  (*close)(void *ctx);
    void   *ctx;
} p5_dl_transport;

typedef struct {
    uint32_t connections;          /* 1..P5_DL_MAX_CONN, 0 = auto (4)        */
    uint32_t connect_timeout_ms;   /* default 8000                            */
    uint32_t io_timeout_ms;        /* default 15000                           */
    bool     resume;               /* use/keep .p5part sidecar                */
    p5_progress_cb on_progress;
    void          *progress_user;
} p5_dl_options;

/* Create/destroy a download job for url -> dest_path. */
p5_status p5_dl_create(p5_dl **out, const char *url, const char *dest_path,
                       const p5_dl_options *opts);
void      p5_dl_destroy(p5_dl *dl);

/* Blocking start; returns when finished, failed, or paused/cancelled. */
p5_status p5_dl_start(p5_dl *dl);

/* Cooperative controls (thread-safe). */
void      p5_dl_pause(p5_dl *dl);    /* leaves resumable state on disk */
void      p5_dl_cancel(p5_dl *dl);   /* discards sidecar state         */

/* Stats snapshot (thread-safe). */
p5_status p5_dl_stats(p5_dl *dl, uint64_t *done, uint64_t *total,
                      double *bytes_per_sec);

/* Install a custom transport factory (e.g. TLS). May be NULL for plain HTTP. */
typedef p5_dl_transport *(*p5_dl_transport_factory)(const char *host,
                                                    uint16_t port, int timeout_ms);
void p5_dl_set_transport_factory(p5_dl *dl, p5_dl_transport_factory factory);

#ifdef __cplusplus
}
#endif
#endif /* P5_DOWNLOADER_H */
