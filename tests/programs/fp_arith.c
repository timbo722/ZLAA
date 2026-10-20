/* Scalar floating point: arithmetic, compares, selects, fused multiply-add, rounding, constants.
 * Inputs are used two ways: as raw bit patterns (inf, NaN, subnormals, -0.0) and as ordinary numbers. */
#include <stdint.h>
#include "testlib.h"

uint64_t test_f32_ops(uint64_t a, uint64_t b) {
    float x = asf(a), y = asf(b);
    float p = (float)(int32_t)a * 0.25f, q = (float)(int16_t)b + 0.5f;
    uint64_t h = 0;
    h = mix(h, fbits(x + y)); h = mix(h, fbits(x - y));
    h = mix(h, fbits(x * y)); h = mix(h, fbits(x / y));
    h = mix(h, fbits(p + q)); h = mix(h, fbits(p - q));
    h = mix(h, fbits(p * q)); h = mix(h, fbits(p / q));
    h = mix(h, fbits(-(p * y)));                 /* FNMUL */
    h = mix(h, fbits(-x));                       /* FNEG */
    h = mix(h, fbits(__builtin_fabsf(y)));       /* FABS */
    return h;
}

uint64_t test_f64_ops(uint64_t a, uint64_t b) {
    double x = asd(a), y = asd(b);
    double p = (double)(int64_t)a * 0.125, q = (double)(int32_t)b - 0.75;
    uint64_t h = 0;
    h = mix(h, dbits(x + y)); h = mix(h, dbits(x - y));
    h = mix(h, dbits(x * y)); h = mix(h, dbits(x / y));
    h = mix(h, dbits(p + q)); h = mix(h, dbits(p - q));
    h = mix(h, dbits(p * q)); h = mix(h, dbits(p / q));
    h = mix(h, dbits(-(x * q)));
    h = mix(h, dbits(-y));
    h = mix(h, dbits(__builtin_fabs(x)));
    return h;
}

uint64_t test_fp_compare(uint64_t a, uint64_t b) {
    float x = asf(a), y = asf(b), p = (float)(int8_t)a, q = (float)(int8_t)b;
    double dx = asd(a), dy = asd(b);
    uint64_t r = 0;
    r |= (uint64_t)(x < y) << 0;   r |= (uint64_t)(x <= y) << 1;
    r |= (uint64_t)(x > y) << 2;   r |= (uint64_t)(x >= y) << 3;
    r |= (uint64_t)(x == y) << 4;  r |= (uint64_t)(x != y) << 5;
    r |= (uint64_t)__builtin_isunordered(x, y) << 6;
    r |= (uint64_t)(dx < dy) << 7;  r |= (uint64_t)(dx >= dy) << 8;
    r |= (uint64_t)(dx == dy) << 9; r |= (uint64_t)__builtin_isunordered(dx, dy) << 10;
    r |= (uint64_t)(x < 0.0f) << 11; r |= (uint64_t)(dx == 0.0) << 12;
    r |= (uint64_t)(p < q && x > 1.0f) << 13;          /* FCCMP */
    r |= (uint64_t)(dx == dy || p >= q) << 14;
    r |= (uint64_t)(p > q || (y < x && q != 0.0f)) << 15;
    uint64_t h = mix(r, fbits(x < y ? x : y));          /* FCSEL */
    h = mix(h, fbits(p > 1.5f ? p : q));
    h = mix(h, dbits(dx >= dy ? dy : -dx));
    return h;
}

uint64_t test_fp_fma(uint64_t a, uint64_t b) {
    float x = asf(a), y = asf(b), p = (float)(int32_t)a * 0.5f, q = (float)(uint16_t)b;
    double dx = asd(a), dy = asd(b), dp = (double)(int64_t)b * 0.25;
    uint64_t h = 0;
    h = mix(h, fbits(__builtin_fmaf(p, q, x)));      /* FMADD   p*q + x */
    h = mix(h, fbits(__builtin_fmaf(-p, q, y)));     /* FMSUB   y - p*q */
    h = mix(h, fbits(__builtin_fmaf(-x, q, -p)));    /* FNMADD -p - x*q */
    h = mix(h, fbits(__builtin_fmaf(x, y, -q)));     /* FNMSUB -q + x*y */
    h = mix(h, dbits(__builtin_fma(dx, dp, dy)));
    h = mix(h, dbits(__builtin_fma(-dp, dp, dx)));
    h = mix(h, dbits(__builtin_fma(dp, 3.0, -dy)));
    return h;
}

uint64_t test_fp_round(uint64_t a, uint64_t b) {
    float x = asf(a), p = (float)(int32_t)a * 0.25f;    /* lots of .25 .5 .75: rounding ties */
    double dx = asd(b), dp = (double)(int64_t)b * 0.5;
    uint64_t h = 0;
    h = mix(h, fbits(__builtin_sqrtf(x)));  h = mix(h, fbits(__builtin_sqrtf(p)));
    h = mix(h, fbits(__builtin_floorf(p))); h = mix(h, fbits(__builtin_ceilf(p)));
    h = mix(h, fbits(__builtin_truncf(p))); h = mix(h, fbits(__builtin_roundf(p)));
    h = mix(h, fbits(__builtin_rintf(p)));  h = mix(h, fbits(__builtin_nearbyintf(x)));
    h = mix(h, fbits(__builtin_floorf(x))); h = mix(h, fbits(__builtin_roundf(x)));
    h = mix(h, dbits(__builtin_sqrt(dx)));  h = mix(h, dbits(__builtin_sqrt(dp)));
    h = mix(h, dbits(__builtin_floor(dp))); h = mix(h, dbits(__builtin_ceil(dx)));
    h = mix(h, dbits(__builtin_trunc(dp))); h = mix(h, dbits(__builtin_round(dp)));
    h = mix(h, dbits(__builtin_rint(dp)));  h = mix(h, dbits(__builtin_nearbyint(dx)));
    return h;
}

uint64_t test_fp_constants(uint64_t a, uint64_t b) {
    float p = (float)(int32_t)a;
    double dp = (double)(int64_t)b;
    /* simple constants become FMOV #imm; others are loaded from memory (LDR literal / ADRP+LDR) */
    float r = p * 1.5f + 0.125f - 31.0f / (p + 3.0f) + p * -0.5f + 0.1f * p + 1.0e10f;
    double d = dp * 2.5 - 0.3125 + dp * 16.0 - 1.0 / (dp + 0.75) + 3.14159265358979 * dp;
    return mix(fbits(r), dbits(d));
}

uint64_t test_fp_precision(uint64_t a, uint64_t b) {
    float x = asf(a), p = (float)(int32_t)b / 7.0f;
    double dx = asd(b), dp = (double)(int64_t)a / 3.0;
    uint64_t h = 0;
    h = mix(h, dbits((double)x));  h = mix(h, dbits((double)p));     /* FCVT Dd, Sn */
    h = mix(h, fbits((float)dx));  h = mix(h, fbits((float)dp));     /* FCVT Sd, Dn (rounds) */
    h = mix(h, fbits((float)(dp * dp) + p));
    return h;
}
