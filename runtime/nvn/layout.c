/*
 * layout.c - NVN texture formats, and how the Tegra X1 lays textures out in memory.
 *
 * In C, a 2D array `pixel[y][x]` is stored row after row ("pitch linear"). The Switch GPU
 * mostly stores textures "block-linear" instead, which keeps pixels that are close in 2D close
 * in memory too:
 *   - A GOB ("group of bytes") is a tile 64 bytes wide and 8 rows tall: 512 bytes.
 *   - A block is 2^block_height GOBs stacked vertically (and 2^block_depth deep, for 3D).
 *   - The surface is a grid of blocks: left to right, then top to bottom.
 * Inside a GOB the 512 bytes are also shuffled, in 16-byte pieces (see gob_offset()).
 *
 * The game allocates texture memory itself, using the size nvnTextureBuilderGetStorageSize
 * reports, and textures packaged in its files (BNTX) are already in this layout. So these sizes
 * must match what the real NVN driver computes.
 */
#include "nvn.h"

/* ---- formats (NVNformat values) ---- */

#define COL(n)       { n, 1, 1, FORMAT_COLOR }
#define BLK(n, w, h) { n, w, h, FORMAT_COMPRESSED }
static const FormatInfo formats[] = {
    [0x01] = COL(1), [0x02] = COL(1), [0x03] = COL(1), [0x04] = COL(1),                     /* R8 */
    [0x05] = COL(2), [0x06] = COL(2), [0x07] = COL(2), [0x08] = COL(2), [0x09] = COL(2),    /* R16 */
    [0x0A] = COL(4), [0x0B] = COL(4), [0x0C] = COL(4),                                      /* R32 */
    [0x0D] = COL(2), [0x0E] = COL(2), [0x0F] = COL(2), [0x10] = COL(2),                     /* RG8 */
    [0x11] = COL(4), [0x12] = COL(4), [0x13] = COL(4), [0x14] = COL(4), [0x15] = COL(4),    /* RG16 */
    [0x16] = COL(8), [0x17] = COL(8), [0x18] = COL(8),                                      /* RG32 */
    [0x19] = COL(3), [0x1A] = COL(3), [0x1B] = COL(3), [0x1C] = COL(3),                     /* RGB8 */
    [0x1D] = COL(6), [0x1E] = COL(6), [0x1F] = COL(6), [0x20] = COL(6), [0x21] = COL(6),    /* RGB16 */
    [0x22] = COL(12), [0x23] = COL(12), [0x24] = COL(12),                                   /* RGB32 */
    [0x25] = COL(4), [0x26] = COL(4), [0x27] = COL(4), [0x28] = COL(4),                     /* RGBA8 */
    [0x29] = COL(8), [0x2A] = COL(8), [0x2B] = COL(8), [0x2C] = COL(8), [0x2D] = COL(8),    /* RGBA16 */
    [0x2E] = COL(16), [0x2F] = COL(16), [0x30] = COL(16),                                   /* RGBA32 */
    [0x31] = { 1, 1, 1, FORMAT_STENCIL },                                                   /* STENCIL8 */
    [0x32] = { 2, 1, 1, FORMAT_DEPTH },                                                     /* DEPTH16 */
    [0x33] = { 4, 1, 1, FORMAT_DEPTH }, [0x34] = { 4, 1, 1, FORMAT_DEPTH },                 /* DEPTH24, DEPTH32F */
    [0x35] = { 4, 1, 1, FORMAT_DEPTH_STENCIL },                                             /* DEPTH24_STENCIL8 */
    [0x36] = { 8, 1, 1, FORMAT_DEPTH_STENCIL },                                             /* DEPTH32F_STENCIL8 */
    [0x37] = COL(4), [0x38] = COL(4),                                                       /* RGBX8 / RGBA8 sRGB */
    [0x39] = COL(2), [0x3A] = COL(2), [0x3B] = COL(2), [0x3C] = COL(2),                     /* RGBA4, RGB5, RGB5A1, RGB565 */
    [0x3D] = COL(4), [0x3E] = COL(4), [0x3F] = COL(4), [0x40] = COL(4),                     /* RGB10A2(UI), R11G11B10F, RGB9E5F */
    [0x41] = BLK(8, 4, 4), [0x42] = BLK(8, 4, 4), [0x43] = BLK(16, 4, 4), [0x44] = BLK(16, 4, 4), /* BC1-3 */
    [0x45] = BLK(8, 4, 4), [0x46] = BLK(8, 4, 4), [0x47] = BLK(16, 4, 4), [0x48] = BLK(16, 4, 4), /* ... sRGB */
    [0x49] = BLK(8, 4, 4), [0x4A] = BLK(8, 4, 4), [0x4B] = BLK(16, 4, 4), [0x4C] = BLK(16, 4, 4), /* BC4, BC5 */
    [0x4D] = BLK(16, 4, 4), [0x4E] = BLK(16, 4, 4), [0x4F] = BLK(16, 4, 4), [0x50] = BLK(16, 4, 4), /* BC6H, BC7 */
    /* vertex-only formats (integers read as floats) */
    [0x51] = COL(1), [0x52] = COL(1), [0x53] = COL(2), [0x54] = COL(2), [0x55] = COL(4), [0x56] = COL(4),
    [0x57] = COL(2), [0x58] = COL(2), [0x59] = COL(4), [0x5A] = COL(4), [0x5B] = COL(8), [0x5C] = COL(8),
    [0x5D] = COL(3), [0x5E] = COL(3), [0x5F] = COL(6), [0x60] = COL(6), [0x61] = COL(12), [0x62] = COL(12),
    [0x63] = COL(4), [0x64] = COL(4), [0x65] = COL(8), [0x66] = COL(8), [0x67] = COL(16), [0x68] = COL(16),
    [0x69] = COL(4), [0x6A] = COL(4), [0x6B] = COL(4), [0x6C] = COL(4),
    [0x6D] = COL(4), [0x6E] = COL(4), [0x6F] = COL(4), [0x70] = COL(4),                     /* RGBX8 */
    [0x71] = COL(8), [0x72] = COL(8), [0x73] = COL(8), [0x74] = COL(8), [0x75] = COL(8),    /* RGBX16 */
    [0x76] = COL(16), [0x77] = COL(16), [0x78] = COL(16),                                   /* RGBX32 */
    /* ASTC: block sizes 4x4 ... 12x12, then the same again in sRGB */
    [0x79] = BLK(16, 4, 4), [0x7A] = BLK(16, 5, 4), [0x7B] = BLK(16, 5, 5), [0x7C] = BLK(16, 6, 5),
    [0x7D] = BLK(16, 6, 6), [0x7E] = BLK(16, 8, 5), [0x7F] = BLK(16, 8, 6), [0x80] = BLK(16, 8, 8),
    [0x81] = BLK(16, 10, 5), [0x82] = BLK(16, 10, 6), [0x83] = BLK(16, 10, 8), [0x84] = BLK(16, 10, 10),
    [0x85] = BLK(16, 12, 10), [0x86] = BLK(16, 12, 12),
    [0x87] = BLK(16, 4, 4), [0x88] = BLK(16, 5, 4), [0x89] = BLK(16, 5, 5), [0x8A] = BLK(16, 6, 5),
    [0x8B] = BLK(16, 6, 6), [0x8C] = BLK(16, 8, 5), [0x8D] = BLK(16, 8, 6), [0x8E] = BLK(16, 8, 8),
    [0x8F] = BLK(16, 10, 5), [0x90] = BLK(16, 10, 6), [0x91] = BLK(16, 10, 8), [0x92] = BLK(16, 10, 10),
    [0x93] = BLK(16, 12, 10), [0x94] = BLK(16, 12, 12),
    [0x95] = COL(2), [0x96] = COL(2), [0x97] = COL(2),                                      /* BGR5, BGR5A1, A1BGR5 */
    [0x98] = COL(4), [0x99] = COL(4), [0x9A] = COL(4), [0x9B] = COL(4),                     /* BGRX8, BGRA8 (+ sRGB) */
};

