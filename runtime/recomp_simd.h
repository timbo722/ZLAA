/*
 * recomp_simd.h - helpers for SIMD (NEON) instructions. Included by recomp_runtime.h.
 *
 * Generated code works on whole V128 values with small per-lane loops; the C compiler turns
 * most of those loops back into vector instructions. The helpers below cover lane operations
 * whose exact ARM behaviour is fiddly (saturation, rounding shifts, estimates, crypto...).
 */
#ifndef RECOMP_SIMD_H
#define RECOMP_SIMD_H

/* Saturate a wide intermediate to a signed / unsigned `bits`-wide result. */
static inline int64_t sat_s(__int128 v, int bits) {
    __int128 hi = ((__int128)1 << (bits - 1)) - 1, lo = -hi - 1;
    return (int64_t)(v > hi ? hi : v < lo ? lo : v);
}
static inline uint64_t sat_u(__int128 v, int bits) {
    __int128 hi = ((__int128)1 << bits) - 1;
    return (uint64_t)(v > hi ? hi : v < 0 ? 0 : v);
}

/* Shift by a signed amount (SSHL/USHL/SRSHL/URSHL/SQSHL...): positive = left, negative = right.
 * v is the lane value already sign- or zero-extended. The result is exact (not truncated). */
static inline __int128 shift_gen(__int128 v, int sh, int round) {
    if (sh >= 0) {
        if (sh >= 64)                          /* overflows every lane width: saturating ops saturate */
            return v == 0 ? 0 : (v > 0 ? ((__int128)1 << 100) : -((__int128)1 << 100));
        return v << sh;                        /* |v| < 2^64, so this fits in 128 bits */
    }
    int rs = -sh;
    if (rs > 65) return round ? 0 : (v < 0 ? -1 : 0);
    if (round) return (v + ((__int128)1 << (rs - 1))) >> rs;
    return v >> rs;
}

static inline uint64_t popcount8(uint64_t v) { return (uint64_t)__builtin_popcount((unsigned)(v & 0xFF)); }
static inline uint64_t clz_n(uint64_t v, int bits) {
    return v ? (uint64_t)(__builtin_clzll(v) - (64 - bits)) : (uint64_t)bits;
}
static inline uint64_t cls_n(uint64_t v, int bits) {          /* v: the lane, zero-extended */
    uint64_t m = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    uint64_t x = (v ^ (v >> 1)) & (m >> 1);   /* bit i set where bit i differs from bit i+1 */
    return clz_n(x, bits - 1);
}
static inline uint8_t rbit8(uint8_t v) { return (uint8_t)(rbit32(v) >> 24); }

/* polynomial (carry-less) multiply: PMUL, PMULL */
static inline unsigned __int128 pmull64(uint64_t a, uint64_t b) {
    unsigned __int128 r = 0;
    for (int i = 0; i < 64; i++) if ((b >> i) & 1) r ^= (unsigned __int128)a << i;
    return r;
}

