/*
 * downloader.c - high-speed multi-threaded HTTP/1.1 download engine.
 *
 * Architecture:
 *   p5_dl_start()
 *     |-- resolve_redirects(HEAD)  -> final URL, content length, range support
 *     |-- load/save sidecar <dest>.p5part (resume)
 *     |-- preallocate dest via ftruncate
 *     `-- spawn N worker threads, one chunk each:
 *           connect (non-blocking, poll) -> GET with Range header
 *           -> stream body into P5_DL_BUFFER_CAP memory buffer
 *           -> flush with pwrite() into the chunk's file region
 *           -> checkpoint chunk progress into sidecar
 *
 * Pause is cooperative: workers flush, checkpoint, and exit; the sidecar
 * remains for a later resume. Cancel discards the sidecar.
 */
#include "downloader.h"

#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SIDECAR_MAGIC   0x5035504152543031ull  /* "P5PART01" */
#define HEADER_CAP      (16 * 1024)
#define MAX_REDIRECTS   5

/* ---------------------------------------------------------------- URL -- */

typedef struct {
    char     host[256];
    uint16_t port;
    char     path[P5_MAX_PATH];
    bool     https;
} p5_url;

static p5_status url_parse(const char *url, p5_url *out)
{
    if (!url || !out)
        return P5_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (!strncasecmp(p, "http://", 7)) {
        out->port = 80;
        p += 7;
    } else if (!strncasecmp(p, "https://", 8)) {
        out->port = 443;
        out->https = true;
        p += 8;
    } else {
        return P5_ERR_INVALID_ARG;   /* payload only speaks http(s) */
    }

    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    size_t hostlen = colon ? (size_t)(colon - p) : (size_t)(hostend - p);
    if (hostlen == 0 || hostlen >= sizeof(out->host))
        return P5_ERR_INVALID_ARG;
    memcpy(out->host, p, hostlen);
    out->host[hostlen] = '\0';

    if (colon) {
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535)
            return P5_ERR_INVALID_ARG;
        out->port = (uint16_t)port;
    }
    if (!slash || !slash[0])
        strcpy(out->path, "/");
    else if (strlen(slash) >= sizeof(out->path))
        return P5_ERR_INVALID_ARG;
    else
        strcpy(out->path, slash);
    return P5_OK;
}

/* -------------------------------------------------------- transport ---- */

typedef struct { int fd; } plain_ctx;

static int plain_send_all(void *vctx, const void *buf, size_t len)
{
    plain_ctx *c = (plain_ctx *)vctx;
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(c->fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                struct pollfd pfd = { c->fd, POLLOUT, 0 };
                if (poll(&pfd, 1, 15000) <= 0) return -1;
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int plain_recv_some(void *vctx, void *buf, size_t cap, int timeout_ms)
{
    plain_ctx *c = (plain_ctx *)vctx;
    struct pollfd pfd = { c->fd, POLLIN, 0 };
    int pr;
    do { pr = poll(&pfd, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0)
        return pr;                          /* 0 timeout, <0 error */
    ssize_t n = recv(c->fd, buf, cap, 0);
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    return (int)n;                          /* 0 on orderly close too */
}

static void plain_close(void *vctx)
{
    plain_ctx *c = (plain_ctx *)vctx;
    if (c) {
        if (c->fd >= 0) close(c->fd);
        free(c);
    }
}

/* Plain TCP transport: non-blocking connect with poll handshake. */
static p5_dl_transport *plain_transport_open(const char *host, uint16_t port,
                                             int timeout_ms)
{
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%u", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;             /* PSN LAN/WAN are IPv4-first */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return NULL; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return NULL; }
    if (rc < 0) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) { close(fd); return NULL; }
        int err = 0; socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            close(fd);
            return NULL;
        }
    }

    /* Large receive buffer helps saturate gigabit links. */
    int rcvbuf = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    plain_ctx *c = (plain_ctx *)calloc(1, sizeof(*c));
    p5_dl_transport *t = (p5_dl_transport *)calloc(1, sizeof(*t));
    if (!c || !t) { free(c); free(t); close(fd); return NULL; }
    c->fd = fd;
    t->send_all  = plain_send_all;
    t->recv_some = plain_recv_some;
    t->close     = plain_close;
    t->ctx       = c;
    return t;
}

/* ------------------------------------------------------------ chunk ---- */

