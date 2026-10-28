/*
 * fs.c - "fsp-srv", the filesystem service.
 *
 *   IStorage     a block of bytes: the game's RomFS (romfs.c builds it from the dump folder)
 *   IFileSystem  files and folders: save data, kept in a folder on the host
 *   IFile, IDirectory
 *
 * Save data goes to $LAA_SAVE_DIR, or ~/.local/share/LAARecomp/save, then
 * <program id>/<user id>/. Paths from the game are absolute inside the mounted filesystem
 * ("/save.bin"); ".." is refused so the game can't reach outside its folder.
 * switchbrew.org/wiki/Filesystem_services
 */
#define _DEFAULT_SOURCE
#include "services.h"
#include <time.h>
#include "nvn/nvn.h"
#include "kernel.h"
#include "romfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define FS_RESULT(desc) MAKE_RESULT(2, desc)
#define RESULT_PATH_NOT_FOUND      FS_RESULT(1)
#define RESULT_PATH_ALREADY_EXISTS FS_RESULT(2)
#define RESULT_TARGET_NOT_FOUND    FS_RESULT(1002)
#define RESULT_OUT_OF_RANGE        FS_RESULT(3005)
#define RESULT_INVALID_PATH        FS_RESULT(6001)

#define FS_PATH_MAX 0x301

static uint32_t errno_result(void) {
    return errno == ENOENT || errno == ENOTDIR ? RESULT_PATH_NOT_FOUND
         : errno == EEXIST || errno == ENOTEMPTY ? RESULT_PATH_ALREADY_EXISTS : FS_RESULT(4000);
}

/* mkdir -p */
static int make_dirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* ================================================================ RomFS as IStorage */
static void romfs_storage_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                               /* Read(offset, size) -> B buffer */
        uint64_t off = ipc_in64(call, 0), size = ipc_in64(call, 8);
        IpcBuffer b = ipc_out_buffer(call, 0);
        if (size > b.size) size = b.size;
        void *dst = ipc_guest_ptr(b.addr, size);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (!dst || romfs_read(off, dst, size) != 0) call->result = RESULT_OUT_OF_RANGE;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (nvn_sync_tracing())                     /* LAA_TRACE_SYNC: file reads in the timeline */
            fprintf(stderr, "sync: %.2f thread %d read 0x%llx bytes from the RomFS in %.2f ms (%s)\n",
                    t0.tv_sec * 1e3 + t0.tv_nsec / 1e6, kthread_current() ? (int)kthread_current()->id : -1,
                    (unsigned long long)size, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6,
                    romfs_file_at(off) ? strstr(romfs_file_at(off), "romfs") ? strstr(romfs_file_at(off), "romfs") : romfs_file_at(off) : "tables");
        static uint64_t reads, bytes;
        reads++;
        bytes += size;
        if (ipc_trace && (reads % 200 == 0 || size >= (1u << 20)))
            fprintf(stderr, "fs: romfs read #%" PRIu64 " 0x%" PRIx64 " +0x%" PRIx64 " (total %.1f MB)\n", reads, off,
                    size, (double)bytes / 1e6);
        break;
    }
    case 4:                                 /* GetSize -> u64 */
        ipc_out64(call, romfs_size());
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType romfs_storage_type = { "fs:IStorage(RomFS)", romfs_storage_dispatch, NULL };

/* ================================================================ host folder as IFileSystem */
typedef struct { char root[2048]; } HostFs;

/* Does the path contain a ".." component? */
static int has_dotdot(const char *path) {
    for (const char *p = path; *p;) {
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 1;
        p += n + (end != NULL);
    }
    return 0;
}

/* Host path for a game path in the X buffer `index`. Returns 0, or a result code. */
static uint32_t host_path(const HostFs *fs, const IpcCall *call, int index, char *out, size_t out_size) {
    IpcBuffer b = ipc_in_buffer(call, index);
    char path[FS_PATH_MAX + 1] = { 0 };
    const char *p = ipc_guest_ptr(b.addr, b.size);
    if (!p) return RESULT_INVALID_PATH;
    memcpy(path, p, b.size < FS_PATH_MAX ? b.size : FS_PATH_MAX);
    if (path[0] != '/' || has_dotdot(path)) return RESULT_INVALID_PATH;
    snprintf(out, out_size, "%s%s", fs->root, path);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;   /* "dir/" -> "dir" */
    return 0;
}

/* ---- IFile ---- */
typedef struct { int fd; } HostFile;

static void file_destroy(ServiceObject *self) {
    HostFile *f = self->state;
    close(f->fd);
    free(f);
}

