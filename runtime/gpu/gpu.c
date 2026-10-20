/*
 * gpu.c - the null GPU: address space and syncpoints, no drawing yet. See gpu.h.
 */
#include "gpu.h"
#include "guest_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;

/* ================================================================ address space */
/* Defaults like the real driver: small (4 KiB) pages below `split`, big pages above */
static uint64_t va_start = 0x04000000, va_split = 1ull << 34, va_end = 1ull << 37;
static uint32_t big_page = 0x20000;
static uint64_t next_small, next_big;            /* bump allocators: GPU VA is plentiful */

/* Mappings: sorted list of (va, guest address, size) */
typedef struct { uint64_t va, addr, size; } GpuMapping;
static GpuMapping *maps;
static int nmaps, cap_maps;

void gpu_va_set_layout(uint64_t start, uint64_t split, uint64_t end, uint32_t big_page_size) {
    pthread_mutex_lock(&gpu_lock);
    if (start) va_start = start;
    if (split) va_split = split;
    if (end) va_end = end;
    if (big_page_size) big_page = big_page_size;
    pthread_mutex_unlock(&gpu_lock);
}

void gpu_va_get_layout(uint64_t *start, uint64_t *split, uint64_t *end, uint32_t *big_page_size) {
    *start = va_start; *split = va_split; *end = va_end; *big_page_size = big_page;
}

uint64_t gpu_va_alloc(uint64_t size, uint64_t page_size) {
    pthread_mutex_lock(&gpu_lock);
    int big = page_size >= big_page;
    uint64_t align = big ? big_page : 0x1000;
    uint64_t *next = big ? &next_big : &next_small;
    if (!*next) *next = big ? va_split : va_start;
    uint64_t at = (*next + align - 1) & ~(align - 1);
    uint64_t limit = big ? va_end : va_split;
    uint64_t r = 0;
    if (at + size <= limit && at + size > at) {
        r = at;
        *next = at + ((size + align - 1) & ~(align - 1));
    }
    pthread_mutex_unlock(&gpu_lock);
    return r;
}

static void unmap_locked(uint64_t va, uint64_t size) {
    uint64_t end = va + size;
    for (int k = 0; k < nmaps; k++) {
        GpuMapping *m = &maps[k];
        uint64_t m_end = m->va + m->size;
        if (m_end <= va || m->va >= end) continue;
        if (m->va < va && m_end > end) {             /* split in two (callers reserved room) */
            memmove(m + 2, m + 1, (nmaps - k - 1) * sizeof *m);
            maps[k + 1] = (GpuMapping){ end, m->addr + (end - m->va), m_end - end };
            nmaps++;
            m->size = va - m->va;
            return;
        }
        if (m->va < va) { m->size = va - m->va; continue; }
        if (m_end > end) { m->addr += end - m->va; m->size = m_end - end; m->va = end; continue; }
        memmove(m, m + 1, (nmaps - k - 1) * sizeof *m);
        nmaps--;
        k--;
    }
}

static void reserve_locked(int extra) {
    if (nmaps + extra <= cap_maps) return;
    int ncap = cap_maps ? cap_maps * 2 : 256;
    while (ncap < nmaps + extra) ncap *= 2;
    GpuMapping *n = realloc(maps, ncap * sizeof *n);
    if (!n) { fprintf(stderr, "gpu: out of memory\n"); exit(1); }
    maps = n;
    cap_maps = ncap;
}

void gpu_map(uint64_t va, uint64_t guest_addr, uint64_t size) {
    pthread_mutex_lock(&gpu_lock);
    reserve_locked(2);
    unmap_locked(va, size);
    int at = 0;
    while (at < nmaps && maps[at].va < va) at++;
    memmove(&maps[at + 1], &maps[at], (nmaps - at) * sizeof *maps);
    maps[at] = (GpuMapping){ va, guest_addr, size };
    nmaps++;
    pthread_mutex_unlock(&gpu_lock);
}

void gpu_unmap(uint64_t va, uint64_t size) {
    pthread_mutex_lock(&gpu_lock);
    reserve_locked(1);
    unmap_locked(va, size);
    pthread_mutex_unlock(&gpu_lock);
}

