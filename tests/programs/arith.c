/* Arithmetic, multiply/divide, comparisons and flags.
 * Rules for every test program: every test_* takes (uint64_t, uint64_t) and returns
 * uint64_t, and the code must be free of undefined behaviour (guard division by zero,
 * mask shift amounts, bound loops), because native x86 and ARM64 handle UB differently. */
#include <stdint.h>

uint64_t test_add_sub(uint64_t a, uint64_t b) {
    uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
    return (a + b) ^ ((a - b) << 1) ^ (uint64_t)(a32 + b32) ^ ((uint64_t)(a32 - b32) << 3)
         ^ (a + 0xabc) ^ (a - 0x123000) ^ (uint64_t)(a32 + 4095u);
}

uint64_t test_mul(uint64_t a, uint64_t b) {
    uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
    int32_t s32 = (int32_t)a, t32 = (int32_t)b;
    uint64_t r = a * b;
    r ^= (uint64_t)(a32 * b32);
    r += (int64_t)s32 * t32;                /* SMULL */
    r -= (uint64_t)a32 * b32;               /* UMULL */
    r ^= a * 10 + b * 7 - a * b * 3;        /* MADD/MSUB */
    return r;
}

uint64_t test_mulhigh(uint64_t a, uint64_t b) {
    unsigned __int128 p = (unsigned __int128)a * b;
    __int128 s = (__int128)(int64_t)a * (int64_t)b;
    return (uint64_t)(p >> 64) ^ ((uint64_t)(s >> 64) * 3) ^ (uint64_t)p;
}

uint64_t test_div(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    if (b) r += a / b + a % b * 5;
    uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
    if (b32) r += (uint64_t)(a32 / b32) * 3 + a32 % b32;
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    if (sb && !(sa == INT64_MIN && sb == -1)) r ^= (uint64_t)(sa / sb + sa % sb);
    int32_t sa32 = (int32_t)a, sb32 = (int32_t)b;
    if (sb32 && !(sa32 == INT32_MIN && sb32 == -1)) r += (uint64_t)(int64_t)(sa32 / sb32) - (uint64_t)(sa32 % sb32);
    r += a / 10 + (a32 / 7) + (uint64_t)(sa / 3) + (uint64_t)(sa32 / 16);   /* constant division tricks */
    return r;
}

uint64_t test_compare(uint64_t a, uint64_t b) {
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    int32_t wa = (int32_t)a, wb = (int32_t)b;
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    uint64_t r = 0;
    r |= (uint64_t)(a < b) << 0;   r |= (uint64_t)(a <= b) << 1;
    r |= (uint64_t)(sa < sb) << 2; r |= (uint64_t)(sa <= sb) << 3;
    r |= (uint64_t)(sa > sb) << 4; r |= (uint64_t)(a > b) << 5;
    r |= (uint64_t)(a == b) << 6;  r |= (uint64_t)(wa < wb) << 7;
    r |= (uint64_t)(ua < ub) << 8; r |= (uint64_t)(wa >= wb) << 9;
    r |= (uint64_t)(ua >= ub) << 10; r |= (uint64_t)(a == 0) << 11;
    r |= (uint64_t)(sa < 0) << 12; r |= (uint64_t)(wa > 100) << 13;
    r |= (uint64_t)(a == 0x1234) << 14; r |= (uint64_t)(ua != 0xfffff000u) << 15;
    int64_t s; int32_t w; uint64_t u; uint32_t uw;
    r |= (uint64_t)__builtin_add_overflow(sa, sb, &s) << 16;   /* signed overflow -> V flag */
    r |= (uint64_t)__builtin_sub_overflow(wa, wb, &w) << 17;
    r |= (uint64_t)__builtin_add_overflow(a, b, &u) << 18;     /* carry -> C flag */
    r |= (uint64_t)__builtin_sub_overflow(ua, ub, &uw) << 19;
    r |= (uint64_t)__builtin_mul_overflow(sa, sb, &s) << 20;
    return r ^ ((uint64_t)s << 24) ^ ((uint64_t)w << 8) ^ u ^ uw;
}

uint64_t test_select(uint64_t a, uint64_t b) {
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    int64_t mx = sa > sb ? sa : sb;
    uint64_t mn = a < b ? a : b;
    uint64_t abs_a = sa < 0 ? 0 - a : a;
    uint64_t x = (a & 1) ? a + 1 : b;
    uint64_t y = (a > 100) ? ~b : b;
    int32_t t = (int32_t)a == (int32_t)b ? 5 : -(int32_t)(b & 0x7fffffff);
    uint32_t z = (uint32_t)a > 7u ? (uint32_t)b + 1u : (uint32_t)a;
    return (uint64_t)mx ^ (mn * 3) ^ abs_a ^ (x * 5) ^ y ^ (uint64_t)(int64_t)t ^ ((uint64_t)z << 17);
}

uint64_t test_logic_chain(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    if (a > 5 && b < 10) r |= 1;
    if (a == b || a == 3) r |= 2;
    if ((int32_t)a < 0 && (int32_t)b > 0) r |= 4;
    if ((a & 0xff) == 0x80 || (b >> 60) == 0xf) r |= 8;
    if (a != 0 && b != 0 && a + b < a) r |= 16;
    return r;
}

uint64_t test_wide128(uint64_t a, uint64_t b) {
    unsigned __int128 x = ((unsigned __int128)a << 64) | b;
    unsigned __int128 y = ((unsigned __int128)b << 64) | (a ^ 0x5555);
    unsigned __int128 s = x + y, d = x - y;
    return (uint64_t)(s >> 64) ^ (uint64_t)s ^ ((uint64_t)(d >> 64) * 3) ^ (uint64_t)d ^ (uint64_t)(x < y);
}
