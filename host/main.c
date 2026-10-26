/*
 * host/main.c - the host program: desktop (Linux) and Android.
 *
 *   laa_host <exefs dir> [--romfs DIR] [--headless] [--dump-image FILE]
 *
 * On Android this is libmain.so: SDL's Java activity loads it and calls main() (SDL renames
 * it SDL_main). There is no command line there; everything lives in the app's own folder,
 * /storage/emulated/0/Android/data/<package>/files ("the app folder", adb can push to it):
 *   exefs/, romfs/   the player's own dump
 *   env.txt          KEY=VALUE lines, set as environment variables (LAA_* options)
 *   log.txt          everything the runtime prints (also sent to logcat, tag LAARecomp)
 *   save/            the game's saves
 * Reserves guest memory, loads the game's modules from the player's own dump, opens a window
 * (SDL3 + Vulkan; --headless: none), then starts the game the way the Switch kernel would.
 * The game runs on its own threads; this thread handles the window's events.
 * Set LAA_TRACE_SVC=1 to log every SVC, LAA_TRACE_IPC=1 to log every service command.
 *
 * The RomFS is the extracted folder, by default "romfs" next to the ExeFS folder.
 * --dump-image writes the loaded modules' memory to FILE (and doesn't run anything) so
 * tests/check_loader.py can compare it byte for byte with what the recompiler saw.
 */
#include "recomp_runtime.h"
#include "guest_memory.h"
#include "nso_loader.h"
#include "kernel.h"
#include "romfs.h"
#include "hle.h"
#include "nvn/nvn.h"
#include "movie/movie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "input.h"
#ifdef LAA_HAVE_SDL
#include <SDL3/SDL.h>
#include "render/render.h"

/* Keyboard: arrows = left stick, I/J/K/L = right stick, X = A, Z = B, S = X, A = Y, Q = L, W = R,
 * 1 = ZL, 2 = ZR, Enter = +, Backspace = -, T/F/G/H = d-pad. Gamepads map one to one. */
/* The four face buttons go by the letter printed on them when the controller says (the Thor in
 * Nintendo layout: its A is on the right but SDL calls it "south", Xbox's A's position), else by
 * position as on a Switch controller (east = A, south = B, north = X, west = Y). */
static uint64_t face_bit[4];                         /* SOUTH, EAST, WEST, NORTH -> BTN_* */

static void map_face_buttons(SDL_Gamepad *pad) {
    static const SDL_GamepadButton pos[4] = { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                              SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH };
    static const uint64_t by_position[4] = { BTN_B, BTN_A, BTN_Y, BTN_X };
    static const char *names[] = { "A", "B", "X", "Y" };
    for (int i = 0; i < 4; i++) {
        SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabel(pad, pos[i]);
        face_bit[i] = l == SDL_GAMEPAD_BUTTON_LABEL_A ? BTN_A : l == SDL_GAMEPAD_BUTTON_LABEL_B ? BTN_B
                    : l == SDL_GAMEPAD_BUTTON_LABEL_X ? BTN_X : l == SDL_GAMEPAD_BUTTON_LABEL_Y ? BTN_Y : by_position[i];
    }
    int letter[4];
    for (int i = 0; i < 4; i++) letter[i] = face_bit[i] == BTN_A ? 0 : face_bit[i] == BTN_B ? 1 : face_bit[i] == BTN_X ? 2 : 3;
    printf("input: %s: south = %s, east = %s, west = %s, north = %s\n", SDL_GetGamepadName(pad), names[letter[0]],
           names[letter[1]], names[letter[2]], names[letter[3]]);
}

