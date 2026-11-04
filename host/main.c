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
#include "trace_symbols.h"
#include "game_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include "input.h"
#ifdef LAA_HAVE_SDL
#include <SDL3/SDL.h>
#include "render/render.h"
#ifdef LAA_HAVE_UI
#include "ui/ui.h"
#endif

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
    if (getenv("LAA_SWAP_FACE"))                     /* (the menu's "Button layout": A <-> B, X <-> Y) */
        for (int i = 0; i < 4; i++)
            face_bit[i] = face_bit[i] == BTN_A ? BTN_B : face_bit[i] == BTN_B ? BTN_A : face_bit[i] == BTN_X ? BTN_Y : BTN_X;
    int letter[4];
    for (int i = 0; i < 4; i++) letter[i] = face_bit[i] == BTN_A ? 0 : face_bit[i] == BTN_B ? 1 : face_bit[i] == BTN_X ? 2 : 3;
    printf("input: %s: south = %s, east = %s, west = %s, north = %s\n", SDL_GetGamepadName(pad), names[letter[0]],
           names[letter[1]], names[letter[2]], names[letter[3]]);
}

static SDL_Gamepad *pad;

/* The Switch button of a gamepad button (0: none) */
static uint64_t pad_bit(SDL_GamepadButton b) {
    static const struct { SDL_GamepadButton b; uint64_t bit; } map[] = {
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, BTN_L },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, BTN_R }, { SDL_GAMEPAD_BUTTON_START, BTN_PLUS },
        { SDL_GAMEPAD_BUTTON_BACK, BTN_MINUS }, { SDL_GAMEPAD_BUTTON_DPAD_UP, BTN_UP },
        { SDL_GAMEPAD_BUTTON_DPAD_DOWN, BTN_DOWN }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, BTN_LEFT },
        { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, BTN_RIGHT }, { SDL_GAMEPAD_BUTTON_LEFT_STICK, BTN_LSTICK },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, BTN_RSTICK },
    };
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return face_bit[0];
    case SDL_GAMEPAD_BUTTON_EAST: return face_bit[1];
    case SDL_GAMEPAD_BUTTON_WEST: return face_bit[2];
    case SDL_GAMEPAD_BUTTON_NORTH: return face_bit[3];
    default: break;
    }
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (map[i].b == b) return map[i].bit;
    return 0;
}

static const struct { SDL_Scancode key; uint64_t bit; } keys[] = {
    { SDL_SCANCODE_X, BTN_A }, { SDL_SCANCODE_Z, BTN_B }, { SDL_SCANCODE_S, BTN_X }, { SDL_SCANCODE_A, BTN_Y },
    { SDL_SCANCODE_Q, BTN_L }, { SDL_SCANCODE_W, BTN_R }, { SDL_SCANCODE_1, BTN_ZL }, { SDL_SCANCODE_2, BTN_ZR },
    { SDL_SCANCODE_RETURN, BTN_PLUS }, { SDL_SCANCODE_BACKSPACE, BTN_MINUS }, { SDL_SCANCODE_T, BTN_UP },
    { SDL_SCANCODE_G, BTN_DOWN }, { SDL_SCANCODE_F, BTN_LEFT }, { SDL_SCANCODE_H, BTN_RIGHT },
};

/* Android's HOME (or the other screen's app switcher): the game is suspended in the background,
 * as on a Switch, and resumes on the new window it comes back with. An event watch: SDL calls it as
 * the event happens (its event loop then blocks until the app is back). */
static int menu, game_started, pause_on_return;   /* the menus run; the game has started; pause it (main loop) */

static bool SDLCALL lifecycle(void *userdata, SDL_Event *e) {
    (void)userdata;
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        printf("app: in the background, game suspended\n");
        if (menu && game_started) {                  /* back to the pause menu (main loop), not straight into the game */
            kernel_set_suspended(SUSPEND_MENU, 1);
            pause_on_return = 1;
        }
        kernel_set_suspended(SUSPEND_BACKGROUND, 1);
        render_background(1);
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        printf("app: back in the foreground\n");
        render_background(0);
        kernel_set_suspended(SUSPEND_BACKGROUND, 0);
    }
    return true;
}

/* The first gamepad, once there is one: its events only come after it is opened (so the menus,
 * which run before the game reads input, open it too) */
