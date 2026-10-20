/* Helpers shared by the floating-point test programs (compiled for both ARM64 and native). */
#ifndef TESTLIB_H
#define TESTLIB_H
#include <stdint.h>

/* Reinterpret raw input bits as a float/double: this gives infinities, NaNs, subnormals, -0.0... */
static inline float asf(uint64_t a) { uint32_t u = (uint32_t)a; float f; __builtin_memcpy(&f, &u, 4); return f; }
static inline double asd(uint64_t a) { double d; __builtin_memcpy(&d, &a, 8); return d; }

/* Result bits, with every NaN folded into one value. NaN *payloads* legitimately differ between
 * x86 (the reference) and ARM, so the tests check "is it a NaN", not which NaN. */
static inline uint64_t fbits(float f) {
    if (f != f) return 0x7FC00000u;
    uint32_t u; __builtin_memcpy(&u, &f, 4); return u;
}
static inline uint64_t dbits(double d) {
    if (d != d) return 0x7FF8000000000000ull;
    uint64_t u; __builtin_memcpy(&u, &d, 8); return u;
}

/* Combine many results into one hash, so one test checks lots of operations. */
static inline uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull + (h >> 29); }

#endif
