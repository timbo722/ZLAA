/*
 * builder.c - builds the game's code on the device, from the player's own dump (Android). The app ships no
 * game code: this makes libgame-<main build id>.so the way the PC build does, in four steps on a thread of its
 * own while the start screen shows the progress:
 *
 *   1. unpack the toolchain (assets/builder/builder.tar: Python's standard library, zig's headers, the recompiler,
 *      the runtime headers; tools/android/builder/make_toolchain.py) into files/toolchain, once per app version
 *   2. translate: recomp.py (run by libpylaunch.so, the python command) turns the dump's ARM64 code into ~290 C files
 *   3. compile them with zig cc (libzig.so), a few at a time, fewer as the device warms, none when it is warm
 *      enough (heat() below; the player chooses how warm: Cool, Balanced, Fast): the Thor reached 94 C with six at once
 *   4. link them (zig's ld.lld) against libmain.so into files/game/libgame-<id>.so, where game_lib.c looks
 *
 * The programs are in the APK's native-library folder (the only place Android lets an app run programs from)
 * under lib*.so names. Work goes to files/build; what is done stays done if the app closes (translated C with a
 * marker, compiled objects newer than their source), so a build carries on where it stopped. Everything is
 * logged ("build: ...", and the programs' own output in the app folder's build/: translate.log, compile.log,
 * link.log; a failed step's last lines go to log.txt too).
 *
 * Heat: the tools run on the middle cores (3-6 of the Thor's eight: not the big one, which runs hottest) at low
 * priority, fewer of them as the device warms, and they are frozen while it is hot (heat() below).
 */
#define _GNU_SOURCE
#include "builder.h"
#include "game_lib.h"
#include <SDL3/SDL.h>
#include <dirent.h>
#include <dlfcn.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef RECOMP_ABI
#define RECOMP_ABI "unknown"
#endif

extern char **environ;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static BuildState state;                             /* (under lock) */
static volatile int cancel;
static char exefs_dir[4096], game_dir[4096], lib_dir[4096], files_dir[4096], tool_dir[4200], work_dir[4200], log_dir[4200];
static char game_id[17];
static char stamp[96];                               /* "<game id> <toolchain id>": what translated C and objects are for */

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void set_state(int step, double fraction, const char *detail) {
    pthread_mutex_lock(&lock);
    state.step = step;
    if (fraction >= 0) state.fraction = fraction;
    if (detail) snprintf(state.detail, sizeof state.detail, "%s", detail);
    pthread_mutex_unlock(&lock);
}

static int fail(const char *fmt, const char *arg) {
    pthread_mutex_lock(&lock);
    state.step = BUILD_FAILED;
    snprintf(state.detail, sizeof state.detail, fmt, arg ? arg : "");
    pthread_mutex_unlock(&lock);
    printf("build: failed: %s\n", state.detail);
    return -1;
}

static int make_dirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    char sub[4400];
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { snprintf(sub, sizeof sub, "%s/%s", path, e->d_name); remove_tree(sub); }
    closedir(d);
    rmdir(path);
}

/* A small text file's contents ("" if none) */
static void read_text(const char *path, char *out, size_t size) {
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* A failed tool: its exit status and the last lines of its log, into log.txt */
static void show_log_tail(const char *log, int status) {
    printf("build: exit status %d; the end of %s:\n", status, log);
    FILE *f = fopen(log, "r");
    if (!f) return;
    static char lines[20][300];
    int n = 0;
    while (fgets(lines[n % 20], sizeof lines[0], f)) n++;
    fclose(f);
    for (int i = n > 20 ? n - 20 : 0; i < n; i++) printf("build:   %s", lines[i % 20]);
}

/* The build's own threads and tools (they inherit it): the middle cores, low priority */
static void calm_down(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpu >= 8) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c = 3; c <= 6; c++) CPU_SET(c, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
}

static void estimate(double still_to_go);
static double typical_total(void);

