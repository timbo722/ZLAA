/*
 * input.h - the controller state the game sees (written by the host, read by the hid service).
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* Button bits, as the Switch numbers them (NpadButton) */
enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3, BTN_LSTICK = 1 << 4, BTN_RSTICK = 1 << 5,
    BTN_L = 1 << 6, BTN_R = 1 << 7, BTN_ZL = 1 << 8, BTN_ZR = 1 << 9, BTN_PLUS = 1 << 10, BTN_MINUS = 1 << 11,
    BTN_LEFT = 1 << 12, BTN_UP = 1 << 13, BTN_RIGHT = 1 << 14, BTN_DOWN = 1 << 15,
};

typedef struct {
    uint64_t buttons;
    int32_t lx, ly, rx, ry;          /* sticks: -32767..32767, y up */
} InputState;

/* The host sets what its keyboard/gamepad say; LAA_INPUT_SCRIPT adds scripted presses. */
void input_set(const InputState *s);
InputState input_get(void);          /* what the game should see now */

#endif