typedef struct {
    uint64_t start;   /* first byte of region (inclusive) */
    uint64_t end;     /* end of region (exclusive)        */
    uint64_t fetched; /* bytes already on disk            */
} dl_chunk;

struct p5_dl {
    char url[P5_MAX_URL];
    char dest[P5_MAX_PATH];
    char sidecar[P5_MAX_PATH];
    p5_dl_options opts;
    p5_dl_transport_factory factory;

    dl_chunk chunks[P5_DL_MAX_CONN];
    uint32_t nchunks;
    uint64_t total_size;
    bool     accept_ranges;

    int      fd;                 /* destination file            */
    atomic_ullong bytes_done;    /* fetched across all chunks   */
    atomic_int    state;         /* 0 run, 1 pause, 2 cancel    */
    atomic_int    failures;
    pthread_mutex_t sidecar_lock;
    uint64_t      start_ms;
};

/* --------------------------------------------------------- sidecar ----- */

typedef struct {
    uint64_t magic;
    uint64_t total_size;
    uint32_t nchunks;
    uint32_t _pad;
    dl_chunk chunks[P5_DL_MAX_CONN];
} sidecar_hdr;

static void sidecar_save(p5_dl *dl)
{
    pthread_mutex_lock(&dl->sidecar_lock);
    sidecar_hdr h;
    memset(&h, 0, sizeof(h));
    h.magic      = SIDECAR_MAGIC;
    h.total_size = dl->total_size;
    h.nchunks    = dl->nchunks;
    for (uint32_t i = 0; i < dl->nchunks; i++)
        h.chunks[i] = dl->chunks[i];

    int fd = open(dl->sidecar, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pthread_mutex_unlock(&dl->sidecar_lock);
        return;
    }
    size_t written = 0;
    while (written < sizeof(h)) {
        ssize_t n = write(fd, (const uint8_t *)&h + written,
                          sizeof(h) - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    close(fd);
    pthread_mutex_unlock(&dl->sidecar_lock);
}

static bool sidecar_load(p5_dl *dl)
{
    sidecar_hdr h;
    int fd = open(dl->sidecar, O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, &h, sizeof(h));
    close(fd);
    if (n != (ssize_t)sizeof(h) || h.magic != SIDECAR_MAGIC ||
        h.total_size != dl->total_size || h.nchunks == 0 ||
        h.nchunks > P5_DL_MAX_CONN)
        return false;
    for (uint32_t i = 0; i < h.nchunks; i++) {
        if (h.chunks[i].end < h.chunks[i].start ||
            h.chunks[i].fetched > h.chunks[i].end - h.chunks[i].start)
            return false;
        dl->chunks[i] = h.chunks[i];
        atomic_fetch_add(&dl->bytes_done, h.chunks[i].fetched);
    }
    dl->nchunks = h.nchunks;
    return true;
}

/* ------------------------------------------------------- http layer ---- */

static int http_request(p5_dl *dl, p5_dl_transport *t, const p5_url *u,
                        const char *method, uint64_t range_from,
                        uint64_t range_to /* UINT64_MAX = open-ended */)
{
    (void)dl;
    char req[2048];
    char range_end[24];
    int len;
    if (range_to != UINT64_MAX || range_from > 0) {
        if (range_to != UINT64_MAX)
            snprintf(range_end, sizeof(range_end), "%llu",
                     (unsigned long long)range_to);
        else
            range_end[0] = '\0';
        len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ps5-payload/" P5_VERSION
            "\r\nAccept: */*\r\nRange: bytes=%llu-%s\r\nConnection: close\r\n\r\n",
            method, u->path, u->host,
            (unsigned long long)range_from,
            range_end);
    } else {
        len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ps5-payload/" P5_VERSION
            "\r\nAccept: */*\r\nConnection: close\r\n\r\n",
            method, u->path, u->host);
    }
    if (len <= 0 || (size_t)len >= sizeof(req))
        return -1;
    return t->send_all(t->ctx, req, (size_t)len);
}

