/*
 * astc.c - decode ASTC textures on the CPU, for GPUs that can't sample them.
 *
 * ASTC ("Adaptive Scalable Texture Compression") stores every block of texels (4x4 up to
 * 12x12) in 128 bits. Phone GPUs (like the Thor's Adreno) read it directly; most desktop GPUs
 * can't, so there we decode to RGBA8 once, when the texture is uploaded.
 *
 * A block holds, from bit 0 up:
 *   block mode (11 bits)  how many weights (a small grid stretched over the block), their
 *                         precision, one or two weight "planes"
 *   partitions (2 bits)   1-4 sets of texels, each with its own pair of colours (endpoints);
 *                         a 10-bit seed picks which texel belongs to which set
 *   endpoint modes        how each set's colour numbers are turned into two RGBA endpoints
 *   endpoint numbers      "integer sequence encoded" (below), growing upwards
 *   weights               also integer sequence encoded, growing DOWN from bit 127
 * Each texel's colour = mix(endpoint 0, endpoint 1, its weight / 64).
 *
 * "Integer sequence encoding" packs numbers in ranges like 0..5 or 0..19 with almost no wasted
 * bits: each number is (t * 2^n + low n bits), where t is a "trit" (0-2) or a "quint" (0-4);
 * 5 trits share 8 bits, 3 quints share 7 bits.
 *
 * Only the LDR profile (8-bit colours) is decoded; HDR blocks and invalid blocks come out
 * magenta, as the specification says. Specification: Khronos Data Format Specification 1.3,
 * chapter "ASTC Compressed Texture Image Formats". Checked against ARM's astcenc
 * (tests/test_astc.py).
 */
#include "astc.h"
#include <string.h>

/* ---- reading bits ---- */
typedef struct { const uint8_t *p; int pos, end; } Bits;   /* bit pos..end-1 readable; past end: zeros */

static uint32_t bits_read(Bits *b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++, b->pos++)
        if (b->pos < b->end) v |= (uint32_t)(b->p[b->pos >> 3] >> (b->pos & 7) & 1) << i;
    return v;
}

static uint32_t field(const uint8_t *blk, int at, int n) {    /* bits at..at+n-1 of the block */
    Bits b = { blk, at, 128 };
    return bits_read(&b, n);
}

/* ---- integer sequence encoding ---- */

/* The ranges a sequence can use, in the order block modes and endpoint precision index them. */
typedef struct { uint8_t trits, quints, bits; uint16_t levels; } Range;
static const Range ranges[] = {
    { 0, 0, 1, 2 },   { 1, 0, 0, 3 },   { 0, 0, 2, 4 },   { 0, 1, 0, 5 },   { 1, 0, 1, 6 },   { 0, 0, 3, 8 },
    { 0, 1, 1, 10 },  { 1, 0, 2, 12 },  { 0, 0, 4, 16 },  { 0, 1, 2, 20 },  { 1, 0, 3, 24 },  { 0, 0, 5, 32 },
    { 0, 1, 3, 40 },  { 1, 0, 4, 48 },  { 0, 0, 6, 64 },  { 0, 1, 4, 80 },  { 1, 0, 5, 96 },  { 0, 0, 7, 128 },
    { 0, 1, 5, 160 }, { 1, 0, 6, 192 }, { 0, 0, 8, 256 },
};
#define NRANGES (int)(sizeof ranges / sizeof *ranges)

static int sequence_bits(int count, const Range *r) {
    int bits = count * r->bits;
    if (r->trits) bits += (count * 8 + 4) / 5;
    if (r->quints) bits += (count * 7 + 2) / 3;
    return bits;
}

static uint8_t trit_table[256][5], quint_table[128][3];

