/*
 * png.h - minimal PNG writer (uncompressed), for screenshots.
 */
#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/* rgba: height rows of width pixels, `stride` bytes apart (negative: rows go upwards). 0 on success. */
int png_write(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height, int32_t stride);

#endif
