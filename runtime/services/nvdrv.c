/*
 * nvdrv.c - "nvdrv", the NVIDIA GPU driver interface.
 *
 * The game opens device files and sends them Linux-style ioctls:
 *
 *   /dev/nvmap            memory handles: a block of the game's memory the GPU may use
 *   /dev/nvhost-as-gpu    the GPU's address space: map nvmap handles at GPU addresses
 *   /dev/nvhost-gpu       a GPU channel: submit command lists (GPFIFO), get fences back
 *   /dev/nvhost-ctrl      syncpoints: read them, wait for them
 *   /dev/nvhost-ctrl-gpu  facts about the GPU (model, sizes)
 *   /dev/nvhost-nvdec, -vic, -nvjpg   video decoder, image compositor, JPEG engine: "host1x"
 *                         channels; for now their work completes at once, doing nothing
 *
 * An ioctl code packs: direction (bits 31:30: 1 = in, 2 = out), argument size (29:16),
 * device type (15:8) and command number (7:0). The argument struct comes in the input
 * buffer and goes back, updated, in the output buffer.
 * Codes and layouts: switchbrew.org/wiki/NV_services. GPU side: runtime/gpu/gpu.c.
 */
#include "services.h"
#include "kernel.h"
#include "gpu/gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* NvResult codes the driver returns (not IPC results) */
enum { NV_SUCCESS = 0, NV_NOT_IMPLEMENTED = 1, NV_NOT_SUPPORTED = 2, NV_BAD_PARAMETER = 4, NV_TIMEOUT = 5,
       NV_INSUFFICIENT_MEMORY = 6, NV_BAD_VALUE = 0xB, NV_CONFIG_VAR_NOT_FOUND = 0x30006 };

enum { DEV_NVMAP = 1, DEV_CTRL, DEV_CTRL_GPU, DEV_AS_GPU, DEV_GPU, DEV_HOST1X };
static const struct { const char *path; int type; } devices[] = {
    { "/dev/nvmap", DEV_NVMAP },          { "/dev/nvhost-ctrl", DEV_CTRL },
    { "/dev/nvhost-ctrl-gpu", DEV_CTRL_GPU }, { "/dev/nvhost-as-gpu", DEV_AS_GPU },
    { "/dev/nvhost-gpu", DEV_GPU },          { "/dev/nvhost-nvdec", DEV_HOST1X },
    { "/dev/nvhost-vic", DEV_HOST1X },       { "/dev/nvhost-nvjpg", DEV_HOST1X },
};

#define MAX_FDS 64
static struct { int type; uint32_t syncpoint; } fds[MAX_FDS];   /* type 0 = closed */

/* ---- nvmap handles: a block of guest memory (allocated by the game) for the GPU ---- */
#define MAX_NVMAP 4096
static struct { int used; uint32_t size, align, flags, heap; uint8_t kind; uint64_t addr; } nvmap[MAX_NVMAP];

/* ---- buffer mappings in the GPU address space, so UNMAP_BUFFER knows the size ---- */
#define MAX_GPU_MAPPINGS 8192
static struct { uint64_t va, size; } gmaps[MAX_GPU_MAPPINGS];

static uint32_t g32(const uint8_t *b, int off) { uint32_t v; memcpy(&v, b + off, 4); return v; }
static uint64_t g64(const uint8_t *b, int off) { uint64_t v; memcpy(&v, b + off, 8); return v; }
static void p32(uint8_t *b, int off, uint32_t v) { memcpy(b + off, &v, 4); }
static void p64(uint8_t *b, int off, uint64_t v) { memcpy(b + off, &v, 8); }

/* Ioctl3 passes a second output buffer: newer SDKs get the bulk of the answer there */
static void out2_write(uint8_t *out2, uint32_t out2_size, const void *p, uint32_t n) {
    if (out2) memcpy(out2, p, n < out2_size ? n : out2_size);
}

static int unknown_ioctl(const char *dev, uint32_t code) {
    fprintf(stderr, "\nnvdrv: %s ioctl 0x%08x (type 0x%02x nr 0x%02x, %u bytes) is not implemented yet\n",
            dev, code, (code >> 8) & 0xFF, code & 0xFF, (code >> 16) & 0x3FFF);
    kernel_dump_state();
    exit(1);
}

