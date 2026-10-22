/*
 * half.c - Maxwell's packed half-precision instructions: HADD2, HMUL2, HFMA2, HSETP2, HSET2.
 *
 * A register can hold two 16-bit floats ("halves"): h0 in its low 16 bits, h1 in the high 16.
 * These instructions work on both at once. We compute in 32-bit floats (GLSL UnpackHalf2x16)
 * and round back to halves when the result is stored (PackHalf2x16).
 *
 * Each operand is read with a "swizzle": F16 (h0, h1 as they are), F32 (the register is one
 * 32-bit float, used for both), H0H0 or H1H1 (one half, used for both). The result is stored
 * as both halves (F16), one 32-bit float (F32), or into one half of Rd, keeping the other
 * (MRG_H0 / MRG_H1). Immediates are 10 bits per half: a half's top 10 bits.
 *
 * Bit positions differ between the forms (R: register B, I: immediate, C: constant buffer,
 * 32I: 32-bit immediate); they follow Ryujinx (Ryujinx.Graphics.Shader/Decoders/InstDecoders.cs).
 */
#include "translate_internal.h"

enum { SWZ_F16, SWZ_F32, SWZ_H0H0, SWZ_H1H1 };
enum { OUT_F16, OUT_F32, OUT_MRG_H0, OUT_MRG_H1 };

typedef struct { uint32_t h[2]; } Pair;          /* two f32 ids */

static uint32_t elem(T *t, uint32_t v2, uint32_t i) { return op2(t, SpvOpCompositeExtract, t->t_f32, v2, i); }

static Pair unpack(T *t, uint32_t u) {
    uint32_t v = ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, u);
    return (Pair){ { elem(t, v, 0), elem(t, v, 1) } };
}

static uint32_t pack(T *t, uint32_t h0, uint32_t h1) {
    return ext1(t, t->t_u32, GLSLstd450PackHalf2x16, op2(t, SpvOpCompositeConstruct, t->t_v2f, h0, h1));
}

/* an operand: a 32-bit value read with a swizzle, then |x| and -x per half */
static Pair half_src(T *t, uint32_t u, int swz, int neg, int abs) {
    Pair p;
    if (swz == SWZ_F32) {
        p.h[0] = p.h[1] = bitcast(t, t->t_f32, u);
    } else {
        Pair v = unpack(t, u);
        p.h[0] = swz == SWZ_H1H1 ? v.h[1] : v.h[0];
        p.h[1] = swz == SWZ_H0H0 ? v.h[0] : v.h[1];
    }
    for (int i = 0; i < 2; i++) {
        if (abs) p.h[i] = ext1(t, t->t_f32, GLSLstd450FAbs, p.h[i]);
        if (neg) p.h[i] = op1(t, SpvOpFNegate, t->t_f32, p.h[i]);
    }
    return p;
}

/* the 10-bit-per-half immediate of the I forms: bits 20-29 (h0), 30-38 + 56 (h1) */
static uint32_t imm_halves(T *t, uint64_t w) {
    uint32_t h0 = bits(w, 20, 10), h1 = bits(w, 30, 9) | bits(w, 56, 1) << 9;
    return cu(t, h0 << 6 | h1 << 22);
}

static uint32_t cbuf_operand(T *t, uint64_t w) { return cbuf_u(t, (int)bits(w, 34, 5), bits(w, 20, 14) * 4); }

static void half_dest(T *t, int rd, Pair r, int ofmt, int saturate) {
    for (int i = 0; i < 2; i++) r.h[i] = sat(t, r.h[i], saturate);
    switch (ofmt) {
    case OUT_F16: set_u(t, rd, pack(t, r.h[0], r.h[1])); break;
    case OUT_F32: set_f(t, rd, r.h[0]); break;
    case OUT_MRG_H0: set_u(t, rd, pack(t, r.h[0], unpack(t, get_u(t, rd)).h[1])); break;
    case OUT_MRG_H1: set_u(t, rd, pack(t, unpack(t, get_u(t, rd)).h[0], r.h[1])); break;
    }
}

/* the operands of the arithmetic forms */
typedef struct { Pair a, b, c; int ofmt, sat; } HalfArgs;