/* ---- float lane helpers with ARM rules ---- */
static inline float fabd_s(float a, float b) { return u2f(f2u(a - b) & 0x7FFFFFFFu); }
static inline double fabd_d(double a, double b) { return u2d(d2u(a - b) & 0x7FFFFFFFFFFFFFFFull); }
static inline int isinf_s(float a) { return (f2u(a) & 0x7FFFFFFFu) == 0x7F800000u; }
static inline int isinf_d(double a) { return (d2u(a) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
/* FMULX: like FMUL, except infinity x zero = +-2.0 */
static inline float fmulx_s(float a, float b) {
    if (!isnan_s(a) && !isnan_s(b) && ((isinf_s(a) && b == 0) || (a == 0 && isinf_s(b))))
        return u2f(((f2u(a) ^ f2u(b)) & 0x80000000u) | 0x40000000u);
    return a * b;
}
static inline double fmulx_d(double a, double b) {
    if (!isnan_d(a) && !isnan_d(b) && ((isinf_d(a) && b == 0) || (a == 0 && isinf_d(b))))
        return u2d(((d2u(a) ^ d2u(b)) & 0x8000000000000000ull) | 0x4000000000000000ull);
    return a * b;
}
/* FRECPS: 2 - a*b (fused);  FRSQRTS: (3 - a*b) / 2 (fused). infinity x zero gives 2.0 / 1.5.
 * FRSQRTS rounds once, after the halving: when 3 - a*b overflows but its half doesn't, compute
 * 1.5 - (a/2)*b instead (a is then large, so halving it is exact). */
static inline float frecps_s(float a, float b) {
    if (!isnan_s(a) && !isnan_s(b) && ((isinf_s(a) && b == 0) || (a == 0 && isinf_s(b)))) return 2.0f;
    return __builtin_fmaf(-a, b, 2.0f);
}
static inline double frecps_d(double a, double b) {
    if (!isnan_d(a) && !isnan_d(b) && ((isinf_d(a) && b == 0) || (a == 0 && isinf_d(b)))) return 2.0;
    return __builtin_fma(-a, b, 2.0);
}
static inline float frsqrts_s(float a, float b) {
    if (!isnan_s(a) && !isnan_s(b) && ((isinf_s(a) && b == 0) || (a == 0 && isinf_s(b)))) return 1.5f;
    float r = __builtin_fmaf(-a, b, 3.0f) * 0.5f;
    if (isinf_s(r) && !isinf_s(a) && !isinf_s(b)) r = __builtin_fmaf(-(a * 0.5f), b, 1.5f);
    return r;
}
static inline double frsqrts_d(double a, double b) {
    if (!isnan_d(a) && !isnan_d(b) && ((isinf_d(a) && b == 0) || (a == 0 && isinf_d(b)))) return 1.5;
    double r = __builtin_fma(-a, b, 3.0) * 0.5;
    if (isinf_d(r) && !isinf_d(a) && !isinf_d(b)) r = __builtin_fma(-(a * 0.5), b, 1.5);
    return r;
}

/* ---- FRECPE / FRSQRTE / URECPE / URSQRTE: the Arm manual's estimate tables ---- */
static inline uint32_t recip_estimate(uint32_t a) {      /* a in [256, 511] -> [256, 511] */
    a = a * 2 + 1;
    uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}
static inline uint32_t rsqrt_estimate(uint32_t a) {      /* a in [128, 511] -> [256, 511] */
    static uint16_t table[512];
    if (!table[a]) {
        uint32_t x = a < 256 ? a * 2 + 1 : (((a >> 1) << 1) + 1) * 2;
        uint32_t b = 512;
        while ((uint64_t)x * (b + 1) * (b + 1) < (1ull << 28)) b++;
        table[a] = (uint16_t)((b + 1) / 2);
    }
    return table[a];
}
#define RECOMP_M52 ((1ull << 52) - 1)
/* Shared by single and double: works on the 52-bit fraction like the Arm pseudocode. */
static inline uint64_t fp_recpe_bits(uint64_t op, int N) {
    int E = N == 32 ? 8 : 11, F = N == 32 ? 23 : 52;
    uint64_t sign = (op >> (N - 1)) & 1, exp = (op >> F) & ((1u << E) - 1), mant = op & ((1ull << F) - 1);
    uint64_t emax = (1u << E) - 1, signbit = sign << (N - 1);
    if (exp == emax) {
        if (mant) return op | (1ull << (F - 1));                        /* NaN: quieten */
        return signbit;                                                 /* 1/inf = 0 */
    }
    if (exp == 0 && mant == 0) return signbit | (emax << F);            /* 1/0 = inf */
    if (exp == 0 && mant < (1ull << (F - 2))) return signbit | (emax << F); /* too small: overflows */
    uint64_t frac = mant << (52 - F);
    int e = (int)exp;
    if (e == 0) {
        if (!((frac >> 51) & 1)) { e = -1; frac = (frac << 2) & RECOMP_M52; }
        else frac = (frac << 1) & RECOMP_M52;
    }
    uint32_t est = recip_estimate(0x100 | (uint32_t)(frac >> 44));
    int rexp = (N == 32 ? 253 : 2045) - e;
    uint64_t rf = (uint64_t)(est & 0xFF) << 44;
    if (rexp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (rexp == -1) { rf = (1ull << 50) | (rf >> 2); rexp = 0; }
    return signbit | ((uint64_t)rexp << F) | (rf >> (52 - F));
}
static inline uint64_t fp_rsqrte_bits(uint64_t op, int N) {
    int E = N == 32 ? 8 : 11, F = N == 32 ? 23 : 52;
    uint64_t sign = (op >> (N - 1)) & 1, exp = (op >> F) & ((1u << E) - 1), mant = op & ((1ull << F) - 1);
    uint64_t emax = (1u << E) - 1;
    if (exp == emax && mant) return op | (1ull << (F - 1));            /* NaN: quieten */
    if (exp == 0 && mant == 0) return (sign << (N - 1)) | (emax << F);  /* 1/sqrt(+-0) = +-inf */
    if (sign) return N == 32 ? 0x7FC00000u : 0x7FF8000000000000ull;    /* negative: default NaN */
    if (exp == emax) return 0;                                          /* 1/sqrt(inf) = +0 */
    uint64_t frac = mant << (52 - F);
    int e = (int)exp;
    if (e == 0) {
        while (!((frac >> 51) & 1)) { frac = (frac << 1) & RECOMP_M52; e--; }
        frac = (frac << 1) & RECOMP_M52;
    }
    uint32_t scaled = (e & 1) == 0 ? (0x100 | (uint32_t)(frac >> 44)) : (0x80 | (uint32_t)(frac >> 45));
    int rexp = ((N == 32 ? 380 : 3068) - e) / 2;
    uint32_t est = rsqrt_estimate(scaled);
    return ((uint64_t)rexp << F) | ((uint64_t)(est & 0xFF) << (F - 8));
}
static inline float frecpe_s(float a) { return u2f((uint32_t)fp_recpe_bits(f2u(a), 32)); }
static inline double frecpe_d(double a) { return u2d(fp_recpe_bits(d2u(a), 64)); }
static inline float frsqrte_s(float a) { return u2f((uint32_t)fp_rsqrte_bits(f2u(a), 32)); }
static inline double frsqrte_d(double a) { return u2d(fp_rsqrte_bits(d2u(a), 64)); }
static inline uint32_t urecpe(uint32_t a) {
    if (!(a >> 31)) return 0xFFFFFFFFu;
    return recip_estimate(a >> 23) << 23;
}
static inline uint32_t ursqrte(uint32_t a) {
    if (!(a >> 30)) return 0xFFFFFFFFu;
    return rsqrt_estimate(a >> 23) << 23;
}
/* FRECPX: keeps the sign, inverts the exponent, clears the fraction */
static inline uint64_t frecpx_bits(uint64_t op, int N) {
    int E = N == 32 ? 8 : 11, F = N == 32 ? 23 : 52;
    uint64_t exp = (op >> F) & ((1u << E) - 1), mant = op & ((1ull << F) - 1), emax = (1u << E) - 1;
    uint64_t signbit = op & (1ull << (N - 1));
    if (exp == emax && mant) return op | (1ull << (F - 1));
    if (exp == 0) return signbit | ((emax - 1) << F);
    return signbit | ((~exp & emax) << F);
}

/* ---- loads/stores of whole structures: LD1-LD4 / ST1-ST4 ---- */
static inline void simd_ldst_multi(CPU *c, int t, uint64_t a, int q, int esz, int rpt, int selem, int load) {
    int lanes = (q ? 16 : 8) / esz;
    RECOMP_MEMCHK(c, a, (uint64_t)(rpt * selem * lanes * esz));
    for (int r = 0; r < rpt; r++)
        for (int e = 0; e < lanes; e++)
            for (int s = 0; s < selem; s++) {
                uint8_t *reg = (uint8_t *)&c->V[(t + r + s) & 31] + e * esz;
                if (load) memcpy(reg, c->mem + a, esz); else memcpy(c->mem + a, reg, esz);
                a += esz;
            }
    if (load && !q)
        for (int r = 0; r < rpt + selem - 1; r++) c->V[(t + r) & 31].d[1] = 0;
}
static inline void simd_ldst_single(CPU *c, int t, uint64_t a, int esz, int index, int selem, int load) {
    RECOMP_MEMCHK(c, a, (uint64_t)(selem * esz));
    for (int s = 0; s < selem; s++) {
        uint8_t *reg = (uint8_t *)&c->V[(t + s) & 31] + index * esz;
        if (load) memcpy(reg, c->mem + a, esz); else memcpy(c->mem + a, reg, esz);
        a += esz;
    }
}
static inline void simd_ld_replicate(CPU *c, int t, uint64_t a, int q, int esz, int selem) {
    RECOMP_MEMCHK(c, a, (uint64_t)(selem * esz));
    for (int s = 0; s < selem; s++) {
        V128 r; memset(&r, 0, sizeof r);
        for (int e = 0; e < (q ? 16 : 8) / esz; e++) memcpy((uint8_t *)&r + e * esz, c->mem + a, esz);
        c->V[(t + s) & 31] = r;
        a += esz;
    }
}

/* CRC32 / CRC32C instructions: bit-reflected CRC over `bytes` bytes of data */
static inline uint32_t crc32_update(uint32_t acc, uint64_t data, int bytes, uint32_t poly) {
    for (int i = 0; i < bytes * 8; i++) {
        acc ^= (uint32_t)(data >> i) & 1;
        acc = (acc >> 1) ^ ((acc & 1) ? poly : 0);
    }
    return acc;
}

/* ---- crypto extension (AES, SHA1, SHA256): implemented in recomp_crypto.c ---- */
void recomp_aes(V128 *d, const V128 *n, int op);                 /* 0 AESE, 1 AESD, 2 AESMC, 3 AESIMC */
void recomp_sha3(V128 *d, const V128 *n, const V128 *m, int op); /* 0-6: SHA1C/P/M/SU0, SHA256H/H2/SU1 */
void recomp_sha2(V128 *d, const V128 *n, int op);                /* 0-2: SHA1H, SHA1SU1, SHA256SU0 */

#endif /* RECOMP_SIMD_H */