uint64_t gpu_translate(uint64_t va) {
    pthread_mutex_lock(&gpu_lock);
    uint64_t r = 0;
    int lo = 0, hi = nmaps;                          /* binary search: last mapping with start <= va */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (maps[mid].va <= va) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && va - maps[lo - 1].va < maps[lo - 1].size) r = maps[lo - 1].addr + (va - maps[lo - 1].va);
    pthread_mutex_unlock(&gpu_lock);
    return r;
}

/* ================================================================ syncpoints */
static struct { uint32_t min, max; int used; } syncpoints[GPU_MAX_SYNCPOINTS];

uint32_t gpu_syncpoint_alloc(void) {
    pthread_mutex_lock(&gpu_lock);
    uint32_t id = 0;
    for (uint32_t k = 1; k < GPU_MAX_SYNCPOINTS; k++) {   /* 0 is reserved: "no syncpoint" */
        if (!syncpoints[k].used) { syncpoints[k].used = 1; id = k; break; }
    }
    pthread_mutex_unlock(&gpu_lock);
    return id;
}

uint32_t gpu_syncpoint_read(uint32_t id) {
    return id < GPU_MAX_SYNCPOINTS ? __atomic_load_n(&syncpoints[id].min, __ATOMIC_SEQ_CST) : 0;
}
uint32_t gpu_syncpoint_read_max(uint32_t id) {
    return id < GPU_MAX_SYNCPOINTS ? __atomic_load_n(&syncpoints[id].max, __ATOMIC_SEQ_CST) : 0;
}

int gpu_fence_reached(GpuFence f) {
    return (int32_t)(gpu_syncpoint_read(f.id) - f.value) >= 0;
}

/* ================================================================ command processor
 * A GPFIFO entry points at a command list ("pushbuffer"): bits 0-39 its GPU address, bits
 * 42-62 its length in 32-bit words. A command list is a series of method calls: a header
 * word, then data words:
 *   bits 0-12 method (register index), 13-15 subchannel, 16-28 count (or inline data),
 *   29-31 mode: 1 increasing (method+1 per word), 3 non-increasing, 4 inline (the value is
 *   in the count field, no data words), 5 increase once (first word to method, rest to +1).
 * Each subchannel has an engine bound to it (method 0: "bind object" with its class).
 * Methods below 0x40 are for the channel itself, whatever the subchannel.
 *
 * The null GPU only carries out what the CPU can see: semaphore releases and inline
 * memory writes. Drawing comes in phase 4.
 * References: NVIDIA's open-gpu-doc (clb06f.h, clb197.h, cla140.h) and yuzu's
 * dma_pusher/puller/maxwell_3d. */
enum { CLASS_3D = 0xB197, CLASS_I2M = 0xA140 };

static uint32_t sub_class[8];                    /* engine class bound to each subchannel */
static uint32_t regs[8][0x2000];                 /* each subchannel's method registers */
static uint64_t submissions, entries_seen;
static uint64_t i2m_dst, i2m_written;            /* inline-to-memory: where the next word goes */

static uint8_t *gpu_ptr(uint64_t va, uint64_t size) {
    uint64_t a = gpu_translate(va);
    if (!a || a + size > guest_mem_size) return NULL;
    if (gpu_translate(va + size - 1) != a + size - 1) return NULL;   /* must be one contiguous mapping */
    return guest_mem + a;
}

