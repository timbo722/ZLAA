/* Conversions between floats and integers, half precision, and ARM-specific min/max.
 *
 * Some ARM instructions have no defined C equivalent: FCVTZS saturates out-of-range values
 * (a C cast is undefined there), FMIN/FMAX have their own NaN and -0.0 rules, and half
 * precision has no portable C type. For these, the ARM64 build runs the exact instruction
 * with inline assembly, and the native build runs a C reference written to the Arm manual's
 * definition. The test checks that the recompiled instruction matches the reference. */
#include <stdint.h>
#include "testlib.h"

#ifdef __aarch64__
/* ---------------- ARM64: the real instructions ---------------- */
#define CVT(name, insn, OT, ow, IT, iw) \
    static OT name(IT x) { OT r; __asm__(insn " %" ow "0, %" iw "1" : "=r"(r) : "w"(x)); return r; }
#define BIN(name, insn, T, w) \
    static T name(T x, T y) { T r; __asm__(insn " %" w "0, %" w "1, %" w "2" : "=w"(r) : "w"(x), "w"(y)); return r; }

static uint16_t f2h(float x) { uint32_t r; __asm__("fcvt h16, %s1\n\tfmov %w0, s16" : "=r"(r) : "w"(x) : "v16"); return (uint16_t)r; }
static uint16_t d2h(double x) { uint32_t r; __asm__("fcvt h16, %d1\n\tfmov %w0, s16" : "=r"(r) : "w"(x) : "v16"); return (uint16_t)r; }
static float h2f(uint16_t h) { float r; __asm__("fmov s16, %w1\n\tfcvt %s0, h16" : "=w"(r) : "r"((uint32_t)h) : "v16"); return r; }
static double h2d(uint16_t h) { double r; __asm__("fmov s16, %w1\n\tfcvt %d0, h16" : "=w"(r) : "r"((uint32_t)h) : "v16"); return r; }
static uint64_t vec_hi(uint64_t lo, uint64_t hi) {   /* FMOV Vd.D[1], Xn and back */
    uint64_t r;
    __asm__("fmov d16, %1\n\tfmov v16.d[1], %2\n\tfmov %0, v16.d[1]" : "=r"(r) : "r"(lo), "r"(hi) : "v16");
    return r ^ lo;
}

#else
/* ---------------- native: reference implementations from the Arm manual ---------------- */
#include <math.h>
static uint64_t ref_cvt(double x, int mode, int is_signed, int bits) {
    if (x != x) return 0;                                      /* NaN -> 0 */
    double r = mode == 0 ? rint(x) : mode == 1 ? ceil(x) : mode == 2 ? floor(x)
             : mode == 3 ? trunc(x) : round(x);
    uint64_t umax = bits == 64 ? ~0ull : (1ull << bits) - 1;
    if (is_signed) {
        double hi = ldexp(1.0, bits - 1);
        if (r >= hi) return umax >> 1;                         /* saturate to INT_MAX */
        if (r < -hi) return (umax >> 1) + 1;                   /* ...and INT_MIN */
        return (uint64_t)(int64_t)r & umax;
    }
    if (r >= ldexp(1.0, bits)) return umax;
    if (r < 0) return 0;
    return (uint64_t)r;
}
#define CVT(name, insn, OT, ow, IT, iw) static OT name(IT x) { return (OT)ref_cvt(x, MODE_##name, SIGNED_##name, sizeof(OT) * 8); }
#define BIN(name, insn, T, w) static T name(T x, T y) { return ref_##name(x, y); }

static int isq(double v) { uint64_t u; __builtin_memcpy(&u, &v, 8); return v != v && (u & (1ull << 51)); }
static double mm(double a, double b, int is_max, int nm, int a_qnan, int b_qnan) {
    int an = a != a, bn = b != b;
    if (nm && a_qnan && !bn) return b;                         /* FMAXNM/FMINNM: quiet NaN loses */
    if (nm && b_qnan && !an) return a;
    if (an || bn) return NAN;
    if (a == 0 && b == 0) return (signbit(a) == is_max) ? b : a;   /* -0 < +0 */
    return is_max ? (a > b ? a : b) : (a < b ? a : b);
}
static int fq(float v) { uint32_t u; __builtin_memcpy(&u, &v, 4); return v != v && (u & (1u << 22)); }
static float ref_fmin_s(float a, float b)   { return (float)mm(a, b, 0, 0, fq(a), fq(b)); }
static float ref_fmax_s(float a, float b)   { return (float)mm(a, b, 1, 0, fq(a), fq(b)); }
static float ref_fminnm_s(float a, float b) { return (float)mm(a, b, 0, 1, fq(a), fq(b)); }
static float ref_fmaxnm_s(float a, float b) { return (float)mm(a, b, 1, 1, fq(a), fq(b)); }
static double ref_fmin_d(double a, double b)   { return mm(a, b, 0, 0, isq(a), isq(b)); }
static double ref_fmax_d(double a, double b)   { return mm(a, b, 1, 0, isq(a), isq(b)); }
static double ref_fminnm_d(double a, double b) { return mm(a, b, 0, 1, isq(a), isq(b)); }
static double ref_fmaxnm_d(double a, double b) { return mm(a, b, 1, 1, isq(a), isq(b)); }

