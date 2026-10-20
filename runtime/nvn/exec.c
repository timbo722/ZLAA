/*
 * exec.c - running recorded NVN command lists, and presenting frames.
 *
 * For now nothing is drawn. We do what the game can observe from the CPU: fences become
 * signalled, counters and reported values are written, buffer copies/clears/updates happen,
 * and presents are paced to the display's 60 Hz. The Vulkan renderer will take over the
 * drawing commands.
 */
#include "nvn.h"
#include <stdio.h>
#include <inttypes.h>
#include <time.h>

static void run_list(uint64_t at, int depth) {
    if (depth > 8) { fprintf(stderr, "nvn: command lists call each other too deeply\n"); return; }
    for (;;) {
        Cmd *r = gptr(at);
        if (r->op >= CMD_COUNT || r->size < sizeof(Cmd)) {
            fprintf(stderr, "nvn: bad command record at %#" PRIx64 " (op %u size %u)\n", at, r->op, r->size);
            return;
        }
        if (nvn_trace > 1) fprintf(stderr, "nvn:   %s\n", nvn_command_names[r->op]);
        switch (r->op) {
        case CMD_END:
            return;
        case CMD_JUMP:
            at = r->a[0];
            continue;
        case CMD_CALL:                                   /* a[0] = count, extra = handles */
            for (uint64_t i = 0; i < r->a[0]; i++) {
                uint64_t h;
                memcpy(&h, r->extra + 8 * i, 8);
                run_list(h, depth + 1);
            }
            break;
        case CMD_FENCE_SYNC:                             /* a = sync, condition, flags */
            ((Sync *)gptr(r->a[0]))->signaled = 1;
            break;
        case CMD_SIGNAL_EVENT: {                         /* a = event, mode, location, flags, value */
            uint64_t addr = ((Event *)gptr(r->a[0]))->address;
            uint32_t v = (uint32_t)r->a[4];
            g_st32(addr, r->a[1] == 1 ? g_ld32(addr) + v : v);
            break;
        }
        case CMD_REPORT_COUNTER:                         /* a = counter type, address: {u64 value, u64 timestamp} */
            if (r->a[1]) { g_st64(r->a[1], 0); g_st64(r->a[1] + 8, nvn_gpu_ticks()); }
            break;
        case CMD_REPORT_VALUE:                           /* a = value, address */
            if (r->a[1]) g_st32(r->a[1], (uint32_t)r->a[0]);
            break;
        case CMD_CLEAR_BUFFER:                           /* a = address, size, u32 value */
            for (uint64_t o = 0; o + 4 <= r->a[1]; o += 4) g_st32(r->a[0] + o, (uint32_t)r->a[2]);
            break;
        case CMD_COPY_BUFFER_TO_BUFFER:                  /* a = src, dst, size, flags */
            memmove(gptr(r->a[1]), gptr(r->a[0]), r->a[2]);
            break;
        case CMD_UPDATE_UNIFORM_BUFFER:                  /* a = buffer, buffer size, offset, size; extra = data */
            memcpy(gptr(r->a[0] + r->a[2]), r->extra, r->a[3]);
            break;
        default:
            break;
        }
        at += r->size;
    }
}

void nvn_execute(uint64_t handle) {
    if (nvn_trace > 1) fprintf(stderr, "nvn: submit %#" PRIx64 "\n", handle);
    run_list(handle, 0);
}

/* ---- present: wait for the next vblank (60 Hz), or the interval-th one ---- */

static struct timespec next_vblank;
static uint64_t frames;

void nvn_present(const Window *w, uint64_t texture) {
    (void)texture;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t interval = w->present_interval ? w->present_interval : 1;
    if (!next_vblank.tv_sec) next_vblank = now;
    for (uint32_t i = 0; i < interval; i++) {
        next_vblank.tv_nsec += 16666667;
        if (next_vblank.tv_nsec >= 1000000000) { next_vblank.tv_sec++; next_vblank.tv_nsec -= 1000000000; }
    }
    /* if we fell far behind (a long load), start counting from now instead of catching up */
    double behind = (double)(now.tv_sec - next_vblank.tv_sec) + (now.tv_nsec - next_vblank.tv_nsec) / 1e9;
    if (behind > 0.1) next_vblank = now;
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_vblank, NULL);
    frames++;
    if (frames == 1 || frames % 300 == 0) fprintf(stderr, "nvn: frame %" PRIu64 " presented\n", frames);
}