static void build_tables(void) {
    for (int T = 0; T < 256; T++) {                 /* 8 bits -> 5 trits (spec: "Integer Sequence Encoding") */
        int C, t4, t3, t2, t1, t0;
        if ((T >> 2 & 7) == 7) { C = (T >> 5 & 7) << 2 | (T & 3); t4 = t3 = 2; }
        else {
            C = T & 31;
            if ((T >> 5 & 3) == 3) { t4 = 2; t3 = T >> 7 & 1; }
            else { t4 = T >> 7 & 1; t3 = T >> 5 & 3; }
        }
        if ((C & 3) == 3) { t2 = 2; t1 = C >> 4 & 1; t0 = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1); }
        else if ((C >> 2 & 3) == 3) { t2 = 2; t1 = 2; t0 = C & 3; }
        else { t2 = C >> 4 & 1; t1 = C >> 2 & 3; t0 = (C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1); }
        trit_table[T][0] = (uint8_t)t0; trit_table[T][1] = (uint8_t)t1; trit_table[T][2] = (uint8_t)t2;
        trit_table[T][3] = (uint8_t)t3; trit_table[T][4] = (uint8_t)t4;
    }
    for (int Q = 0; Q < 128; Q++) {                 /* 7 bits -> 3 quints */
        int C, q2, q1, q0;
        if ((Q >> 1 & 3) == 3 && (Q >> 5 & 3) == 0) {
            q2 = (Q & 1) << 2 | ((Q >> 4 & 1) & ~Q & 1) << 1 | ((Q >> 3 & 1) & ~Q & 1);
            q1 = q0 = 4;
        } else {
            if ((Q >> 1 & 3) == 3) { q2 = 4; C = (Q >> 3 & 3) << 3 | (~Q >> 5 & 3) << 1 | (Q & 1); }
            else { q2 = Q >> 5 & 3; C = Q & 31; }
            if ((C & 7) == 5) { q1 = 4; q0 = C >> 3 & 3; }
            else { q1 = C >> 3 & 3; q0 = C & 7; }
        }
        quint_table[Q][0] = (uint8_t)q0; quint_table[Q][1] = (uint8_t)q1; quint_table[Q][2] = (uint8_t)q2;
    }
}

/* Decode `count` numbers of range r from b. out[i] = (trit or quint) << n | low bits, and
 * low[i] = the low n bits alone; tq[i] = the trit/quint (unquantization needs both). */
static void sequence_decode(Bits *b, const Range *r, int count, uint8_t *tq, uint8_t *low) {
    int n = r->bits;
    for (int i = 0; i < count;) {
        if (r->trits) {                              /* 5 values: m0 T0-1 m1 T2-3 m2 T4 m3 T5-6 m4 T7 */
            static const int tbits[5] = { 2, 2, 1, 2, 1 };
            uint32_t T = 0, m[5];
            for (int k = 0, at = 0; k < 5; k++) { m[k] = bits_read(b, n); T |= bits_read(b, tbits[k]) << at; at += tbits[k]; }
            for (int k = 0; k < 5 && i < count; k++, i++) { low[i] = (uint8_t)m[k]; tq[i] = trit_table[T][k]; }
        } else if (r->quints) {                      /* 3 values: m0 Q0-2 m1 Q3-4 m2 Q5-6 */
            static const int qbits[3] = { 3, 2, 2 };
            uint32_t Q = 0, m[3];
            for (int k = 0, at = 0; k < 3; k++) { m[k] = bits_read(b, n); Q |= bits_read(b, qbits[k]) << at; at += qbits[k]; }
            for (int k = 0; k < 3 && i < count; k++, i++) { low[i] = (uint8_t)m[k]; tq[i] = quint_table[Q][k]; }
        } else {
            low[i] = (uint8_t)bits_read(b, n);
            tq[i++] = 0;
        }
    }
}

/* ---- unquantization: numbers in a range -> colour 0..255 / weight 0..64 ----
 * For trit/quint ranges the specification scrambles the bits (so that values round well):
 * T = D * C + B, where D is the trit/quint, B a pattern of the low bits, C a constant. */