/* half precision reference: integer bit manipulation, round to nearest even */
static uint16_t f2h(float f) {
    uint32_t u; __builtin_memcpy(&u, &f, 4);
    uint32_t sign = (u >> 16) & 0x8000, exp = (u >> 23) & 0xFF, man = u & 0x7FFFFF;
    if (exp == 0xFF) return (uint16_t)(sign | 0x7C00 | (man ? 0x200 | (man >> 13) : 0));
    int e = (int)exp - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    uint32_t m, shift;
    if (e <= 0) {                     /* half subnormal: unit is 2^-24 */
        if (exp == 0) return (uint16_t)sign;
        shift = 126 - exp;
        if (shift > 24) return (uint16_t)sign;
        m = man | 0x800000;
    } else {
        shift = 13;
        m = man;
    }
    uint32_t q = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    if (e <= 0) return (uint16_t)(sign | q);
    if (q == 0x400) { q = 0; e++; if (e >= 31) return (uint16_t)(sign | 0x7C00); }
    return (uint16_t)(sign | ((uint32_t)e << 10) | q);
}
static float h2f(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF, u;
    if (exp == 0x1F) u = sign | 0x7F800000 | (man ? 0x400000 | (man << 13) : 0);
    else if (exp == 0) {
        if (!man) u = sign;
        else { uint32_t e = 113; while (!(man & 0x400)) { man <<= 1; e--; } u = sign | (e << 23) | ((man & 0x3FF) << 13); }
    } else u = sign | ((exp + 112) << 23) | (man << 13);
    float f; __builtin_memcpy(&f, &u, 4); return f;
}
static double h2d(uint16_t h) { return (double)h2f(h); }   /* float -> double is exact */
static uint16_t d2h(double d) {
    /* double -> half directly: only equal to going through float when that's exact */
    float f = (float)d;
    if ((double)f == d || d != d) return f2h(f);
    return 0xFFFF;   /* not checked: see test_half */
}
static uint64_t vec_hi(uint64_t lo, uint64_t hi) { return hi ^ lo; }

#define MODE_zs_ws 3
#define MODE_zs_xs 3
#define MODE_zu_ws 3
#define MODE_zu_xd 3
#define MODE_zs_wd 3
#define MODE_zs_xd 3
#define MODE_ns_ws 0
#define MODE_nu_xd 0
#define MODE_ms_wd 2
#define MODE_mu_ws 2
#define MODE_ps_xs 1
#define MODE_pu_wd 1
#define MODE_as_ws 4
#define MODE_as_xd 4
#define MODE_au_xs 4
#define SIGNED_zs_ws 1
#define SIGNED_zs_xs 1
#define SIGNED_zu_ws 0
#define SIGNED_zu_xd 0
#define SIGNED_zs_wd 1
#define SIGNED_zs_xd 1
#define SIGNED_ns_ws 1
#define SIGNED_nu_xd 0
#define SIGNED_ms_wd 1
#define SIGNED_mu_ws 0
#define SIGNED_ps_xs 1
#define SIGNED_pu_wd 0
#define SIGNED_as_ws 1
#define SIGNED_as_xd 1
#define SIGNED_au_xs 0
#endif

/* name           instruction  result       source */
CVT(zs_ws, "fcvtzs", int32_t,  "w", float,  "s")
CVT(zs_xs, "fcvtzs", int64_t,  "x", float,  "s")
CVT(zu_ws, "fcvtzu", uint32_t, "w", float,  "s")
CVT(zu_xd, "fcvtzu", uint64_t, "x", double, "d")
CVT(zs_wd, "fcvtzs", int32_t,  "w", double, "d")
CVT(zs_xd, "fcvtzs", int64_t,  "x", double, "d")
CVT(ns_ws, "fcvtns", int32_t,  "w", float,  "s")
CVT(nu_xd, "fcvtnu", uint64_t, "x", double, "d")
CVT(ms_wd, "fcvtms", int32_t,  "w", double, "d")
CVT(mu_ws, "fcvtmu", uint32_t, "w", float,  "s")
CVT(ps_xs, "fcvtps", int64_t,  "x", float,  "s")
CVT(pu_wd, "fcvtpu", uint32_t, "w", double, "d")
CVT(as_ws, "fcvtas", int32_t,  "w", float,  "s")
CVT(as_xd, "fcvtas", int64_t,  "x", double, "d")
CVT(au_xs, "fcvtau", uint64_t, "x", float,  "s")
BIN(fmin_s,   "fmin",   float,  "s")
BIN(fmax_s,   "fmax",   float,  "s")
BIN(fminnm_s, "fminnm", float,  "s")
BIN(fmaxnm_s, "fmaxnm", float,  "s")
BIN(fmin_d,   "fmin",   double, "d")
BIN(fmax_d,   "fmax",   double, "d")
BIN(fminnm_d, "fminnm", double, "d")
BIN(fmaxnm_d, "fmaxnm", double, "d")

