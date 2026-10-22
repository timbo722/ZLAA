/*
 * input.c - the controller state the game sees.
 *
 * LAA_INPUT_SCRIPT presses buttons by itself, for testing without a person:
 *     LAA_INPUT_SCRIPT="2900:L+R,3200:A,3400:A"
 * holds each button (or buttons joined by +) for 10 frames starting at that presented frame. Names: A B X Y L R ZL ZR
 * PLUS MINUS UP DOWN LEFT RIGHT.
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

static struct { uint64_t frame; uint64_t buttons; } script[64];
static int nscript = -1;

static void load_script(void) {
    nscript = 0;
    const char *s = getenv("LAA_INPUT_SCRIPT");
    static const struct { const char *name; uint64_t bit; } names[] = {
        { "A", BTN_A }, { "B", BTN_B }, { "X", BTN_X }, { "Y", BTN_Y }, { "L", BTN_L }, { "R", BTN_R },
        { "ZL", BTN_ZL }, { "ZR", BTN_ZR }, { "PLUS", BTN_PLUS }, { "MINUS", BTN_MINUS }, { "UP", BTN_UP },
        { "DOWN", BTN_DOWN }, { "LEFT", BTN_LEFT }, { "RIGHT", BTN_RIGHT },
    };
    while (s && *s && nscript < 64) {
        char name[16] = { 0 };
        unsigned long long frame;
        int used = 0;
        if (sscanf(s, "%llu:%n", &frame, &used) != 1 || !used) break;
        s += used;
        script[nscript].frame = frame;
        script[nscript].buttons = 0;
        while (sscanf(s, "%15[A-Z]%n", name, &used) == 1) {          /* NAME, or NAME+NAME+... */
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strcmp(names[i].name, name) == 0) script[nscript].buttons |= names[i].bit;
            s += used;
            if (*s != '+') break;
            s++;
        }
        nscript++;
        if (*s == ',') s++;
    }
}

InputState input_get(void) {
    pthread_mutex_lock(&lock);
    InputState s = host;
    pthread_mutex_unlock(&lock);
    if (nscript < 0) load_script();
    uint64_t frame = nvn_frame_count();
    for (int i = 0; i < nscript; i++)
        if (frame >= script[i].frame && frame < script[i].frame + 10) s.buttons |= script[i].buttons;
    return s;
}
