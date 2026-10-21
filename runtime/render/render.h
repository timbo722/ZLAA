/*
 * render.h - the renderer, as the rest of the runtime sees it.
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include "nvn/nvn.h"

/* Set up Vulkan for an SDL window. 0 on success; otherwise the game runs without a screen. */
int render_init(void *sdl_window);
int render_active(void);
/* Show this NVNtexture (guest address) in the window. Waits for the display (vsync). */
void render_present(uint64_t texture);
/* The window changed size. */
void render_resized(void);
/* Record the GPU side of one NVN command (nvn/exec.c), and finish a submission. */
void render_command(const Cmd *r);
void render_submitted(void);
/* If the GPU rendered this texture, copy its level 0 (32-bit pixels) to out and return 0. */
int render_read_back(uint64_t texture, uint8_t *out);
void render_stats(void);             /* print and reset draw statistics */

#endif
