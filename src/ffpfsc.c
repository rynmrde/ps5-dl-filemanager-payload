/*
 * ffpfsc.c - Folder-to-FFPFSC converter.
 *
 * On-disk layout (all integers little-endian, host-native on amd64):
 *
 *   offset 0    : header { magic[8]="FFPFSC1\0", u64 entry_count,
 *                          u64 index_offset }
 *   ...         : file blobs back-to-back, each starting at a 4096 boundary
 *   index_offset: index records { char path[960]; u64 offset; u64 size;
 *                                 u64 fnv1a64; u8 flags; pad[7] }
 *
 * The writer streams file payloads through a P5_FFPFSC_WR_BUFFER bounce
 * buffer, so a tree of thousands of small files still hits the disk with
 * large sequential write(2) calls. Checksums are computed on the fly while
 * copying, so packing is a single read pass over the source tree.
 */
#include "ffpfsc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define HDR_SIZE     24
#define IDX_PATH_MAX 960

typedef struct {
    char     path[IDX_PATH_MAX];
    uint64_t offset;
    uint64_t size;
    uint64_t checksum;
    uint8_t  flags;          /* bit0: is_dir (size/offset unused) */
    uint8_t  pad[7];
} ffpfsc_index_rec;

typedef struct {
    int      fd;
    uint8_t *buf;
    size_t   used;
    uint64_t file_off;       /* absolute offset of buf[0] in the container */
    uint64_t write_pos;      /* absolute offset of next byte to write      */
    bool     error;
} wr_stream;

/* ------------------------------------------------------------ stream --- */

static p5_status ws_init(wr_stream *ws, int fd)
{
    memset(ws, 0, sizeof(*ws));
    ws->fd = fd;
    ws->buf = (uint8_t *)malloc(P5_FFPFSC_WR_BUFFER);
    if (!ws->buf)
        return P5_ERR_NOMEM;
    ws->write_pos = 0;
    return P5_OK;
}

static p5_status ws_flush(wr_stream *ws)
{
    if (ws->used == 0)
        return P5_OK;
    size_t w = 0;
    while (w < ws->used) {
        ssize_t n = pwrite(ws->fd, ws->buf + w, ws->used - w,
                           (off_t)(ws->file_off + w));
        if (n < 0) {
            if (errno == EINTR) continue;
            ws->error = true;
            return P5_ERR_IO;
        }
        if (n == 0) {
            ws->error = true;
            return P5_ERR_IO;
        }
        w += (size_t)n;
    }
    ws->file_off += ws->used;
    ws->used = 0;
    return P5_OK;
}

static p5_status ws_write(wr_stream *ws, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        if (ws->used == P5_FFPFSC_WR_BUFFER) {
            p5_status st = ws_flush(ws);
            if (st != P5_OK)
                return st;
        }
        size_t room = P5_FFPFSC_WR_BUFFER - ws->used;
        size_t take = len < room ? len : room;
        memcpy(ws->buf + ws->used, p, take);
        ws->used      += take;
        ws->write_pos += take;
        p   += take;
        len -= take;
    }
    return P5_OK;
}

static p5_status ws_pad_to(wr_stream *ws, uint64_t alignment)
{
    static const uint8_t zeros[512] = {0};
    uint64_t rem = ws->write_pos % alignment;
    if (!rem)
        return P5_OK;
    uint64_t need = alignment - rem;
    while (need > 0) {
        size_t take = need > sizeof(zeros) ? sizeof(zeros) : (size_t)need;
        p5_status st = ws_write(ws, zeros, take);
        if (st != P5_OK)
            return st;
        need -= take;
    }
    return P5_OK;
}

static void ws_destroy(wr_stream *ws)
{
    free(ws->buf);
    ws->buf = NULL;
}

/* ----------------------------------------------------------- checksum -- */