static int unquant_color(const Range *r, int tq, int m) {
    int n = r->bits;
    if (!r->trits && !r->quints) {                   /* plain bits: repeat them to fill 8 bits */
        int v = 0;
        for (int s = 8 - n; s > -n; s -= n) v |= s >= 0 ? m << s : m >> -s;
        return v & 255;
    }
    int A = m & 1 ? 0x1FF : 0, b = m >> 1 & 1, c = m >> 2 & 1, d = m >> 3 & 1, e = m >> 4 & 1, f = m >> 5 & 1;
    int B = 0, C = 0;
    if (r->trits) {
        switch (n) {
        case 1: C = 204; break;
        case 2: C = 93;  B = b << 8 | b << 4 | b << 2 | b << 1; break;                               /* b000b0bb0 */
        case 3: C = 44;  B = c << 8 | b << 7 | c << 3 | b << 2 | c << 1 | b; break;                   /* cb000cbcb */
        case 4: C = 22;  B = d << 8 | c << 7 | b << 6 | d << 2 | c << 1 | b; break;                   /* dcb000dcb */
        case 5: C = 11;  B = e << 8 | d << 7 | c << 6 | b << 5 | e << 1 | d; break;                   /* edcb000ed */
        case 6: C = 5;   B = f << 8 | e << 7 | d << 6 | c << 5 | b << 4 | f; break;                   /* fedcb000f */
        }
    } else {
        switch (n) {
        case 1: C = 113; break;
        case 2: C = 54;  B = b << 8 | b << 3 | b << 2; break;                                        /* b0000bb00 */
        case 3: C = 26;  B = c << 8 | b << 7 | c << 2 | b << 1 | c; break;                           /* cb0000cbc */
        case 4: C = 13;  B = d << 8 | c << 7 | b << 6 | d << 1 | c; break;                           /* dcb0000dc */
        case 5: C = 6;   B = e << 8 | d << 7 | c << 6 | b << 5 | e; break;                           /* edcb0000e */
        }
    }
    int T = tq * C + B;
    T ^= A;
    return (A & 0x80) | T >> 2;
}

static int unquant_weight(const Range *r, int tq, int m) {
    int n = r->bits, v;
    if (!r->trits && !r->quints) {                   /* repeat the bits to fill 6 */
        v = 0;
        for (int s = 6 - n; s > -n; s -= n) v |= s >= 0 ? m << s : m >> -s;
        v &= 63;
    } else if (n == 0) {
        v = r->trits ? (int[]){ 0, 32, 63 }[tq] : (int[]){ 0, 16, 32, 47, 63 }[tq];
    } else {
        int A = m & 1 ? 0x7F : 0, b = m >> 1 & 1, c = m >> 2 & 1, B = 0, C = 0;
        if (r->trits) {
            if (n == 1) C = 50;
            else if (n == 2) { C = 23; B = b << 6 | b << 2 | b; }                    /* b000b0b */
            else { C = 11; B = c << 6 | b << 5 | c << 1 | b; }                       /* cb000cb */
        } else {
            if (n == 1) C = 28;
            else { C = 13; B = b << 6 | b << 1; }                                    /* b0000b0 */
        }
        int T = tq * C + B;
        T ^= A;
        v = (A & 0x20) | T >> 2;
    }
    return v > 32 ? v + 1 : v;
}

/* ---- block mode: the weight grid ---- */
typedef struct { int xw, yw, dual, range; } BlockMode;

static int block_mode(uint32_t mode, BlockMode *m) {     /* 0: reserved / invalid */
    uint32_t base = mode >> 4 & 1, H = mode >> 9 & 1, D = mode >> 10 & 1, A = mode >> 5 & 3, B;
    if (mode & 3) {
        base |= (mode & 3) << 1;
        B = mode >> 7 & 3;
        switch (mode >> 2 & 3) {
        case 0: m->xw = (int)B + 4; m->yw = (int)A + 2; break;
        case 1: m->xw = (int)B + 8; m->yw = (int)A + 2; break;
        case 2: m->xw = (int)A + 2; m->yw = (int)B + 8; break;
        default:
            B &= 1;
            if (mode & 0x100) { m->xw = (int)B + 2; m->yw = (int)A + 2; }
            else { m->xw = (int)A + 2; m->yw = (int)B + 6; }
        }
    } else {
        base |= (mode >> 2 & 3) << 1;
        if ((mode >> 2 & 3) == 0) return 0;
        B = mode >> 9 & 3;
        switch (mode >> 7 & 3) {
        case 0: m->xw = 12; m->yw = (int)A + 2; break;
        case 1: m->xw = (int)A + 2; m->yw = 12; break;
        case 2: m->xw = (int)A + 6; m->yw = (int)B + 6; D = 0; H = 0; break;
        default:
            if (A == 0) { m->xw = 6; m->yw = 10; }
            else if (A == 1) { m->xw = 10; m->yw = 6; }
            else return 0;
        }
    }
    m->dual = (int)D;
    m->range = (int)(base - 2 + 6 * H);              /* index into ranges[]: 2..32 levels */
    return m->range >= 0 && m->range < 12;
}

/* ---- partitions: which set a texel belongs to (the specification's hash) ---- */
static uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;
    p ^= p >> 5;  p += p << 16; p ^= p >> 7; p ^= p >> 3;
    p ^= p << 6;  p ^= p >> 17;
    return p;
}

