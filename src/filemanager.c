/*
 * filemanager.c - asynchronous file manager core.
 *
 * A bounded FIFO job queue feeds a fixed worker pool. Copies stream through
 * an P5_FM_COPY_BUFFER bounce buffer with pread/pwrite so big transfers run
 * at a handful of syscalls per megabyte. rename(2) is attempted first for
 * moves; EXDEV (cross-mount, e.g. /user -> /mnt/usb0) falls back to
 * copy+delete. All recursive walkers honour a cooperative cancel flag.
 */
#include "filemanager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FM_QUEUE_CAP 64

typedef struct {
    p5_fm_op        op;
    char            src[P5_MAX_PATH];
    char            dst[P5_MAX_PATH];
    p5_progress_cb  on_progress;
    p5_fm_done_cb   on_done;
    void           *user;
    volatile bool   cancel;
} fm_job;

struct p5_fm {
    pthread_t       threads[16];
    uint32_t        nthreads;
    fm_job          queue[FM_QUEUE_CAP];
    uint32_t        head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    atomic_bool     shutdown;
};

/* ------------------------------------------------------- primitives ---- */

static p5_status copy_one_file(const char *src, const char *dst,
                               p5_progress_cb cb, void *user,
                               volatile bool *cancel)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return P5_ERR_IO;

    struct stat st;
    if (fstat(in, &st) != 0) { close(in); return P5_ERR_IO; }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) { close(in); return P5_ERR_IO; }

    uint8_t *buf = (uint8_t *)malloc(P5_FM_COPY_BUFFER);
    if (!buf) { close(in); close(out); return P5_ERR_NOMEM; }

    p5_status rc = P5_OK;
    uint64_t done = 0, total = (uint64_t)st.st_size;
    off_t off = 0;

    while (off < st.st_size) {
        if (cancel && *cancel) { rc = P5_ERR_CANCELLED; break; }

        size_t want = P5_FM_COPY_BUFFER;
        if ((uint64_t)want > (uint64_t)(st.st_size - off))
            want = (size_t)(st.st_size - off);

        ssize_t n = pread(in, buf, want, off);
        if (n < 0) {
            if (errno == EINTR) continue;
            rc = P5_ERR_IO;
            break;
        }
        if (n == 0) break;                     /* unexpected EOF */

        size_t w = 0;
        while (w < (size_t)n) {
            ssize_t m = pwrite(out, buf + w, (size_t)n - w, off + (off_t)w);
            if (m < 0) {
                if (errno == EINTR) continue;
                rc = P5_ERR_IO;
                goto done;
            }
            w += (size_t)m;
        }
        off  += n;
        done += (uint64_t)n;
        if (cb)
            cb("fm:copy", total ? (int)(done * 100 / total) : -1,
               done, total, user);
    }

done:
    free(buf);
    close(in);
    if (close(out) != 0 && rc == P5_OK)
        rc = P5_ERR_IO;
    if (rc != P5_OK)
        unlink(dst);                           /* never leave partial files */
    return rc;
}

static p5_status ensure_parent_dirs(const char *path)
{
    char tmp[P5_MAX_PATH];
    if (strlen(path) >= sizeof(tmp))
        return P5_ERR_INVALID_ARG;
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return P5_ERR_IO;
            *p = '/';
        }
    }
    return P5_OK;
}

/* --------------------------------------------------- recursive walkers - */

p5_status p5_fm_copy_recursive(const char *src, const char *dst,
                               p5_progress_cb cb, void *user,
                               volatile bool *cancel_flag)
{
    if (!src || !dst)
        return P5_ERR_INVALID_ARG;
    if (cancel_flag && *cancel_flag)
        return P5_ERR_CANCELLED;

    struct stat st;
    if (lstat(src, &st) != 0)
        return P5_ERR_IO;

    if (S_ISREG(st.st_mode)) {
        if (ensure_parent_dirs(dst) != P5_OK)
            return P5_ERR_IO;
        return copy_one_file(src, dst, cb, user, cancel_flag);
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST)
            return P5_ERR_IO;

        DIR *d = opendir(src);
        if (!d)
            return P5_ERR_IO;

        p5_status rc = P5_OK;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (cancel_flag && *cancel_flag) { rc = P5_ERR_CANCELLED; break; }

            char csrc[P5_MAX_PATH], cdst[P5_MAX_PATH];
            if (snprintf(csrc, sizeof(csrc), "%s/%s", src, de->d_name) >=
                    (int)sizeof(csrc) ||
                snprintf(cdst, sizeof(cdst), "%s/%s", dst, de->d_name) >=
                    (int)sizeof(cdst)) {
                rc = P5_ERR_INVALID_ARG;
                break;
            }
            rc = p5_fm_copy_recursive(csrc, cdst, cb, user, cancel_flag);
            if (rc != P5_OK)
                break;
        }
        closedir(d);
        return rc;
    }

    /* Symlinks and device nodes are skipped deliberately in payload context. */
    return P5_OK;
}