/* Read response headers; returns HTTP status, fills content_length/-ranges. */
static int http_read_headers(p5_dl *dl, p5_dl_transport *t,
                             int64_t *content_length, bool *accept_ranges,
                             char *location, size_t location_cap,
                             uint8_t *body_prefix, size_t body_cap,
                             size_t *body_len)
{
    char hdr[HEADER_CAP];
    size_t used = 0;
    for (;;) {
        if (used >= sizeof(hdr) - 1)
            return -1;                       /* header too large */
        int n = t->recv_some(t->ctx, hdr + used, sizeof(hdr) - 1 - used,
                             dl->opts.io_timeout_ms
                                 ? (int)dl->opts.io_timeout_ms : 15000);
        if (n <= 0)
            return -1;
        used += (size_t)n;
        hdr[used] = '\0';
        char *marker = strstr(hdr, "\r\n\r\n");
        if (marker) {
            size_t header_len = (size_t)(marker - hdr) + 4;
            size_t extra = used - header_len;
            if (extra > body_cap) return -1;
            if (extra && body_prefix)
                memcpy(body_prefix, hdr + header_len, extra);
            if (body_len) *body_len = extra;
            break;
        }
    }

    int status = 0;
    if (sscanf(hdr, "HTTP/%*s %d", &status) != 1)
        return -1;

    if (content_length) *content_length = -1;
    if (accept_ranges)  *accept_ranges = false;
    if (location && location_cap) location[0] = '\0';

    char *save = NULL;
    char *line = strtok_r(hdr, "\r\n", &save);
    while ((line = strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (!strncasecmp(line, "Content-Length:", 15) && content_length)
            *content_length = strtoll(line + 15, NULL, 10);
        else if (!strncasecmp(line, "Accept-Ranges:", 14) && accept_ranges)
            *accept_ranges = (strstr(line + 14, "bytes") != NULL);
        else if (!strncasecmp(line, "Content-Range:", 14) && content_length) {
            const char *slash = strchr(line, '/');
            if (slash) *content_length = strtoll(slash + 1, NULL, 10);
        } else if (!strncasecmp(line, "Location:", 9) && location && location_cap) {
            const char *v = line + 9;
            while (*v == ' ') v++;
            snprintf(location, location_cap, "%s", v);
        }
    }
    /* Note: any body bytes already read past the header are discarded here;
     * workers size requests so this only affects HEAD responses (no body). */
    return status;
}

/* Follow redirects with HEAD; returns final URL + size + range support. */
static p5_status resolve_target(p5_dl *dl, p5_url *final, int64_t *size_out,
                                bool *ranges_out)
{
    char current[P5_MAX_URL];
    snprintf(current, sizeof(current), "%s", dl->url);

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        p5_url u;
        if (url_parse(current, &u) != P5_OK)
            return P5_ERR_INVALID_ARG;
        if (u.https && !dl->factory)
            return P5_ERR_NET;   /* no TLS hook installed */

        p5_dl_transport *t = dl->factory
            ? dl->factory(u.host, u.port, (int)dl->opts.connect_timeout_ms)
            : plain_transport_open(u.host, u.port,
                                   dl->opts.connect_timeout_ms
                                       ? (int)dl->opts.connect_timeout_ms
                                       : 8000);
        if (!t)
            return P5_ERR_NET;

        if (http_request(dl, t, &u, "HEAD", 0, UINT64_MAX) == 0) {
            int64_t clen = -1; bool ranges = false;
            char loc[P5_MAX_URL];
            int status = http_read_headers(dl, t, &clen, &ranges, loc,
                                           sizeof(loc), NULL, 0, NULL);
            t->close(t->ctx); free(t);

            if (status >= 300 && status < 400 && loc[0]) {
                snprintf(current, sizeof(current), "%s", loc);
                continue;                    /* follow redirect */
            }
            if (status >= 200 && status < 300 && clen > 0) {
                *final = u;
                *size_out = clen;
                *ranges_out = ranges;
                return P5_OK;
            }
            return P5_ERR_HTTP;
        }
        t->close(t->ctx); free(t);
        return P5_ERR_NET;
    }
    return P5_ERR_HTTP;                      /* redirect loop */
}

/* ------------------------------------------------------------ worker --- */

typedef struct { p5_dl *dl; uint32_t idx; p5_url url; } worker_arg;