uint64_t p5_fnv1a64(const void *data, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = seed ? seed : 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* ------------------------------------------------------------- walker -- */

typedef struct {
    wr_stream        *ws;
    ffpfsc_index_rec *recs;
    size_t            nrecs, cap;
    const char       *root;        /* absolute source root, no trailing / */
    uint64_t          total_bytes, done_bytes;
    p5_progress_cb    on_progress;
    void             *user;
} pack_ctx;

static p5_status push_rec(pack_ctx *c, const ffpfsc_index_rec *rec)
{
    if (c->nrecs == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 256;
        ffpfsc_index_rec *nr =
            (ffpfsc_index_rec *)realloc(c->recs, ncap * sizeof(*nr));
        if (!nr)
            return P5_ERR_NOMEM;
        c->recs = nr;
        c->cap  = ncap;
    }
    c->recs[c->nrecs++] = *rec;
    return P5_OK;
}

/* Stream one file into the container, updating its index record. */
static p5_status pack_file(pack_ctx *c, const char *full, const char *rel,
                           uint64_t size)
{
    p5_status st = ws_pad_to(c->ws, P5_FFPFSC_ALIGN);
    if (st != P5_OK)
        return st;

    ffpfsc_index_rec rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.path, sizeof(rec.path), "%s", rel);
    rec.offset   = c->ws->write_pos;
    rec.size     = size;
    rec.checksum = 14695981039346656037ull;

    int in = open(full, O_RDONLY);
    if (in < 0)
        return P5_ERR_IO;

    uint8_t tmp[256 * 1024];
    uint64_t left = size;
    while (left > 0) {
        size_t want = left > sizeof(tmp) ? sizeof(tmp) : (size_t)left;
        ssize_t n = read(in, tmp, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(in);
            return P5_ERR_IO;
        }
        if (n == 0)
            break;                           /* file shrank mid-pack */
        rec.checksum = p5_fnv1a64(tmp, (size_t)n, rec.checksum);
        st = ws_write(c->ws, tmp, (size_t)n);
        if (st != P5_OK) { close(in); return st; }
        left -= (uint64_t)n;
        c->done_bytes += (uint64_t)n;

        if (c->on_progress && c->total_bytes)
            c->on_progress("ffpfsc",
                           (int)(c->done_bytes * 100 / c->total_bytes),
                           c->done_bytes, c->total_bytes, c->user);
    }
    close(in);

    if (left != 0)
        rec.size = size - left;              /* tolerate racy shrink */
    return push_rec(c, &rec);
}

static p5_status pack_walk(pack_ctx *c, const char *full, const char *rel);

static p5_status pack_dir(pack_ctx *c, const char *full, const char *rel)
{
    ffpfsc_index_rec rec;
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.path, sizeof(rec.path), "%s", rel);
    rec.flags = 1;                           /* directory */
    p5_status st = push_rec(c, &rec);
    if (st != P5_OK)
        return st;

    DIR *d = opendir(full);
    if (!d)
        return P5_ERR_IO;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        char cfull[P5_MAX_PATH], crel[P5_MAX_PATH];
        if (snprintf(cfull, sizeof(cfull), "%s/%s", full, de->d_name) >=
                (int)sizeof(cfull) ||
            snprintf(crel, sizeof(crel), "%s/%s", rel, de->d_name) >=
                (int)sizeof(crel)) {
            closedir(d);
            return P5_ERR_INVALID_ARG;
        }
        st = pack_walk(c, cfull, crel);
        if (st != P5_OK) {
            closedir(d);
            return st;
        }
    }
    closedir(d);
    return P5_OK;
}

static p5_status pack_walk(pack_ctx *c, const char *full, const char *rel)
{
    struct stat st;
    if (lstat(full, &st) != 0)
        return P5_ERR_IO;
    if (S_ISDIR(st.st_mode))
        return pack_dir(c, full, rel);
    if (S_ISREG(st.st_mode))
        return pack_file(c, full, rel, (uint64_t)st.st_size);
    return P5_OK;                            /* skip links/devices */
}

/* Single pass to pre-compute total bytes for progress reporting. */
static p5_status sum_tree(const char *path, uint64_t *total)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return P5_ERR_IO;
    if (S_ISREG(st.st_mode)) {
        *total += (uint64_t)st.st_size;
        return P5_OK;
    }
    if (!S_ISDIR(st.st_mode))
        return P5_OK;

    DIR *d = opendir(path);
    if (!d)
        return P5_ERR_IO;
    p5_status rc = P5_OK;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char child[P5_MAX_PATH];
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
            (int)sizeof(child)) {
            rc = P5_ERR_INVALID_ARG;
            break;
        }
        rc = sum_tree(child, total);
        if (rc != P5_OK)
            break;
    }
    closedir(d);
    return rc;
}

/* ---------------------------------------------------------------- API -- */