static uint64_t gpu_ticks(void) {                /* GPU timestamps are in nanoseconds */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Write a semaphore: 4 bytes (payload) or 16 bytes (payload as u64, then a timestamp) */
static void semaphore_write(uint64_t va, uint64_t value, int long_form) {
    uint8_t *p = gpu_ptr(va, long_form ? 16 : 4);
    if (!p) { fprintf(stderr, "gpu: semaphore at unmapped GPU address 0x%llx\n", (unsigned long long)va); return; }
    if (long_form) {
        uint64_t t = gpu_ticks();
        memcpy(p, &value, 8);
        memcpy(p + 8, &t, 8);
    } else {
        uint32_t v = (uint32_t)value;
        __atomic_store_n((uint32_t *)p, v, __ATOMIC_RELEASE);
    }
}

/* Inline to memory (the I2M class, also part of the 3D class): methods 0x60/0x61 set the
 * destination, 0x6C starts a transfer, each 0x6D data word is written next. Only linear
 * ("pitch") destinations are handled; swizzled texture uploads come with the renderer. */
static void i2m_method(int sub, uint32_t m, uint32_t v) {
    uint32_t *r = regs[sub];
    if (m == 0x6C) {                             /* LAUNCH_DMA */
        i2m_dst = ((uint64_t)(r[0x60] & 0xFF) << 32) | r[0x61];
        i2m_written = 0;
    } else if (m == 0x6D) {                      /* LOAD_INLINE_DATA */
        if ((r[0x6C] & 1) == 0) return;          /* block-linear destination: not yet */
        uint8_t *p = gpu_ptr(i2m_dst + i2m_written, 4);
        if (p) memcpy(p, &v, 4);
        i2m_written += 4;
    }
}

static void method(int sub, uint32_t m, uint32_t v) {
    if (m < 0x40) {                              /* the channel's own methods */
        if (m == 0) { sub_class[sub] = v & 0xFFFF; return; }   /* bind an engine */
        regs[0][m] = v;
        if (m == 0x07 && (v & 0x1F) == 2) {      /* SEMAPHORED: release (4:0 operation = 2) */
            uint64_t va = ((uint64_t)(regs[0][0x04] & 0xFF) << 32) | regs[0][0x05];
            semaphore_write(va, regs[0][0x06], !(v & (1u << 24)));   /* bit 24: 4-byte release */
        }
        return;                                  /* (acquires: nothing to wait for here) */
    }
    if (m >= 0x2000) return;
    regs[sub][m] = v;
    uint32_t cls = sub_class[sub];
    if ((cls == CLASS_3D || cls == CLASS_I2M) && m >= 0x60 && m <= 0x6D) i2m_method(sub, m, v);
    if (cls == CLASS_3D && m == 0x6C3) {         /* report semaphore: address 0x6C0/0x6C1, payload 0x6C2 */
        uint32_t *r = regs[sub];
        uint64_t va = ((uint64_t)(r[0x6C0] & 0xFF) << 32) | r[0x6C1];
        uint32_t op = v & 3, type = (v >> 23) & 0x1F, short_form = (v >> 28) & 1;
        if (op == 0) semaphore_write(va, r[0x6C2], !short_form);          /* release */
        else if (op == 2) semaphore_write(va, type == 0 ? r[0x6C2] : 0, !short_form);   /* report */
    }
}

static void run_command_list(uint64_t entry) {
    uint64_t va = entry & 0xFFFFFFFFFCull, words = (entry >> 42) & 0x1FFFFF;
    if (!words) return;
    const uint8_t *p = gpu_ptr(va, words * 4);
    if (!p) { fprintf(stderr, "gpu: command list at unmapped GPU address 0x%llx\n", (unsigned long long)va); return; }
    uint32_t w[1];
    for (uint64_t i = 0; i < words;) {
        uint32_t h;
        memcpy(&h, p + 4 * i++, 4);
        uint32_t m = h & 0x1FFF, sub = (h >> 13) & 7, count = (h >> 16) & 0x1FFF, mode = h >> 29;
        if (mode == 4) { method((int)sub, m, count); continue; }        /* inline: value in the header */
        for (uint32_t k = 0; k < count && i < words; k++, i++) {
            memcpy(w, p + 4 * i, 4);
            uint32_t target = mode == 1 || mode == 0 ? m + k : mode == 5 ? m + (k > 0) : m;
            method((int)sub, target, w[0]);
        }
    }
}

GpuFence gpu_submit(uint32_t syncpoint, const uint64_t *entries, uint32_t count, uint32_t increments) {
    pthread_mutex_lock(&gpu_lock);
    submissions++;
    entries_seen += count;
    pthread_mutex_unlock(&gpu_lock);
    for (uint32_t k = 0; k < count; k++) run_command_list(entries[k]);   /* (gpu_translate locks) */
    pthread_mutex_lock(&gpu_lock);
    GpuFence f = { syncpoint, 0 };
    if (syncpoint < GPU_MAX_SYNCPOINTS) {
        syncpoints[syncpoint].max += increments;
        f.value = syncpoints[syncpoint].max;
        /* Null GPU: the work is "done" now, including the syncpoint increments it contained */
        __atomic_store_n(&syncpoints[syncpoint].min, syncpoints[syncpoint].max, __ATOMIC_SEQ_CST);
    }
    pthread_mutex_unlock(&gpu_lock);
    return f;
}