uint64_t test_fcvt_int(uint64_t a, uint64_t b) {
    float x = asf(a), p = (float)(int32_t)b * 0.25f;     /* raw bits, and values with .25/.5/.75 */
    double dx = asd(b), dp = (double)(int64_t)a * 0.5;
    uint64_t h = 0;
    float fs[2] = { x, p };
    double ds[2] = { dx, dp };
    for (int k = 0; k < 2; k++) {
        float f = fs[k]; double d = ds[k];
        h = mix(h, (uint32_t)zs_ws(f)); h = mix(h, (uint64_t)zs_xs(f)); h = mix(h, zu_ws(f));
        h = mix(h, zu_xd(d));           h = mix(h, (uint32_t)zs_wd(d)); h = mix(h, (uint64_t)zs_xd(d));
        h = mix(h, (uint32_t)ns_ws(f)); h = mix(h, nu_xd(d));           h = mix(h, (uint32_t)ms_wd(d));
        h = mix(h, mu_ws(f));           h = mix(h, (uint64_t)ps_xs(f)); h = mix(h, pu_wd(d));
        h = mix(h, (uint32_t)as_ws(f)); h = mix(h, (uint64_t)as_xd(d)); h = mix(h, au_xs(f));
    }
    return h;
}

uint64_t test_int_to_fp(uint64_t a, uint64_t b) {
    uint64_t h = 0;
    h = mix(h, fbits((float)(int32_t)a));  h = mix(h, fbits((float)(uint32_t)b));   /* SCVTF/UCVTF */
    h = mix(h, fbits((float)(int64_t)a));  h = mix(h, fbits((float)b));
    h = mix(h, dbits((double)(int32_t)b)); h = mix(h, dbits((double)(uint32_t)a));
    h = mix(h, dbits((double)(int64_t)b)); h = mix(h, dbits((double)a));
    h = mix(h, fbits((float)(int32_t)a / 65536.0f));        /* fixed point: SCVTF Sd, Wn, #16 */
    h = mix(h, dbits((double)(uint32_t)b / 256.0));
    float f = (float)(int16_t)a * 0.75f;                    /* in range, so the C cast is defined */
    h = mix(h, (uint64_t)(int32_t)(f * 1024.0f));           /* FCVTZS Wd, Sn, #10 */
    h = mix(h, (uint64_t)(int64_t)f);
    return h;
}

uint64_t test_fminmax(uint64_t a, uint64_t b) {
    float x = asf(a), y = asf(b), p = (float)(int8_t)a;
    double dx = asd(a), dy = asd(b);
    uint64_t h = 0;
    h = mix(h, fbits(fmin_s(x, y)));   h = mix(h, fbits(fmax_s(x, y)));
    h = mix(h, fbits(fminnm_s(x, y))); h = mix(h, fbits(fmaxnm_s(p, y)));
    h = mix(h, fbits(fmin_s(p, -p)));  h = mix(h, fbits(fmax_s(-0.0f, p * 0.0f)));
    h = mix(h, dbits(fmin_d(dx, dy))); h = mix(h, dbits(fmax_d(dy, dx)));
    h = mix(h, dbits(fminnm_d(dx, dy))); h = mix(h, dbits(fmaxnm_d(dx, 1.5)));
    return h;
}

uint64_t test_half(uint64_t a, uint64_t b) {
    float x = asf(a), p = (float)(int32_t)b * 0.001f;
    uint64_t h = 0;
    h = mix(h, f2h(x)); h = mix(h, f2h(p));                    /* FCVT Hd, Sn */
    h = mix(h, fbits(h2f((uint16_t)a)));                       /* FCVT Sd, Hn */
    h = mix(h, dbits(h2d((uint16_t)b)));                       /* FCVT Dd, Hn */
    double d = (double)p;                                      /* exactly a float: FCVT Hd, Dn checkable */
    h = mix(h, d2h(d));
    /* rounding edge cases: carries into the exponent, ties, overflow, subnormal boundaries */
    static const float edge[] = {
        2047.99f, 1.99951171875f, 1.99902343750f, 65504.0f, 65519.0f, 65520.0f, 70000.0f,
        6.1035156e-5f, 6.0975552e-5f, 6.1e-5f, 5.9604645e-8f, 2.9802322e-8f, 4.4703484e-8f,
        1.0e-9f, 0.33333334f, 1023.5f, 1024.5f, 2049.0f, 2051.0f,
    };
    for (unsigned k = 0; k < sizeof edge / sizeof edge[0]; k++) {
        float e = (b & 1) ? -edge[k] : edge[k];
        h = mix(h, f2h(e));
        h = mix(h, d2h((double)e));
    }
    h = mix(h, vec_hi(a, b));                                  /* FMOV Vd.D[1] */
    return h;
}
