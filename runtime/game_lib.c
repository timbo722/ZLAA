/*
 * game_lib.c - find and open the game's code library.
 *
 * The recompiled game (~150K functions, ~150 MB of machine code) is not part of the app:
 * it is made from the player's own dump (recomp.py, then the C compiler) and opened here at
 * start-up with dlopen, the C library's way to load a shared library while the program runs;
 * dlsym then gives the address of a named symbol inside it. The library exports only its
 * tables and an ABI id; the game's functions call back into the runtime (recomp_call_addr,
 * recomp_svc, ...), which the program that opens it provides.
 *
 * Its name carries the build id of the dump's main (the first 8 bytes in hex, as the log
 * prints them): each game version has its own library, and the dump picks the right one.
 * 1.1.0's main is the same file as 1.0.1's, so those two share a library.
 */
#include "game_lib.h"
#include "recomp_runtime.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* SHA-256 of recomp_runtime.h, set by CMakeLists.txt for the runtime and the game library
 * alike: the generated code is compiled against that header (the CPU struct, the inline
 * helpers), so a library built against another version of it must not run */
#ifndef RECOMP_ABI
#define RECOMP_ABI "unknown"
#endif

/* The game's functions by host address, to name them in backtraces */
static struct { uintptr_t fn; uint64_t addr; } *by_host;
static uint64_t by_host_count;