static void open_pad(void) {
    if (pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (pad) map_face_buttons(pad);
}

/* Presses as events, so a quick tap isn't lost between two samples (input_press) */
static void input_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && pad && e->gbutton.which == SDL_GetGamepadID(pad))
        input_press(pad_bit((SDL_GamepadButton)e->gbutton.button));
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) if (e->key.scancode == keys[i].key) input_press(keys[i].bit);
}

#ifdef LAA_HAVE_UI
/* Buttons, keys and the stick for the menus: d-pad / arrows move, A / X / Enter accept, B / Z / Esc /
 * Android's Back go back, L / R (Q / W) switch tabs; Home (the controller's Guide or Mode button), Esc
 * or Back open the pause menu during the game */
static int menu_action(const SDL_Event *e, int *down) {
    *down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_KEY_DOWN;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        SDL_GamepadButton b = (SDL_GamepadButton)e->gbutton.button;
        if (b == SDL_GAMEPAD_BUTTON_GUIDE) return UI_MENU;
        switch (pad_bit(b)) {
        case BTN_UP: return UI_UP;
        case BTN_DOWN: return UI_DOWN;
        case BTN_LEFT: return UI_LEFT;
        case BTN_RIGHT: return UI_RIGHT;
        case BTN_A: return UI_ACCEPT;
        case BTN_B: return UI_BACK;
        case BTN_L: return UI_TAB_PREV;
        case BTN_R: return UI_TAB_NEXT;
        default: return -1;
        }
    }
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) && !e->key.repeat) {
        switch (e->key.scancode) {
        case SDL_SCANCODE_UP: return UI_UP;
        case SDL_SCANCODE_DOWN: return UI_DOWN;
        case SDL_SCANCODE_LEFT: return UI_LEFT;
        case SDL_SCANCODE_RIGHT: return UI_RIGHT;
        case SDL_SCANCODE_X: case SDL_SCANCODE_RETURN: return UI_ACCEPT;
        case SDL_SCANCODE_Z: case SDL_SCANCODE_BACKSPACE: return UI_BACK;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_AC_BACK: return ui_open() ? UI_BACK : UI_MENU;
        case SDL_SCANCODE_Q: return UI_TAB_PREV;
        case SDL_SCANCODE_W: return UI_TAB_NEXT;
        default: return -1;
        }
    }
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e->gaxis.axis <= SDL_GAMEPAD_AXIS_LEFTY) {
        static int held[2];                          /* the left stick as a d-pad: -1, 0, 1 per axis */
        int a = e->gaxis.axis, v = e->gaxis.value > 20000 ? 1 : e->gaxis.value < -20000 ? -1 : 0;
        if (v == held[a] || (v && held[a])) return -1;
        int dir = v ? v : held[a];
        held[a] = v;
        *down = v != 0;
        return a == SDL_GAMEPAD_AXIS_LEFTX ? (dir < 0 ? UI_LEFT : UI_RIGHT) : (dir < 0 ? UI_UP : UI_DOWN);
    }
    return -1;
}

static void menu_event(const SDL_Event *e) {
    open_pad();
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)    /* (which button is which on this controller) */
        printf("menu: button %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e->gbutton.button));
    int down, a = menu_action(e, &down);
    if (a >= 0) ui_input(a, down);
    if (e->type >= SDL_EVENT_MOUSE_MOTION && e->type <= SDL_EVENT_MOUSE_WHEEL) ui_pointer(e);
}

/* The player changed a setting: as environment variables now, read again where they can change live
 * (the controller's button layout here, the FPS counter, the volume, the stick dead zone) */
static void settings_changed(void) {
    ui_apply_settings();
    if (pad) map_face_buttons(pad);
    kernel_settings_gen++;
}

static void pause_game(void) {                       /* the pause menu: the game stops */
    printf("menu: paused\n");
    kernel_set_suspended(SUSPEND_MENU, 1);
    render_ui_set(1);
    ui_pause();
    input_set(&(InputState){ 0 });
}

/* The menus run unless a test drives the game (LAA_INPUT_SCRIPT) or LAA_NO_MENU is set; on the desktop
 * only with LAA_MENU=1 (there it is a tool) */
