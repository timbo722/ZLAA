/*
 * exec.c - running recorded NVN command lists, and presenting frames.
 *
 * For now nothing is drawn. We do what the game can observe from the CPU: fences become
 * signalled, counters and reported values are written, buffer copies/clears/updates happen,
 * and presents are paced to the display's 60 Hz. The Vulkan renderer will take over the
 * drawing commands.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                  /* sched_setaffinity */
#endif
#include "nvn.h"
#ifdef LAA_HAVE_SDL
#include "render/render.h"
#endif
#include "render/png.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>

static int sync_tracing(void);

/* One GPU: submissions and presents from different game threads take turns. */
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;

/* guest memory, in order with the GPU's work when there is a GPU */
static void gpu_write(uint64_t address, const void *data, uint32_t size) {
#ifdef LAA_HAVE_SDL
    if (render_active()) { render_guest_write(address, data, size); return; }
#endif
    memcpy(gptr(address), data, size);
}

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
        /* What the game reads back to see how far the GPU got: written when the GPU gets there
         * (the renderer runs ahead of the CPU), not when the command list is read */
        case CMD_FENCE_SYNC:                             /* a = sync, condition, flags */
            nvn_sync_fence(gptr(r->a[0]));
            break;
        case CMD_SIGNAL_EVENT: {                         /* a = event, mode, location, flags, value */
            uint64_t addr = ((Event *)gptr(r->a[0]))->address;
            uint32_t v = (uint32_t)r->a[4];
            gpu_write(addr, &(uint32_t){ r->a[1] == 1 ? g_ld32(addr) + v : v }, 4);
            break;
        }
        case CMD_REPORT_COUNTER:                         /* a = counter type, address: {u64 value, u64 timestamp} */
            if (sync_tracing()) fprintf(stderr, "sync: report counter %llu at %#llx\n", (unsigned long long)r->a[0],
                                        (unsigned long long)r->a[1]);
#ifdef LAA_HAVE_SDL
            /* timestamps: when the GPU gets there (the game times its frame with two and picks
             * its resolution by that); other counters, and if that fails: now */
            if (r->a[1] && r->a[0] == 0 && render_active() && render_guest_timestamp(r->a[1]) == 0) break;
#endif
            if (r->a[1]) gpu_write(r->a[1], (uint64_t[2]){ 0, nvn_gpu_ticks() }, 16);
            break;
        case CMD_REPORT_VALUE:                           /* a = value, address */
            if (r->a[1]) gpu_write(r->a[1], &(uint32_t){ (uint32_t)r->a[0] }, 4);
            break;
        /* buffer writes: in order with the GPU's other work, so through the renderer when there is one */
        case CMD_CLEAR_BUFFER:                           /* a = address, size, u32 value */
#ifdef LAA_HAVE_SDL
            if (render_active()) { render_command(r); break; }
#endif
            for (uint64_t o = 0; o + 4 <= r->a[1]; o += 4) g_st32(r->a[0] + o, (uint32_t)r->a[2]);
            break;
        case CMD_COPY_BUFFER_TO_BUFFER:                  /* a = src, dst, size, flags */
#ifdef LAA_HAVE_SDL
            if (render_active()) { render_command(r); break; }
#endif
            memmove(gptr(r->a[1]), gptr(r->a[0]), r->a[2]);
            break;
        case CMD_UPDATE_UNIFORM_BUFFER:                  /* a = buffer, buffer size, offset, size; extra = data */
#ifdef LAA_HAVE_SDL
            if (render_active()) { render_command(r); break; }
#endif
            memcpy(gptr(r->a[0] + r->a[2]), r->extra, r->a[3]);
            break;
        default:
#ifdef LAA_HAVE_SDL
            if (render_active()) render_command(r);
#endif
            break;
        }
        at += r->size;
    }
}

/* LAA_FRAME_STATS: with each "frame N presented", how the last 300 frames went: time between
 * presents (and how many took over 20 ms, i.e. missed a vsync), time in submissions (recording
 * plus waiting for the GPU) and in present */
static int frame_stats = -1;
static struct { double last, interval, interval_max, submit, present; int slow; } fs;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static uint64_t frames;

/* LAA_TRACE_SYNC=frame: a timeline of 3 frames from that one: submissions, presents, GPU waits */
int nvn_sync_tracing(void) { return sync_tracing(); }

