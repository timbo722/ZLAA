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
/* Show this NVNtexture (guest address) in the window, the part `crop` (x, y, width, height;
 * width 0: all of it) scaled to fill it. Waits for the display (vsync). */
void render_present(uint64_t texture, const int32_t crop[4]);
/* The window changed size. */
void render_resized(void);
/* Android: the app went to the background (1: the pipeline cache is saved, in case Android ends
 * the app there) or came back (0: with a new window). */
void render_background(int on);
/* Record the GPU side of one NVN command (nvn/exec.c), and finish a submission. */
void render_command(const Cmd *r);
void render_submitted(void);
/* If the GPU rendered this texture, copy its level 0 (32-bit pixels) to out and return 0. */
int render_read_back(uint64_t texture, uint8_t *out);
/* The same, saved as a PNG (that crop of it, scaled down `scale` times) by a capture thread once
 * the GPU gets there: the game doesn't wait. 0 if it will be (or was dropped: too many waiting). */
int render_capture(uint64_t texture, const char *path, const int32_t crop[4], uint32_t scale);
void render_stats(void);             /* print and reset draw statistics */
extern double render_compile_ms;      /* time creating shaders and pipelines; how many pipelines */
extern unsigned render_compiles;
extern double render_gpu_wait_ms;    /* total time waiting for the GPU's submissions to finish */
/* Replace a 32-bit texture's level 0 with these pixels (rows of width * 4 bytes) */
void render_write_texture(uint64_t texture, const uint8_t *rgba, uint32_t width, uint32_t height);
/* The game made a texture (nvnTextureInitialize, any thread): it is uploaded ahead of its first
 * use, a little at each present (render_preload: up to `ms` milliseconds) */
void render_texture_made(const Texture *t);
void render_preload(double ms);
unsigned render_take_frame_draws(void);   /* draws since the last call (once per present) */
/* The game set up a program (nvnProgramSetShaders): its shaders are translated ahead, on a thread
 * (pipeline.c); render_adopt_shaders (at presents) takes the finished ones */
void render_program_made(const Program *p);
void render_adopt_shaders(void);
extern int render_prof_on;          /* LAA_TRACE_SYNC: time draws' parts into render_prof (draw.c) */
extern double render_prof[4];
/* The CPU wrote texels into a texture's guest memory (region in blocks: x, y, z, w, h, d). */
void render_texels_written(uint64_t texture, uint32_t level, uint32_t layer, const uint32_t region[6]);
/* The GPU works behind the CPU (submit.c): */
void render_flush(void);             /* submit what was recorded and wait until the GPU has done everything */
uint64_t render_fence_serial(void);  /* the GPU submission that finishes the work recorded so far */
int render_wait_serial(uint64_t serial, uint64_t timeout_ns);   /* 0: finished, -1: timeout */
void render_wait_done(uint64_t serial);   /* the same, from any thread, not counted as the game waiting */
uint64_t render_serial_completed(void);   /* the newest submission the GPU has finished */
/* Write guest memory in order with the GPU's work (a GPU write into its pool when it has one) */
void render_guest_write(uint64_t address, const void *data, uint32_t size);
int render_guest_timestamp(uint64_t address);   /* {0, GPU time in NVN ticks} there when the GPU gets there; 0 if recorded */
void render_timestamps_update(void);            /* store the timestamps of finished submissions */
/* The game made a memory pool, or is about to finalize one (buffer.c: its pages may become GPU memory) */
void render_pool_created(uint64_t storage, uint64_t size);
void render_pool_destroyed(uint64_t storage, uint64_t size);

#endif