p5_status p5_fm_delete_recursive(const char *path, volatile bool *cancel_flag)
{
    if (!path)
        return P5_ERR_INVALID_ARG;
    if (cancel_flag && *cancel_flag)
        return P5_ERR_CANCELLED;

    struct stat st;
    if (lstat(path, &st) != 0)
        return P5_ERR_IO;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return P5_ERR_IO;
        p5_status rc = P5_OK;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (cancel_flag && *cancel_flag) { rc = P5_ERR_CANCELLED; break; }
            char child[P5_MAX_PATH];
            if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
                (int)sizeof(child)) {
                rc = P5_ERR_INVALID_ARG;
                break;
            }
            rc = p5_fm_delete_recursive(child, cancel_flag);
            if (rc != P5_OK)
                break;
        }
        closedir(d);
        if (rc == P5_OK && rmdir(path) != 0)
            rc = P5_ERR_IO;
        return rc;
    }

    return (unlink(path) == 0) ? P5_OK : P5_ERR_IO;
}

/* ------------------------------------------------------------ listing -- */

static int entry_cmp(const void *a, const void *b)
{
    const p5_fm_entry *ea = (const p5_fm_entry *)a;
    const p5_fm_entry *eb = (const p5_fm_entry *)b;
    if (ea->is_dir != eb->is_dir)
        return ea->is_dir ? -1 : 1;          /* dirs first */
    return strcasecmp(ea->name, eb->name);
}

p5_status p5_fm_list(const char *path, p5_fm_entry **entries, size_t *count)
{
    if (!path || !entries || !count)
        return P5_ERR_INVALID_ARG;

    DIR *d = opendir(path);
    if (!d)
        return P5_ERR_IO;

    size_t cap = 64, n = 0;
    p5_fm_entry *list = (p5_fm_entry *)malloc(cap * sizeof(*list));
    if (!list) { closedir(d); return P5_ERR_NOMEM; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap) {
            cap *= 2;
            p5_fm_entry *nl =
                (p5_fm_entry *)realloc(list, cap * sizeof(*list));
            if (!nl) { free(list); closedir(d); return P5_ERR_NOMEM; }
            list = nl;
        }
        p5_fm_entry *e = &list[n];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);

        char full[P5_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(full, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode);
            e->size   = (uint64_t)st.st_size;
            e->mtime  = (int64_t)st.st_mtime;
        }
        n++;
    }
    closedir(d);

    qsort(list, n, sizeof(*list), entry_cmp);
    *entries = list;
    *count   = n;
    return P5_OK;
}

void p5_fm_list_free(p5_fm_entry *entries) { free(entries); }

/* --------------------------------------------------------- worker pool - */

static void job_execute(fm_job *j)
{
    p5_status rc;
    switch (j->op) {
    case P5_FM_COPY:
        rc = p5_fm_copy_recursive(j->src, j->dst, j->on_progress, j->user,
                                  &j->cancel);
        break;
    case P5_FM_MOVE:
        if (rename(j->src, j->dst) == 0) {
            rc = P5_OK;
            break;
        }
        if (errno == EXDEV) {
            rc = p5_fm_copy_recursive(j->src, j->dst, j->on_progress, j->user,
                                      &j->cancel);
            if (rc == P5_OK)
                rc = p5_fm_delete_recursive(j->src, &j->cancel);
            break;
        }
        rc = P5_ERR_IO;
        break;
    case P5_FM_RENAME:
        rc = (rename(j->src, j->dst) == 0) ? P5_OK : P5_ERR_IO;
        break;
    case P5_FM_DELETE:
        rc = p5_fm_delete_recursive(j->src, &j->cancel);
        break;
    default:
        rc = P5_ERR_INVALID_ARG;
        break;
    }

    if (j->on_done)
        j->on_done(j->op, j->src, j->dst[0] ? j->dst : NULL, rc, j->user);
}