static int menu_wanted(void) {
    if (getenv("LAA_NO_MENU") || getenv("LAA_INPUT_SCRIPT")) return 0;
#ifdef __ANDROID__
    return 1;
#else
    return getenv("LAA_MENU") != NULL;
#endif
}
#endif

static void read_input(void) {
    open_pad();
    const bool *k = SDL_GetKeyboardState(NULL);
    InputState s = { 0 };
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
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) s.buttons |= pad_bit((SDL_GamepadButton)b);
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) s.buttons |= BTN_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) s.buttons |= BTN_ZR;
        int lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        int rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        static int dead, gen = -1;                   /* stick dead zone (LAA_DEADZONE, of 32767; the menu's) */
        if (gen != kernel_settings_gen) gen = kernel_settings_gen, dead = getenv("LAA_DEADZONE") ? atoi(getenv("LAA_DEADZONE")) : 4000;
        if (abs(lx) > dead || abs(ly) > dead) { s.lx = lx; s.ly = ly; }
        if (abs(rx) > dead || abs(ry) > dead) { s.rx = rx; s.ry = ry; }
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
    /* the pipeline cache: in internal storage (a real file system: the shared folder above is
     * FUSE, slow for megabyte writes) */
    const char *internal = SDL_GetAndroidInternalStoragePath();
    snprintf(path, sizeof path, "%s/cache", internal ? internal : base);
    setenv("LAA_CACHE_DIR", path, 0);
    snprintf(path, sizeof path, "%s/cache/pipelines.bin", base);   /* (where it was before: read once) */
    setenv("LAA_CACHE_FALLBACK", path, 0);
    /* the game library: opened from internal storage (the shared folder can't hold code that runs);
     * one built on a PC and pushed to <app folder>/game is copied there first */
    snprintf(path, sizeof path, "%s/game", internal ? internal : base);
    setenv("LAA_GAME_DIR", path, 0);
    snprintf(path, sizeof path, "%s/game", base);
    setenv("LAA_GAME_IMPORT", path, 0);
    /* the dump: files/exefs + files/romfs, or LAA_EXEFS / LAA_ROMFS (another version next to it) */
    const char *e = getenv("LAA_EXEFS"), *r = getenv("LAA_ROMFS");
    if (e) snprintf(exefs_path, sizeof exefs_path, "%s", e);
    else snprintf(exefs_path, sizeof exefs_path, "%s/exefs", base);
    if (r) snprintf(romfs_path, sizeof romfs_path, "%s", r);
    else snprintf(romfs_path, sizeof romfs_path, "%s/romfs", base);
    *exefs = exefs_path;
    *romfs = romfs_path;
    snprintf(path, sizeof path, "%s/config.json", base);
    setenv("LAA_CONFIG", path, 0);
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

    /* the window first: the start menu shows in it before anything of the game is loaded */
    void *window = NULL;
#ifdef LAA_HAVE_SDL
    if (!headless) {
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   /* (Back is ours: the menus) */
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
            else SDL_AddEventWatch(lifecycle, NULL);
        }
    }
#else
    (void)headless;
#endif
    if (!window) printf("Running without a window\n");

    char game_dir[4096];   /* the game's code: libgame-<build id>.so, in $LAA_GAME_DIR or next to this program */
    const char *dir = getenv("LAA_GAME_DIR");
    ssize_t n = dir ? 0 : readlink("/proc/self/exe", game_dir, sizeof game_dir - 1);
    if (dir) snprintf(game_dir, sizeof game_dir, "%s", dir);
    else if (n > 0) { game_dir[n] = 0; *strrchr(game_dir, '/') = 0; }
    else snprintf(game_dir, sizeof game_dir, ".");
    char romfs_default[4096];
    if (!romfs) {
        snprintf(romfs_default, sizeof romfs_default, "%s/../romfs", exefs);
        romfs = romfs_default;
    }