const FormatInfo *nvn_format(uint32_t format) {
    static const FormatInfo unknown = COL(4);
    if (format >= sizeof formats / sizeof *formats || !formats[format].bytes) return &unknown;
    return &formats[format];
}

/* ---- layout ---- */

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint32_t ceil_log2(uint32_t v) { uint32_t n = 0; while ((1u << n) < v) n++; return n; }

void tex_shape(const TexDesc *d, TexShape *s) {
    s->fmt = nvn_format(d->format);
    s->width = d->width ? d->width : 1;
    s->height = d->height ? d->height : 1;
    s->depth = 1;
    s->layers = 1;
    s->levels = d->levels ? d->levels : 1;
    switch (d->target) {
    case TARGET_1D:             s->height = 1; break;
    case TARGET_3D:             s->depth = d->depth ? d->depth : 1; break;
    case TARGET_1D_ARRAY:       s->layers = s->height; s->height = 1; break;
    case TARGET_2D_ARRAY:
    case TARGET_2D_MS_ARRAY:
    case TARGET_CUBE_ARRAY:     s->layers = d->depth ? d->depth : 1; break;
    case TARGET_CUBE:           s->layers = 6; break;
    }
    /* multisampled textures store the samples as a bigger image: 2 = 2x1, 4 = 2x2, 8 = 4x2 */
    if (d->target == TARGET_2D_MS || d->target == TARGET_2D_MS_ARRAY) {
        uint32_t n = d->samples;
        s->width *= n >= 8 ? 4 : n >= 2 ? 2 : 1;
        s->height *= n >= 4 ? 2 : 1;
    }
    /* Default block height: tall enough for the texture, at most 16 GOBs (128 rows). */
    uint32_t hb = div_up(s->height, s->fmt->bh);
    s->block_height_log2 = ceil_log2(div_up(hb, 8));
    if (s->block_height_log2 > 4) s->block_height_log2 = 4;
    if (d->block_height_log2 != ~0u) s->block_height_log2 = d->block_height_log2;   /* from a texture file */
    s->block_depth_log2 = ceil_log2(s->depth);
    if (s->block_depth_log2 > 5) s->block_depth_log2 = 5;
}