static int sync_tracing(void) {                    /* (+ LAA_TRACE_SYNC_FRAMES=n, default 3) */
    static int64_t from = -2, n;
    if (from == -2) {
        from = getenv("LAA_TRACE_SYNC") ? strtoll(getenv("LAA_TRACE_SYNC"), NULL, 0) : -1;
        n = getenv("LAA_TRACE_SYNC_FRAMES") ? strtoll(getenv("LAA_TRACE_SYNC_FRAMES"), NULL, 0) : 3;
    }
    return from >= 0 && frames >= (uint64_t)from && frames < (uint64_t)(from + n);
}

void nvn_sync_fence(Sync *s) {
    s->signaled = 1;
    s->serial = 0;
#ifdef LAA_HAVE_SDL
    if (render_active()) s->serial = render_fence_serial();
#endif
}

/* A wait for work of the frame presented last (or later) only waits for the frame before it:
 * the GPU may run one frame behind, as in a native game. The game fences each frame and waits
 * for it a few ms after presenting, so its CPU and our GPU took turns and frames missed 60 Hz
 * (it alternates two syncs: its per-frame data looks double-buffered). Walking in Mabe Village:
 * GPU waits 4.5 -> 0 ms a frame, slow frames 110 -> 47 of 1500. LAA_FRAME_LAG=0: wait fully. */
static uint64_t present_serial[2];                 /* GPU serials at the last two presents */
static int frame_lag = -1;

int nvn_sync_wait(const Sync *s, uint64_t timeout_ns) {
#ifdef LAA_HAVE_SDL
    if (frame_lag < 0) frame_lag = !getenv("LAA_FRAME_LAG") || atoi(getenv("LAA_FRAME_LAG")) != 0;
    if (frame_lag && s->serial > present_serial[1] && present_serial[1]) {
        Sync capped = *s;
        capped.serial = present_serial[1];
        s = &capped;
        return render_active() ? render_wait_serial(capped.serial, timeout_ns) : 0;
    }
    /* LAA_TRACE_SYNC=frame: for 3 frames from that one, each wait: for which submission (and
     * the newest submitted / finished then), how long it took, who called */
    if (sync_tracing() && render_active() && s->serial) {
        uint64_t newest = render_fence_serial(), done = render_serial_completed();
        double t0 = now_ms();
        int r = render_wait_serial(s->serial, timeout_ns);
        fprintf(stderr, "sync: %.2f frame %" PRIu64 " wait for %" PRIu64 " (submitted %" PRIu64 ", done %" PRIu64 "), timeout %" PRIu64 ": %.2f ms, sync %#" PRIx64 "\n",
                t0, frames, s->serial, newest, done, timeout_ns, now_ms() - t0, (uint64_t)((uintptr_t)s - (uintptr_t)gptr(0)));
        return r;
    }
    if (render_active() && s->serial) return render_wait_serial(s->serial, timeout_ns);
#endif
    (void)timeout_ns;
    return s->signaled ? 0 : -1;
}

void nvn_queue_finish(void) {
#ifdef LAA_HAVE_SDL
    if (render_active()) {
        pthread_mutex_lock(&gpu_lock);
        render_flush();
        pthread_mutex_unlock(&gpu_lock);
    }
#endif
}

void nvn_execute(uint64_t handle) {
    if (nvn_trace > 1) fprintf(stderr, "nvn: submit %#" PRIx64 "\n", handle);
    double t0 = frame_stats > 0 ? now_ms() : 0;
    pthread_mutex_lock(&gpu_lock);
    run_list(handle, 0);
#ifdef LAA_HAVE_SDL
    if (render_active()) render_submitted();
    if (sync_tracing()) fprintf(stderr, "sync: %.2f submitted %" PRIu64 " (took %.2f ms)\n", now_ms(), render_fence_serial(), now_ms() - t0);
#endif
    pthread_mutex_unlock(&gpu_lock);
    if (frame_stats > 0) fs.submit += now_ms() - t0;
}

/* ---- present: wait for the next vblank (60 Hz), or the interval-th one ---- */

static struct timespec next_vblank;

/* LAA_SCREENSHOT=dir: save every 300th frame (LAA_SCREENSHOT_EVERY=n) as dir/frame_NNNNNN.png.
 * The picture comes from the texture's guest memory. */
