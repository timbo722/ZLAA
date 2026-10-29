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
#include "kernel.h"
#ifdef LAA_HAVE_SDL
#include "render/render.h"
#endif
#include "render/png.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#endif

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

static double now_ms(void);

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
            if (render_active()) {
                double t0 = sync_tracing() ? now_ms() : 0;
                render_prof_on = t0 != 0;
                render_command(r);
                static double slow_ms = -1;              /* (LAA_TRACE_SYNC: the slow ones; LAA_TRACE_SLOW_MS=ms) */
                if (slow_ms < 0) slow_ms = getenv("LAA_TRACE_SLOW_MS") ? atof(getenv("LAA_TRACE_SLOW_MS")) : 1.0;
                if (t0 && now_ms() - t0 > slow_ms)
                    fprintf(stderr, "sync: %.2f slow %s: %.2f ms (shaders %.2f, resources %.2f, pass %.2f, pipeline %.2f)\n",
                            now_ms(), nvn_command_names[r->op], now_ms() - t0, render_prof[0], render_prof[1], render_prof[2],
                            render_prof[3]);
                if (t0) memset(render_prof, 0, sizeof render_prof);
            }
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
    static uint64_t every, from, to;                 /* (+ LAA_SCREENSHOT_FROM / _TO=frame: only then) */
    if (!dir) {
        dir = getenv("LAA_SCREENSHOT") ? getenv("LAA_SCREENSHOT") : "";
        every = getenv("LAA_SCREENSHOT_EVERY") ? strtoull(getenv("LAA_SCREENSHOT_EVERY"), NULL, 0) : 300;
        from = getenv("LAA_SCREENSHOT_FROM") ? strtoull(getenv("LAA_SCREENSHOT_FROM"), NULL, 0) : 0;
        to = getenv("LAA_SCREENSHOT_TO") ? strtoull(getenv("LAA_SCREENSHOT_TO"), NULL, 0) : ~0ull;
    }
    if (!*dir || !texture || !every || frames % every || frames < from || frames > to) return;
    char path[4096];
    snprintf(path, sizeof path, "%s/frame_%06" PRIu64 ".png", dir, frames);
#ifdef LAA_HAVE_SDL
    static int scale = -1;                           /* LAA_SCREENSHOT_SCALE=n: 1/n of the size (with a GPU) */
    if (scale < 0) scale = getenv("LAA_SCREENSHOT_SCALE") ? atoi(getenv("LAA_SCREENSHOT_SCALE")) : 1;
    pthread_mutex_lock(&gpu_lock);
    int captured = render_active() && render_capture(texture, path, crop, scale > 0 ? (uint32_t)scale : 1) == 0;
    pthread_mutex_unlock(&gpu_lock);
    if (captured) return;
#endif
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
        double t0 = now_ms();
        pthread_mutex_lock(&gpu_lock);
        if (created) render_pool_created(storage, size);
        else render_pool_destroyed(storage, size);
        pthread_mutex_unlock(&gpu_lock);
        if (sync_tracing())
            fprintf(stderr, "sync: %.2f pool %#" PRIx64 " (+%#" PRIx64 ") %s on thread %d in %.2f ms\n", now_ms(), storage, size,
                    created ? "made" : "finalized", kthread_current() ? (int)kthread_current()->id : -1, now_ms() - t0);
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
    if (getenv("LAA_MAIN_CORE")) best = atoi(getenv("LAA_MAIN_CORE"));   /* (that core instead) */
    if (best < 0) return;
    host_main_core(best);                            /* (first: which cores the other threads get) */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(best, &set);
    if (sched_setaffinity(0, sizeof set, &set) == 0)
        fprintf(stderr, "nvn: the game's main thread runs on cpu %d\n", best);
#endif
}

/* ADPF (Android 13's performance hints, what native games use): the main thread's CPU time each
 * frame goes to the system with a target, and it picks the lowest clocks that meet it instead of
 * the highest (less heat). The CPU time, not the time between frames: waits don't count, and at a
 * lower clock the same work takes longer, so the system finds where it fits. LAA_NO_ADPF=1: off;
 * LAA_ADPF_TARGET_MS (default 12: margin to 16.7 for the frames that do more). */