static int partition_of(int seed, int x, int y, int count, int small) {
    if (small) { x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    uint32_t r = hash52((uint32_t)seed);
    int s[12] = { (int)(r & 15), (int)(r >> 4 & 15), (int)(r >> 8 & 15), (int)(r >> 12 & 15), (int)(r >> 16 & 15),
                  (int)(r >> 20 & 15), (int)(r >> 24 & 15), (int)(r >> 28 & 15), (int)(r >> 18 & 15), (int)(r >> 22 & 15),
                  (int)(r >> 26 & 15), (int)((r >> 30 | r << 2) & 15) };
    for (int i = 0; i < 12; i++) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    int sh3 = seed & 0x10 ? sh1 : sh2;
    for (int i = 0; i < 8; i++) s[i] >>= i & 1 ? sh2 : sh1;
    for (int i = 8; i < 12; i++) s[i] >>= sh3;
    int a = (s[0] * x + s[1] * y + (int)(r >> 14)) & 63;     /* (z = 0: 2D textures only) */
    int b = (s[2] * x + s[3] * y + (int)(r >> 10)) & 63;
    int c = (s[4] * x + s[5] * y + (int)(r >> 6)) & 63;
    int d = (s[6] * x + s[7] * y + (int)(r >> 2)) & 63;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    return c >= d ? 2 : 3;
}

/* ---- endpoints: colour numbers -> two RGBA colours (LDR modes) ---- */
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void bit_transfer_signed(int *a, int *b) {        /* a: offset (signed 6 bits), b: base */
    *b = *b >> 1 | (*a & 0x80);
    *a = *a >> 1 & 0x3F;
    if (*a & 0x20) *a -= 0x40;
}

static void blue_contract(int e[4]) { e[0] = (e[0] + e[2]) >> 1; e[1] = (e[1] + e[2]) >> 1; }

static void set4(int e[4], int r, int g, int b, int a) { e[0] = r; e[1] = g; e[2] = b; e[3] = a; }

static void clamp4(int e[4]) { for (int i = 0; i < 4; i++) e[i] = clamp255(e[i]); }

static int endpoints(int mode, const int *v, int e0[4], int e1[4]) {     /* 0: HDR (not decoded) */
    int a = v[0], b = v[1], c = v[2], d = v[3], f = v[4], g = v[5], h = v[6], i = v[7];
    switch (mode) {
    case 0: set4(e0, a, a, a, 255); set4(e1, b, b, b, 255); return 1;
    case 1: {
        int l0 = a >> 2 | (b & 0xC0), l1 = l0 + (b & 0x3F);
        if (l1 > 255) l1 = 255;
        set4(e0, l0, l0, l0, 255); set4(e1, l1, l1, l1, 255);
        return 1;
    }
    case 4: set4(e0, a, a, a, c); set4(e1, b, b, b, d); return 1;
    case 5:
        bit_transfer_signed(&b, &a); bit_transfer_signed(&d, &c);
        set4(e0, a, a, a, c); set4(e1, clamp255(a + b), clamp255(a + b), clamp255(a + b), clamp255(c + d));
        return 1;
    case 6: set4(e0, a * d >> 8, b * d >> 8, c * d >> 8, 255); set4(e1, a, b, c, 255); return 1;
    case 8:
        if (b + d + g >= a + c + f) { set4(e0, a, c, f, 255); set4(e1, b, d, g, 255); }
        else { set4(e0, b, d, g, 255); set4(e1, a, c, f, 255); blue_contract(e0); blue_contract(e1); }
        return 1;
    case 9:                                          /* (sums are clamped after blue contraction) */
        bit_transfer_signed(&b, &a); bit_transfer_signed(&d, &c); bit_transfer_signed(&g, &f);
        if (b + d + g >= 0) { set4(e0, a, c, f, 255); set4(e1, a + b, c + d, f + g, 255); }
        else { set4(e0, a + b, c + d, f + g, 255); set4(e1, a, c, f, 255); blue_contract(e0); blue_contract(e1); }
        clamp4(e0); clamp4(e1);
        return 1;
    case 10: set4(e0, a * d >> 8, b * d >> 8, c * d >> 8, f); set4(e1, a, b, c, g); return 1;
    case 12:
        if (b + d + g >= a + c + f) { set4(e0, a, c, f, h); set4(e1, b, d, g, i); }
        else { set4(e0, b, d, g, i); set4(e1, a, c, f, h); blue_contract(e0); blue_contract(e1); }
        return 1;
    case 13:
        bit_transfer_signed(&b, &a); bit_transfer_signed(&d, &c); bit_transfer_signed(&g, &f); bit_transfer_signed(&i, &h);
        if (b + d + g >= 0) { set4(e0, a, c, f, h); set4(e1, a + b, c + d, f + g, h + i); }
        else { set4(e0, a + b, c + d, f + g, h + i); set4(e1, a, c, f, h); blue_contract(e0); blue_contract(e1); }
        clamp4(e0); clamp4(e1);
        return 1;
    default:
        return 0;                                    /* 2, 3, 7, 11, 14, 15: HDR */
    }
}

/* ---- one block ---- */
static void fill(uint8_t *out, int bw, int bh, int stride, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            uint8_t *p = out + y * stride + x * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = a;
        }
}
#ifdef ASTC_DEBUG
#include <stdio.h>
#define ERROR_COLOR(out, bw, bh, stride) (fprintf(stderr, "astc: error at line %d\n", __LINE__), fill(out, bw, bh, stride, 255, 0, 255, 255))
#else
#define ERROR_COLOR(out, bw, bh, stride) fill(out, bw, bh, stride, 255, 0, 255, 255)
#endif