static int half_args(T *t, const Inst *in, HalfArgs *x) {
    uint64_t w = in->w;
    int ra = (int)bits(w, 8, 8), rb = (int)bits(w, 20, 8), rc = (int)bits(w, 39, 8);
    int aswz = (int)bits(w, 47, 2);
    x->ofmt = (int)bits(w, 49, 2);
    switch (in->op) {
    case OP_HADD2R:
        x->a = half_src(t, get_u(t, ra), aswz, bit(w, 43), bit(w, 44));
        x->b = half_src(t, get_u(t, rb), (int)bits(w, 28, 2), bit(w, 31), bit(w, 30));
        x->sat = bit(w, 32);
        return 1;
    case OP_HMUL2R:                                  /* (bit 31 negates the product: applied to B) */
        x->a = half_src(t, get_u(t, ra), aswz, 0, bit(w, 44));
        x->b = half_src(t, get_u(t, rb), (int)bits(w, 28, 2), bit(w, 31), bit(w, 30));
        x->sat = bit(w, 32);
        return 1;
    case OP_HADD2I: case OP_HMUL2I:
        x->a = half_src(t, get_u(t, ra), aswz, bit(w, 43), bit(w, 44));
        x->b = half_src(t, imm_halves(t, w), SWZ_F16, 0, 0);
        x->sat = bit(w, 52);
        return 1;
    case OP_HADD2C: case OP_HMUL2C:
        x->a = half_src(t, get_u(t, ra), aswz, bit(w, 43), bit(w, 44));
        x->b = half_src(t, cbuf_operand(t, w), SWZ_F16, in->op == OP_HADD2C && bit(w, 56), bit(w, 54));
        x->sat = bit(w, 52);
        return 1;
    case OP_HADD232I: case OP_HMUL232I:              /* B: two halves in bits 20-51; result F16 */
        x->a = half_src(t, get_u(t, ra), (int)bits(w, 53, 2), in->op == OP_HADD232I && bit(w, 56), 0);
        x->b = half_src(t, cu(t, bits(w, 20, 32)), SWZ_F16, 0, 0);
        x->sat = bit(w, 52);
        x->ofmt = OUT_F16;
        return 1;
    case OP_HFMA2R:
        x->a = half_src(t, get_u(t, ra), aswz, 0, 0);
        x->b = half_src(t, get_u(t, rb), (int)bits(w, 28, 2), bit(w, 31), 0);
        x->c = half_src(t, get_u(t, rc), (int)bits(w, 35, 2), bit(w, 30), 0);
        x->sat = bit(w, 32);
        return 1;
    case OP_HFMA2I:
        x->a = half_src(t, get_u(t, ra), aswz, 0, 0);
        x->b = half_src(t, imm_halves(t, w), SWZ_F16, 0, 0);
        x->c = half_src(t, get_u(t, rc), (int)bits(w, 53, 2), bit(w, 51), 0);
        x->sat = bit(w, 52);
        return 1;
    case OP_HFMA2C:
        x->a = half_src(t, get_u(t, ra), aswz, 0, 0);
        x->b = half_src(t, cbuf_operand(t, w), SWZ_F16, bit(w, 56), 0);
        x->c = half_src(t, get_u(t, rc), (int)bits(w, 53, 2), bit(w, 51), 0);
        x->sat = bit(w, 52);
        return 1;
    case OP_HFMA2RC:                                 /* B is the register (bits 39-46), C the constant */
        x->a = half_src(t, get_u(t, ra), aswz, 0, 0);
        x->b = half_src(t, get_u(t, rc), (int)bits(w, 53, 2), bit(w, 56), 0);
        x->c = half_src(t, cbuf_operand(t, w), SWZ_F16, bit(w, 51), 0);
        x->sat = bit(w, 52);
        return 1;
    case OP_HFMA232I:                                /* C is Rd */
        x->a = half_src(t, get_u(t, ra), aswz, 0, 0);
        x->b = half_src(t, cu(t, bits(w, 20, 32)), SWZ_F16, 0, 0);
        x->c = half_src(t, get_u(t, (int)bits(w, 0, 8)), SWZ_F16, bit(w, 51), 0);
        x->sat = 0;
        x->ofmt = OUT_F16;
        return 1;
    default:
        return 0;
    }
}

/* the comparison forms: operands, the comparison, where the "both halves" / bool-float bit is */
typedef struct { Pair a, b; int cmp, flag; } HalfCompare;

