/*
 * input.c - the controller state the game sees.
 *
 * LAA_INPUT_SCRIPT presses buttons by itself, for testing without a person:
 *     LAA_INPUT_SCRIPT="2900:L+R,3200:A,3400:A"
 * holds each button (or buttons joined by +) for 10 frames starting at that presented frame, or for n frames with
 * "frame:NAME*n" ("2200:SDOWN*120": walk down for 2 seconds). Names: A B X Y L R ZL ZR PLUS MINUS, the d-pad UP
 * DOWN LEFT RIGHT, the left stick pushed all the way SUP SDOWN SLEFT SRIGHT.
 */
#include "input.h"
#include "nvn/nvn.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static InputState host;

void input_set(const InputState *s) {
    pthread_mutex_lock(&lock);
    host = *s;
    pthread_mutex_unlock(&lock);
}

/* The game reads the controller once a frame. The host samples it every ~10 ms, so a tap shorter
 * than a frame (a slow one, while loading) could fall between two of the game's reads: presses
 * are held for PRESS_FRAMES presented frames (the game frame after this one and the next). */
#define PRESS_FRAMES 2
static uint64_t press_frame[64];                     /* per button bit: frame of the last press + 1 */

void input_press(uint64_t buttons) {
    uint64_t frame = nvn_frame_count();
    pthread_mutex_lock(&lock);
    for (int b = 0; b < 64; b++)
        if (buttons >> b & 1) press_frame[b] = frame + 1;
    pthread_mutex_unlock(&lock);
}

#define MAX_SCRIPT 512
enum { STICK_UP = 1, STICK_DOWN = 2, STICK_LEFT = 4, STICK_RIGHT = 8 };
static struct { uint64_t frame, frames, buttons; uint32_t stick; } script[MAX_SCRIPT];
static int nscript = -1;

static void load_script(void) {
    nscript = 0;
    const char *s = getenv("LAA_INPUT_SCRIPT");
    static const struct { const char *name; uint64_t bit; } names[] = {
        { "A", BTN_A }, { "B", BTN_B }, { "X", BTN_X }, { "Y", BTN_Y }, { "L", BTN_L }, { "R", BTN_R },
        { "ZL", BTN_ZL }, { "ZR", BTN_ZR }, { "PLUS", BTN_PLUS }, { "MINUS", BTN_MINUS }, { "UP", BTN_UP },
        { "DOWN", BTN_DOWN }, { "LEFT", BTN_LEFT }, { "RIGHT", BTN_RIGHT },
    };
    static const struct { const char *name; uint32_t dir; } sticks[] = {
        { "SUP", STICK_UP }, { "SDOWN", STICK_DOWN }, { "SLEFT", STICK_LEFT }, { "SRIGHT", STICK_RIGHT },
    };
    while (s && *s && nscript < MAX_SCRIPT) {
        char name[16] = { 0 };
        unsigned long long frame;
        int used = 0;
        if (sscanf(s, "%llu:%n", &frame, &used) != 1 || !used) break;
        s += used;
        script[nscript].frame = frame;
        script[nscript].frames = 10;
        script[nscript].buttons = 0;
        script[nscript].stick = 0;
        while (sscanf(s, "%15[A-Z]%n", name, &used) == 1) {          /* NAME, or NAME+NAME+... */
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strcmp(names[i].name, name) == 0) script[nscript].buttons |= names[i].bit;
            for (size_t i = 0; i < sizeof sticks / sizeof *sticks; i++)
                if (strcmp(sticks[i].name, name) == 0) script[nscript].stick |= sticks[i].dir;
            s += used;
            if (*s != '+') break;
            s++;
        }
        unsigned long long n;
        if (*s == '*' && sscanf(s + 1, "%llu%n", &n, &used) == 1) { script[nscript].frames = n; s += 1 + used; }
        nscript++;
        if (*s == ',') s++;
    }
}

InputState input_get(void) {
    uint64_t frame = nvn_frame_count();
    pthread_mutex_lock(&lock);
    InputState s = host;
    for (int b = 0; b < 64; b++)
        if (press_frame[b] && frame + 1 - press_frame[b] < PRESS_FRAMES + 1) s.buttons |= 1ull << b;
    pthread_mutex_unlock(&lock);
    if (nscript < 0) load_script();
    for (int i = 0; i < nscript; i++) {
        if (frame < script[i].frame || frame >= script[i].frame + script[i].frames) continue;
        s.buttons |= script[i].buttons;
        uint32_t d = script[i].stick;
        if (d & STICK_UP) s.ly = 32767;
        if (d & STICK_DOWN) s.ly = -32767;
        if (d & STICK_LEFT) s.lx = -32767;
        if (d & STICK_RIGHT) s.lx = 32767;
    }
    return s;
}