static void read_input(void) {
    static SDL_Gamepad *pad;
    if (!pad) {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (pad) map_face_buttons(pad);
    }
    const bool *k = SDL_GetKeyboardState(NULL);
    InputState s = { 0 };
    static const struct { SDL_Scancode key; uint64_t bit; } keys[] = {
        { SDL_SCANCODE_X, BTN_A }, { SDL_SCANCODE_Z, BTN_B }, { SDL_SCANCODE_S, BTN_X }, { SDL_SCANCODE_A, BTN_Y },
        { SDL_SCANCODE_Q, BTN_L }, { SDL_SCANCODE_W, BTN_R }, { SDL_SCANCODE_1, BTN_ZL }, { SDL_SCANCODE_2, BTN_ZR },
        { SDL_SCANCODE_RETURN, BTN_PLUS }, { SDL_SCANCODE_BACKSPACE, BTN_MINUS }, { SDL_SCANCODE_T, BTN_UP },
        { SDL_SCANCODE_G, BTN_DOWN }, { SDL_SCANCODE_F, BTN_LEFT }, { SDL_SCANCODE_H, BTN_RIGHT },
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) if (k[keys[i].key]) s.buttons |= keys[i].bit;
    if (k[SDL_SCANCODE_LEFT]) s.lx -= 32767;
    if (k[SDL_SCANCODE_RIGHT]) s.lx += 32767;
    if (k[SDL_SCANCODE_UP]) s.ly += 32767;
    if (k[SDL_SCANCODE_DOWN]) s.ly -= 32767;
    if (k[SDL_SCANCODE_J]) s.rx -= 32767;
    if (k[SDL_SCANCODE_L]) s.rx += 32767;
    if (k[SDL_SCANCODE_I]) s.ry += 32767;
    if (k[SDL_SCANCODE_K]) s.ry -= 32767;
    if (pad) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH)) s.buttons |= face_bit[0];
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST)) s.buttons |= face_bit[1];
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST)) s.buttons |= face_bit[2];
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH)) s.buttons |= face_bit[3];
        static const struct { SDL_GamepadButton b; uint64_t bit; } map[] = {
            { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L },
            { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R }, { SDL_GAMEPAD_BUTTON_START, BTN_PLUS },
            { SDL_GAMEPAD_BUTTON_BACK, BTN_MINUS }, { SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP },
            { SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT },
            { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT }, { SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_LSTICK },
            { SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_RSTICK },
        };
        for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (SDL_GetGamepadButton(pad, map[i].b)) s.buttons |= map[i].bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) s.buttons |= BTN_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) s.buttons |= BTN_ZR;
        int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        int rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        if (abs(lx) > 4000 || abs(ly) > 4000) { s.lx = lx; s.ly = ly; }
        if (abs(rx) > 4000 || abs(ry) > 4000) { s.rx = rx; s.ry = ry; }
    }
    if (s.lx > 32767) s.lx = 32767;
    if (s.ly > 32767) s.ly = 32767;
    if (s.lx < -32767) s.lx = -32767;
    if (s.ly < -32767) s.ly = -32767;
    input_set(&s);
}
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

static FILE *log_file;

/* Native stdout/stderr go nowhere on Android: they are pointed at a pipe, and this thread
 * copies each line to logcat and to the app folder's log.txt. */
static void log_line(const char *line) {
    __android_log_write(ANDROID_LOG_INFO, "LAARecomp", line);
    if (log_file) { fprintf(log_file, "%s\n", line); fflush(log_file); }
}

static void *log_pump(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[4096];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, sizeof buf - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', used - (size_t)(start - buf)))) {
            *nl = 0;
            log_line(start);
            start = nl + 1;
        }
        used -= (size_t)(start - buf);
        memmove(buf, start, used);
        if (used == sizeof buf - 1) { buf[used] = 0; log_line(buf); used = 0; }   /* a very long line */
    }
    return NULL;
}

/* The app folder's paths; logging; env.txt */
static void android_setup(const char **exefs, const char **romfs) {
    static char exefs_path[1024], romfs_path[1024];
    const char *base = SDL_GetAndroidExternalStoragePath();
    if (!base) base = SDL_GetAndroidInternalStoragePath();
    char path[1024];
    snprintf(path, sizeof path, "%s/log.txt", base);
    log_file = fopen(path, "w");
    int p[2];
    if (pipe(p) == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        pthread_t t;
        pthread_create(&t, NULL, log_pump, (void *)(intptr_t)p[0]);
    }
    snprintf(path, sizeof path, "%s/env.txt", base);
    FILE *env = fopen(path, "r");
    char line[8192];
    while (env && fgets(line, sizeof line, env)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        printf("env: %s=%s\n", line, eq + 1);
    }
    if (env) fclose(env);
    snprintf(path, sizeof path, "%s/save", base);
    setenv("LAA_SAVE_DIR", path, 0);
    snprintf(path, sizeof path, "%s/fonts", base);
    setenv("LAA_FONT_DIR", path, 0);
    snprintf(exefs_path, sizeof exefs_path, "%s/exefs", base);
    snprintf(romfs_path, sizeof romfs_path, "%s/romfs", base);
    *exefs = exefs_path;
    *romfs = romfs_path;
    printf("app folder: %s\n", base);
}
#endif

