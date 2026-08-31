/*
 * main.c - unified PS5 payload entry point.
 *
 * The ELF is loaded by a PS5 payload loader (e.g. elfldr/ps5-payload-loader).
 * Commands arrive either as argv (when launched with arguments by a frontend)
 * or line-by-line on stdin when the payload is resident and driven over the
 * loader's socket-redirected stdio.
 *
 * Commands:
 *   dl <url> <dest> [connections]      download a file
 *   ls <path>                          list a directory
 *   cp <src> <dst>                     copy file/dir (async pool, waits here)
 *   mv <src> <dst>                     move (rename or copy+delete)
 *   rn <src> <dst>                     rename
 *   rm <path>                          delete file/dir recursively
 *   unrar <archive.rar> <destdir>      extract archive
 *   pack <dir> <out.ffpfsc>            folder -> FFPFSC container
 *   verify <file.ffpfsc>               verify container integrity
 *   help                               show usage
 *   exit                               terminate payload
 */
#include "common.h"
#include "downloader.h"
#include "filemanager.h"
#include "unrar_engine.h"
#include "ffpfsc.h"

#include <signal.h>
#include <unistd.h>

static p5_fm *g_fm;

static void on_progress(const char *tag, int percent, uint64_t done,
                        uint64_t total, void *user)
{
    (void)user;
    static uint64_t last_ms;
    uint64_t now = p5_now_ms();
    if (now - last_ms < 500 && percent != 100)   /* throttle console spam */
        return;
    last_ms = now;
    if (percent >= 0)
        P5_LOG("%s: %d%% (%llu/%llu bytes)", tag, percent,
               (unsigned long long)done, (unsigned long long)total);
    else
        P5_LOG("%s: %llu bytes", tag, (unsigned long long)done);
}

static void on_fm_done(p5_fm_op op, const char *src, const char *dst,
                       p5_status result, void *user)
{
    (void)dst; (void)op;
    int *done_flag = (int *)user;
    if (result == P5_OK)
        P5_LOG("fm: done %s", src);
    else
        P5_ERRL("fm: failed %s (status %d)", src, result);
    __atomic_store_n(done_flag, 1, __ATOMIC_RELEASE);
}

/* Submit an fm job and block until the pool finishes it. */
static p5_status run_fm_sync(p5_fm_op op, const char *src, const char *dst)
{
    volatile int done = 0;
    p5_status st = p5_fm_submit(g_fm, op, src, dst, on_progress, on_fm_done,
                                (void *)&done);
    if (st != P5_OK)
        return st;
    while (!__atomic_load_n((int *)&done, __ATOMIC_ACQUIRE))
        usleep(10 * 1000);
    return P5_OK;
}

static void usage(void)
{
    P5_LOG("ps5-dl-filemanager-payload v" P5_VERSION);
    P5_LOG("commands:");
    P5_LOG("  dl <url> <dest> [conn]   - multi-connection HTTP download");
    P5_LOG("  ls <path>                - list directory");
    P5_LOG("  cp|mv|rn <src> <dst>     - copy/move/rename");
    P5_LOG("  rm <path>                - recursive delete");
    P5_LOG("  unrar <arc> <dir>        - extract RAR archive");
    P5_LOG("  pack <dir> <out>         - folder -> .ffpfsc");
    P5_LOG("  verify <file>            - verify .ffpfsc");
    P5_LOG("  exit                     - quit");
}

static p5_status cmd_dl(const char *url, const char *dest, const char *conn_s)
{
    p5_dl_options opts = {0};
    opts.resume = true;
    opts.on_progress = on_progress;
    if (conn_s) {
        uint32_t c = (uint32_t)strtoul(conn_s, NULL, 10);
        if (c >= 1 && c <= P5_DL_MAX_CONN)
            opts.connections = c;
    }

    p5_dl *dl = NULL;
    p5_status st = p5_dl_create(&dl, url, dest, &opts);
    if (st != P5_OK) {
        P5_ERRL("dl: create failed (%d)", st);
        return st;
    }
    st = p5_dl_start(dl);
    if (st == P5_OK) {
        uint64_t done = 0, total = 0; double bps = 0;
        p5_dl_stats(dl, &done, &total, &bps);
        P5_LOG("dl: finished %llu bytes at %.1f MB/s",
               (unsigned long long)done, bps / (1024.0 * 1024.0));
    } else {
        P5_ERRL("dl: failed (%d)", st);
    }
    p5_dl_destroy(dl);
    return st;
}

