/*
 * astc.h - ASTC texture decoding on the CPU (LDR), for GPUs without ASTC support.
 */
#ifndef ASTC_H
#define ASTC_H

#include <stddef.h>
#include <stdint.h>

/* One 16-byte block of bw x bh texels -> RGBA8 at out (rows `stride` bytes apart). srgb: the
 * texture is an sRGB format (the result is still sRGB-encoded; the GPU linearizes it). */
void astc_decode_block(const uint8_t blk[16], int bw, int bh, int srgb, uint8_t *out, int stride);

/* A whole image: rows of blocks `row_stride` bytes apart -> width x height RGBA8. */
void astc_decode(const uint8_t *blocks, size_t row_stride, uint32_t width, uint32_t height, int bw, int bh, int srgb,
                 uint8_t *rgba, size_t out_stride);

#endif