/* ---- 1. the toolchain: a tar file among the APK's assets (SDL reads assets for relative paths) ---- */
static int unpack_toolchain(void) {
    char want[64], have[64], path[4400];
    SDL_IOStream *io = SDL_IOFromFile("builder/builder.id", "rb");
    size_t n = io ? SDL_ReadIO(io, want, sizeof want - 1) : 0;
    if (io) SDL_CloseIO(io);
    want[n] = 0;
    want[strcspn(want, "\r\n")] = 0;
    if (!want[0]) return fail("this app has no toolchain to build the game code with", NULL);
    snprintf(stamp, sizeof stamp, "%s %s", game_id, want);   /* (another app version: everything made again) */
    snprintf(path, sizeof path, "%s/id", tool_dir);
    read_text(path, have, sizeof have);
    if (strcmp(want, have) == 0) return 0;
    printf("build: unpacking the toolchain %s\n", want);
    set_state(BUILD_PREPARING, 0, "Unpacking the build tools");
    remove_tree(tool_dir);
    make_dirs(tool_dir);
    io = SDL_IOFromFile("builder/builder.tar", "rb");
    if (!io) return fail("the app's build tools are missing", NULL);
    Sint64 total = SDL_GetIOSize(io), done = 0;
    unsigned char hdr[512];
    static char buf[1 << 16];
    while (!cancel && SDL_ReadIO(io, hdr, 512) == 512 && hdr[0]) {   /* ustar: a 512-byte header per file */
        char name[300];                              /* (a long one: its folder in the prefix field) */
        if (hdr[345]) snprintf(name, sizeof name, "%.155s/%.100s", (const char *)hdr + 345, (const char *)hdr);
        else snprintf(name, sizeof name, "%.100s", (const char *)hdr);
        unsigned long long size = strtoull((const char *)hdr + 124, NULL, 8);
        int mode = (int)strtol((const char *)hdr + 100, NULL, 8);
        snprintf(path, sizeof path, "%s/%s", tool_dir, name);
        char *slash = strrchr(path, '/');
        *slash = 0;
        make_dirs(path);
        *slash = '/';
        int fd = hdr[156] == '0' || hdr[156] == 0 ? open(path, O_CREAT | O_TRUNC | O_WRONLY, mode & 0777) : -1;
        for (unsigned long long left = size; left;) {
            size_t chunk = left < sizeof buf ? (size_t)left : sizeof buf;
            if (SDL_ReadIO(io, buf, chunk) != chunk) { if (fd >= 0) close(fd); SDL_CloseIO(io); return fail("the build tools are damaged", NULL); }
            if (fd >= 0 && write(fd, buf, chunk) != (ssize_t)chunk) { close(fd); SDL_CloseIO(io); return fail("no space for the build tools", NULL); }
            left -= chunk;
        }
        if (fd >= 0) close(fd);
        if (size % 512) SDL_SeekIO(io, (Sint64)(512 - size % 512), SDL_IO_SEEK_CUR);
        done += 512 + (Sint64)((size + 511) / 512 * 512);
        estimate(60 * (1 - (double)done / (double)(total > 0 ? total : 1)) + typical_total());
    }
    SDL_CloseIO(io);
    if (cancel) return -1;
    snprintf(path, sizeof path, "%s/id", tool_dir);
    write_text(path, want);
    return 0;
}

/* ---- running the tools ---- */
typedef struct { pid_t pid; int out; char name[256]; double bytes; } Job;

