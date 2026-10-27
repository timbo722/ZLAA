/*
 * recomp_runtime.h - the C side of the recompiler.
 *
 * Every recompiled ARM64 function becomes a C function `void f_XXXX(CPU *c)`.
 * The ARM64 registers live in the CPU struct below, and guest memory is one
 * flat byte array (c->mem) indexed by the guest (ARM64) address.
 *
 * Requires GCC or Clang (uses __int128 and __builtin_* helpers). The Android
 * NDK compiler is Clang, so this is fine for the final target.
 */
#ifndef RECOMP_RUNTIME_H
#define RECOMP_RUNTIME_H

#include <stdint.h>
#include <string.h>

/* One 128-bit SIMD & floating-point register, viewable at every lane width.
 * Sn is the low 32 bits of Vn, Dn the low 64 bits, Qn all 128.
 * (Reading a union member other than the one last written is allowed in GCC/Clang C.) */
typedef union {
    uint8_t  b[16];
    uint16_t h[8];
    uint32_t s[4];
    uint64_t d[2];
    int8_t   sb[16];   /* signed views of the same lanes */
    int16_t  sh[8];
    int32_t  sw[4];
    int64_t  sx[2];
    float    f[4];
    double   fd[2];
} V128;

typedef struct CPU {
    uint64_t x[31];      /* X0..X30. Register number 31 is SP or XZR depending on the instruction. */
    uint64_t sp;         /* stack pointer */
    uint8_t  n, z, c, v; /* NZCV condition flags, each 0 or 1 */
    uint8_t *mem;        /* host pointer to guest address 0 */
    uint64_t mem_size;   /* bytes of guest memory (used for bounds checks) */
    uint64_t excl_addr;  /* exclusive monitor: address of the last LDXR, or ~0 if none */
    uint64_t excl_val;   /* ...and the value it read */
    V128     V[32];      /* SIMD & FP registers V0..V31 */
    uint64_t fpcr, fpsr; /* FP control/status registers (rounding mode etc.; not used yet) */
    uint64_t tpidr_el0;  /* thread pointer registers (Horizon puts the thread's TLS here) */
    uint64_t tpidrro_el0;
} CPU;

typedef void (*RecompFunc)(CPU *c);

typedef struct {
    uint64_t addr;
    RecompFunc fn;
} RecompFuncEntry;

/* The modules the code was generated from (chunked game builds only), so the loader
 * can check that the player's dump is the same version. */
typedef struct {
    const char *name;
    uint64_t base, size;
    uint8_t build_id[32];
} RecompModuleInfo;

/* Provided by the generated code */
extern const RecompFuncEntry recomp_func_table[];
extern const uint64_t recomp_func_count;
extern const RecompModuleInfo recomp_modules[];
extern const uint64_t recomp_module_count;

/* Provided by recomp_runtime.c */
/* If set, asked first for every indirect call: returns a replacement for the guest function
 * at addr (high-level emulation, runtime/hle.c), or NULL to run the recompiled one. */
extern RecompFunc (*recomp_override)(uint64_t addr);
void recomp_call_addr(CPU *c, uint64_t addr);       /* indirect call (BLR, tail calls) */
RecompFunc recomp_lookup(uint64_t addr);             /* the recompiled function at addr (not replacements) */
void recomp_set_target(uint64_t addr, RecompFunc fn); /* indirect calls to addr run fn from now on */
void recomp_unimplemented(CPU *c, uint64_t pc, uint32_t insn);
void recomp_bad_jump(CPU *c, uint64_t pc, uint64_t target);
void recomp_svc(CPU *c, uint64_t pc, uint32_t imm);
void recomp_trap(CPU *c, uint64_t pc, uint32_t insn);
void recomp_mem_fault(CPU *c, uint64_t addr, int size);
uint64_t recomp_cntpct(void);                        /* system counter, 19.2 MHz like the Switch */