#ifdef __ANDROID__
static struct {
    void *session;
    int (*report)(void *session, int64_t ns);
    int64_t last_cpu_ns;
} adpf;

static int64_t thread_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void adpf_start(void) {
    if (getenv("LAA_NO_ADPF")) return;
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    void *(*get_manager)(void) = lib ? (void *(*)(void))dlsym(lib, "APerformanceHint_getManager") : NULL;
    void *(*create)(void *, const int32_t *, size_t, int64_t) =
        lib ? (void *(*)(void *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession") : NULL;
    adpf.report = lib ? (int (*)(void *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration") : NULL;
    void *manager = get_manager ? get_manager() : NULL;
    double target_ms = getenv("LAA_ADPF_TARGET_MS") ? atof(getenv("LAA_ADPF_TARGET_MS")) : 12.0;
    int32_t tid = (int32_t)gettid();
    if (manager && create && adpf.report) adpf.session = create(manager, &tid, 1, (int64_t)(target_ms * 1e6));
    fprintf(stderr, "nvn: performance hints %s (target %.1f ms of the main thread's CPU time a frame)\n",
            adpf.session ? "on" : "not available", target_ms);
    adpf.last_cpu_ns = thread_cpu_ns();
}

static void adpf_frame(void) {
    if (!adpf.session) return;
    int64_t now = thread_cpu_ns(), used = now - adpf.last_cpu_ns;
    adpf.last_cpu_ns = now;
    if (used > 0) adpf.report(adpf.session, used);
}
#endif

/* LAA_STALL_PROFILE=ms: when the game's main thread goes that long without presenting, a watchdog
 * samples where it is (every 2 ms, by a signal: its pc, lr and frame-pointer chain), and when the
 * frame finally comes, prints the functions the stall was spent in (f_<guest address>: game code).
 * For pauses the sync trace can't see (no submission, wait or file read in them). */
#ifdef __linux__
#define STALL_SAMPLES 512
#define STALL_DEPTH 20
static struct {
    pthread_t thread;
    uintptr_t stack_lo, stack_hi;
    volatile double last_present;
    double threshold;
    volatile int n;
    uintptr_t pc[STALL_SAMPLES][STALL_DEPTH];
} stall;

static void stall_sample(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    int i = stall.n;
    if (i >= STALL_SAMPLES) return;
    ucontext_t *uc = uc_;
    uintptr_t *out = stall.pc[i];
    memset(out, 0, sizeof stall.pc[i]);
#if defined(__aarch64__)
    out[0] = (uintptr_t)uc->uc_mcontext.pc;
    out[1] = (uintptr_t)uc->uc_mcontext.regs[30];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
#else
    out[0] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
#endif
    for (int d = 2; d < STALL_DEPTH && fp >= stall.stack_lo && fp + 16 <= stall.stack_hi && !(fp & 7); d++) {
        out[d] = ((uintptr_t *)fp)[1];
        uintptr_t next = ((uintptr_t *)fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    stall.n = i + 1;
}

static const char *stall_name(uintptr_t pc, char *buf, size_t size) {
    Dl_info info;
#if defined(__aarch64__)
    pc &= (1ull << 39) - 1;                          /* (return addresses carry pointer-authentication bits) */
#endif
    if (pc && dladdr((void *)pc, &info) && info.dli_sname) return info.dli_sname;
    snprintf(buf, size, "%#lx", (unsigned long)pc);
    return buf;
}

static void stall_report(double ms) {
    int n = stall.n;
    struct timespec wall;                            /* (wall clock: to match the system's log, logcat) */
    clock_gettime(CLOCK_REALTIME, &wall);
    struct tm tm;
    localtime_r(&wall.tv_sec, &tm);
    fprintf(stderr, "stall: %.0f ms without a frame (frame %" PRIu64 ", ended %02d:%02d:%02d.%03ld), %d samples:\n", ms, frames,
            tm.tm_hour, tm.tm_min, tm.tm_sec, wall.tv_nsec / 1000000, n);
    /* per function: samples where it is running (self), and where it is anywhere on the stack */
    static struct { char name[64]; int self, any; } fn[256];
    int nfn = 0;
    for (int i = 0; i < n; i++) {
        const char *seen[STALL_DEPTH];
        int nseen = 0;
        for (int d = 0; d < STALL_DEPTH; d++) {
            if (!stall.pc[i][d]) continue;
            char buf[32];
            const char *name = stall_name(stall.pc[i][d], buf, sizeof buf);
            int k = 0;
            while (k < nfn && strcmp(fn[k].name, name)) k++;
            if (k == nfn) {
                if (nfn == 256) continue;
                snprintf(fn[nfn++].name, sizeof fn[0].name, "%s", name);
                fn[k].self = fn[k].any = 0;
            }
            int dup = 0;
            for (int j = 0; j < nseen; j++) dup |= seen[j] == fn[k].name;
            if (!dup) { fn[k].any++; seen[nseen++] = fn[k].name; }
            if (d == 0) fn[k].self++;
        }
    }
    if (n) {                                         /* one whole sample, innermost first */
        fprintf(stderr, "stall:   a sample:");
        for (int d = 0; d < STALL_DEPTH && stall.pc[n / 2][d]; d++) {
            char buf[32];
            fprintf(stderr, " %s", stall_name(stall.pc[n / 2][d], buf, sizeof buf));
        }
        fprintf(stderr, "\n");
    }
    for (int round = 0; round < 2; round++) {          /* the top 12 by self, then by "on the stack" */
        fprintf(stderr, "stall:   %s\n", round ? "on the stack:" : "running:");
        int used[256] = { 0 };
        for (int top = 0; top < 12; top++) {
            int best = -1;
            for (int k = 0; k < nfn; k++)
                if (!used[k] && (best < 0 || (round ? fn[k].any > fn[best].any : fn[k].self > fn[best].self))) best = k;
            if (best < 0 || !(round ? fn[best].any : fn[best].self)) break;
            used[best] = 1;
            fprintf(stderr, "stall:     %3d%%  %s\n", 100 * (round ? fn[best].any : fn[best].self) / (n ? n : 1), fn[best].name);
        }
    }
}

static void *stall_watch(void *arg) {
    (void)arg;
    double stalled_since = 0;
    for (;;) {
        struct timespec t = { 0, 2000000 };
        nanosleep(&t, NULL);
        double now = now_ms(), last = stall.last_present;
        if (now - last > stall.threshold) {
            if (!stalled_since) { stalled_since = last; stall.n = 0; }
            pthread_kill(stall.thread, SIGURG);
        } else if (stalled_since) {
            stall_report(last - stalled_since);
            stalled_since = 0;
        }
    }
    return NULL;
}

static void stall_start(void) {
    const char *env = getenv("LAA_STALL_PROFILE");
    if (!env) return;
    stall.threshold = atof(env) > 0 ? atof(env) : 100;
    stall.thread = pthread_self();
    pthread_attr_t a;
    void *lo;
    size_t size;
    if (pthread_getattr_np(stall.thread, &a) == 0 && pthread_attr_getstack(&a, &lo, &size) == 0) {
        stall.stack_lo = (uintptr_t)lo;
        stall.stack_hi = (uintptr_t)lo + size;
        pthread_attr_destroy(&a);
    }
    struct sigaction sa = { 0 };
    sa.sa_sigaction = stall_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGURG, &sa, NULL);
    stall.last_present = now_ms();
    pthread_t t;
    pthread_create(&t, NULL, stall_watch, NULL);
    pthread_detach(t);
    fprintf(stderr, "nvn: stall profiler on (frames over %.0f ms)\n", stall.threshold);
}
#endif

void nvn_present(const Window *w, uint64_t texture) {
    static int pinned;
    if (!pinned) {
        pinned = 1;
        pin_to_fastest_core();
#ifdef __linux__
        stall_start();
#endif
#ifdef __ANDROID__
        adpf_start();
#endif
    }
#ifdef __ANDROID__
    adpf_frame();
#endif
    int32_t rect[4];
    display_rect(w, rect);
    frames++;
#ifdef __linux__
    stall.last_present = now_ms();
#endif
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
        /* textures made since: ahead of their first use. 3 ms a frame, 10 in light frames like a
         * loading screen's (a new scene's textures are often still queued when it first shows) */
        render_preload(render_take_frame_draws() < 100 ? 10.0 : 3.0);
        render_adopt_shaders();
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