/* Start a program from the native-library folder; its stdout to a pipe (out_pipe) or with stderr to log */
static pid_t run(const char *program, char *const argv[], const char *log, int *out_pipe) {
    char path[4400], env_buf[8][4400];
    snprintf(path, sizeof path, "%s/%s", lib_dir, program);
    snprintf(env_buf[0], sizeof env_buf[0], "PYTHONHOME=%s/py", tool_dir);
    snprintf(env_buf[1], sizeof env_buf[1], "PYTHONPATH=%s/py/stdlib.zip:%s/py/lib-dynload", tool_dir, tool_dir);
    snprintf(env_buf[2], sizeof env_buf[2], "LD_LIBRARY_PATH=%s", lib_dir);
    snprintf(env_buf[3], sizeof env_buf[3], "ZIG_LIB_DIR=%s/zig/lib", tool_dir);
    snprintf(env_buf[4], sizeof env_buf[4], "ZIG_GLOBAL_CACHE_DIR=%s/zigcache", work_dir);
    snprintf(env_buf[5], sizeof env_buf[5], "ZIG_LOCAL_CACHE_DIR=%s/zigcache", work_dir);
    snprintf(env_buf[6], sizeof env_buf[6], "HOME=%s", work_dir);
    snprintf(env_buf[7], sizeof env_buf[7], "TMPDIR=%s", work_dir);
    char *env[] = { env_buf[0], env_buf[1], env_buf[2], env_buf[3], env_buf[4], env_buf[5], env_buf[6], env_buf[7],
                    "PYTHONDONTWRITEBYTECODE=1", "PYTHONUNBUFFERED=1", NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    int p[2] = { -1, -1 };
    if (out_pipe && pipe(p) == 0) posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    else posix_spawn_file_actions_addopen(&fa, 1, log, O_CREAT | O_WRONLY | O_APPEND, 0644);
    posix_spawn_file_actions_addopen(&fa, 2, log, O_CREAT | O_WRONLY | O_APPEND, 0644);
    posix_spawnattr_t at;                            /* (a process group of its own: zig cc runs the compiler as a */
    posix_spawnattr_init(&at);                       /*  child of its own, and pausing or stopping must reach it too) */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid = -1;
    int r = posix_spawn(&pid, path, &fa, &at, argv, env);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (p[1] >= 0) close(p[1]);
    if (r != 0) { if (p[0] >= 0) close(p[0]); fprintf(stderr, "build: can't run %s: %s\n", path, strerror(r)); return -1; }
    if (out_pipe) *out_pipe = p[0];
    return pid;
}

static int finished(pid_t pid, int block) {         /* -1 running, else the exit status (nonzero: failed) */
    int st;
    pid_t r = waitpid(pid, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* ---- heat: how much may run. The board's temperature where the app may read it (the Thor: xo-therm), else the
 * chip's (smoothed), else Android's thermal headroom (0..1, 1 = throttling). Cool: two tools at once; warm: one;
 * hot: the running tools are frozen (SIGSTOP: they lose nothing) until it is cooler again. The device in someone's
 * hands should stay at the warmth of light gaming, not of a benchmark: 94 C on the big core, 88 C on the middle
 * ones, before this. ---- */

typedef struct AThermalManager AThermalManager;
static float thermal_headroom(void) {               /* 0..1 (1 = throttling); -1 if Android can't tell (API < 31) */
    static AThermalManager *(*acquire)(void);
    static float (*headroom)(AThermalManager *, int);
    static AThermalManager *mgr;
    static int tried;
    if (!tried) {
        tried = 1;
        void *h = dlopen("libandroid.so", RTLD_NOW);
        acquire = h ? (AThermalManager *(*)(void))dlsym(h, "AThermal_acquireManager") : NULL;
        headroom = h ? (float (*)(AThermalManager *, int))dlsym(h, "AThermal_getThermalHeadroom") : NULL;
        mgr = acquire && headroom ? acquire() : NULL;
    }
    if (!mgr) return -1;
    float v = headroom(mgr, 10);                     /* (in 10 s at this pace) */
    return v != v ? -1 : v;                          /* (NaN: asked too often; no answer) */
}

/* The hottest zone of a kind (C; -1 if none can be read): "cpu-" for the chip, the board's own sensors otherwise */
static double zone_temperature(const char *const *types, int prefix) {
    double hottest = -1;
    char path[96], type[40];
    for (int z = 0; z < 160; z++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", z);
        FILE *f = fopen(path, "r");
        if (!f) { if (z > 40) break; continue; }
        int ok = fgets(type, sizeof type, f) != NULL;
        fclose(f);
        if (!ok) continue;
        type[strcspn(type, "\n")] = 0;
        int match = 0;
        for (const char *const *t = types; *t && !match; t++) match = prefix ? strncmp(type, *t, strlen(*t)) == 0 : strcmp(type, *t) == 0;
        if (!match) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", z);
        f = fopen(path, "r");
        long v;
        if (f && fscanf(f, "%ld", &v) == 1 && v > 0 && v / 1000.0 > hottest) hottest = v / 1000.0;
        if (f) fclose(f);
    }
    return hottest;
}

/* The speeds the player can choose (the board's temperature, C): all tools below `all`, one fewer below `fewer`,
 * none from `pause` until it is under `resume` again */
static const struct { const char *name; double all, fewer, pause, resume; int jobs; } speeds[] = {
    { "cool", 42, 46, 46, 44, 2 },
    { "balanced", 46, 49, 49, 47, 2 },
    { "fast", 49, 52, 52, 50, 3 },
};
static int speed = BUILD_BALANCED;

/* ---- the progress bar: the share of the time, done / (done + still to go), so the bar and the time left agree.
 * What is still to go: translating, from the translator's own progress (until then a typical 11 min); compiling,
 * from the rate it goes at (MB of C done per second, the pauses included; until it has been going a while,
 * blended with a typical rate for the chosen speed, measured on the AYN Thor). ---- */
#define TYPICAL_TRANSLATE_S 660.0
#define TYPICAL_C_MB 680.0
/* (Thor, 2026-11-07: Balanced compiled 680 MB in ~42 min; Cool took 94 min before the lane loops were written out,
 * which saved 22%: ~73 min now; Fast not yet measured) */
static const double typical_s_per_mb[] = { 6.5, 3.7, 2.8 };   /* cool, balanced, fast */
static double build_t0;

static double typical_total(void) { return TYPICAL_TRANSLATE_S + TYPICAL_C_MB * typical_s_per_mb[speed]; }

static void estimate(double still_to_go) {
    double done = now_s() - build_t0, f = done / (done + (still_to_go > 0 ? still_to_go : 0) + 1e-9);
    pthread_mutex_lock(&lock);
    if (f > state.fraction) state.fraction = f > 0.995 ? 0.995 : f;   /* (never backwards) */
    pthread_mutex_unlock(&lock);
}

static int heat(void) {                              /* how many tools may run now (0: all frozen) */
    static const char *const board_types[] = { "xo-therm", "skin-therm", "quiet-therm", "sys-therm-0", "usb-therm", NULL };
    static const char *const cpu_types[] = { "cpu-", NULL };
    static int paused, allowed = -1;
    static double checked, logged, cpu_avg = -1;
    int max = speeds[speed].jobs;
    const char *jobs_env = getenv("LAA_BUILD_JOBS");    /* (tests: a fixed number when cool) */
    if (jobs_env && atoi(jobs_env) > 0) max = atoi(jobs_env) > 8 ? 8 : atoi(jobs_env);
    if (allowed >= 0 && now_s() - checked < 2) return allowed;
    checked = now_s();
    /* The board's temperature is what the hands feel, and it moves slowly; the chip's hottest spot jumps to 70 C
     * within seconds of any load and back the moment it stops: alone, smoothed (half a minute), and as a cap */
    double board = zone_temperature(board_types, 0), cpu = zone_temperature(cpu_types, 1);
    if (cpu >= 0) cpu_avg = cpu_avg < 0 ? cpu : cpu_avg + (cpu - cpu_avg) * 0.07;
    float h = board < 0 && cpu < 0 ? thermal_headroom() : -1;
    double all = speeds[speed].all, fewer = speeds[speed].fewer, pause = speeds[speed].pause, resume = speeds[speed].resume;
    double t = board;                                /* (no board sensor: the smoothed chip, on a scale 20 C up) */
    if (t < 0 && cpu_avg >= 0) t = cpu_avg - 20;
    else if (t < 0 && h >= 0) t = all + (h - 0.45) / 0.25 * (pause - all);   /* (headroom 0.45 .. 0.70) */
    int was = allowed;
    if (cpu >= 88) paused = 1;                       /* (the chip's cap, whatever the speed) */
    else if (t >= pause) paused = 1;
    else if (paused && (t < 0 || t < resume)) paused = 0;
    allowed = paused ? 0 : t < 0 || t < all ? max : t < fewer ? max - 1 : 1;
    if (allowed < 0) allowed = 0;
    if (allowed != was || now_s() - logged >= 60) {
        logged = now_s();
        printf("build: board %.1f C, cpu %.1f C (smoothed %.1f), thermal headroom %.2f (%s): %d at a time\n", board, cpu,
               cpu_avg, h, speeds[speed].name, allowed);
    }
    return allowed;
}

/* ---- 2. translate ---- */
static int translate(const char *gen) {
    char marker[4400], have[96], log[4400], script[4400], input[4200];
    snprintf(marker, sizeof marker, "%s/.complete", gen);
    read_text(marker, have, sizeof have);
    if (strcmp(have, stamp) == 0) return 0;          /* (done before by this toolchain: the app closed while compiling) */
    printf("build: translating %s\n", exefs_dir);
    set_state(BUILD_TRANSLATING, -1, "Translating the game's code");
    double t_start = now_s(), x = 0, compile_guess = TYPICAL_C_MB * typical_s_per_mb[speed];
    snprintf(log, sizeof log, "%s/translate.log", log_dir);
    snprintf(script, sizeof script, "%s/recompiler/recomp.py", tool_dir);
    snprintf(input, sizeof input, "%s", exefs_dir);
    char python[4400], table[4400];                  /* (its full path: with a bare name, Python inside an app */
    snprintf(python, sizeof python, "%s/libpylaunch.so", lib_dir);   /*  sends its output to logcat instead) */
    snprintf(table, sizeof table, "%s/functable.c", gen);
    char *argv[] = { python, script, input, "-o", (char *)gen, "--no-comments", "--progress", NULL };
    int out = -1;
    pid_t pid = run("libpylaunch.so", argv, log, &out);
    if (pid < 0) return fail("couldn't start the translator", NULL);
    char buf[4096], *line;
    size_t used = 0;
    int paused = 0;
    for (;;) {                                       /* its progress lines; pausing it while the device is hot */
        struct pollfd pf = { out, POLLIN, 0 };
        int r = poll(&pf, 1, 1000);
        if (r > 0) {
            ssize_t n = read(out, buf + used, sizeof buf - 1 - used);
            if (n <= 0) break;
            used += (size_t)n;
            buf[used] = 0;
            char *start = buf;
            while ((line = strchr(start, '\n'))) {
                *line = 0;
                double p;
                if (sscanf(start, "progress %lf", &p) == 1) x = p;
                start = line + 1;
            }
            used -= (size_t)(start - buf);
            memmove(buf, start, used);
            if (used == sizeof buf - 1) used = 0;
        }
        double t = now_s() - t_start;                 /* (still to go: the rest of this at its pace, then compiling) */
        estimate((x > 0.03 && t > 30 ? t * (1 - x) / x : TYPICAL_TRANSLATE_S - t > 60 ? TYPICAL_TRANSLATE_S - t : 60)
                 + compile_guess);
        int hot = heat() == 0;
        if (hot != paused) { kill(-pid, hot ? SIGSTOP : SIGCONT); paused = hot; }
        pthread_mutex_lock(&lock);
        state.cooling = paused;
        snprintf(state.detail, sizeof state.detail, paused ? "Translating the game's code (the device is warm: "
                 "pausing to let it cool)" : "Translating the game's code");
        pthread_mutex_unlock(&lock);
        if (cancel) { kill(-pid, SIGCONT); kill(-pid, SIGTERM); }
    }
    close(out);
    int st = finished(pid, 1);
    if (cancel) return -1;
    /* (status 1: some words couldn't be translated, as always: data among the code; the C is complete) */
    if (st > 1 || (st == 1 && access(table, F_OK) != 0)) {
        show_log_tail(log, st);
        return fail("translating failed (see build/translate.log)", NULL);
    }
    write_text(marker, stamp);
    return 0;
}

/* ---- 3. compile: a few at once, fewer when the device is warm ---- */
static int newer(const char *a, const char *b) {     /* a exists and is newer than b */
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_mtime >= sb.st_mtime;
}

static int compile_all(const char *gen, const char *obj) {
    char sources[512][64];
    int n = 0;
    DIR *d = opendir(gen);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 511) {
        size_t l = strlen(e->d_name);
        if (l > 2 && strcmp(e->d_name + l - 2, ".c") == 0) snprintf(sources[n++], sizeof sources[0], "%s", e->d_name);
    }
    if (d) closedir(d);
    snprintf(sources[n++], sizeof sources[0], "game_abi.c");   /* (from the runtime: the library's ABI id) */
    static double bytes[512];                        /* each file's size: what the progress counts */
    double total_bytes = 0, done_bytes = 0, compiled_bytes = 0, c_start = now_s();
    for (int i = 0; i < n; i++) {
        char p[4400];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", gen, sources[i]);
        bytes[i] = stat(p, &st) == 0 ? (double)st.st_size : 1000;
        total_bytes += bytes[i];
    }
    char obj_stamp[4400], have[96];                  /* (objects from another toolchain or game: none kept) */
    snprintf(obj_stamp, sizeof obj_stamp, "%s/.stamp", obj);
    read_text(obj_stamp, have, sizeof have);
    if (strcmp(have, stamp) != 0) {                  /* (and zig's own cache of compiled files with them) */
        char cache[4400];
        snprintf(cache, sizeof cache, "%s/zigcache", work_dir);
        remove_tree(obj);
        remove_tree(cache);
    }
    make_dirs(obj);
    write_text(obj_stamp, stamp);
    Job running[8];
    int nrun = 0, next = 0, done = 0, failed = 0, paused = 0;
    char log[4400], src[4400], out[4400], inc_gen[4400], inc_rt[4400], abi[200];
    snprintf(log, sizeof log, "%s/compile.log", log_dir);
    snprintf(inc_gen, sizeof inc_gen, "-I%s", gen);
    snprintf(inc_rt, sizeof inc_rt, "-I%s/runtime", tool_dir);
    snprintf(abi, sizeof abi, "-DRECOMP_ABI=\"%s\"", RECOMP_ABI);
    printf("build: compiling %d files (%s)\n", n, speeds[speed].name);
    while (!cancel && (next < n || nrun) && !failed) {
        for (int i = 0; i < nrun; i++) {             /* finished ones */
            int st = finished(running[i].pid, 0);
            if (st < 0) continue;
            if (st != 0) { failed = 1; show_log_tail(log, st); fail("compiling %s failed (see build/compile.log)", running[i].name); }
            done++;
            done_bytes += running[i].bytes;
            compiled_bytes += running[i].bytes;
            running[i--] = running[--nrun];
        }
        int allowed = heat(), hot = allowed == 0;
        if (hot != paused) {                         /* hot: the running ones frozen; cooler: on again */
            for (int i = 0; i < nrun; i++) kill(-running[i].pid, hot ? SIGSTOP : SIGCONT);
            paused = hot;
        }
        while (!failed && next < n && nrun < allowed) {
            double file_bytes = bytes[next];
            const char *name = sources[next++];
            if (strcmp(name, "game_abi.c") == 0) snprintf(src, sizeof src, "%s/runtime/%s", tool_dir, name);
            else snprintf(src, sizeof src, "%s/%s", gen, name);
            snprintf(out, sizeof out, "%s/%.*s.o", obj, (int)(strlen(name) - 2), name);
            if (newer(out, src)) { done++; done_bytes += file_bytes; continue; }   /* (compiled before the app closed) */
            char *argv[] = { "zig", "cc", "-target", "aarch64-linux-musl", "-mcpu=generic+v8_2a", "-O2", "-g0", "-fPIC",
                             "-fwrapv", "-ffp-contract=off", "-fno-math-errno", "-w", "-fvisibility=hidden",
                             "-fno-vectorize", "-fno-slp-vectorize", "-mllvm", "-enable-misched=false", "-mllvm",
                             "-enable-post-misched=false",          /* (as CMakeLists.txt: ~20% faster, as fast) */
                             "-fdata-sections", "-ffunction-sections", "-funwind-tables", abi, inc_gen, inc_rt,
                             "-c", src, "-o", out, NULL };
            pid_t pid = run("libzig.so", argv, log, NULL);
            if (pid < 0) { failed = 1; fail("couldn't start the compiler", NULL); break; }
            running[nrun].pid = pid;
            running[nrun].bytes = file_bytes;
            snprintf(running[nrun].name, sizeof running[0].name, "%s", name);
            nrun++;
        }
        char detail[160];
        snprintf(detail, sizeof detail, hot ? "Compiling: %d of %d (the device is warm: pausing to let it cool)"
                                           : "Compiling: %d of %d", done, n);
        set_state(BUILD_COMPILING, -1, detail);
        double typical = 1e6 / typical_s_per_mb[speed], t = now_s() - c_start, rate = typical;
        if (compiled_bytes > 0 && t > 60) {          /* (its own pace, trusted more as more is done) */
            double w = compiled_bytes / (0.15 * total_bytes);
            if (w > 1) w = 1;
            rate = w * compiled_bytes / t + (1 - w) * typical;
        }
        estimate((total_bytes - done_bytes) / rate + 10);
        pthread_mutex_lock(&lock);
        state.cooling = hot;
        pthread_mutex_unlock(&lock);
        struct timespec ts = { 0, 200000000 };
        nanosleep(&ts, NULL);
    }
    for (int i = 0; i < nrun; i++) {
        if (cancel || failed) { kill(-running[i].pid, SIGCONT); kill(-running[i].pid, SIGTERM); }
        finished(running[i].pid, 1);
    }
    return cancel || failed ? -1 : 0;
}

/* ---- 4. link ---- */
static int link_library(const char *obj, const char *lib) {
    char list[4400], log[4400], main_so[4400], soname[64], rsp[4500];
    snprintf(list, sizeof list, "%s/objects.txt", work_dir);
    snprintf(log, sizeof log, "%s/link.log", log_dir);
    snprintf(main_so, sizeof main_so, "%s/libmain.so", lib_dir);
    snprintf(soname, sizeof soname, "libgame-%s.so", game_id);
    FILE *f = fopen(list, "w");
    DIR *d = opendir(obj);
    struct dirent *e;
    while (f && d && (e = readdir(d)))
        if (strlen(e->d_name) > 2 && strcmp(e->d_name + strlen(e->d_name) - 2, ".o") == 0) fprintf(f, "%s/%s\n", obj, e->d_name);
    if (d) closedir(d);
    if (f) fclose(f);
    snprintf(rsp, sizeof rsp, "@%s", list);
    set_state(BUILD_LINKING, -1, "Linking");
    estimate(10);
    char *argv[] = { "zig", "ld.lld", "-shared", "-o", (char *)lib, rsp, main_so, "-soname", soname, "--gc-sections",
                     "-z", "noexecstack", NULL };
    pid_t pid = run("libzig.so", argv, log, NULL);
    int st = pid < 0 ? -1 : finished(pid, 1);
    if (st != 0) { show_log_tail(log, st); return fail("linking failed (see build/link.log)", NULL); }
    return 0;
}

static void *build_thread(void *arg) {
    (void)arg;
    double t0 = now_s();
    char gen[4400], obj[4400], lib[4400], dst[4400];
    snprintf(gen, sizeof gen, "%s/gen", work_dir);
    snprintf(obj, sizeof obj, "%s/obj", work_dir);
    snprintf(lib, sizeof lib, "%s/libgame-%s.so", work_dir, game_id);
    snprintf(dst, sizeof dst, "%s/libgame-%s.so", game_dir, game_id);
    build_t0 = now_s();
    make_dirs(work_dir);
    make_dirs(log_dir);
    for (int k = 0; k < 3; k++) {                    /* (each build's logs fresh) */
        char p[4400];
        snprintf(p, sizeof p, "%s/%s.log", log_dir, k == 0 ? "translate" : k == 1 ? "compile" : "link");
        unlink(p);
    }
    calm_down();
    SDL_DisableScreenSaver();                         /* (Android: the screen stays on while it builds) */
    int ok = unpack_toolchain() == 0 && translate(gen) == 0 && compile_all(gen, obj) == 0 && link_library(obj, lib) == 0;
    if (ok) {
        make_dirs(game_dir);
        if (rename(lib, dst) != 0) ok = 0, fail("couldn't put the game code in place", NULL);
    }
    SDL_EnableScreenSaver();
    if (ok) {
        printf("build: libgame-%s.so made in %.0f s\n", game_id, now_s() - t0);
        remove_tree(gen);                             /* (~700 MB of C, the objects and zig's cache: not needed */
        remove_tree(obj);                             /*  any more) */
        char cache[4400];
        snprintf(cache, sizeof cache, "%s/zigcache", work_dir);
        remove_tree(cache);
        set_state(BUILD_DONE, 1, "Done");
    } else if (cancel) {
        printf("build: cancelled after %.0f s (what is done is kept)\n", now_s() - t0);
        set_state(BUILD_IDLE, 0, "");
    }
    return NULL;
}

int builder_start(const char *exefs, const char *lib_dir_for_game, int build_speed) {
    pthread_mutex_lock(&lock);
    int busy = state.step > BUILD_IDLE && state.step < BUILD_DONE;
    pthread_mutex_unlock(&lock);
    if (busy) return 0;
    snprintf(exefs_dir, sizeof exefs_dir, "%s", exefs);
    snprintf(game_dir, sizeof game_dir, "%s", lib_dir_for_game);
    if (game_lib_find(exefs, game_dir, NULL, game_id) < 0) return -1;
    Dl_info info;                                     /* the native-library folder: where libmain.so is */
    if (!dladdr((void *)builder_start, &info) || !info.dli_fname) return -1;
    snprintf(lib_dir, sizeof lib_dir, "%s", info.dli_fname);
    *strrchr(lib_dir, '/') = 0;
    const char *internal = SDL_GetAndroidInternalStoragePath();
    snprintf(files_dir, sizeof files_dir, "%s", internal ? internal : ".");
    snprintf(tool_dir, sizeof tool_dir, "%s/toolchain", files_dir);
    snprintf(work_dir, sizeof work_dir, "%s/build", files_dir);
    const char *external = SDL_GetAndroidExternalStoragePath();   /* (the logs where the player can find them) */
    snprintf(log_dir, sizeof log_dir, "%s/build", external ? external : files_dir);
    cancel = 0;
    speed = build_speed >= BUILD_COOL && build_speed <= BUILD_FAST ? build_speed : BUILD_BALANCED;
    pthread_mutex_lock(&lock);
    memset(&state, 0, sizeof state);
    state.step = BUILD_PREPARING;
    state.started = now_s();
    pthread_mutex_unlock(&lock);
    pthread_t t;
    if (pthread_create(&t, NULL, build_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}

void builder_state(BuildState *out) {
    pthread_mutex_lock(&lock);
    *out = state;
    out->elapsed = state.started ? now_s() - state.started : 0;
    pthread_mutex_unlock(&lock);
}

void builder_cancel(void) { cancel = 1; }