/* ---------------------------------------------------------------- memory */
/* memcpy keeps unaligned accesses legal in C; compilers turn it into one load. */
#ifdef RECOMP_CHECK_MEM
#define RECOMP_MEMCHK(c, a, n) do { if ((a) > (c)->mem_size - (n)) recomp_mem_fault((c), (a), (n)); } while (0)
#else
#define RECOMP_MEMCHK(c, a, n) ((void)0)
#endif

#define RECOMP_LD(bits)                                                     \
    static inline uint64_t ld##bits(CPU *c, uint64_t a) {                   \
        uint##bits##_t v; RECOMP_MEMCHK(c, a, bits / 8);                    \
        memcpy(&v, c->mem + a, sizeof v); return v; }
#define RECOMP_ST(bits)                                                     \
    static inline void st##bits(CPU *c, uint64_t a, uint64_t v) {           \
        uint##bits##_t t = (uint##bits##_t)v; RECOMP_MEMCHK(c, a, bits / 8); \
        memcpy(c->mem + a, &t, sizeof t); }
RECOMP_LD(8) RECOMP_LD(16) RECOMP_LD(32) RECOMP_LD(64)
RECOMP_ST(8) RECOMP_ST(16) RECOMP_ST(32) RECOMP_ST(64)

/* ---------------------------------------------------------------- atomics */
/* ARMv8.0 (the Switch's CPU) has no atomic add/swap instructions. Instead code does:
 *     loop: LDAXR x1, [x0]        load and start watching the address ("exclusive monitor")
 *           ADD   x1, x1, #1
 *           STLXR w2, x1, [x0]    store only if nobody wrote it since; w2 = 0 ok, 1 failed
 *           CBNZ  w2, loop
 * We remember the address and value at LDXR, and make STXR a real compare-and-swap
 * against that value. That keeps it correct even with several host threads running guest code. */
#define RECOMP_ATOMIC(bits)                                                                   \
    static inline uint64_t atomic_ld##bits(CPU *c, uint64_t a, int order) {                   \
        RECOMP_MEMCHK(c, a, bits / 8);                                                        \
        return __atomic_load_n((uint##bits##_t *)(c->mem + a), order); }                      \
    static inline void atomic_st##bits(CPU *c, uint64_t a, uint64_t v, int order) {           \
        RECOMP_MEMCHK(c, a, bits / 8);                                                        \
        __atomic_store_n((uint##bits##_t *)(c->mem + a), (uint##bits##_t)v, order); }         \
    static inline uint64_t excl_ld##bits(CPU *c, uint64_t a, int order) {                     \
        uint64_t v = atomic_ld##bits(c, a, order);                                            \
        c->excl_addr = a; c->excl_val = v; return v; }                                        \
    static inline uint32_t excl_st##bits(CPU *c, uint64_t a, uint64_t v) {                    \
        if (c->excl_addr != a) { c->excl_addr = ~0ull; return 1; }                            \
        c->excl_addr = ~0ull;                                                                 \
        uint##bits##_t expected = (uint##bits##_t)c->excl_val;                                \
        return __atomic_compare_exchange_n((uint##bits##_t *)(c->mem + a), &expected,         \
                   (uint##bits##_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED) ? 0 : 1; }
RECOMP_ATOMIC(8) RECOMP_ATOMIC(16) RECOMP_ATOMIC(32) RECOMP_ATOMIC(64)

/* ---------------------------------------------------------------- helpers */
/* Sign-extend the low `bits` bits of v to 64 bits. */
static inline uint64_t sext(uint64_t v, int bits) {
    return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}

/* ADDS/ADCS/SUBS/SBCS/CMP/CMN. Subtraction a-b is a + ~b + 1 (carry = NOT borrow). */
static inline uint64_t add64_flags(CPU *c, uint64_t a, uint64_t b, uint64_t cin) {
    unsigned __int128 u = (unsigned __int128)a + b + cin;
    uint64_t r = (uint64_t)u;
    c->n = (uint8_t)(r >> 63);
    c->z = r == 0;
    c->c = (uint8_t)(u >> 64);
    c->v = (uint8_t)(((~(a ^ b) & (a ^ r)) >> 63) & 1);
    return r;
}
static inline uint64_t add32_flags(CPU *c, uint32_t a, uint32_t b, uint64_t cin) {
    uint64_t u = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)u;
    c->n = (uint8_t)(r >> 31);
    c->z = r == 0;
    c->c = (uint8_t)(u >> 32);
    c->v = (uint8_t)(((~(a ^ b) & (a ^ r)) >> 31) & 1);
    return r;
}
/* ANDS/BICS/TST: N and Z from the result, C and V cleared. */
static inline uint64_t logic64_flags(CPU *c, uint64_t r) {
    c->n = (uint8_t)(r >> 63); c->z = r == 0; c->c = 0; c->v = 0; return r;
}
static inline uint64_t logic32_flags(CPU *c, uint32_t r) {
    c->n = (uint8_t)(r >> 31); c->z = r == 0; c->c = 0; c->v = 0; return r;
}
static inline void set_nzcv(CPU *c, int nzcv) {
    c->n = (nzcv >> 3) & 1; c->z = (nzcv >> 2) & 1; c->c = (nzcv >> 1) & 1; c->v = nzcv & 1;
}

static inline uint64_t ror64(uint64_t v, unsigned s) { s &= 63; return s ? (v >> s) | (v << (64 - s)) : v; }
static inline uint32_t ror32(uint32_t v, unsigned s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

/* ARM64 division never traps: x/0 = 0, and INT_MIN/-1 = INT_MIN. (x86 would crash on both.) */
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (b == 0) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (b == 0) return 0;
    if (a == 0x80000000u && b == ~0u) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

static inline uint64_t umulh(uint64_t a, uint64_t b) {
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
}
static inline uint64_t smulh(uint64_t a, uint64_t b) {
    return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64);
}

static inline uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
static inline uint64_t clz32(uint32_t v) { return v ? (uint64_t)__builtin_clz(v) : 32; }
/* CLS: count leading bits equal to the sign bit, not counting the sign bit itself */
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 1)) - 1; }
static inline uint64_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 1)) - 1; }

static inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return __builtin_bswap64(v);
}
static inline uint32_t rbit32(uint32_t v) { return (uint32_t)(rbit64(v) >> 32); }
static inline uint64_t rev16_64(uint64_t v) {
    return ((v >> 8) & 0x00FF00FF00FF00FFull) | ((v & 0x00FF00FF00FF00FFull) << 8);
}
static inline uint64_t rev32_64(uint64_t v) {  /* byte-reverse each 32-bit half */
    return ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32) | __builtin_bswap32((uint32_t)v);
}

/* ================================================================ floating point
 * Plain C float/double arithmetic (+ - * / sqrt fma) gives exactly the IEEE result ARM does,
 * provided the C compiler doesn't fuse a*b+c on its own: build generated code with
 * -ffp-contract=off (and never -ffast-math). Everything where ARM differs from C
 * (conversions, min/max, compares, half precision) has an explicit helper below. */

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

/* Writing a scalar to Sn/Dn/Hn/Bn zeroes the rest of the 128-bit register. */
static inline void set_vbits(CPU *c, int d, uint64_t lo, int bytes) {
    V128 t; memset(&t, 0, sizeof t); memcpy(&t, &lo, bytes); c->V[d] = t;
}
static inline void set_hbits(CPU *c, int d, uint64_t x) { set_vbits(c, d, x & 0xFFFF, 2); }
static inline void set_sbits(CPU *c, int d, uint64_t x) { set_vbits(c, d, x & 0xFFFFFFFFu, 4); }
static inline void set_dbits(CPU *c, int d, uint64_t x) { set_vbits(c, d, x, 8); }
static inline void set_s(CPU *c, int d, float x) { set_sbits(c, d, f2u(x)); }
static inline void set_d(CPU *c, int d, double x) { set_dbits(c, d, d2u(x)); }

