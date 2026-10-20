/* Shifts, rotates, bitfields, logical immediates, bit counting, byte swapping. */
#include <stdint.h>

uint64_t test_shifts(uint64_t a, uint64_t b) {
    unsigned s = (unsigned)(b & 63), s32 = (unsigned)(b & 31);
    uint32_t a32 = (uint32_t)a;
    return (a << s) ^ (a >> s) ^ (uint64_t)((int64_t)a >> s)
         ^ (uint64_t)(a32 << s32) ^ ((uint64_t)(a32 >> s32) << 1) ^ (uint64_t)(uint32_t)((int32_t)a32 >> s32)
         ^ (a << 7) ^ (a >> 13) ^ (uint64_t)((int64_t)a >> 40) ^ (uint64_t)(a32 << 9) ^ (uint64_t)((int32_t)a32 >> 4);
}

uint64_t test_rotate(uint64_t a, uint64_t b) {
    unsigned s = (unsigned)(b & 63), s32 = (unsigned)(b & 31);
    uint32_t a32 = (uint32_t)a;
    uint64_t r1 = (a >> s) | (a << ((64 - s) & 63));
    uint32_t r2 = (a32 >> s32) | (a32 << ((32 - s32) & 31));
    uint64_t r3 = (a >> 17) | (a << 47);
    uint32_t r4 = (a32 << 5) | (a32 >> 27);
    uint64_t r5 = (a << 20) | (b >> 44);   /* EXTR */
    return r1 ^ ((uint64_t)r2 << 3) ^ r3 ^ r4 ^ r5;
}

uint64_t test_bitfield(uint64_t a, uint64_t b) {
    uint64_t f1 = (a >> 5) & 0x3ff;                          /* UBFX */
    int64_t f2 = ((int64_t)(a << 20)) >> 50;                 /* SBFX */
    uint64_t f3 = (b & 0xff) << 12;                          /* UBFIZ */
    uint64_t f4 = (a & ~(0xffull << 8)) | ((b & 0xff) << 8); /* BFI */
    uint32_t f5 = ((uint32_t)a & 0xffff0000u) | ((uint32_t)b >> 16); /* BFXIL */
    int64_t f6 = (int8_t)a + (int16_t)b + (int32_t)(a >> 3);  /* SXTB/SXTH/SXTW */
    uint64_t f7 = (uint8_t)a * 3u + (uint16_t)b;              /* UXTB/UXTH */
    int32_t f8 = ((int32_t)((uint32_t)b << 4)) >> 9;          /* 32-bit SBFX */
    int64_t f9 = (int64_t)(int32_t)((uint32_t)(int8_t)b << 4); /* SBFIZ */
    return f1 ^ (uint64_t)f2 ^ f3 ^ (f4 * 7) ^ f5 ^ (uint64_t)f6 ^ f7 ^ (uint64_t)(int64_t)f8 ^ (uint64_t)f9;
}

uint64_t test_logic_imm(uint64_t a, uint64_t b) {
    uint64_t r = (a & 0x00ff00ff00ff00ffull) ^ (b | 0xf0f0f0f0f0f0f0f0ull) ^ (a ^ 0x5555555555555555ull);
    r ^= (uint64_t)((uint32_t)a & 0x7ffffffeu) ^ (uint64_t)((uint32_t)b | 0x3c3c3c3cu);
    r ^= (b & ~a) ^ (a | ~b) ^ (a ^ ~b);                     /* BIC, ORN, EON */
    r += (a & 0xff00) == 0;                                  /* TST */
    r += (a & 0x8000000000000000ull) ? 1 : 0;
    return r;
}

uint64_t test_count(uint64_t a, uint64_t b) {
    uint64_t r = a ? (uint64_t)__builtin_clzll(a) : 64;     /* CLZ */
    r += ((uint32_t)b ? (uint64_t)__builtin_clz((uint32_t)b) : 32) << 8;
    r += (a ? (uint64_t)__builtin_ctzll(a) : 64) << 16;      /* RBIT + CLZ */
    r += (uint64_t)__builtin_clrsbll((long long)b) << 24;   /* CLS */
    unsigned pop = 0;
    for (uint64_t v = a; v; v &= v - 1) pop++;               /* popcount loop */
    r += (uint64_t)pop << 32;
    return r;
}

uint64_t test_byteswap(uint64_t a, uint64_t b) {
    return __builtin_bswap64(a) ^ ((uint64_t)__builtin_bswap32((uint32_t)b) << 7)
         ^ (uint64_t)__builtin_bswap16((uint16_t)a)
         ^ (((a & 0xff) << 8) | ((a >> 8) & 0xff));
}

uint64_t test_constants(uint64_t a, uint64_t b) {
    return (a + 0x123456789abcdef0ull) ^ (b * 0xdeadbeefcafeull) ^ 0xffff0000ffffull
         ^ (a & 0xfffffffffffff000ull) ^ (uint64_t)((uint32_t)b + 0x87654321u) ^ (a - 0x10000);
}
