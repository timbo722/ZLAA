/*
 * ui.h - the menus (runtime/ui: RmlUi documents), as host/main.c sees them:
 * the start screen shown before the game loads (Play, Settings, Quit; whether the game files and
 * the game code are there) and the pause menu over the running game. Settings are saved in a
 * config file and become LAA_* environment variables when the game starts (env.txt lines win).
 */
#ifndef UI_H
#define UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *exefs, *romfs;               /* the player's dump */
    const char *game_dir, *game_import;      /* where libgame-<id>.so is opened, and imported from (or NULL) */
    const char *config;                      /* the settings file */
    const char *res;                         /* the menu's own files (documents, style sheets, fonts) */
} UiPaths;

/* What the menu is told; main.c turns buttons, keys and the stick into these */
enum { UI_UP, UI_DOWN, UI_LEFT, UI_RIGHT, UI_ACCEPT, UI_BACK, UI_TAB_PREV, UI_TAB_NEXT, UI_MENU, UI_ACTIONS };
enum { UI_NONE, UI_PLAY, UI_QUIT, UI_RESUME };

int ui_init(void *sdl_window, const UiPaths *paths);   /* after render_init; 0 = ok */
void ui_start_menu(void);                /* show the start screen */
int ui_skip_start(void);                 /* 1: Skip Main Menu is on and the game is ready (settings applied) */
void ui_pause(void);                     /* show the pause menu (the game is suspended by the caller) */
int ui_open(void);                       /* a menu is showing */
void ui_input(int action, int down);     /* a press (down = 1) or release */
void ui_pointer(const void *sdl_event);  /* mouse / touch (SDL_Event) */
/* Draw a frame of the open menu (waits for the display); returns what the player chose (UI_NONE:
 * nothing yet; UI_PLAY, UI_QUIT, UI_RESUME close the menu) */
int ui_frame(void);
void ui_apply_settings(void);            /* settings -> environment variables */
const char *ui_game_exefs(void);         /* the game files in use (the player can choose others: Game Files) */
const char *ui_game_romfs(void);

/* Provided by the host (main.c): finding the game files. Android: "All files access" lets the app read them in
 * place; the picker's answer comes later (host_picked_folder: 1 = answered, out "" = cancelled) */
int host_files_access(void);
void host_request_files_access(void);
void host_pick_folder(void);
int host_picked_folder(char *out, size_t size);

/* Provided by the host: building the game's code on the device (Android: host/builder.c; elsewhere -1 / idle) */
enum { UI_BUILD_IDLE, UI_BUILD_PREPARING, UI_BUILD_TRANSLATING, UI_BUILD_COMPILING, UI_BUILD_LINKING, UI_BUILD_DONE,
       UI_BUILD_FAILED };
int host_build_start(const char *exefs, int speed);   /* 0 = started (or running); speed 0 cool, 1 balanced, 2 fast */
int host_build_progress(double *fraction, char *detail, size_t size, double *elapsed, int *cooling);   /* -> UI_BUILD_* */
void host_build_cancel(void);
int ui_settings_changed(void);           /* 1 once after the player changed a setting */

#ifdef __cplusplus
}
#endif
#endif