/* Loads/stores of SIMD&FP registers (LDR/STR Bt/Ht/St/Dt/Qt) */
static inline void vld8(CPU *c, int t, uint64_t a)  { set_vbits(c, t, ld8(c, a), 1); }
static inline void vld16(CPU *c, int t, uint64_t a) { set_vbits(c, t, ld16(c, a), 2); }
static inline void vld32(CPU *c, int t, uint64_t a) { set_vbits(c, t, ld32(c, a), 4); }
static inline void vld64(CPU *c, int t, uint64_t a) { set_vbits(c, t, ld64(c, a), 8); }
static inline void vld128(CPU *c, int t, uint64_t a) {
    RECOMP_MEMCHK(c, a, 16); memcpy(&c->V[t], c->mem + a, 16);
}
static inline void vst8(CPU *c, int t, uint64_t a)  { st8(c, a, c->V[t].b[0]); }
static inline void vst16(CPU *c, int t, uint64_t a) { st16(c, a, c->V[t].h[0]); }
static inline void vst32(CPU *c, int t, uint64_t a) { st32(c, a, c->V[t].s[0]); }
static inline void vst64(CPU *c, int t, uint64_t a) { st64(c, a, c->V[t].d[0]); }
static inline void vst128(CPU *c, int t, uint64_t a) {
    RECOMP_MEMCHK(c, a, 16); memcpy(c->mem + a, &c->V[t], 16);
}

/* FCMP: unordered (a NaN involved) = 0011, equal = 0110, less = 1000, greater = 0010.
 * float -> double is exact, so one helper covers both precisions. */
static inline void fcmp(CPU *c, double a, double b) {
    if (a != a || b != b) set_nzcv(c, 0x3);
    else if (a == b)      set_nzcv(c, 0x6);
    else if (a < b)       set_nzcv(c, 0x8);
    else                  set_nzcv(c, 0x2);
}