/* ================================================================ /dev/nvmap */
static int nvmap_ioctl(uint32_t code, uint8_t *b) {
    uint32_t h;
    switch (code & 0xFFFF) {
    case 0x0101:                                 /* CREATE(size) -> handle */
        for (h = 1; h < MAX_NVMAP && nvmap[h].used; h++) {}
        if (h == MAX_NVMAP) return NV_INSUFFICIENT_MEMORY;
        memset(&nvmap[h], 0, sizeof nvmap[h]);
        nvmap[h].used = 1;
        nvmap[h].size = g32(b, 0);
        p32(b, 4, h);
        return NV_SUCCESS;
    case 0x0103:                                 /* FROM_ID(id) -> handle (our ids are the handles) */
        h = g32(b, 0);
        if (h == 0 || h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
        p32(b, 4, h);
        return NV_SUCCESS;
    case 0x0104:                                 /* ALLOC(handle, heapmask, flags, align, kind, addr) */
        h = g32(b, 0);
        if (h == 0 || h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
        nvmap[h].heap = g32(b, 4);
        nvmap[h].flags = g32(b, 8);
        nvmap[h].align = g32(b, 12) < 0x1000 ? 0x1000 : g32(b, 12);
        nvmap[h].kind = b[16];
        nvmap[h].addr = g64(b, 24);
        return NV_SUCCESS;
    case 0x0105:                                 /* FREE(handle) -> address, size, flags */
        h = g32(b, 0);
        if (h == 0 || h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
        p64(b, 8, nvmap[h].addr);
        p32(b, 16, nvmap[h].size);
        p32(b, 20, nvmap[h].flags);
        nvmap[h].used = 0;
        return NV_SUCCESS;
    case 0x0109: {                               /* PARAM(handle, which) -> value */
        h = g32(b, 0);
        if (h == 0 || h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
        uint32_t v;
        switch (g32(b, 4)) {
        case 1: v = nvmap[h].size; break;
        case 2: v = nvmap[h].align; break;
        case 4: v = nvmap[h].heap ? nvmap[h].heap : 0x40000000; break;
        case 5: v = nvmap[h].kind; break;
        case 3: case 6: v = 0; break;            /* base address (not given out), compression */
        default: return NV_BAD_VALUE;
        }
        p32(b, 8, v);
        return NV_SUCCESS;
    }
    case 0x010E:                                 /* GET_ID(handle) -> id */
        h = g32(b, 4);
        if (h == 0 || h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
        p32(b, 0, h);
        return NV_SUCCESS;
    }
    return unknown_ioctl("/dev/nvmap", code);
}

/* ================================================================ /dev/nvhost-as-gpu */
static void record_mapping(uint64_t va, uint64_t size) {
    for (int k = 0; k < MAX_GPU_MAPPINGS; k++)
        if (!gmaps[k].size || gmaps[k].va == va) { gmaps[k].va = va; gmaps[k].size = size; return; }
    fprintf(stderr, "nvdrv: too many GPU mappings\n");
    exit(1);
}

static int as_ioctl(uint32_t code, uint8_t *b, uint32_t size, uint8_t *out2, uint32_t out2_size) {
    uint64_t start, split, end;
    uint32_t big;
    gpu_va_get_layout(&start, &split, &end, &big);
    switch (code & 0xFFFF) {
    case 0x4101:                                 /* BIND_CHANNEL */
        return NV_SUCCESS;
    case 0x4102: {                               /* ALLOC_SPACE(pages, page_size, flags, -, offset/align) */
        uint64_t pages = g32(b, 0), page_size = g32(b, 4), bytes = pages * page_size;
        if (g32(b, 8) & 1) return NV_SUCCESS;    /* FIXED: the game chose the address itself */
        uint64_t va = gpu_va_alloc(bytes, page_size);
        if (!va) return NV_INSUFFICIENT_MEMORY;
        p64(b, 16, va);
        return NV_SUCCESS;
    }
    case 0x4103:                                 /* FREE_SPACE(offset, pages, page_size) */
        gpu_unmap(g64(b, 0), (uint64_t)g32(b, 8) * g32(b, 12));
        return NV_SUCCESS;
    case 0x4105: {                               /* UNMAP_BUFFER(offset) */
        uint64_t va = g64(b, 0);
        for (int k = 0; k < MAX_GPU_MAPPINGS; k++) {
            if (gmaps[k].size && gmaps[k].va == va) {
                gpu_unmap(va, gmaps[k].size);
                gmaps[k].size = 0;
                break;
            }
        }
        return NV_SUCCESS;
    }
    case 0x4106: {                               /* MAP_BUFFER_EX(flags, kind, handle, page_size, buf off, size, va) */
        uint32_t flags = g32(b, 0), h = g32(b, 8), page_size = g32(b, 12);
        uint64_t buf_off = g64(b, 16), map_size = g64(b, 24), va = g64(b, 32);
        if (h == 0 || h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
        if (map_size == 0) map_size = nvmap[h].size - buf_off;
        if (!(flags & 1) && !(flags & 0x100)) {  /* not FIXED_OFFSET or REMAP: we pick the address */
            va = gpu_va_alloc(map_size, page_size ? page_size : 0x1000);
            if (!va) return NV_INSUFFICIENT_MEMORY;
        }
        gpu_map(va, nvmap[h].addr + buf_off, map_size);
        record_mapping(va, map_size);
        p64(b, 32, va);
        return NV_SUCCESS;
    }
    case 0x4107:                                 /* ALLOC_AS(big_page_size) */
        gpu_va_set_layout(0, 0, 0, g32(b, 0));
        return NV_SUCCESS;
    case 0x4108: {                               /* GET_VA_REGIONS -> 2 regions: small pages, big pages */
        p32(b, 8, 2 * 24);
        p64(b, 16, start);  p32(b, 24, 0x1000); p32(b, 28, 0); p64(b, 32, (split - start) / 0x1000);
        p64(b, 40, split);  p32(b, 48, big);    p32(b, 52, 0); p64(b, 56, (end - split) / big);
        out2_write(out2, out2_size, b + 16, 2 * 24);
        return NV_SUCCESS;
    }
    case 0x4109:                                 /* ALLOC_AS_EX(big page, fd, flags, -, start, end, split) */
        gpu_va_set_layout(g64(b, 16), g64(b, 32), g64(b, 24), g32(b, 0));
        return NV_SUCCESS;
    case 0x4114: {                               /* REMAP(entries[]): sparse (re)mappings, in big pages */
        for (uint32_t off = 0; off + 20 <= size; off += 20) {
            uint32_t h = g32(b, off + 4);
            uint64_t mem_off = (uint64_t)g32(b, off + 8) * big, va = (uint64_t)g32(b, off + 12) * big;
            uint64_t bytes = (uint64_t)g32(b, off + 16) * big;
            if (h == 0) { gpu_unmap(va, bytes); continue; }
            if (h >= MAX_NVMAP || !nvmap[h].used) return NV_BAD_VALUE;
            gpu_map(va, nvmap[h].addr + mem_off, bytes);
        }
        return NV_SUCCESS;
    }
    }
    return unknown_ioctl("/dev/nvhost-as-gpu", code);
}

/* ================================================================ /dev/nvhost-ctrl-gpu */
static int ctrl_gpu_ioctl(uint32_t code, uint8_t *b, uint8_t *out2, uint32_t out2_size) {
    switch (code & 0xFFFF) {
    case 0x4701:                                 /* ZCULL_GET_CTX_SIZE */
        p32(b, 0, 1);
        return NV_SUCCESS;
    case 0x4702: {                               /* ZCULL_GET_INFO: the Tegra X1's values */
        static const uint32_t info[10] = { 0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10 };
        memcpy(b, info, sizeof info);
        return NV_SUCCESS;
    }
    case 0x4703:                                 /* ZBC_SET_TABLE: fast-clear colours; nothing to do */
    case 0x4707:                                 /* FLUSH_L2 */
        return NV_SUCCESS;
    case 0x4705: {                               /* GET_CHARACTERISTICS: describe the GM20B */
        static const uint32_t ch[40] = {
            0x120, 0xB, 0xA1, 1,                 /* arch GM200, impl GM20B, rev, num_gpc */
            0x40000, 0, 0, 0,                    /* l2 size (u64), video memory size (u64): shared RAM */
            2, 0x20, 0x20000, 0x20000,           /* tpc per gpc, bus AXI, big page, compression page */
            0x1B, 0x30000, 1, 0x503,             /* pde bits, big page sizes, gpc mask, sm version */
            0x503, 0x80, 0x28, 0,                /* spa version, warp count, va bits, reserved */
            0x55, 0,                             /* flags (u64) */
            0x902D, 0xB197, 0xB1C0, 0xB06F,      /* classes: 2D, 3D, compute, gpfifo */
            0xA140, 0xB0B5, 1, 0,                /* inline-to-memory, DMA copy, max fbps, fbp mask */
            2, 1, 0, 1,                          /* ltc per fbp, lts per ltc, tex per tpc, max gpc */
            0x21D70, 0,                          /* rop l2 masks */
            0x30326D67, 0x62,                    /* chip name "gm20b" (u64, little-endian) */
        };
        p64(b, 0, 0xA0);                         /* bytes of characteristics written */
        memcpy(b + 16, ch, sizeof ch);
        out2_write(out2, out2_size, ch, sizeof ch);
        return NV_SUCCESS;
    }
    case 0x4706: {                               /* GET_TPC_MASKS -> 2 TPCs */
        uint32_t mask = 3;
        if (g32(b, 0)) p64(b, 16, mask);
        out2_write(out2, out2_size, &mask, 4);
        return NV_SUCCESS;
    }
    case 0x4712:                                 /* NUM_VSMS */
        p32(b, 0, 2);
        return NV_SUCCESS;
    case 0x4713:                                 /* VSMS_MAPPING */
        p32(b, 0, 0);
        return NV_SUCCESS;
    case 0x4714:                                 /* ZBC_GET_ACTIVE_SLOT_MASK */
        p32(b, 0, 7);
        p32(b, 4, 1);
        return NV_SUCCESS;
    case 0x471C: {                               /* GET_GPU_TIME (ns) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        p64(b, 0, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
        return NV_SUCCESS;
    }
    }
    return unknown_ioctl("/dev/nvhost-ctrl-gpu", code);
}

/* ================================================================ /dev/nvhost-gpu (channel) */
/* SUBMIT_GPFIFO flags. ADD_INCREMENT: the driver appends one syncpoint increment.
 * INCREMENT_VALUE: the command lists already contain fence.value increments (NVN does this
 * and computes its fence values itself), so the syncpoint's max just moves up by that much. */
enum { GPFIFO_FENCE_WAIT = 1, GPFIFO_ADD_INCREMENT = 2, GPFIFO_INCREMENT_VALUE = 0x100 };

static int channel_ioctl(int fd, uint32_t code, uint8_t *b, uint32_t size, const uint8_t *extra, uint32_t extra_size) {
    switch (code & 0xFFFF) {
    case 0x4801:                                 /* SET_NVMAP_FD */
    case 0x4803:                                 /* SET_TIMEOUT */
    case 0x4805:                                 /* ALLOC_GPFIFO */
    case 0x480B:                                 /* ZCULL_BIND */
    case 0x480C:                                 /* SET_ERROR_NOTIFIER */
    case 0x480D:                                 /* SET_PRIORITY */
    case 0x480E: case 0x480F:                    /* ENABLE, DISABLE */
    case 0x4812:                                 /* EVENT_ID_CONTROL */
    case 0x4714:                                 /* SET_USER_DATA */
    case 0x481D:                                 /* SET_TIMESLICE */
    case 0x0007:                                 /* SET_SUBMIT_TIMEOUT */
        return NV_SUCCESS;
    case 0x4818: case 0x481A:                    /* ALLOC_GPFIFO_EX / EX2 -> fence */
        p32(b, 12, fds[fd].syncpoint);
        p32(b, 16, gpu_syncpoint_read_max(fds[fd].syncpoint));
        return NV_SUCCESS;
    case 0x4809:                                 /* ALLOC_OBJ_CTX(class) -> object id */
        p64(b, 8, g32(b, 0));
        return NV_SUCCESS;
    case 0x4715:                                 /* GET_USER_DATA */
        p64(b, 0, 0);
        return NV_SUCCESS;
    case 0x4816:                                 /* GET_ERROR_INFO: no error */
        memset(b, 0, 0x80);
        return NV_SUCCESS;
    case 0x4817:                                 /* GET_ERROR_NOTIFICATION */
        memset(b, 0, 16);
        p32(b, 14, 0xFFFF);
        return NV_SUCCESS;
    case 0x0002: case 0x0003:                    /* GET_SYNCPOINT, GET_WAITBASE */
        p32(b, 4, 0);
        return NV_SUCCESS;
    case 0x4808: case 0x4819:                    /* SUBMIT_GPFIFO(_RETRY)(-, count, flags, fence, entries[]) */
    case 0x481B: case 0x481C: {                  /* SUBMIT_GPFIFO2(_RETRY): same, entries via Ioctl2 */
        uint32_t count = g32(b, 8), flags = g32(b, 12);
        if (ipc_trace) fprintf(stderr, "nvdrv: submit %u entries, flags 0x%x, fence in (%u, %u)\n", count, flags,
                               g32(b, 16), g32(b, 20));
        /* the entries follow the header, or (Ioctl2) come in a second input buffer */
        const uint8_t *e = extra_size ? extra : b + 24;
        uint32_t avail = extra_size ? extra_size : (size > 24 ? size - 24 : 0);
        if ((uint64_t)count * 8 > avail) return NV_BAD_PARAMETER;
        uint64_t *entries = malloc((size_t)count * 8 + 1);
        if (!entries) return NV_INSUFFICIENT_MEMORY;
        memcpy(entries, e, (size_t)count * 8);
        /* FENCE_WAIT: wait for a fence before starting. The null GPU finishes everything
         * at once, so any fence it gave out is already reached. */
        uint32_t increments = (flags & GPFIFO_INCREMENT_VALUE) ? g32(b, 20) : (flags & GPFIFO_ADD_INCREMENT) ? 1 : 0;
        GpuFence f = gpu_submit(fds[fd].syncpoint, entries, count, increments);
        free(entries);
        p32(b, 16, f.id);                        /* the fence to wait for: this work done */
        p32(b, 20, f.value);
        return NV_SUCCESS;
    }
    }
    return unknown_ioctl("/dev/nvhost-gpu", code);
}

/* ================================================================ host1x channels (nvdec, vic, nvjpg) */
static uint32_t next_iova = 0x10000000;          /* fake device addresses for MAP_CMD_BUFFER */

static int host1x_ioctl(int fd, uint32_t code, uint8_t *b, uint32_t size) {
    switch (code & 0xFFFF) {
    case 0x0002:                                 /* GET_SYNCPOINT(module) -> syncpoint id */
        p32(b, 4, fds[fd].syncpoint);
        return NV_SUCCESS;
    case 0x0003:                                 /* GET_WAITBASE -> 0 */
        p32(b, 4, 0);
        return NV_SUCCESS;
    case 0x0007: case 0x0008:                    /* SET_SUBMIT_TIMEOUT, SET_CLK_RATE */
    case 0x4801:                                 /* SET_NVMAP_FD */
        return NV_SUCCESS;
    case 0x0009: case 0x0025: {                  /* MAP_CMD_BUFFER(_EX): handles -> device addresses */
        uint32_t n = g32(b, 0);
        for (uint32_t k = 0; k < n && 12 + 8 * k + 8 <= size; k++) {
            uint32_t h = g32(b, 12 + 8 * k);
            uint32_t len = h < MAX_NVMAP && nvmap[h].used ? nvmap[h].size : 0x1000;
            p32(b, 12 + 8 * k + 4, next_iova);
            next_iova += (len + 0xFFFF) & ~0xFFFFu;
        }
        return NV_SUCCESS;
    }
    case 0x000A: case 0x0026:                    /* UNMAP_CMD_BUFFER(_EX) */
        return NV_SUCCESS;
    case 0x0001: case 0x0024: {                  /* SUBMIT(_EX): command buffers, relocations, increments */
        /* header (4 counts), then: command buffers (12 bytes each), relocations (16), reloc
         * shifts (4), syncpoint increments (0x14: id, count, 12 bytes padding), fences (u32 out) */
        uint32_t ncmd = g32(b, 0), nreloc = g32(b, 4), nincr = g32(b, 8), nfence = g32(b, 12);
        uint32_t at = 16 + 12 * ncmd + 16 * nreloc + 4 * nreloc;
        uint32_t total = 0;
        for (uint32_t k = 0; k < nincr && at + 0x14 <= size; k++, at += 0x14) total += g32(b, at + 4);
        GpuFence f = gpu_submit(fds[fd].syncpoint, NULL, 0, total);   /* nothing decoded yet */
        for (uint32_t k = 0; k < nfence && at + 4 <= size; k++, at += 4) p32(b, at, f.value);
        return NV_SUCCESS;
    }
    }
    return unknown_ioctl("host1x channel (nvdec/vic/nvjpg)", code);
}

/* ================================================================ /dev/nvhost-ctrl */
static int ctrl_ioctl(uint32_t code, uint8_t *b) {
    switch (code & 0xFFFF) {
    case 0x0014:                                 /* SYNCPT_READ(id) -> value */
        p32(b, 4, gpu_syncpoint_read(g32(b, 0)));
        return NV_SUCCESS;
    case 0x001A:                                 /* SYNCPT_READ_MAX(id) -> value */
        p32(b, 4, gpu_syncpoint_read_max(g32(b, 0)));
        return NV_SUCCESS;
    case 0x0016: case 0x0019:                    /* SYNCPT_WAIT(EX)(id, threshold, timeout) */
    case 0x001D: case 0x001E: {                  /* SYNCPT_WAIT_EVENT(_EX) */
        GpuFence f = { g32(b, 0), g32(b, 4) };
        if (!gpu_fence_reached(f)) {
            /* Only work we were given can be waited for, and the null GPU finished it all */
            fprintf(stderr, "nvdrv: wait for syncpoint %u >= %u, which is at %u\n", f.id, f.value,
                    gpu_syncpoint_read(f.id));
            return NV_TIMEOUT;
        }
        if ((code & 0xFFFF) != 0x0016) p32(b, 12, gpu_syncpoint_read(f.id));
        return NV_SUCCESS;
    }
    case 0x001B:                                 /* GET_CONFIG: no debug settings exist */
        return NV_CONFIG_VAR_NOT_FOUND;
    case 0x001C:                                 /* SYNCPT_CLEAR_EVENT_WAIT */
    case 0x001F: case 0x0020: case 0x0021:       /* SYNCPT_ALLOC/FREE_EVENT(_BATCH) */
        return NV_SUCCESS;
    case 0x0022:                                 /* SYNCPT_GET_SHIFT */
        p32(b, 0, 0);
        return NV_SUCCESS;
    }
    return unknown_ioctl("/dev/nvhost-ctrl", code);
}

/* ================================================================ the service */
static int do_ioctl(uint32_t fd, uint32_t code, uint8_t *b, uint32_t size, const uint8_t *extra, uint32_t extra_size,
                    uint8_t *out2, uint32_t out2_size) {
    if (fd >= MAX_FDS || !fds[fd].type) return NV_BAD_VALUE;
    switch (fds[fd].type) {
    case DEV_NVMAP:    return nvmap_ioctl(code, b);
    case DEV_AS_GPU:   return as_ioctl(code, b, size, out2, out2_size);
    case DEV_CTRL_GPU: return ctrl_gpu_ioctl(code, b, out2, out2_size);
    case DEV_GPU:      return channel_ioctl((int)fd, code, b, size, extra, extra_size);
    case DEV_CTRL:     return ctrl_ioctl(code, b);
    case DEV_HOST1X:   return host1x_ioctl((int)fd, code, b, size);
    }
    return NV_NOT_SUPPORTED;
}

static void nvdrv_dispatch(ServiceObject *self, IpcCall *call) {
    switch (call->cmd) {
    case 0: {                                    /* Open(path) -> fd, error */
        IpcBuffer pb = ipc_in_buffer(call, 0);
        char path[64] = { 0 };
        const char *p = ipc_guest_ptr(pb.addr, pb.size);
        if (p) memcpy(path, p, pb.size < sizeof path - 1 ? pb.size : sizeof path - 1);
        int type = 0;
        for (size_t k = 0; k < sizeof devices / sizeof *devices; k++)
            if (strcmp(devices[k].path, path) == 0) type = devices[k].type;
        if (!type) {
            fprintf(stderr, "\nnvdrv: device %s is not implemented yet\n", path);
            ipc_unknown_command(self, call);
        }
        uint32_t fd;
        for (fd = 1; fd < MAX_FDS && fds[fd].type; fd++) {}
        if (fd == MAX_FDS) { ipc_out32(call, 0); ipc_out32(call, NV_INSUFFICIENT_MEMORY); break; }
        fds[fd].type = type;
        fds[fd].syncpoint = type == DEV_GPU || type == DEV_HOST1X ? gpu_syncpoint_alloc() : 0;   /* one per channel */
        if (ipc_trace) fprintf(stderr, "nvdrv: open %s -> fd %u\n", path, fd);
        ipc_out32(call, fd);
        ipc_out32(call, NV_SUCCESS);
        break;
    }
    case 1: case 11: case 12: {                  /* Ioctl / Ioctl2 (extra input) / Ioctl3 (extra output) */
        uint32_t fd = ipc_in32(call, 0), code = ipc_in32(call, 4);
        IpcBuffer in = ipc_in_buffer(call, 0), out = ipc_out_buffer(call, 0);
        uint32_t size = (uint32_t)(in.size > out.size ? in.size : out.size);
        if (size < ((code >> 16) & 0x3FFF)) size = (code >> 16) & 0x3FFF;
        uint8_t *b = calloc(1, size + 16);
        const uint8_t *src = ipc_guest_ptr(in.addr, in.size);
        if (!b) exit(1);
        if (src) memcpy(b, src, in.size);
        const uint8_t *extra = NULL;
        uint32_t extra_size = 0;
        if (call->cmd == 11) {
            IpcBuffer in2 = ipc_in_buffer(call, 1);
            extra = ipc_guest_ptr(in2.addr, in2.size);
            extra_size = extra ? (uint32_t)in2.size : 0;
        }
        uint8_t *out2 = NULL;
        uint32_t out2_size = 0;
        if (call->cmd == 12) {
            IpcBuffer o2 = ipc_out_buffer(call, 1);
            out2 = ipc_guest_ptr(o2.addr, o2.size);
            out2_size = out2 ? (uint32_t)o2.size : 0;
        }
        if (ipc_trace) fprintf(stderr, "nvdrv: fd %u ioctl 0x%08x\n", fd, code);
        uint32_t err = (uint32_t)do_ioctl(fd, code, b, size, extra, extra_size, out2, out2_size);
        uint8_t *dst = ipc_guest_ptr(out.addr, out.size);
        if (dst) memcpy(dst, b, out.size);
        free(b);
        ipc_out32(call, err);
        break;
    }
    case 2: {                                    /* Close(fd) -> error */
        uint32_t fd = ipc_in32(call, 0);
        if (fd < MAX_FDS) fds[fd].type = 0;
        ipc_out32(call, NV_SUCCESS);
        break;
    }
    case 3:                                      /* Initialize(process, transfer memory, size) */
    case 8:                                      /* SetAruid */
        ipc_out32(call, NV_SUCCESS);
        break;
    case 4: {                                    /* QueryEvent(fd, event id) -> event, error */
        char name[64];
        snprintf(name, sizeof name, "nvdrv: fd %u (type %d) event 0x%x", ipc_in32(call, 0),
                 ipc_in32(call, 0) < MAX_FDS ? fds[ipc_in32(call, 0)].type : -1, ipc_in32(call, 4));
        ipc_out_handle(call, kevent_readable_handle(kevent_new(name)), 0);
        ipc_out32(call, NV_SUCCESS);
        break;
    }
    case 13:                                     /* SetGraphicsFirmwareMemoryMarginEnabled */
        break;
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType nvdrv_type = { "nvdrv", nvdrv_dispatch, NULL };

ServiceObject *service_nvdrv_create(void) { return ipc_new_object(&nvdrv_type, NULL); }