static void screenshot(uint64_t texture, const int32_t crop[4]) {
    static const char *dir;
    static uint64_t every;
    if (!dir) {
        dir = getenv("LAA_SCREENSHOT") ? getenv("LAA_SCREENSHOT") : "";
        every = getenv("LAA_SCREENSHOT_EVERY") ? strtoull(getenv("LAA_SCREENSHOT_EVERY"), NULL, 0) : 300;
    }
    if (!*dir || !texture || !every || frames % every) return;
    const Texture *t = gptr(texture);
    TexShape s;
    tex_shape(&t->d, &s);
    if (s.fmt->bytes != 4 || s.fmt->bw != 1 || (t->d.flags & TEXTURE_FLAG_LINEAR)) return;
    LevelShape l = tex_level(&s, 0);
    uint8_t *rgba = malloc((size_t)s.width * s.height * 4);
    if (!rgba) return;
    uint32_t region[6] = { 0, 0, 0, l.wb, l.hb, 1 };
    int from_gpu = 0;
#ifdef LAA_HAVE_SDL
    pthread_mutex_lock(&gpu_lock);
    from_gpu = render_read_back(texture, rgba) == 0;
    pthread_mutex_unlock(&gpu_lock);
#endif
    if (!from_gpu) tex_copy_level(gptr(t->address), rgba, &s, &l, region, (uint64_t)s.width * 4, 0, 1);
    for (size_t i = 3; i < (size_t)s.width * s.height * 4; i += 4) rgba[i] = 255;   /* opaque */
    char path[4096];
    snprintf(path, sizeof path, "%s/frame_%06" PRIu64 ".png", dir, frames);
    /* only the window's crop is shown; row 0 is the bottom of the screen (NVN's default, as in
     * OpenGL), so rows are written upwards */
    uint32_t x = 0, y = 0, w = s.width, h = s.height;
    if (crop[2] > 0 && crop[3] > 0 && (uint32_t)(crop[0] + crop[2]) <= s.width && (uint32_t)(crop[1] + crop[3]) <= s.height) {
        x = (uint32_t)crop[0]; y = (uint32_t)crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3];
    }
    if (png_write(path, rgba + ((size_t)(y + h - 1) * s.width + x) * 4, w, h, -(int32_t)s.width * 4) == 0)
        fprintf(stderr, "nvn: wrote %s\n", path);
    free(rgba);
}

uint64_t nvn_frame_count(void) { return frames; }

void nvn_texels_written(uint64_t texture, uint32_t level, uint32_t layer, const uint32_t region[6]) {
#ifdef LAA_HAVE_SDL
    if (render_active()) {
        pthread_mutex_lock(&gpu_lock);
        render_texels_written(texture, level, layer, region);
        pthread_mutex_unlock(&gpu_lock);
    }
#else
    (void)texture; (void)level; (void)layer; (void)region;
#endif
}

void nvn_pool_changed(uint64_t storage, uint64_t size, int created) {
#ifdef LAA_HAVE_SDL
    if (render_active()) {
        pthread_mutex_lock(&gpu_lock);
        if (created) render_pool_created(storage, size);
        else render_pool_destroyed(storage, size);
        pthread_mutex_unlock(&gpu_lock);
    }
#else
    (void)storage; (void)size; (void)created;
#endif
}

void nvn_write_texture(uint64_t texture, const uint8_t *rgba, uint32_t width, uint32_t height) {
#ifdef LAA_HAVE_SDL
    if (render_active()) {
        pthread_mutex_lock(&gpu_lock);
        render_write_texture(texture, rgba, width, height);
        pthread_mutex_unlock(&gpu_lock);
        return;
    }
#endif
    /* no GPU: into guest memory, in the texture's own layout */
    const Texture *t = gptr(texture);
    TexShape s;
    tex_shape(&t->d, &s);
    if (s.width != width || s.height != height || s.fmt->bytes != 4) return;
    LevelShape l = tex_level(&s, 0);
    uint32_t region[6] = { 0, 0, 0, width, height, 1 };
    if (t->d.flags & TEXTURE_FLAG_LINEAR)
        for (uint32_t y = 0; y < height; y++) memcpy(gptr(t->address + y * t->d.stride), rgba + (size_t)y * width * 4, (size_t)width * 4);
    else
        tex_copy_level(gptr(t->address), (uint8_t *)rgba, &s, &l, region, (uint64_t)width * 4, 0, 0);
}

int applet_handheld(void);

/* What the display shows of the texture: the window's crop, or the screen's size (handheld:
 * 1280x720 from the corner, which is what the game draws into) */