static void *worker_main(void *varg)
{
    worker_arg *wa = (worker_arg *)varg;
    p5_dl *dl = wa->dl;
    dl_chunk *ch = &dl->chunks[wa->idx];
    int io_to = dl->opts.io_timeout_ms ? (int)dl->opts.io_timeout_ms : 15000;

    uint8_t *buf = (uint8_t *)malloc(P5_DL_BUFFER_CAP);
    if (!buf) { atomic_fetch_add(&dl->failures, 1); return NULL; }
    size_t buffered = 0;
    p5_status result = P5_OK;

    p5_dl_transport *t = dl->factory
        ? dl->factory(wa->url.host, wa->url.port,
                      dl->opts.connect_timeout_ms
                          ? (int)dl->opts.connect_timeout_ms : 8000)
        : plain_transport_open(wa->url.host, wa->url.port,
                               dl->opts.connect_timeout_ms
                                   ? (int)dl->opts.connect_timeout_ms : 8000);
    if (!t) { free(buf); atomic_fetch_add(&dl->failures, 1); return NULL; }

    uint64_t from = ch->start + ch->fetched;
    if (http_request(dl, t, &wa->url, "GET", from, ch->end - 1) != 0) {
        result = P5_ERR_NET;
        goto out;
    }
    {
        int64_t clen = -1; bool ranges = false;
        int status = http_read_headers(dl, t, &clen, &ranges, NULL, 0,
                                       buf, P5_DL_BUFFER_CAP, &buffered);
        if (status != 206 && !(status == 200 && dl->nchunks == 1)) {
            result = P5_ERR_HTTP;
            goto out;
        }
    }

    while (ch->fetched < ch->end - ch->start) {
        int st = atomic_load(&dl->state);
        if (st != 0) { result = (st == 1) ? P5_ERR_STATE : P5_ERR_CANCELLED;
                       break; }

        int n = t->recv_some(t->ctx, buf + buffered,
                             P5_DL_BUFFER_CAP - buffered, io_to);
        if (n < 0) { result = P5_ERR_NET; break; }
        if (n == 0) {
            /* Orderly close with everything fetched is fine; else retry once
             * would go here - keep it strict and fail fast. */
            if (ch->fetched + buffered >= ch->end - ch->start) break;
            result = P5_ERR_NET;
            break;
        }
        buffered += (size_t)n;

        if (buffered >= P5_DL_BUFFER_CAP) {
            off_t off = (off_t)(ch->start + ch->fetched);
            size_t written = 0;
            while (written < buffered) {
                ssize_t nw = pwrite(dl->fd, buf + written, buffered - written,
                                    off + (off_t)written);
                if (nw < 0 && errno == EINTR) continue;
                if (nw <= 0) { result = P5_ERR_IO; break; }
                written += (size_t)nw;
            }
            if (result != P5_OK) break;
            ch->fetched += buffered;
            atomic_fetch_add(&dl->bytes_done, buffered);
            buffered = 0;
            sidecar_save(dl);              /* checkpoint per flush */
        }
    }

    /* Final flush of the tail. */
    if (buffered > 0 && result == P5_OK) {
        off_t off = (off_t)(ch->start + ch->fetched);
        size_t written = 0;
        while (written < buffered) {
            ssize_t n = pwrite(dl->fd, buf + written, buffered - written,
                               off + (off_t)written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { result = P5_ERR_IO; break; }
            written += (size_t)n;
        }
        if (result == P5_OK) {
            ch->fetched += buffered;
            atomic_fetch_add(&dl->bytes_done, buffered);
        }
    }
    sidecar_save(dl);

out:
    t->close(t->ctx);
    free(t);
    free(buf);
    if (result != P5_OK && result != P5_ERR_STATE && result != P5_ERR_CANCELLED)
        atomic_fetch_add(&dl->failures, 1);
    return (void *)(intptr_t)result;
}

/* -------------------------------------------------------------- API ---- */

p5_status p5_dl_create(p5_dl **out, const char *url, const char *dest_path,
                       const p5_dl_options *opts)
{
    if (!out || !url || !dest_path)
        return P5_ERR_INVALID_ARG;
    if (strlen(url) >= P5_MAX_URL || strlen(dest_path) >= P5_MAX_PATH - 8)
        return P5_ERR_INVALID_ARG;

    p5_dl *dl = (p5_dl *)calloc(1, sizeof(*dl));
    if (!dl)
        return P5_ERR_NOMEM;
    strcpy(dl->url, url);
    strcpy(dl->dest, dest_path);
    snprintf(dl->sidecar, sizeof(dl->sidecar), "%s.p5part", dest_path);
    if (opts)
        dl->opts = *opts;
    if (!dl->opts.connect_timeout_ms) dl->opts.connect_timeout_ms = 8000;
    if (!dl->opts.io_timeout_ms)      dl->opts.io_timeout_ms = 15000;
    if (!dl->opts.connections || dl->opts.connections > P5_DL_MAX_CONN)
        dl->opts.connections = 4;
    dl->fd = -1;
    pthread_mutex_init(&dl->sidecar_lock, NULL);
    *out = dl;
    return P5_OK;
}

void p5_dl_set_transport_factory(p5_dl *dl, p5_dl_transport_factory factory)
{
    if (dl) dl->factory = factory;
}

p5_status p5_dl_start(p5_dl *dl)
{
    if (!dl) return P5_ERR_INVALID_ARG;

    p5_url final; int64_t size = -1; bool ranges = false;
    p5_status st = resolve_target(dl, &final, &size, &ranges);
    if (st != P5_OK)
        return st;

    dl->total_size = (uint64_t)size;
    dl->accept_ranges = ranges;

    dl->fd = open(dl->dest, O_WRONLY | O_CREAT, 0644);
    if (dl->fd < 0)
        return P5_ERR_IO;
    if (ftruncate(dl->fd, (off_t)dl->total_size) != 0) {
        close(dl->fd); dl->fd = -1;
        return P5_ERR_IO;
    }

    /* Resume from sidecar or plan a fresh chunk layout. */
    bool resumed = dl->opts.resume && sidecar_load(dl);
    if (!resumed) {
        uint32_t n = ranges ? dl->opts.connections : 1;
        uint64_t per = (dl->total_size + n - 1) / n;
        dl->nchunks = n;
        for (uint32_t i = 0; i < n; i++) {
            dl->chunks[i].start   = (uint64_t)i * per;
            dl->chunks[i].end     = (dl->chunks[i].start + per > dl->total_size)
                                    ? dl->total_size : dl->chunks[i].start + per;
            dl->chunks[i].fetched = 0;
        }
    }

    dl->start_ms = p5_now_ms();

    pthread_t tids[P5_DL_MAX_CONN];
    worker_arg args[P5_DL_MAX_CONN];
    bool started[P5_DL_MAX_CONN] = { false };
    for (uint32_t i = 0; i < dl->nchunks; i++) {
        if (dl->chunks[i].fetched >= dl->chunks[i].end - dl->chunks[i].start)
            continue;                        /* chunk already complete */
        args[i].dl = dl; args[i].idx = i; args[i].url = final;
        if (pthread_create(&tids[i], NULL, worker_main, &args[i]) == 0) {
            started[i] = true;
        } else {
            atomic_fetch_add(&dl->failures, 1);
        }
    }

    /* Progress pump on the caller thread while workers run. */
    for (uint32_t i = 0; i < dl->nchunks; i++)
        if (started[i]) pthread_join(tids[i], NULL);

    uint64_t done = atomic_load(&dl->bytes_done);
    int state = atomic_load(&dl->state);
    int fails = atomic_load(&dl->failures);

    if (dl->opts.on_progress) {
        int pct = dl->total_size ? (int)(done * 100 / dl->total_size) : 100;
        dl->opts.on_progress("dl", pct, done, dl->total_size,
                             dl->opts.progress_user);
    }

    close(dl->fd); dl->fd = -1;

    if (state == 2) { unlink(dl->sidecar); return P5_ERR_CANCELLED; }
    if (state == 1) return P5_ERR_STATE;   /* paused, sidecar kept */
    if (fails > 0 || done < dl->total_size) return P5_ERR_NET;

    unlink(dl->sidecar);                   /* completed: drop state file */
    return P5_OK;
}

void p5_dl_pause(p5_dl *dl)  { if (dl) atomic_store(&dl->state, 1); }
void p5_dl_cancel(p5_dl *dl) { if (dl) atomic_store(&dl->state, 2); }

p5_status p5_dl_stats(p5_dl *dl, uint64_t *done, uint64_t *total,
                      double *bytes_per_sec)
{
    if (!dl) return P5_ERR_INVALID_ARG;
    if (done)  *done  = atomic_load(&dl->bytes_done);
    if (total) *total = dl->total_size;
    if (bytes_per_sec) {
        uint64_t elapsed = p5_now_ms() - dl->start_ms;
        *bytes_per_sec = elapsed
            ? (double)atomic_load(&dl->bytes_done) / ((double)elapsed / 1000.0)
            : 0.0;
    }
    return P5_OK;
}

void p5_dl_destroy(p5_dl *dl)
{
    if (!dl) return;
    if (dl->fd >= 0) close(dl->fd);
    pthread_mutex_destroy(&dl->sidecar_lock);
    free(dl);
}