void astc_decode_block(const uint8_t blk[16], int bw, int bh, int srgb, uint8_t *out, int stride) {
    static int tables_built;
    if (!tables_built) { build_tables(); tables_built = 1; }

    uint32_t mode = field(blk, 0, 11);
    if ((mode & 0x1FF) == 0x1FC) {                  /* void extent: one colour for the whole block */
        /* bit 9: HDR; bits 10-11 must be 1; then the area it covers (s low/high, t low/high,
         * 13 bits each), which must be all ones or a real range */
        uint32_t sl = field(blk, 12, 13), sh = field(blk, 25, 13), tl = field(blk, 38, 13), th = field(blk, 51, 13);
        int all_ones = sl == 0x1FFF && sh == 0x1FFF && tl == 0x1FFF && th == 0x1FFF;
        if ((mode & 0x200) || field(blk, 10, 2) != 3 || (!all_ones && (sl >= sh || tl >= th))) {
            ERROR_COLOR(out, bw, bh, stride);
            return;
        }
        fill(out, bw, bh, stride, blk[9], blk[11], blk[13], blk[15]);       /* top bytes of 4 x u16 at bit 64 */
        return;
    }
    BlockMode m;
    if (!block_mode(mode, &m) || m.xw > bw || m.yw > bh) { ERROR_COLOR(out, bw, bh, stride); return; }
    int partitions = (int)field(blk, 11, 2) + 1;
    int nweights = m.xw * m.yw * (m.dual + 1);
    const Range *wr = &ranges[m.range];
    int weight_bits = sequence_bits(nweights, wr);
    if (nweights > 64 || weight_bits < 24 || weight_bits > 96 || (partitions == 4 && m.dual)) {
        ERROR_COLOR(out, bw, bh, stride);
        return;
    }

    /* endpoint modes */
    int cem[4], seed = 0, color_at, below = 128 - weight_bits;   /* `below`: where the weights start */
    if (partitions == 1) {
        cem[0] = (int)field(blk, 13, 4);
        color_at = 17;
    } else {
        seed = (int)field(blk, 13, 10);
        uint32_t enc = field(blk, 23, 6);
        color_at = 29;
        if ((enc & 3) == 0) {
            for (int i = 0; i < partitions; i++) cem[i] = (int)(enc >> 2);
        } else {                                     /* per-partition modes, partly stored below the weights */
            int extra = 3 * partitions - 4;
            below -= extra;
            uint32_t bits = enc >> 2 | field(blk, below, extra) << 4;
            int base = (int)(enc & 3) - 1;
            for (int i = 0; i < partitions; i++) {
                int c = (int)(bits >> i & 1), mm = (int)(bits >> (partitions + 2 * i) & 3);
                cem[i] = (base + c) << 2 | mm;
            }
        }
    }
    int plane2_component = -1;
    if (m.dual) { below -= 2; plane2_component = (int)field(blk, below, 2); }

    /* endpoint numbers: as precise as the bits left allow */
    int nvalues = 0;
    for (int i = 0; i < partitions; i++) nvalues += 2 * ((cem[i] >> 2) + 1);
    int color_bits = below - color_at, cr = -1;
    if (nvalues > 18) { ERROR_COLOR(out, bw, bh, stride); return; }
    for (int r = NRANGES - 1; r >= 0; r--)
        if (sequence_bits(nvalues, &ranges[r]) <= color_bits) { cr = r; break; }
    if (cr < 4) { ERROR_COLOR(out, bw, bh, stride); return; }  /* (the smallest legal range has 6 levels) */
    uint8_t tq[18], low[18];
    Bits cb = { blk, color_at, color_at + sequence_bits(nvalues, &ranges[cr]) };   /* (the last group's missing bits are 0) */
    sequence_decode(&cb, &ranges[cr], nvalues, tq, low);
    int ep[4][2][4];
    for (int i = 0, k = 0; i < partitions; i++) {
        int v[8] = { 0 }, n = 2 * ((cem[i] >> 2) + 1);
        for (int j = 0; j < n; j++, k++) v[j] = unquant_color(&ranges[cr], tq[k], low[k]);
        if (!endpoints(cem[i], v, ep[i][0], ep[i][1])) { ERROR_COLOR(out, bw, bh, stride); return; }
    }

    /* weights: stored bit-reversed from the top of the block */
    uint8_t rev[16];
    for (int i = 0; i < 16; i++) {
        uint8_t x = blk[15 - i];
        x = (uint8_t)((x * 0x0202020202ull & 0x010884422010ull) % 1023);    /* reverse the byte's bits */
        rev[i] = x;
    }
    uint8_t wtq[64], wlow[64];
    int w[64 + 16] = { 0 };                          /* (+16: the infill reads one past the grid's edge) */
    Bits wb = { rev, 0, weight_bits };
    sequence_decode(&wb, wr, nweights, wtq, wlow);
    for (int i = 0; i < nweights; i++) w[i] = unquant_weight(wr, wtq[i], wlow[i]);

    /* each texel: its weights (infilled from the grid), its partition, the mix */
    int ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1), planes = m.dual + 1;
    int small = bw * bh < 31;
    for (int t = 0; t < bh; t++)
        for (int s = 0; s < bw; s++) {
            int gs = (ds * s * (m.xw - 1) + 32) >> 6, gt = (dt * t * (m.yw - 1) + 32) >> 6;
            int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            int v0 = js + jt * m.xw;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int wt[2];
            for (int p = 0; p < planes; p++) {
                int p00 = w[v0 * planes + p], p01 = w[(v0 + 1) * planes + p];
                int p10 = w[(v0 + m.xw) * planes + p], p11 = w[(v0 + m.xw + 1) * planes + p];
                wt[p] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
            int part = partitions > 1 ? partition_of(seed, s, t, partitions, small) : 0;
            uint8_t *px = out + t * stride + s * 4;
            for (int ch = 0; ch < 4; ch++) {
                int weight = ch == plane2_component ? wt[1] : wt[0];
                int e0 = ep[part][0][ch], e1 = ep[part][1][ch];
                int c0 = srgb && ch < 3 ? e0 << 8 | 0x80 : e0 << 8 | e0;    /* 8 -> 16 bits */
                int c1 = srgb && ch < 3 ? e1 << 8 | 0x80 : e1 << 8 | e1;
                int c = (c0 * (64 - weight) + c1 * weight + 32) >> 6;
                px[ch] = (uint8_t)(c >> 8);
            }
        }
}

void astc_decode(const uint8_t *blocks, size_t row_stride, uint32_t width, uint32_t height, int bw, int bh, int srgb,
                 uint8_t *rgba, size_t out_stride) {
    uint8_t tmp[12 * 12 * 4];
    for (uint32_t by = 0; by * bh < height; by++)
        for (uint32_t bx = 0; bx * bw < width; bx++) {
            astc_decode_block(blocks + by * row_stride + bx * 16, bw, bh, srgb, tmp, bw * 4);
            uint32_t w = width - bx * bw < (uint32_t)bw ? width - bx * bw : (uint32_t)bw;
            uint32_t h = height - by * bh < (uint32_t)bh ? height - by * bh : (uint32_t)bh;
            for (uint32_t y = 0; y < h; y++)
                memcpy(rgba + (by * bh + y) * out_stride + bx * bw * 4, tmp + y * bw * 4, w * 4);
        }
}