static void display_rect(const Window *w, int32_t out[4]) {
    memcpy(out, w->crop, 16);
    if (out[2] <= 0 || out[3] <= 0) {
        out[0] = out[1] = 0;
        out[2] = applet_handheld() ? 1280 : 1920;
        out[3] = applet_handheld() ? 720 : 1080;
    }
}

/* The thread that presents is the game's main thread, and it decides the frame rate. Android
 * put it on the Thor's middle cores (2.7 GHz); the fastest core (a Cortex-X3, 3.2 GHz and wider)
 * finishes a frame sooner. So it is moved there, once: the core with the highest top clock.
 * LAA_NO_PIN=1: leave it to the scheduler. */
static void pin_to_fastest_core(void) {
#ifdef __linux__
    if (getenv("LAA_NO_PIN")) return;
    int best = -1;
    long best_khz = 0;
    for (int cpu = 0; cpu < 64; cpu++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1 && khz > best_khz) { best_khz = khz; best = cpu; }
        fclose(f);
    }
    if (best < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(best, &set);
    if (sched_setaffinity(0, sizeof set, &set) == 0)
        fprintf(stderr, "nvn: the game's main thread runs on cpu %d (%ld MHz)\n", best, best_khz / 1000);
#endif
}

void nvn_present(const Window *w, uint64_t texture) {
    static int pinned;
    if (!pinned) { pinned = 1; pin_to_fastest_core(); }
    int32_t rect[4];
    display_rect(w, rect);
    frames++;
    /* LAA_PEEK=address,words,frame: print guest memory as floats (and hex) at that frame (debugging) */
    static uint64_t peek_addr, peek_words, peek_frame;
    static int peek_init;
    if (!peek_init) {
        peek_init = 1;
        if (getenv("LAA_PEEK")) sscanf(getenv("LAA_PEEK"), "%" SCNi64 ",%" SCNi64 ",%" SCNi64, &peek_addr, &peek_words, &peek_frame);
    }
    if (peek_addr && frames == peek_frame) {
        for (uint64_t i = 0; i < peek_words; i++) {
            uint32_t v = g_ld32(peek_addr + 4 * i);
            float f;
            memcpy(&f, &v, 4);
            fprintf(stderr, "peek %#" PRIx64 ": %08x %g\n", peek_addr + 4 * i, v, f);
        }
    }
    if (frame_stats < 0) frame_stats = getenv("LAA_FRAME_STATS") != NULL;
    if (frame_stats) {
        double t = now_ms();
        if (fs.last) {
            double dt = t - fs.last;
            fs.interval += dt;
            if (dt > fs.interval_max) fs.interval_max = dt;
            if (dt > 20) fs.slow++;
        }
        fs.last = t;
    }
    if (frames == 1 || frames % 300 == 0) {
        fprintf(stderr, "nvn: frame %" PRIu64 " presented\n", frames);
        if (frame_stats && frames > 1) {
            double gpu_wait = 0;
#ifdef LAA_HAVE_SDL
            gpu_wait = render_gpu_wait_ms;
            render_gpu_wait_ms = 0;
#endif
            fprintf(stderr, "nvn: frames: %.1f ms apart (max %.1f), %d over 20 ms; per frame %.1f ms in submissions, %.1f in present, "
                    "%.1f waiting for the GPU\n", fs.interval / 300, fs.interval_max, fs.slow, fs.submit / 300, fs.present / 300,
                    gpu_wait / 300);
            double last = fs.last;
            memset(&fs, 0, sizeof fs);
            fs.last = last;
        }
#ifdef LAA_HAVE_SDL
        if (render_active()) render_stats();
#endif
    }
    screenshot(texture, rect);
#ifdef LAA_HAVE_SDL
    if (render_active()) {                           /* the display's vsync paces us */
        double t0 = frame_stats > 0 ? now_ms() : 0;
        pthread_mutex_lock(&gpu_lock);
        present_serial[1] = present_serial[0];       /* (LAA_FRAME_LAG) */
        present_serial[0] = render_fence_serial();
        render_present(texture, rect);
        if (sync_tracing()) fprintf(stderr, "sync: %.2f presented frame %" PRIu64 "\n", now_ms(), frames);
        pthread_mutex_unlock(&gpu_lock);
        if (frame_stats > 0) fs.present += now_ms() - t0;
        return;
    }
#endif
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
}