/* Smaller mip levels use smaller blocks, so a block is never much taller than the level. */
LevelShape tex_level(const TexShape *s, uint32_t level) {
    LevelShape l = { 0 };
    for (uint32_t i = 0; i <= level; i++) {
        uint32_t w = s->width >> i, h = s->height >> i, d = s->depth >> i;
        l.width = w ? w : 1;
        l.height = h ? h : 1;
        l.depth = d ? d : 1;
        l.wb = div_up(l.width, s->fmt->bw);
        l.hb = div_up(l.height, s->fmt->bh);
        l.bh = s->block_height_log2;
        while (l.bh > 0 && (8u << (l.bh - 1)) >= l.hb) l.bh--;
        l.bd = s->block_depth_log2;
        while (l.bd > 0 && (1u << (l.bd - 1)) >= l.depth) l.bd--;
        uint64_t width_gobs = div_up(l.wb * s->fmt->bytes, 64);
        uint64_t block_rows = div_up(l.hb, 8u << l.bh), block_slices = div_up(l.depth, 1u << l.bd);
        l.size = width_gobs * block_rows * block_slices * (512ull << (l.bh + l.bd));
        if (i < level) l.offset += l.size;
    }
    return l;
}

uint64_t tex_layer_size(const TexShape *s) {
    LevelShape last = tex_level(s, s->levels - 1);
    uint64_t size = last.offset + last.size;
    if (s->layers > 1) {                                  /* layers start on a level-0 block */
        uint64_t block = 512ull << (s->block_height_log2 + s->block_depth_log2);
        size = (size + block - 1) / block * block;
    }
    return size;
}

uint64_t texture_storage_size(const TexDesc *d) {
    const FormatInfo *f = nvn_format(d->format);
    if (d->target == TARGET_BUFFER) return (uint64_t)d->width * f->bytes;
    if (d->flags & TEXTURE_FLAG_LINEAR) return d->stride * div_up(d->height ? d->height : 1, f->bh);
    TexShape s;
    tex_shape(d, &s);
    return tex_layer_size(&s) * s.layers;
}

uint32_t texture_storage_alignment(const TexDesc *d) {
    return d->target == TARGET_BUFFER || (d->flags & TEXTURE_FLAG_LINEAR) ? 32 : 512;
}

/* Byte offset of (x bytes, y rows, z slices) in a block-linear surface whose rows are
 * width_gobs GOBs wide and whose block grid has block_rows rows per slice of blocks. */
static uint64_t gob_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t block_rows,
                           uint32_t bh, uint32_t bd) {
    uint32_t gob_y = y >> 3;
    uint64_t block_size = 512ull << (bh + bd);
    uint64_t block = ((uint64_t)(z >> bd) * block_rows + (gob_y >> bh)) * width_gobs + (x >> 6);
    uint64_t gob = ((uint64_t)(z & ((1u << bd) - 1)) << bh) | (gob_y & ((1u << bh) - 1));
    /* inside the GOB: 16-byte pieces in this order (from the Tegra X1 TRM / envytools) */
    uint32_t xi = x & 63, yi = y & 7;
    uint32_t in_gob = ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 15);
    return block * block_size + gob * 512 + in_gob;
}

/* Copy one level between block-linear `tiled` and a linear buffer with the given row and slice
 * pitch. Region in blocks: x/y/z offset and width/height/depth. */
void tex_copy_level(uint8_t *tiled, uint8_t *linear, const TexShape *s, const LevelShape *l,
                    const uint32_t region[6], uint64_t row_pitch, uint64_t slice_pitch, int to_linear) {
    uint32_t bpb = s->fmt->bytes;
    uint32_t width_gobs = div_up(l->wb * bpb, 64);
    uint32_t block_rows = div_up(l->hb, 8u << l->bh);
    for (uint32_t z = 0; z < region[5]; z++)
        for (uint32_t y = 0; y < region[4]; y++) {
            uint8_t *row = linear + z * slice_pitch + y * row_pitch;
            /* a texel of 3, 6 or 12 bytes can straddle two 16-byte pieces: go byte by byte */
            uint32_t unit = (bpb & (bpb - 1)) ? 1 : bpb, n = region[3] * bpb / unit;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t o = gob_offset(region[0] * bpb + i * unit, region[1] + y, region[2] + z, width_gobs,
                                        block_rows, l->bh, l->bd);
                if (to_linear) memcpy(row + i * unit, tiled + o, unit);
                else memcpy(tiled + o, row + i * unit, unit);
            }
        }
}
