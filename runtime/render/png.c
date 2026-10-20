/*
 * png.c - write an RGBA image as a PNG file (for screenshots while debugging).
 *
 * A PNG is a signature and "chunks" (length, type, data, CRC). The pixels go in IDAT chunks,
 * zlib-compressed; we use zlib's "stored" blocks, which don't compress at all, so no
 * compression library is needed: the file is just as big as the pixels.
 */
#include "png.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t head[8];
    put32(head, len);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    fwrite(data, 1, len, f);
    uint8_t c[4];
    put32(c, crc(crc(0, head + 4, 4), data, len));
    fwrite(c, 1, 4, f);
}

int png_write(const char *path, const uint8_t *rgba, uint32_t width, uint32_t height, int32_t stride) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, width);
    put32(ihdr + 4, height);
    ihdr[8] = 8;                                     /* bits per channel */
    ihdr[9] = 6;                                     /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;              /* deflate, standard filters, no interlace */
    chunk(f, "IHDR", ihdr, 13);

    /* Raw data: each row is a filter byte (0 = none) and the pixels. zlib stream: 2-byte
     * header, stored blocks of up to 65535 bytes (5-byte header each), Adler-32 checksum. */
    size_t row = (size_t)width * 4 + 1, raw = row * height;
    size_t blocks = (raw + 65534) / 65535;
    size_t total = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(total), *o = z;
    if (!z) { fclose(f); return -1; }
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    size_t done = 0;
    uint32_t y = 0, x = 0;                           /* position in the raw data: row y, byte x */
    while (done < raw) {
        uint32_t n = raw - done > 65535 ? 65535 : (uint32_t)(raw - done);
        *o++ = done + n == raw;                      /* "last block" flag, stored type */
        *o++ = n & 0xFF; *o++ = n >> 8; *o++ = ~n & 0xFF; *o++ = (~n >> 8) & 0xFF;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t v = x == 0 ? 0 : rgba[(ptrdiff_t)y * stride + x - 1];
            *o++ = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            if (++x == row) { x = 0; y++; }
        }
        done += n;
    }
    put32(o, b << 16 | a);
    chunk(f, "IDAT", z, (uint32_t)total);
    free(z);
    chunk(f, "IEND", NULL, 0);
    return fclose(f);
}