#define LOAD_BASE 0x8000000ull   /* must match recomp.py --base (same as yuzu/Ryujinx) */

static int dump_image(const GameImage *img, const char *path) {
    uint64_t start = img->mods[0].base;
    uint64_t end = img->mods[img->count - 1].base + img->mods[img->count - 1].size;
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(guest_mem + start, 1, end - start, f) != end - start) {
        perror(path);
        if (f) fclose(f);
        return -1;
    }
    fclose(f);
    printf("Wrote guest memory 0x%" PRIx64 "..0x%" PRIx64 " to %s\n", start, end, path);
    return 0;
}

int main(int argc, char **argv) {
    const char *exefs = NULL, *romfs = NULL, *dump = NULL;
    int headless = 0;
    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--romfs") == 0 && k + 1 < argc) romfs = argv[++k];
        else if (strcmp(argv[k], "--headless") == 0) headless = 1;
        else if (strcmp(argv[k], "--dump-image") == 0 && k + 1 < argc) dump = argv[++k];
        else if (!exefs && argv[k][0] != '-') exefs = argv[k];
        else exefs = NULL, k = argc;   /* anything else: show usage */
    }
#ifdef __ANDROID__
    android_setup(&exefs, &romfs);
#endif
    if (!exefs) {
        fprintf(stderr, "usage: %s <exefs dir> [--romfs DIR] [--headless] [--dump-image FILE]\n", argv[0]);
        return 2;
    }
    if (gmem_reserve() != 0) return 1;

    GameImage img;
    if (nso_load_exefs(exefs, LOAD_BASE, &img) != 0) return 1;
    for (int k = 0; k < img.count; k++) {
        const GameModule *m = &img.mods[k];
        printf("  %-8s 0x%09" PRIx64 "  size 0x%08" PRIx64 "  build id ", m->name, m->base, m->size);
        for (int b = 0; b < 8; b++) printf("%02X", m->build_id[b]);
        printf("...\n");
    }
    printf("Loaded %d modules; %" PRIu64 " recompiled functions linked\n", img.count, recomp_func_count);

    if (dump) return dump_image(&img, dump) != 0;

    char romfs_default[4096];
    if (!romfs) {
        snprintf(romfs_default, sizeof romfs_default, "%s/../romfs", exefs);
        romfs = romfs_default;
    }
    if (romfs_open_dir(romfs) != 0) return 1;
    if (kernel_init(exefs, &img) != 0) return 1;
    hle_init(&kproc.image);
    nvn_init();
    movie_init(&kproc.image);             /* the game's graphics calls now come to runtime/nvn */

    void *window = NULL;
#ifdef LAA_HAVE_SDL
    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            fprintf(stderr, "No window: %s\n", SDL_GetError());
        } else {
#ifdef __ANDROID__
            /* SDL's activity sets the screen orientation from the window's size, and a 0x0 window
             * isn't wider than tall: without this hint it turns the app to portrait */
            SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
            window = SDL_CreateWindow("LAARecomp", 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
#else
            window = SDL_CreateWindow("LAARecomp", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
#endif
            if (!window) fprintf(stderr, "No window: %s\n", SDL_GetError());
            else if (render_init(window) != 0) { SDL_DestroyWindow(window); window = NULL; }
        }
    }
#else
    (void)headless;
#endif
    if (!window) printf("Running without a window\n");
    if (kernel_start_main() != 0) return 1;

    for (;;) {
#ifdef LAA_HAVE_SDL
        SDL_Event e;
        while (window && SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) exit(0);
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) render_resized();
        }
        if (window) read_input();
#endif
        if (kernel_wait_main(window ? 10 : 1000)) break;   /* the game ended */
    }
    printf("kernel: main thread exited\n");
    return 0;
}