static void half_compare_args(T *t, const Inst *in, HalfCompare *x) {
    uint64_t w = in->w;
    int ra = (int)bits(w, 8, 8), rb = (int)bits(w, 20, 8);
    x->a = half_src(t, get_u(t, ra), (int)bits(w, 47, 2), bit(w, 43), bit(w, 44));
    switch (in->op) {
    case OP_HSETP2R: case OP_HSET2R:
        x->b = half_src(t, get_u(t, rb), (int)bits(w, 28, 2), bit(w, 31), bit(w, 30));
        x->cmp = (int)bits(w, 35, 4);
        x->flag = bit(w, 49);                        /* HSETP2: H_AND; HSET2: 1.0 instead of all ones */
        break;
    case OP_HSETP2I: case OP_HSET2I:
        x->b = half_src(t, imm_halves(t, w), SWZ_F16, 0, 0);
        x->cmp = (int)bits(w, 49, 4);
        x->flag = bit(w, 53);
        break;
    default:                                         /* C forms */
        x->b = half_src(t, cbuf_operand(t, w), SWZ_F16, bit(w, 56), bit(w, 54));
        x->cmp = (int)bits(w, 49, 4);
        x->flag = bit(w, 53);
        break;
    }
}

int emit_half(T *t, const Inst *in) {
    uint64_t w = in->w;
    int rd = (int)bits(w, 0, 8);
    switch (in->op) {
    case OP_HADD2R: case OP_HADD2I: case OP_HADD2C: case OP_HADD232I: {
        HalfArgs x;
        half_args(t, in, &x);
        Pair r = { { op2(t, SpvOpFAdd, t->t_f32, x.a.h[0], x.b.h[0]), op2(t, SpvOpFAdd, t->t_f32, x.a.h[1], x.b.h[1]) } };
        half_dest(t, rd, r, x.ofmt, x.sat);
        return 1;
    }
    case OP_HMUL2R: case OP_HMUL2I: case OP_HMUL2C: case OP_HMUL232I: {
        HalfArgs x;
        half_args(t, in, &x);
        Pair r = { { op2(t, SpvOpFMul, t->t_f32, x.a.h[0], x.b.h[0]), op2(t, SpvOpFMul, t->t_f32, x.a.h[1], x.b.h[1]) } };
        half_dest(t, rd, r, x.ofmt, x.sat);
        return 1;
    }
    case OP_HFMA2R: case OP_HFMA2I: case OP_HFMA2C: case OP_HFMA2RC: case OP_HFMA232I: {
        HalfArgs x;
        half_args(t, in, &x);
        Pair r;
        for (int i = 0; i < 2; i++) r.h[i] = ext3(t, t->t_f32, GLSLstd450Fma, x.a.h[i], x.b.h[i], x.c.h[i]);
        half_dest(t, rd, r, x.ofmt, x.sat);
        return 1;
    }
    case OP_HSETP2R: case OP_HSETP2I: case OP_HSETP2C: {
        /* P = h0 test, Q = h1 test (or with H_AND: P = both, Q = not both), each combined with a predicate */
        HalfCompare x;
        half_compare_args(t, in, &x);
        uint32_t r0 = fcompare(t, x.cmp, x.a.h[0], x.b.h[0]), r1 = fcompare(t, x.cmp, x.a.h[1], x.b.h[1]);
        if (x.flag) {
            r0 = op2(t, SpvOpLogicalAnd, t->t_bool, r0, r1);
            r1 = op1(t, SpvOpLogicalNot, t->t_bool, r0);
        }
        uint32_t src = get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42));
        int bop = (int)bits(w, 45, 2);
        set_p(t, (int)bits(w, 3, 3), bool_op(t, bop, r0, src));
        set_p(t, (int)bits(w, 0, 3), bool_op(t, bop, r1, src));
        return 1;
    }
    case OP_HSET2R: case OP_HSET2I: case OP_HSET2C: {
        /* each half: test, combined with a predicate -> 1.0 / 0.0 (as halves) or 0xFFFF / 0 */
        HalfCompare x;
        half_compare_args(t, in, &x);
        uint32_t src = get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42));
        int bop = (int)bits(w, 45, 2);
        uint32_t r[2];
        for (int i = 0; i < 2; i++) r[i] = bool_op(t, bop, fcompare(t, x.cmp, x.a.h[i], x.b.h[i]), src);
        uint32_t lo = select_(t, t->t_u32, r[0], cu(t, x.flag ? 0x3C00 : 0xFFFF), cu(t, 0));
        uint32_t hi = select_(t, t->t_u32, r[1], cu(t, x.flag ? 0x3C000000u : 0xFFFF0000u), cu(t, 0));
        set_u(t, rd, op2(t, SpvOpBitwiseOr, t->t_u32, lo, hi));
        return 1;
    }
    default:
        return 0;
    }
}