static int cmp_host(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static int main_build_id(const char *exefs_dir, char hex[17]) {
    char path[4096];
    snprintf(path, sizeof path, "%s/main", exefs_dir);
    uint8_t h[0x60];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(h, 1, sizeof h, f) : 0;
    if (f) fclose(f);
    if (n != sizeof h || memcmp(h, "NSO0", 4) != 0) {
        fprintf(stderr, "game: %s is missing or not an NSO file\n", path);
        return -1;
    }
    for (int b = 0; b < 8; b++) snprintf(hex + 2 * b, 3, "%02X", h[0x40 + b]);   /* (NSO header +0x40: build id) */
    return 0;
}

/* src -> dst through dst.tmp (a half-copied library is never opened), read-only, with src's
 * modification time: a later import with another size or time replaces it */
static int copy_file(const char *src, const char *dst, const struct stat *st) {
    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    int in = open(src, O_RDONLY), out = in < 0 ? -1 : open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = in >= 0 && out >= 0;
    static char buf[1 << 20];
    for (ssize_t n; ok && (n = read(in, buf, sizeof buf)) != 0;)
        ok = n > 0 && write(out, buf, (size_t)n) == n;
    struct timespec times[2] = { st->st_atim, st->st_mtim };
    ok = ok && futimens(out, times) == 0 && fchmod(out, 0444) == 0;
    if (out >= 0 && close(out) != 0) ok = 0;
    if (in >= 0) close(in);
    if (ok && rename(tmp, dst) == 0) return 0;
    perror(tmp);
    unlink(tmp);
    return -1;
}

/* Was the library built against this runtime's headers? Its id (game_abi.c) is a plain string in it, so the file is
 * searched rather than loaded; the answer is kept for the file as it is (size, time). */
static int abi_matches(const char *path, const struct stat *st) {
    static char last_path[4200];
    static off_t last_size;
    static time_t last_time;
    static int last;
    if (strcmp(path, last_path) == 0 && st->st_size == last_size && st->st_mtime == last_time) return last;
    int fd = open(path, O_RDONLY), found = 0;
    static char buf[1 << 20];
    size_t n = strlen(RECOMP_ABI), keep = 0;
    ssize_t r;
    while (fd >= 0 && !found && (r = read(fd, buf + keep, sizeof buf - keep)) > 0) {
        size_t len = keep + (size_t)r;
        found = memmem(buf, len, RECOMP_ABI, n) != NULL;
        keep = len < n ? len : n - 1;                /* (an id split between two reads) */
        memmove(buf, buf + len - keep, keep);
    }
    if (fd >= 0) close(fd);
    snprintf(last_path, sizeof last_path, "%s", path);
    last_size = st->st_size;
    last_time = st->st_mtime;
    return last = found;
}

int game_lib_outdated(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && !abi_matches(path, &st);
}

int game_lib_find(const char *exefs_dir, const char *lib_dir, const char *import_dir, char id[17]) {
    if (main_build_id(exefs_dir, id) != 0) return -1;
    char path[4200];
    struct stat st;
    int outdated = 0;
    for (int k = 0; k < 2; k++) {                    /* (an import that fits wins, as in game_lib_load) */
        const char *dir = k == 0 ? import_dir : lib_dir;
        if (!dir) continue;
        snprintf(path, sizeof path, "%s/libgame-%s.so", dir, id);
        if (stat(path, &st) != 0) continue;
        if (abi_matches(path, &st)) return 0;
        outdated = 1;
    }
    return outdated ? 2 : 1;
}

int game_lib_load(const char *exefs_dir, const char *lib_dir, const char *import_dir) {
    char id[17], name[64], path[4200];
    if (main_build_id(exefs_dir, id) != 0) return -1;
    snprintf(name, sizeof name, "libgame-%s.so", id);
    snprintf(path, sizeof path, "%s/%s", lib_dir, name);

    /* a library put in the import folder (built on a PC and pushed) replaces ours */
    struct stat in, have;
    char src[4200];
    if (import_dir) snprintf(src, sizeof src, "%s/%s", import_dir, name);
    if (import_dir && stat(src, &in) == 0 && abi_matches(src, &in) &&     /* (an outdated one is left there) */
        (stat(path, &have) != 0 || have.st_size != in.st_size || have.st_mtim.tv_sec != in.st_mtim.tv_sec)) {
        mkdir(lib_dir, 0755);
        printf("game: copying %s (%lld MB) to %s\n", src, (long long)in.st_size >> 20, lib_dir);
        if (copy_file(src, path, &in) != 0) return -1;
    }

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "game: no game code for this dump (main build id %s...): %s\n"
                        "      It is built from your own dump: recompiler/recomp.py, then the game library build.\n",
                id, dlerror());
        return -1;
    }
    const char *abi = dlsym(h, "recomp_game_abi");
    if (!abi || strcmp(abi, RECOMP_ABI) != 0) {
        fprintf(stderr, "game: %s was built against another recomp_runtime.h (%.16s, this runtime %.16s): rebuild it\n",
                path, abi ? abi : "no id", RECOMP_ABI);
        dlclose(h);
        return -1;
    }
    const RecompFuncEntry *funcs = dlsym(h, "recomp_func_table");
    const RecompModuleInfo *mods = dlsym(h, "recomp_modules");
    const uint64_t *nfuncs = dlsym(h, "recomp_func_count"), *nmods = dlsym(h, "recomp_module_count");
    if (!funcs || !mods || !nfuncs || !nmods) {
        fprintf(stderr, "game: %s has no function table\n", path);
        dlclose(h);
        return -1;
    }
    recomp_use_game(funcs, *nfuncs, mods, *nmods);

    by_host = malloc(*nfuncs * sizeof *by_host);
    if (by_host) {
        for (uint64_t k = 0; k < *nfuncs; k++) by_host[k].fn = (uintptr_t)funcs[k].fn, by_host[k].addr = funcs[k].addr;
        qsort(by_host, *nfuncs, sizeof *by_host, cmp_host);
        by_host_count = *nfuncs;
    }
    printf("game: %s (%llu functions)\n", path, (unsigned long long)*nfuncs);
    return 0;
}

const char *game_lib_symbol(uintptr_t pc, char *buf, size_t size, unsigned long *offset) {
    if (!by_host_count || pc < by_host[0].fn || pc > by_host[by_host_count - 1].fn + (1u << 20)) return NULL;
    uint64_t lo = 0, hi = by_host_count;            /* the last function starting at or before pc */
    while (hi - lo > 1) {
        uint64_t mid = (lo + hi) / 2;
        if (by_host[mid].fn <= pc) lo = mid; else hi = mid;
    }
    snprintf(buf, size, "f_%llx", (unsigned long long)by_host[lo].addr);
    if (offset) *offset = (unsigned long)(pc - by_host[lo].fn);
    return buf;
}