p5_status p5_ffpfsc_pack(const char *src_dir, const char *out_path,
                         const p5_ffpfsc_options *opts)
{
    if (!src_dir || !out_path)
        return P5_ERR_INVALID_ARG;

    struct stat st;
    if (stat(src_dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return P5_ERR_INVALID_ARG;

    int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return P5_ERR_IO;

    pack_ctx c;
    memset(&c, 0, sizeof(c));
    c.on_progress = opts ? opts->on_progress : NULL;
    c.user        = opts ? opts->progress_user : NULL;

    p5_status rc = sum_tree(src_dir, &c.total_bytes);
    if (rc != P5_OK) { close(fd); return rc; }

    wr_stream ws;
    if (ws_init(&ws, fd) != P5_OK) { close(fd); return P5_ERR_NOMEM; }
    c.ws = &ws;

    /* Reserve header space; backpatched after the walk. */
    uint8_t zero_hdr[HDR_SIZE] = {0};
    rc = ws_write(&ws, zero_hdr, sizeof(zero_hdr));

    const char *root_name = strrchr(src_dir, '/');
    root_name = root_name ? root_name + 1 : src_dir;
    if (rc == P5_OK)
        rc = pack_walk(&c, src_dir, root_name);

    /* Index, then backpatch header. */
    if (rc == P5_OK)
        rc = ws_pad_to(&ws, P5_FFPFSC_ALIGN);

    uint64_t index_offset = ws.write_pos;
    if (rc == P5_OK) {
        for (size_t i = 0; i < c.nrecs && rc == P5_OK; i++)
            rc = ws_write(&ws, &c.recs[i], sizeof(ffpfsc_index_rec));
    }
    if (rc == P5_OK)
        rc = ws_flush(&ws);

    if (rc == P5_OK) {
        uint8_t hdr[HDR_SIZE];
        memcpy(hdr, P5_FFPFSC_MAGIC, 8);
        uint64_t nrecs = (uint64_t)c.nrecs;
        memcpy(hdr + 8,  &nrecs, 8);
        memcpy(hdr + 16, &index_offset, 8);
        size_t written = 0;
        while (written < sizeof(hdr)) {
            ssize_t n = pwrite(fd, hdr + written, sizeof(hdr) - written,
                               (off_t)written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { rc = P5_ERR_IO; break; }
            written += (size_t)n;
        }
    }

    free(c.recs);
    ws_destroy(&ws);
    if (close(fd) != 0 && rc == P5_OK)
        rc = P5_ERR_IO;
    if (rc != P5_OK)
        unlink(out_path);
    return rc;
}

p5_status p5_ffpfsc_verify(const char *container_path, bool *ok)
{
    if (!container_path || !ok)
        return P5_ERR_INVALID_ARG;
    *ok = false;

    int fd = open(container_path, O_RDONLY);
    if (fd < 0)
        return P5_ERR_IO;

    p5_status rc = P5_OK;
    uint8_t hdr[HDR_SIZE];
    if (pread(fd, hdr, HDR_SIZE, 0) != HDR_SIZE ||
        memcmp(hdr, P5_FFPFSC_MAGIC, 8) != 0) {
        close(fd);
        return P5_OK;                        /* *ok stays false */
    }

    uint64_t nrecs, index_offset;
    memcpy(&nrecs, hdr + 8, 8);
    memcpy(&index_offset, hdr + 16, 8);
    if (nrecs > (1ull << 22)) {              /* sanity bound */
        close(fd);
        return P5_OK;
    }

    uint8_t buf[256 * 1024];
    for (uint64_t i = 0; i < nrecs && rc == P5_OK; i++) {
        ffpfsc_index_rec rec;
        off_t ioff = (off_t)(index_offset + i * sizeof(rec));
        if (pread(fd, &rec, sizeof(rec), ioff) != (ssize_t)sizeof(rec)) {
            rc = P5_ERR_IO;
            break;
        }
        if (rec.flags & 1)
            continue;                        /* directories carry no payload */

        uint64_t h = 14695981039346656037ull;
        uint64_t left = rec.size;
        uint64_t pos = rec.offset;
        while (left > 0) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            ssize_t n = pread(fd, buf, want, (off_t)pos);
            if (n <= 0) { rc = P5_ERR_IO; break; }
            h = p5_fnv1a64(buf, (size_t)n, h);
            pos  += (uint64_t)n;
            left -= (uint64_t)n;
        }
        if (rc != P5_OK)
            break;
        if (h != rec.checksum) {
            close(fd);
            return P5_OK;                    /* mismatch: *ok false */
        }
    }

    close(fd);
    if (rc == P5_OK)
        *ok = true;
    return rc;
}
