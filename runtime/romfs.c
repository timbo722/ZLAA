/*
 * romfs.c - build a RomFS image's tables from a folder; serve reads from the files.
 *
 * Image layout we produce (all offsets are in the header):
 *   0x000  header (0x50 bytes: ten u64 offsets/sizes), zero-padded to 0x200
 *          directory hash table, directory entries, file hash table, file entries
 *          file data (each file 16-byte aligned), read from the host files on demand
 *
 * Directory entry: parent, next sibling, first child dir, first child file, next in hash
 * chain, name length (all u32 byte offsets into the directory table; ~0 = none), name.
 * File entry: parent, next sibling, u64 data offset, u64 size, next in hash chain,
 * name length, name. Names are padded to 4 bytes. The root directory is at offset 0.
 */
#define _DEFAULT_SOURCE
#include "romfs.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NONE 0xFFFFFFFFu

typedef struct {
    char *name;
    int parent;
    int first_dir, first_file, next_sibling;   /* indices, -1 = none */
    uint32_t off;                              /* offset in the directory table */
} Dir;

typedef struct {
    char *name, *host_path;
    int parent, next_sibling;
    uint64_t size, data_off;                   /* data_off: from the start of file data */
    uint32_t off;                              /* offset in the file table */
} File;

static Dir *dirs;
static File *files;
static int ndirs, nfiles, cap_dirs, cap_files;
static uint8_t *meta;                          /* header + tables */
static uint64_t meta_size, data_start, image_size;

static void *grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    *cap = *cap ? *cap * 2 : 256;
    p = realloc(p, (size_t)*cap * elem);
    if (!p) { fprintf(stderr, "romfs: out of memory\n"); exit(1); }
    return p;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Add the contents of host directory `path` as children of dirs[d] (sorted, so the image
 * is the same every run). */
static int scan(int d, const char *path) {
    DIR *h = opendir(path);
    if (!h) { perror(path); return -1; }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(h)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        names = grow(names, &cap, n + 1, sizeof *names);
        names[n++] = strdup(e->d_name);
    }
    closedir(h);
    qsort(names, (size_t)n, sizeof *names, cmp_name);

    int last_dir = -1, last_file = -1;
    for (int k = 0; k < n; k++) {
        char *full = malloc(strlen(path) + strlen(names[k]) + 2);
        sprintf(full, "%s/%s", path, names[k]);
        struct stat st;
        if (stat(full, &st) != 0) { perror(full); return -1; }
        if (S_ISDIR(st.st_mode)) {
            dirs = grow(dirs, &cap_dirs, ndirs + 1, sizeof *dirs);
            int c = ndirs++;
            dirs[c] = (Dir){ names[k], d, -1, -1, -1, 0 };
            if (last_dir < 0) dirs[d].first_dir = c; else dirs[last_dir].next_sibling = c;
            last_dir = c;
            if (scan(c, full) != 0) return -1;
            free(full);
        } else {
            files = grow(files, &cap_files, nfiles + 1, sizeof *files);
            int c = nfiles++;
            files[c] = (File){ names[k], full, d, -1, (uint64_t)st.st_size, 0, 0 };
            if (last_file < 0) dirs[d].first_file = c; else files[last_file].next_sibling = c;
            last_file = c;
        }
    }
    free(names);
    return 0;
}

static uint32_t name_hash(uint32_t parent, const char *name) {
    uint32_t h = parent ^ 123456789u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h = (h >> 5) | (h << 27);
        h ^= *p;
    }
    return h;
}

/* How many hash buckets the official tools use for n entries (n, nudged to avoid small factors) */
static uint32_t bucket_count(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}

static uint32_t align4(uint32_t v) { return (v + 3) & ~3u; }
static void put32(uint64_t off, uint32_t v) { memcpy(meta + off, &v, 4); }
static void put64(uint64_t off, uint64_t v) { memcpy(meta + off, &v, 8); }