static p5_status cmd_ls(const char *path)
{
    p5_fm_entry *entries = NULL;
    size_t count = 0;
    p5_status st = p5_fm_list(path, &entries, &count);
    if (st != P5_OK) {
        P5_ERRL("ls: %s failed (%d)", path, st);
        return st;
    }
    for (size_t i = 0; i < count; i++)
        P5_LOG("%s %10llu  %s", entries[i].is_dir ? "d" : "-",
               (unsigned long long)entries[i].size, entries[i].name);
    p5_fm_list_free(entries);
    return P5_OK;
}

static int dispatch(int argc, char **argv)
{
    if (argc < 1)
        return 1;

    const char *cmd = argv[0];
    if (!strcmp(cmd, "help")) {
        usage();
    } else if (!strcmp(cmd, "dl") && argc >= 3) {
        cmd_dl(argv[1], argv[2], argc >= 4 ? argv[3] : NULL);
    } else if (!strcmp(cmd, "ls") && argc >= 2) {
        cmd_ls(argv[1]);
    } else if (!strcmp(cmd, "cp") && argc >= 3) {
        run_fm_sync(P5_FM_COPY, argv[1], argv[2]);
    } else if (!strcmp(cmd, "mv") && argc >= 3) {
        run_fm_sync(P5_FM_MOVE, argv[1], argv[2]);
    } else if (!strcmp(cmd, "rn") && argc >= 3) {
        run_fm_sync(P5_FM_RENAME, argv[1], argv[2]);
    } else if (!strcmp(cmd, "rm") && argc >= 2) {
        run_fm_sync(P5_FM_DELETE, argv[1], NULL);
    } else if (!strcmp(cmd, "unrar") && argc >= 3) {
        p5_unrar_options o = {0};
        o.on_progress = on_progress;
        o.overwrite = true;
        p5_unrar *u = NULL;
        if (p5_unrar_create(&u, argv[1], argv[2], &o) == P5_OK) {
            p5_status st = p5_unrar_extract_all(u);
            P5_LOG("unrar: %s", st == P5_OK ? "ok" : "failed");
            p5_unrar_destroy(u);
        }
    } else if (!strcmp(cmd, "pack") && argc >= 3) {
        p5_ffpfsc_options o = {0};
        o.on_progress = on_progress;
        p5_status st = p5_ffpfsc_pack(argv[1], argv[2], &o);
        P5_LOG("pack: %s", st == P5_OK ? "ok" : "failed");
    } else if (!strcmp(cmd, "verify") && argc >= 2) {
        bool ok = false;
        p5_ffpfsc_verify(argv[1], &ok);
        P5_LOG("verify: %s", ok ? "INTEGRITY OK" : "CORRUPT");
    } else if (!strcmp(cmd, "exit")) {
        return 0;
    } else {
        P5_ERRL("unknown/invalid command: %s", cmd);
        usage();
    }
    return 1;
}

#define MAX_ARGS 8

static int split_line(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    P5_LOG("ps5-dl-filemanager-payload v" P5_VERSION " starting (pid %d)",
           getpid());

    if (p5_fm_create(&g_fm, 0) != P5_OK) {
        P5_ERRL("fatal: file manager pool init failed");
        return 1;
    }

    if (argc > 1) {
        /* One-shot mode: payload.elf <cmd> ... */
        dispatch(argc - 1, argv + 1);
        p5_fm_destroy(g_fm);
        return 0;
    }

    /* Resident mode: read commands from stdin until "exit" / EOF. */
    usage();
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        char *args[MAX_ARGS];
        int n = split_line(line, args);
        if (n == 0)
            continue;
        if (!dispatch(n, args))
            break;
    }

    p5_fm_destroy(g_fm);
    P5_LOG("payload exiting");
    return 0;
}