/* NaN tests on raw bits (also catch signalling NaNs, which C can't name) */
static inline int isnan_s(float a)  { return (f2u(a) & 0x7FFFFFFFu) > 0x7F800000u; }
static inline int isnan_d(double a) { return (d2u(a) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline int isqnan_s(float a)  { return isnan_s(a) && (f2u(a) & 0x00400000u); }
static inline int isqnan_d(double a) { return isnan_d(a) && (d2u(a) & 0x0008000000000000ull); }

/* FMAX/FMIN: a NaN input gives NaN; max(-0, +0) = +0 and min(-0, +0) = -0.
 * FMAXNM/FMINNM: a quiet NaN loses to a number (this is what C's fmax/fmin do). */
#define RECOMP_FMINMAX(T, sfx, SIGN)                                                       \
    static inline T fmax_##sfx(T a, T b) {                                                 \
        if (isnan_##sfx(a) || isnan_##sfx(b)) return a + b;  /* propagates a NaN */     \
        if (a == 0 && b == 0) return (SIGN(a)) ? b : a;                                    \
        return a > b ? a : b; }                                                            \
    static inline T fmin_##sfx(T a, T b) {                                                 \
        if (isnan_##sfx(a) || isnan_##sfx(b)) return a + b;                                \
        if (a == 0 && b == 0) return (SIGN(a)) ? a : b;                                    \
        return a < b ? a : b; }                                                            \
    static inline T fmaxnm_##sfx(T a, T b) {                                               \
        if (isqnan_##sfx(a) && !isnan_##sfx(b)) return b;                                  \
        if (isqnan_##sfx(b) && !isnan_##sfx(a)) return a;                                  \
        return fmax_##sfx(a, b); }                                                         \
    static inline T fminnm_##sfx(T a, T b) {                                               \
        if (isqnan_##sfx(a) && !isnan_##sfx(b)) return b;                                  \
        if (isqnan_##sfx(b) && !isnan_##sfx(a)) return a;                                  \
        return fmin_##sfx(a, b); }
#define RECOMP_SIGN_S(a) (f2u(a) >> 31)
#define RECOMP_SIGN_D(a) (d2u(a) >> 63)
RECOMP_FMINMAX(float, s, RECOMP_SIGN_S)
RECOMP_FMINMAX(double, d, RECOMP_SIGN_D)

/* Rounding to an integral value. Modes match ARM's: 0=N (nearest, ties even), 1=P (+inf),
 * 2=M (-inf), 3=Z (toward zero), 4=A (nearest, ties away), 5=X/I (current mode = nearest) */
static inline double fround(double x, int mode) {
    switch (mode) {
    case 0: return __builtin_rint(x);
    case 1: return __builtin_ceil(x);
    case 2: return __builtin_floor(x);
    case 3: return __builtin_trunc(x);
    case 4: return __builtin_round(x);
    default: return __builtin_rint(x);
    }
}
static inline float froundf(float x, int mode) {
    switch (mode) {
    case 0: return __builtin_rintf(x);
    case 1: return __builtin_ceilf(x);
    case 2: return __builtin_floorf(x);
    case 3: return __builtin_truncf(x);
    case 4: return __builtin_roundf(x);
    default: return __builtin_rintf(x);
    }
}

/* FCVT[NPMZA][SU]: float -> integer. ARM saturates out-of-range values and turns NaN into 0;
 * a plain C cast would be undefined behaviour there. (float -> double is exact.) */
static inline uint64_t fcvt_s(double x, int mode, int bits) {
    if (x != x) return 0;
    x = fround(x, mode);
    double lim = bits == 64 ? 9223372036854775808.0 : 2147483648.0;
    if (x >= lim)  return bits == 64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
    if (x < -lim)  return bits == 64 ? 0x8000000000000000ull : 0x80000000ull;
    return bits == 64 ? (uint64_t)(int64_t)x : (uint32_t)(int32_t)x;
}
static inline uint64_t fcvt_u(double x, int mode, int bits) {
    if (x != x) return 0;
    x = fround(x, mode);
    double lim = bits == 64 ? 18446744073709551616.0 : 4294967296.0;
    if (x >= lim) return bits == 64 ? ~0ull : 0xFFFFFFFFull;
    if (x <= 0)   return 0;
    return (uint64_t)x;
}

/* Half precision (only conversions exist on the Switch's CPU). */
static inline double f16_to_f64(uint16_t h) {
    uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    double sign = (h & 0x8000) ? -1.0 : 1.0;
    if (exp == 0x1F) {
        if (!man) return sign * __builtin_inf();
        /* NaN: keep the payload, set the quiet bit */
        return u2d(((uint64_t)(h & 0x8000) << 48) | 0x7FF8000000000000ull | ((uint64_t)man << 42));
    }
    if (exp == 0) return sign * man * 0x1p-24;                  /* zero / subnormal */
    return sign * (1024 + man) * __builtin_ldexp(1.0, (int)exp - 25);
}
static inline uint16_t f64_to_f16(double x) {
    uint64_t u = d2u(x);
    uint16_t sign = (uint16_t)((u >> 48) & 0x8000);
    if (x != x) return sign | 0x7E00 | (uint16_t)((u >> 42) & 0x1FF);   /* quiet NaN, top payload bits */
    double a = __builtin_fabs(x);
    if (a >= 65520.0) return sign | 0x7C00;          /* rounds to infinity */
    if (a < 0x1p-14)                                  /* subnormal: count units of 2^-24 */
        return sign | (uint16_t)__builtin_rint(a * 0x1p24);
    int e;
    double fr = __builtin_frexp(a, &e);               /* a = fr * 2^e, fr in [0.5, 1) */
    uint32_t m = (uint32_t)__builtin_rint((fr * 2.0 - 1.0) * 1024.0);
    uint32_t ex = (uint32_t)(e - 1 + 15);
    if (m == 1024) { m = 0; ex++; }
    if (ex >= 31) return sign | 0x7C00;
    return sign | (uint16_t)(ex << 10) | (uint16_t)m;
}

#include "recomp_simd.h"

#endif /* RECOMP_RUNTIME_H */