#ifdef LAA_HAVE_UI
    menu = window && menu_wanted();
    if (menu) {
        char config[4096], res[4096];
        const char *home = getenv("HOME");
        if (getenv("LAA_CONFIG")) snprintf(config, sizeof config, "%s", getenv("LAA_CONFIG"));
        else {
            snprintf(config, sizeof config, "%s/.config/LAARecomp", home ? home : ".");
            char parent[4096];
            snprintf(parent, sizeof parent, "%s/.config", home ? home : ".");
            mkdir(parent, 0755);
            mkdir(config, 0755);
            strncat(config, "/config.json", sizeof config - strlen(config) - 1);
        }
#ifdef __ANDROID__
        snprintf(res, sizeof res, "%s", getenv("LAA_RES_DIR") ? getenv("LAA_RES_DIR") : "res");   /* (the APK's assets) */
#else
        snprintf(res, sizeof res, "%s", getenv("LAA_RES_DIR") ? getenv("LAA_RES_DIR") : LAA_RES_DIR);
#endif
        UiPaths p = { exefs, romfs, game_dir, getenv("LAA_GAME_IMPORT"), config, res };
        menu = ui_init(window, &p) == 0;
        if (!menu) fprintf(stderr, "menu: couldn't start; starting the game\n");
    }
    if (menu) {                                      /* the start screen, until Play or Quit */
        ui_start_menu();
        for (int r = UI_NONE; r != UI_PLAY;) {
            open_pad();
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_QUIT) { fflush(NULL); _exit(0); }
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) render_resized();
                menu_event(&e);
            }
            r = ui_frame();
            if (r == UI_QUIT) { fflush(NULL); _exit(0); }
            if (ui_settings_changed()) settings_changed();
        }
        if (pad) map_face_buttons(pad);              /* (the settings as Play applied them) */
        kernel_settings_gen++;
    }
#endif

    if (game_lib_load(exefs, game_dir, getenv("LAA_GAME_IMPORT")) != 0) return 1;

    GameImage img;
    if (nso_load_exefs(exefs, LOAD_BASE, &img) != 0) return 1;
    for (int k = 0; k < img.count; k++) {
        const GameModule *m = &img.mods[k];
        printf("  %-8s 0x%09" PRIx64 "  size 0x%08" PRIx64 "  build id ", m->name, m->base, m->size);
        for (int b = 0; b < 8; b++) printf("%02X", m->build_id[b]);
        printf("...\n");
    }
    printf("Loaded %d modules; %" PRIu64 " recompiled functions linked\n", img.count, game_func_count);

    if (dump) return dump_image(&img, dump) != 0;

    if (romfs_open_dir(romfs) != 0) return 1;
    if (kernel_init(exefs, &img) != 0) return 1;
    hle_init(&kproc.image);
    nvn_init();
    movie_init(&kproc.image);             /* the game's graphics calls now come to runtime/nvn */
    trace_symbols_init(&kproc.image);     /* LAA_TRACE_SYMBOLS (debugging) */

#ifdef LAA_HAVE_SDL
    game_started = 1;
#endif
    if (kernel_start_main() != 0) return 1;

    for (;;) {
#ifdef LAA_HAVE_SDL
        SDL_Event e;
        while (window && SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) { fflush(NULL); _exit(0); }   /* (exit() under running threads: see vk_fail) */
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) render_resized();
#ifdef LAA_HAVE_UI
            if (menu && ui_open()) { menu_event(&e); continue; }
            int down;
            if (menu && menu_action(&e, &down) == UI_MENU && down) { pause_game(); continue; }
#endif
            input_event(&e);
        }
#ifdef LAA_HAVE_UI
        static uint64_t pause_at = 1;                /* LAA_UI_PAUSE_AT=seconds: pause once then (tests) */
        if (pause_at == 1) pause_at = getenv("LAA_UI_PAUSE_AT") ? SDL_GetTicks() + (uint64_t)(atof(getenv("LAA_UI_PAUSE_AT")) * 1000) : 0;
        if (menu && pause_at && SDL_GetTicks() >= pause_at && !ui_open()) { pause_at = 0; pause_game(); }
        if (menu && pause_on_return && !ui_open()) pause_game();
        pause_on_return = 0;
        if (menu && ui_open()) {
            int r = ui_frame();
            if (r == UI_QUIT) { fflush(NULL); _exit(0); }
            if (ui_settings_changed()) settings_changed();
            if (r == UI_RESUME) {
                printf("menu: resumed\n");
                render_ui_set(0);
                kernel_set_suspended(SUSPEND_MENU, 0);
            }
            continue;
        }
#endif
        if (window) read_input();
#endif
        if (kernel_wait_main(window ? 10 : 1000)) break;   /* the game ended */
    }
    printf("kernel: main thread exited\n");
    return 0;
}