static void *fm_worker(void *arg)
{
    p5_fm *fm = (p5_fm *)arg;
    for (;;) {
        pthread_mutex_lock(&fm->lock);
        while (fm->count == 0 && !atomic_load(&fm->shutdown))
            pthread_cond_wait(&fm->not_empty, &fm->lock);

        if (fm->count == 0 && atomic_load(&fm->shutdown)) {
            pthread_mutex_unlock(&fm->lock);
            return NULL;
        }

        fm_job j = fm->queue[fm->head];
        fm->head  = (fm->head + 1) % FM_QUEUE_CAP;
        fm->count--;
        pthread_cond_signal(&fm->not_full);
        pthread_mutex_unlock(&fm->lock);

        job_execute(&j);
    }
}

p5_status p5_fm_create(p5_fm **out, uint32_t workers)
{
    if (!out)
        return P5_ERR_INVALID_ARG;
    if (workers == 0)          workers = P5_WORKER_THREADS;
    if (workers > 16)          workers = 16;

    p5_fm *fm = (p5_fm *)calloc(1, sizeof(*fm));
    if (!fm)
        return P5_ERR_NOMEM;

    pthread_mutex_init(&fm->lock, NULL);
    pthread_cond_init(&fm->not_empty, NULL);
    pthread_cond_init(&fm->not_full, NULL);
    atomic_store(&fm->shutdown, false);

    for (uint32_t i = 0; i < workers; i++) {
        if (pthread_create(&fm->threads[i], NULL, fm_worker, fm) != 0) {
            /* Roll back already-started threads cleanly. */
            atomic_store(&fm->shutdown, true);
            pthread_cond_broadcast(&fm->not_empty);
            for (uint32_t k = 0; k < i; k++)
                pthread_join(fm->threads[k], NULL);
            pthread_mutex_destroy(&fm->lock);
            pthread_cond_destroy(&fm->not_empty);
            pthread_cond_destroy(&fm->not_full);
            free(fm);
            return P5_ERR_STATE;
        }
        fm->nthreads++;
    }
    *out = fm;
    return P5_OK;
}

void p5_fm_destroy(p5_fm *fm)
{
    if (!fm) return;
    pthread_mutex_lock(&fm->lock);
    atomic_store(&fm->shutdown, true);
    pthread_cond_broadcast(&fm->not_empty);
    pthread_mutex_unlock(&fm->lock);

    for (uint32_t i = 0; i < fm->nthreads; i++)
        pthread_join(fm->threads[i], NULL);

    pthread_mutex_destroy(&fm->lock);
    pthread_cond_destroy(&fm->not_empty);
    pthread_cond_destroy(&fm->not_full);
    free(fm);
}

p5_status p5_fm_submit(p5_fm *fm, p5_fm_op op, const char *src, const char *dst,
                       p5_progress_cb on_progress, p5_fm_done_cb on_done,
                       void *user)
{
    if (!fm || !src || strlen(src) >= P5_MAX_PATH)
        return P5_ERR_INVALID_ARG;
    if ((op == P5_FM_COPY || op == P5_FM_MOVE || op == P5_FM_RENAME) &&
        (!dst || strlen(dst) >= P5_MAX_PATH))
        return P5_ERR_INVALID_ARG;
    if (atomic_load(&fm->shutdown))
        return P5_ERR_STATE;

    pthread_mutex_lock(&fm->lock);
    if (fm->count == FM_QUEUE_CAP) {
        pthread_mutex_unlock(&fm->lock);
        return P5_ERR_STATE;                 /* queue full: caller retries  */
    }

    fm_job *j = &fm->queue[fm->tail];
    memset(j, 0, sizeof(*j));
    j->op = op;
    strcpy(j->src, src);
    if (dst) strcpy(j->dst, dst);
    j->on_progress = on_progress;
    j->on_done     = on_done;
    j->user        = user;
    j->cancel      = false;

    fm->tail = (fm->tail + 1) % FM_QUEUE_CAP;
    fm->count++;
    pthread_cond_signal(&fm->not_empty);
    pthread_mutex_unlock(&fm->lock);
    return P5_OK;
}