int romfs_open_dir(const char *host_dir) {
    dirs = grow(dirs, &cap_dirs, 1, sizeof *dirs);
    dirs[0] = (Dir){ strdup(""), 0, -1, -1, -1, 0 };
    ndirs = 1;
    if (scan(0, host_dir) != 0) return -1;

    /* table offsets of every entry */
    uint32_t dir_tab = 0, file_tab = 0;
    for (int k = 0; k < ndirs; k++) { dirs[k].off = dir_tab; dir_tab += 0x18 + align4((uint32_t)strlen(dirs[k].name)); }
    for (int k = 0; k < nfiles; k++) { files[k].off = file_tab; file_tab += 0x20 + align4((uint32_t)strlen(files[k].name)); }
    uint32_t dir_buckets = bucket_count((uint32_t)ndirs), file_buckets = bucket_count((uint32_t)nfiles);

    uint64_t dir_hash_off = 0x200, dir_meta_off = dir_hash_off + 4ull * dir_buckets;
    uint64_t file_hash_off = dir_meta_off + dir_tab, file_meta_off = file_hash_off + 4ull * file_buckets;
    meta_size = file_meta_off + file_tab;
    data_start = (meta_size + 0xF) & ~0xFull;
    uint64_t data = 0;
    for (int k = 0; k < nfiles; k++) {
        files[k].data_off = data;
        data = (data + files[k].size + 0xF) & ~0xFull;
    }
    image_size = data_start + data;

    meta = calloc(1, meta_size);
    if (!meta) { fprintf(stderr, "romfs: out of memory\n"); return -1; }
    const uint64_t header[10] = { 0x50, dir_hash_off, 4ull * dir_buckets, dir_meta_off, dir_tab,
                                  file_hash_off, 4ull * file_buckets, file_meta_off, file_tab, data_start };
    memcpy(meta, header, sizeof header);
    memset(meta + dir_hash_off, 0xFF, 4ull * dir_buckets);
    memset(meta + file_hash_off, 0xFF, 4ull * file_buckets);

    for (int k = 0; k < ndirs; k++) {
        Dir *d = &dirs[k];
        uint64_t e = dir_meta_off + d->off;
        uint32_t parent = dirs[d->parent].off, len = (uint32_t)strlen(d->name);
        uint32_t b = name_hash(parent, d->name) % dir_buckets;
        put32(e + 0x00, parent);
        put32(e + 0x04, d->next_sibling < 0 ? NONE : dirs[d->next_sibling].off);
        put32(e + 0x08, d->first_dir < 0 ? NONE : dirs[d->first_dir].off);
        put32(e + 0x0C, d->first_file < 0 ? NONE : files[d->first_file].off);
        memcpy(meta + e + 0x10, meta + dir_hash_off + 4 * b, 4);    /* chain: next = old bucket head */
        put32(dir_hash_off + 4 * b, d->off);
        put32(e + 0x14, len);
        memcpy(meta + e + 0x18, d->name, len);
    }
    for (int k = 0; k < nfiles; k++) {
        File *f = &files[k];
        uint64_t e = file_meta_off + f->off;
        uint32_t parent = dirs[f->parent].off, len = (uint32_t)strlen(f->name);
        uint32_t b = name_hash(parent, f->name) % file_buckets;
        put32(e + 0x00, parent);
        put32(e + 0x04, f->next_sibling < 0 ? NONE : files[f->next_sibling].off);
        put64(e + 0x08, f->data_off);
        put64(e + 0x10, f->size);
        memcpy(meta + e + 0x18, meta + file_hash_off + 4 * b, 4);
        put32(file_hash_off + 4 * b, f->off);
        put32(e + 0x1C, len);
        memcpy(meta + e + 0x20, f->name, len);
    }
    printf("romfs: %d directories, %d files, %.2f GB image from %s\n", ndirs, nfiles,
           (double)image_size / 1e9, host_dir);
    return 0;
}

uint64_t romfs_size(void) { return image_size; }

/* A few open host files, so reading a big file in pieces doesn't reopen it every time */
#define FD_CACHE 32
static struct { int file, fd; uint64_t last_use; } fd_cache[FD_CACHE];
static uint64_t use_counter;

static int file_fd(int f) {
    int victim = 0;
    for (int k = 0; k < FD_CACHE; k++) {
        if (fd_cache[k].fd > 0 && fd_cache[k].file == f) { fd_cache[k].last_use = ++use_counter; return fd_cache[k].fd; }
        if (fd_cache[k].last_use < fd_cache[victim].last_use) victim = k;
    }
    if (fd_cache[victim].fd > 0) close(fd_cache[victim].fd);
    int fd = open(files[f].host_path, O_RDONLY);
    if (fd < 0) { perror(files[f].host_path); fd_cache[victim].fd = 0; return -1; }
    fd_cache[victim].file = f;
    fd_cache[victim].fd = fd;
    fd_cache[victim].last_use = ++use_counter;
    return fd;
}

int romfs_read(uint64_t offset, void *dst, uint64_t size) {
    uint8_t *out = dst;
    if (offset > image_size || size > image_size - offset) return -1;
    while (size) {
        if (offset < data_start) {                       /* header and tables (+ padding) */
            uint64_t n = data_start - offset < size ? data_start - offset : size;
            for (uint64_t k = 0; k < n; k++) out[k] = offset + k < meta_size ? meta[offset + k] : 0;
            out += n; offset += n; size -= n;
            continue;
        }
        /* binary search: the last file starting at or before this offset */
        uint64_t d = offset - data_start;
        int lo = 0, hi = nfiles;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (files[mid].data_off <= d) lo = mid + 1; else hi = mid;
        }
        int f = lo - 1;
        uint64_t in_file = f >= 0 ? d - files[f].data_off : 0;
        uint64_t n;
        if (f >= 0 && in_file < files[f].size) {         /* inside a file's data */
            n = files[f].size - in_file < size ? files[f].size - in_file : size;
            int fd = file_fd(f);
            if (fd < 0) return -1;
            for (uint64_t done = 0; done < n;) {
                ssize_t r = pread(fd, out + done, n - done, (off_t)(in_file + done));
                if (r <= 0) { perror(files[f].host_path); return -1; }
                done += (uint64_t)r;
            }
        } else {                                         /* alignment padding between files */
            uint64_t next = f + 1 < nfiles ? files[f + 1].data_off : image_size - data_start;
            n = next - d < size ? next - d : size;
            memset(out, 0, n);
        }
        out += n; offset += n; size -= n;
    }
    return 0;
}