static void file_dispatch(ServiceObject *self, IpcCall *call) {
    HostFile *f = self->state;
    switch (call->cmd) {
    case 0: {                               /* Read(option, offset, size) -> bytes read, B buffer */
        uint64_t off = ipc_in64(call, 8), size = ipc_in64(call, 16);
        IpcBuffer b = ipc_out_buffer(call, 0);
        if (size > b.size) size = b.size;
        uint8_t *dst = ipc_guest_ptr(b.addr, size);
        if (!dst) { call->result = RESULT_OUT_OF_RANGE; break; }
        ssize_t r = pread(f->fd, dst, size, (off_t)off);
        if (r < 0) { call->result = errno_result(); break; }
        ipc_out64(call, (uint64_t)r);
        break;
    }
    case 1: {                               /* Write(option, offset, size, A buffer) */
        uint64_t off = ipc_in64(call, 8), size = ipc_in64(call, 16);
        IpcBuffer b = ipc_in_buffer(call, 0);
        if (size > b.size) size = b.size;
        const uint8_t *src = ipc_guest_ptr(b.addr, size);
        if (!src || pwrite(f->fd, src, size, (off_t)off) != (ssize_t)size) call->result = errno_result();
        break;
    }
    case 2:                                 /* Flush */
        break;
    case 3:                                 /* SetSize(size) */
        if (ftruncate(f->fd, (off_t)ipc_in64(call, 0)) != 0) call->result = errno_result();
        break;
    case 4: {                               /* GetSize -> u64 */
        struct stat st;
        if (fstat(f->fd, &st) != 0) { call->result = errno_result(); break; }
        ipc_out64(call, (uint64_t)st.st_size);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType file_type = { "fs:IFile", file_dispatch, file_destroy };

/* ---- IDirectory ---- */
typedef struct { char path[4096]; int mode; long pos; } HostDir;   /* mode: 1 = dirs, 2 = files */

/* The next entry the directory should report, starting at d->pos. 0 when there are no more. */
static int next_entry(HostDir *d, char *name, int *is_file, uint64_t *size) {
    DIR *h = opendir(d->path);
    if (!h) return 0;
    long idx = 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(h)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[8192];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
        if (stat(full, &st) != 0) continue;
        int file = !S_ISDIR(st.st_mode);
        if (!(d->mode & (file ? 2 : 1))) continue;
        if (idx++ < d->pos) continue;
        snprintf(name, FS_PATH_MAX, "%s", e->d_name);
        *is_file = file;
        *size = file ? (uint64_t)st.st_size : 0;
        d->pos++;
        found = 1;
        break;
    }
    closedir(h);
    return found;
}

static void directory_dispatch(ServiceObject *self, IpcCall *call) {
    HostDir *d = self->state;
    switch (call->cmd) {
    case 0: {                               /* Read -> count; entries (0x310 bytes each) in B buffer */
        IpcBuffer b = ipc_out_buffer(call, 0);
        uint8_t *out = ipc_guest_ptr(b.addr, b.size);
        uint64_t n = 0;
        char name[FS_PATH_MAX];
        int is_file;
        uint64_t size;
        while (out && (n + 1) * 0x310 <= b.size && next_entry(d, name, &is_file, &size)) {
            uint8_t *e = out + n * 0x310;   /* name[0x301], pad, u8 type @0x304, u64 size @0x308 */
            memset(e, 0, 0x310);
            memcpy(e, name, strlen(name));
            e[0x304] = (uint8_t)is_file;
            memcpy(e + 0x308, &size, 8);
            n++;
        }
        ipc_out64(call, n);
        break;
    }
    case 1: {                               /* GetEntryCount -> u64 */
        HostDir tmp = *d;
        tmp.pos = 0;
        char name[FS_PATH_MAX];
        int is_file;
        uint64_t size, n = 0;
        while (next_entry(&tmp, name, &is_file, &size)) n++;
        ipc_out64(call, n);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType directory_type = { "fs:IDirectory", directory_dispatch, NULL };

static void hostfs_destroy(ServiceObject *self) { free(self->state); }

static void hostfs_dispatch(ServiceObject *self, IpcCall *call) {
    HostFs *fs = self->state;
    char path[4096], path2[4096];
    uint32_t r = 0;
    switch (call->cmd) {
    case 0: {                               /* CreateFile(mode, size, path) */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) { r = errno_result(); break; }
        if (ftruncate(fd, (off_t)ipc_in64(call, 8)) != 0) r = errno_result();
        close(fd);
        break;
    }
    case 1:                                 /* DeleteFile(path) */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        if (unlink(path) != 0) r = errno_result();
        break;
    case 2:                                 /* CreateDirectory(path) */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        if (mkdir(path, 0755) != 0) r = errno_result();
        break;
    case 3:                                 /* DeleteDirectory(path) */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        if (rmdir(path) != 0) r = errno_result();
        break;
    case 5: case 6:                         /* RenameFile, RenameDirectory(old path, new path) */
        if ((r = host_path(fs, call, 0, path, sizeof path)) || (r = host_path(fs, call, 1, path2, sizeof path2))) break;
        if (rename(path, path2) != 0) r = errno_result();
        break;
    case 7: {                               /* GetEntryType(path) -> 0 directory, 1 file */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        struct stat st;
        if (stat(path, &st) != 0) { r = RESULT_PATH_NOT_FOUND; break; }
        ipc_out32(call, S_ISDIR(st.st_mode) ? 0 : 1);
        break;
    }
    case 8: {                               /* OpenFile(mode, path) -> IFile. mode: 1 read, 2 write, 4 append */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        uint32_t mode = ipc_in32(call, 0);
        int fd = open(path, (mode & 6) ? O_RDWR : O_RDONLY);
        if (fd < 0) { r = errno_result(); break; }
        HostFile *f = malloc(sizeof *f);
        if (!f) exit(1);
        f->fd = fd;
        ipc_out_object(call, ipc_new_object(&file_type, f));
        break;
    }
    case 9: {                               /* OpenDirectory(mode, path) -> IDirectory */
        if ((r = host_path(fs, call, 0, path, sizeof path))) break;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) { r = RESULT_PATH_NOT_FOUND; break; }
        HostDir *d = calloc(1, sizeof *d);
        if (!d) exit(1);
        snprintf(d->path, sizeof d->path, "%s", path);
        d->mode = (int)(ipc_in32(call, 0) & 3);
        ipc_out_object(call, ipc_new_object(&directory_type, d));
        break;
    }
    case 10:                                /* Commit: writes already went to the host */
        break;
    case 11: case 12: {                     /* GetFreeSpaceSize, GetTotalSpaceSize -> u64 */
        struct statvfs sv;
        if (statvfs(fs->root, &sv) != 0) { r = errno_result(); break; }
        ipc_out64(call, (uint64_t)(call->cmd == 11 ? sv.f_bavail : sv.f_blocks) * sv.f_frsize);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
    call->result = r;
}
static const ServiceType hostfs_type = { "fs:IFileSystem", hostfs_dispatch, hostfs_destroy };

static ServiceObject *open_host_fs(const char *dir) {
    HostFs *fs = calloc(1, sizeof *fs);
    if (!fs || make_dirs(dir) != 0) { perror(dir); exit(1); }
    snprintf(fs->root, sizeof fs->root, "%s", dir);
    return ipc_new_object(&hostfs_type, fs);
}

static void save_root(char *out, size_t n) {
    const char *env = getenv("LAA_SAVE_DIR"), *home = getenv("HOME");
    if (env) snprintf(out, n, "%s", env);
    else snprintf(out, n, "%s/.local/share/LAARecomp/save", home ? home : ".");
}

/* ---- ISaveDataInfoReader: lists save data; we report none ---- */
static void save_info_reader_dispatch(ServiceObject *self, IpcCall *call) {
    if (call->cmd == 0) { ipc_out64(call, 0); return; }   /* Read -> count */
    ipc_unknown_command(self, call);
}
static const ServiceType save_info_reader_type = { "fs:ISaveDataInfoReader", save_info_reader_dispatch, NULL };

/* ================================================================ fsp-srv */
static void fsp_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 1:                                 /* SetCurrentProcess(pid) */
    case 1003:                              /* DisableAutoSaveDataCreation */
    case 1004:                              /* SetGlobalAccessLogMode */
    case 1006:                              /* OutputAccessLogToSdCard */
        break;
    case 1005:                              /* GetGlobalAccessLogMode -> u32 */
        ipc_out32(call, 0);
        break;
    case 22:                                /* CreateSaveDataFileSystem: made when opened */
        break;
    case 51: {                              /* OpenSaveDataFileSystem(space, SaveDataAttribute) */
        /* attribute (at +8): program id, user id (16 bytes), system save id, type... */
        uint64_t program = ipc_in64(call, 8), user_lo = ipc_in64(call, 16), user_hi = ipc_in64(call, 24);
        uint8_t type = (uint8_t)ipc_in32(call, 0x28);
        if (program == 0) program = kproc.program_id;   /* 0 = "this program" */
        char dir[2048], root[1024];
        save_root(root, sizeof root);
        snprintf(dir, sizeof dir, "%s/%016" PRIx64 "/%016" PRIx64 "%016" PRIx64 "_type%u", root, program,
                 user_hi, user_lo, type);
        if (ipc_trace) fprintf(stderr, "fs: save data -> %s\n", dir);
        ipc_out_object(call, open_host_fs(dir));
        break;
    }
    case 61:                                /* OpenSaveDataInfoReaderBySaveDataSpaceId */
        ipc_out_object(call, ipc_new_object(&save_info_reader_type, NULL));
        break;
    case 200:                               /* OpenDataStorageByCurrentProcess -> the RomFS */
        ipc_out_object(call, ipc_new_object(&romfs_storage_type, NULL));
        break;
    case 203:                               /* OpenPatchDataStorageByCurrentProcess: no update */
        call->result = RESULT_TARGET_NOT_FOUND;
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType fsp_type = { "fsp-srv", fsp_dispatch, NULL };

ServiceObject *service_fsp_create(void) { return ipc_new_object(&fsp_type, NULL); }
