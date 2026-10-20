/*
 * alu.c - Maxwell arithmetic, logic, comparison and conversion instructions -> SPIR-V.
 *
 * Operand A is a register (bits 8-15) and the destination is bits 0-7, for all of these.
 * Operand B comes in forms (register, immediate, constant buffer): operand_b() picks it.
 * Semantics follow the hardware as Ryujinx's shader translator models it.
 */
#include "translate_internal.h"

/* ---- small helpers ---- */

static uint32_t fneg(T *t, uint32_t v) { return op1(t, SpvOpFNegate, t->t_f32, v); }
static uint32_t fabs_(T *t, uint32_t v) { return ext1(t, t->t_f32, GLSLstd450FAbs, v); }
static uint32_t fmod_(T *t, uint32_t v, int abs, int neg) {
    if (abs) v = fabs_(t, v);
    if (neg) v = fneg(t, v);
    return v;
}
static uint32_t sat(T *t, uint32_t v, int on) {
    return on ? ext3(t, t->t_f32, GLSLstd450FClamp, v, cf(t, 0.0f), cf(t, 1.0f)) : v;
}
static uint32_t as_f(T *t, uint32_t u) { return bitcast(t, t->t_f32, u); }
static uint32_t as_u(T *t, uint32_t v) { return bitcast(t, t->t_u32, v); }
static uint32_t as_i(T *t, uint32_t v) { return bitcast(t, t->t_i32, v); }
static uint32_t inot(T *t, uint32_t v) { return op1(t, SpvOpNot, t->t_u32, v); }
static uint32_t ineg(T *t, uint32_t v) { return op1(t, SpvOpSNegate, t->t_u32, v); }
static uint32_t iadd(T *t, uint32_t a, uint32_t b) { return op2(t, SpvOpIAdd, t->t_u32, a, b); }

static uint32_t bool_op(T *t, int op, uint32_t a, uint32_t b) {
    switch (op) {
    case 0: return op2(t, SpvOpLogicalAnd, t->t_bool, a, b);
    case 1: return op2(t, SpvOpLogicalOr, t->t_bool, a, b);
    case 2: return op2(t, SpvOpLogicalNotEqual, t->t_bool, a, b);
    }
    fail(t, "bool op %d", op);
    return a;
}

static uint32_t isnan_(T *t, uint32_t v) { return op1(t, SpvOpIsNan, t->t_bool, v); }

/* FComp: F, LT, EQ, LE, GT, NE, GE, NUM, NAN, LTU, EQU, LEU, GTU, NEU, GEU, T
 * (ordered comparisons are false if either side is NaN; the U ones are true) */
static uint32_t fcompare(T *t, int cmp, uint32_t a, uint32_t b) {
    static const SpvOp ops[16] = {
        0, SpvOpFOrdLessThan, SpvOpFOrdEqual, SpvOpFOrdLessThanEqual, SpvOpFOrdGreaterThan, SpvOpFOrdNotEqual,
        SpvOpFOrdGreaterThanEqual, 0, 0, SpvOpFUnordLessThan, SpvOpFUnordEqual, SpvOpFUnordLessThanEqual,
        SpvOpFUnordGreaterThan, SpvOpFUnordNotEqual, SpvOpFUnordGreaterThanEqual, 0,
    };
    if (cmp == 0) return cb(t, 0);
    if (cmp == 15) return cb(t, 1);
    uint32_t either_nan = op2(t, SpvOpLogicalOr, t->t_bool, isnan_(t, a), isnan_(t, b));
    if (cmp == 7) return op1(t, SpvOpLogicalNot, t->t_bool, either_nan);
    if (cmp == 8) return either_nan;
    return op2(t, ops[cmp], t->t_bool, a, b);
}

/* IComp: F, LT, EQ, LE, GT, NE, GE, T */
static uint32_t icompare(T *t, int cmp, int is_signed, uint32_t a, uint32_t b) {
    switch (cmp) {
    case 0: return cb(t, 0);
    case 1: return op2(t, is_signed ? SpvOpSLessThan : SpvOpULessThan, t->t_bool, a, b);
    case 2: return op2(t, SpvOpIEqual, t->t_bool, a, b);
    case 3: return op2(t, is_signed ? SpvOpSLessThanEqual : SpvOpULessThanEqual, t->t_bool, a, b);
    case 4: return op2(t, is_signed ? SpvOpSGreaterThan : SpvOpUGreaterThan, t->t_bool, a, b);
    case 5: return op2(t, SpvOpINotEqual, t->t_bool, a, b);
    case 6: return op2(t, is_signed ? SpvOpSGreaterThanEqual : SpvOpUGreaterThanEqual, t->t_bool, a, b);
    }
    return cb(t, 1);
}

/* compare results combined with a predicate, into two predicates (the second inverted) */
static void set_compare_preds(T *t, uint64_t w, uint32_t res) {
    uint32_t src = get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42));
    int bop = (int)bits(w, 45, 2);
    set_p(t, (int)bits(w, 3, 3), bool_op(t, bop, res, src));
    set_p(t, (int)bits(w, 0, 3), bool_op(t, bop, op1(t, SpvOpLogicalNot, t->t_bool, res), src));
}

/* condition codes (for 64-bit adds: IADD.CC then IADD.X) */
static uint32_t cc_var(T *t, uint32_t *v) {
    if (!*v) {
        *v = spv_id(&t->s);
        spv_op(&t->s, SEC_VARS, SpvOpVariable, 4, ptr_type(t, SpvStorageClassFunction, t->t_bool), *v,
               SpvStorageClassFunction, cb(t, 0));
    }
    return *v;
}
static uint32_t get_carry(T *t) { return op1(t, SpvOpLoad, t->t_bool, cc_var(t, &t->cc_carry)); }
static void set_cc(T *t, uint32_t *var, uint32_t v) { spv_op(&t->s, SEC_BODY, SpvOpStore, 2, cc_var(t, var), v); }
static void set_zn(T *t, uint32_t res) {
    set_cc(t, &t->cc_zero, op2(t, SpvOpIEqual, t->t_bool, res, cu(t, 0)));
    set_cc(t, &t->cc_sign, op2(t, SpvOpSLessThan, t->t_bool, as_i(t, res), ci(t, 0)));
}
/* IADD flags: C = carry out, V = signed overflow (as Ryujinx models them) */
static void set_add_flags(T *t, uint32_t res, uint32_t a, uint32_t b, int extended) {
    uint32_t c = op2(t, SpvOpULessThan, t->t_bool, res, a);
    if (extended)
        c = op2(t, SpvOpLogicalOr, t->t_bool, c,
                op2(t, SpvOpLogicalAnd, t->t_bool, op2(t, SpvOpIEqual, t->t_bool, res, a), get_carry(t)));
    uint32_t v = op2(t, SpvOpBitwiseAnd, t->t_u32, op2(t, SpvOpBitwiseXor, t->t_u32, res, a),
                     inot(t, op2(t, SpvOpBitwiseXor, t->t_u32, a, b)));
    set_cc(t, &t->cc_carry, c);
    set_cc(t, &t->cc_overflow, op2(t, SpvOpSLessThan, t->t_bool, as_i(t, v), ci(t, 0)));
    set_zn(t, res);
}

static uint32_t carry_u(T *t) { return select_(t, t->t_u32, get_carry(t), cu(t, 1), cu(t, 0)); }

/* x >> n / x << n with the hardware's rule for n >= 32 (unless the shift wraps) */
static uint32_t shift(T *t, SpvOp op, uint32_t a, uint32_t n, int wrap, int is_signed) {
    uint32_t r = op == SpvOpShiftRightArithmetic
        ? as_u(t, op2(t, op, t->t_i32, as_i(t, a), op2(t, SpvOpBitwiseAnd, t->t_u32, n, cu(t, 31))))
        : op2(t, op, t->t_u32, a, op2(t, SpvOpBitwiseAnd, t->t_u32, n, cu(t, 31)));
    if (wrap) return r;
    uint32_t big = op2(t, SpvOpUGreaterThanEqual, t->t_bool, n, cu(t, 32));
    uint32_t fill = is_signed ? as_u(t, op2(t, SpvOpShiftRightArithmetic, t->t_i32, as_i(t, a), ci(t, 31))) : cu(t, 0);
    return select_(t, t->t_u32, big, fill, r);
}

static uint32_t logic(T *t, int op, uint32_t a, uint32_t b) {
    switch (op) {
    case 0: return op2(t, SpvOpBitwiseAnd, t->t_u32, a, b);
    case 1: return op2(t, SpvOpBitwiseOr, t->t_u32, a, b);
    case 2: return op2(t, SpvOpBitwiseXor, t->t_u32, a, b);
    }
    return b;                                            /* PASS_B */
}

/* PredicateOp: write a predicate from the result: F, T, Z (== 0), NZ */
static void logic_pred(T *t, int pred_op, int dest, uint32_t res) {
    if (dest == PT) return;
    uint32_t v = pred_op == 0 ? cb(t, 0) : pred_op == 1 ? cb(t, 1)
               : op2(t, pred_op == 2 ? SpvOpIEqual : SpvOpINotEqual, t->t_bool, res, cu(t, 0));
    set_p(t, dest, v);
}

/* Bitfield extract/insert with the hardware's handling of out-of-range positions/lengths */
static uint32_t bf_clamp_count(T *t, uint32_t pos, uint32_t len) {
    uint32_t room = op2(t, SpvOpISub, t->t_u32, cu(t, 32), ext2(t, t->t_u32, GLSLstd450UMin, pos, cu(t, 32)));
    return ext2(t, t->t_u32, GLSLstd450UMin, len, room);
}

/* 16-bit half of a register, sign- or zero-extended */
static uint32_t half16(T *t, uint32_t v, int high, int is_signed) {
    if (is_signed)
        return as_u(t, high ? op2(t, SpvOpShiftRightArithmetic, t->t_i32, as_i(t, v), ci(t, 16))
                            : op3(t, SpvOpBitFieldSExtract, t->t_i32, as_i(t, v), ci(t, 0), ci(t, 16)));
    return high ? op2(t, SpvOpShiftRightLogical, t->t_u32, v, cu(t, 16)) : op2(t, SpvOpBitwiseAnd, t->t_u32, v, cu(t, 0xFFFF));
}

/* ---- the instructions ---- */

int emit_alu(T *t, const Inst *in) {
    uint64_t w = in->w;
    int rd = (int)bits(w, 0, 8), ra = (int)bits(w, 8, 8);
    switch (in->op) {

    /* ---- float arithmetic ---- */
    case OP_FADDR: case OP_FADDI: case OP_FADDC: {
        uint32_t a = fmod_(t, get_f(t, ra), bit(w, 46), bit(w, 48));
        uint32_t b = fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 49), bit(w, 45));
        set_f(t, rd, sat(t, op2(t, SpvOpFAdd, t->t_f32, a, b), bit(w, 50)));
        return 1;
    }
    case OP_FADD32I: {
        uint32_t a = fmod_(t, get_f(t, ra), bit(w, 54), bit(w, 56));
        uint32_t b = fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 57), bit(w, 53));
        set_f(t, rd, op2(t, SpvOpFAdd, t->t_f32, a, b));
        return 1;
    }
    case OP_FMULR: case OP_FMULI: case OP_FMULC: {
        static const float scale[8] = { 1, 0.5f, 0.25f, 0.125f, 8, 4, 2, 1 };
        uint32_t a = fmod_(t, get_f(t, ra), 0, bit(w, 48));
        uint32_t r = op2(t, SpvOpFMul, t->t_f32, a, as_f(t, operand_b(t, in, 1)));
        if (bits(w, 41, 3)) r = op2(t, SpvOpFMul, t->t_f32, r, cf(t, scale[bits(w, 41, 3)]));
        set_f(t, rd, sat(t, r, bit(w, 50)));
        return 1;
    }
    case OP_FMUL32I:
        set_f(t, rd, sat(t, op2(t, SpvOpFMul, t->t_f32, get_f(t, ra), as_f(t, operand_b(t, in, 1))), bit(w, 55)));
        return 1;
    case OP_FFMAR: case OP_FFMAI: case OP_FFMAC: case OP_FFMARC: {
        uint32_t a = fmod_(t, get_f(t, ra), 0, bit(w, 48));        /* negating A negates the product */
        uint32_t c = fmod_(t, as_f(t, operand_c(t, in)), 0, bit(w, 49));
        set_f(t, rd, sat(t, ext3(t, t->t_f32, GLSLstd450Fma, a, as_f(t, operand_b(t, in, 1)), c), bit(w, 50)));
        return 1;
    }
    case OP_FFMA32I: {                                    /* C is the destination register */
        uint32_t a = fmod_(t, get_f(t, ra), 0, bit(w, 56));
        uint32_t c = fmod_(t, get_f(t, rd), 0, bit(w, 57));
        set_f(t, rd, sat(t, ext3(t, t->t_f32, GLSLstd450Fma, a, as_f(t, operand_b(t, in, 1)), c), bit(w, 55)));
        return 1;
    }
    case OP_MUFU: {
        uint32_t a = fmod_(t, get_f(t, ra), bit(w, 46), bit(w, 48)), r;
        switch (bits(w, 20, 4)) {
        case 0: r = ext1(t, t->t_f32, GLSLstd450Cos, a); break;
        case 1: r = ext1(t, t->t_f32, GLSLstd450Sin, a); break;
        case 2: r = ext1(t, t->t_f32, GLSLstd450Exp2, a); break;
        case 3: r = ext1(t, t->t_f32, GLSLstd450Log2, a); break;
        case 4: r = op2(t, SpvOpFDiv, t->t_f32, cf(t, 1.0f), a); break;
        case 5: r = ext1(t, t->t_f32, GLSLstd450InverseSqrt, a); break;
        case 8: r = ext1(t, t->t_f32, GLSLstd450Sqrt, a); break;
        default: fail(t, "MUFU op %u", bits(w, 20, 4)); return 1;
        }
        set_f(t, rd, sat(t, r, bit(w, 50)));
        return 1;
    }
    case OP_RROR: case OP_RROI: case OP_RROC:              /* range reduction: a no-op for us */
        set_f(t, rd, fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 49), bit(w, 45)));
        return 1;
    case OP_FMNMXR: case OP_FMNMXI: case OP_FMNMXC: {     /* predicate true: min, else max */
        uint32_t a = fmod_(t, get_f(t, ra), bit(w, 46), bit(w, 48));
        uint32_t b = fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 49), bit(w, 45));
        uint32_t is_min = get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42));
        set_f(t, rd, select_(t, t->t_f32, is_min, ext2(t, t->t_f32, GLSLstd450NMin, a, b),
                             ext2(t, t->t_f32, GLSLstd450NMax, a, b)));
        return 1;
    }
    case OP_FSETPR: case OP_FSETPI: case OP_FSETPC: {
        uint32_t a = fmod_(t, get_f(t, ra), bit(w, 7), bit(w, 43));
        uint32_t b = fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 44), bit(w, 6));
        set_compare_preds(t, w, fcompare(t, (int)bits(w, 48, 4), a, b));
        return 1;
    }
    case OP_FSETR: case OP_FSETI: case OP_FSETC: {        /* Rd = cond ? (1.0 or all ones) : 0 */
        uint32_t a = fmod_(t, get_f(t, ra), bit(w, 54), bit(w, 43));
        uint32_t b = fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 44), bit(w, 53));
        uint32_t r = bool_op(t, (int)bits(w, 45, 2), fcompare(t, (int)bits(w, 48, 4), a, b),
                             get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42)));
        set_u(t, rd, select_(t, t->t_u32, r, cu(t, bit(w, 52) ? 0x3F800000u : 0xFFFFFFFFu), cu(t, 0)));
        return 1;
    }

    /* ---- conversions ---- */
    case OP_F2IR: case OP_F2II: case OP_F2IC: {
        uint32_t v = fmod_(t, as_f(t, operand_b(t, in, 1)), bit(w, 49), bit(w, 45));
        static const enum GLSLstd450 round[4] = { GLSLstd450RoundEven, GLSLstd450Floor, GLSLstd450Ceil, GLSLstd450Trunc };
        v = ext1(t, t->t_f32, round[bits(w, 39, 2)], v);
        int fmt = (int)(bits(w, 12, 1) << 2 | bits(w, 8, 2));   /* U16 1, U32 2, S16 5, S32 6 */
        int is_signed = fmt >= 4;
        float lo = is_signed ? (fmt == 5 ? -32768.0f : -2147483648.0f) : 0.0f;
        float hi = is_signed ? (fmt == 5 ? 32767.0f : 2147483520.0f) : (fmt == 1 ? 65535.0f : 4294967040.0f);
        uint32_t clamped = ext3(t, t->t_f32, GLSLstd450FClamp, v, cf(t, lo), cf(t, hi));
        uint32_t r = is_signed ? as_u(t, op1(t, SpvOpConvertFToS, t->t_i32, clamped)) : op1(t, SpvOpConvertFToU, t->t_u32, clamped);
        if (fmt == 2 || fmt == 6) {                       /* values the clamp couldn't represent */
            uint32_t over = op2(t, SpvOpFOrdGreaterThanEqual, t->t_bool, v, cf(t, is_signed ? 2147483648.0f : 4294967296.0f));
            r = select_(t, t->t_u32, over, cu(t, is_signed ? 0x7FFFFFFF : 0xFFFFFFFF), r);
        }
        set_u(t, rd, select_(t, t->t_u32, isnan_(t, v), cu(t, 0), r));
        return 1;
    }
    case OP_I2FR: case OP_I2FI: case OP_I2FC: {
        int fmt = (int)(bits(w, 13, 1) << 2 | bits(w, 10, 2));  /* U8 0, U16 1, U32 2, S8 4, S16 5, S32 6 */
        int is_signed = fmt >= 4, size = fmt & 3, sel = (int)bits(w, 41, 2);
        uint32_t v = operand_b(t, in, 0);
        if (size == 0) v = op3(t, is_signed ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, v, cu(t, 8u * sel), cu(t, 8));
        else if (size == 1) v = op3(t, is_signed ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, v, cu(t, 16u * (sel >> 1)), cu(t, 16));
        else if (size == 3) { fail(t, "64-bit I2F"); return 1; }
        if (bit(w, 49)) v = as_u(t, ext1(t, t->t_i32, GLSLstd450SAbs, as_i(t, v)));
        if (bit(w, 45)) v = ineg(t, v);
        uint32_t f = is_signed ? op1(t, SpvOpConvertSToF, t->t_f32, as_i(t, v)) : op1(t, SpvOpConvertUToF, t->t_f32, v);
        if (bits(w, 8, 2) != 2) { fail(t, "I2F to a non-32-bit float"); return 1; }
        set_f(t, rd, f);
        return 1;
    }
    case OP_F2FR: case OP_F2FI: case OP_F2FC: {
        int dst = (int)bits(w, 8, 2), src = (int)bits(w, 10, 2);         /* 1 F16, 2 F32, 3 F64 */
        int mode = (int)(bits(w, 42, 1) << 2 | bits(w, 39, 2));
        uint32_t v = operand_b(t, in, 1);
        if (src == 1) {                                   /* half: low or high 16 bits (Sh) */
            uint32_t h = bit(w, 41) ? op2(t, SpvOpShiftRightLogical, t->t_u32, v, cu(t, 16)) : v;
            v = op2(t, SpvOpCompositeExtract, t->t_f32, ext1(t, t->t_v2f, GLSLstd450UnpackHalf2x16, h), 0);
        } else if (src == 2) {
            v = as_f(t, v);
        } else { fail(t, "F2F from 64-bit"); return 1; }
        v = fmod_(t, v, bit(w, 49), bit(w, 45));
        if (mode >= 4) {
            static const enum GLSLstd450 round[4] = { GLSLstd450RoundEven, GLSLstd450Floor, GLSLstd450Ceil, GLSLstd450Trunc };
            v = ext1(t, t->t_f32, round[mode - 4], v);
        }
        v = sat(t, v, bit(w, 50));
        if (dst == 1) {
            uint32_t pair = op2(t, SpvOpCompositeConstruct, t->t_v2f, v, cf(t, 0.0f));
            set_u(t, rd, ext1(t, t->t_u32, GLSLstd450PackHalf2x16, pair));
        } else if (dst == 2) {
            set_f(t, rd, v);
        } else { fail(t, "F2F to 64-bit"); }
        return 1;
    }
    case OP_I2IR: case OP_I2II: case OP_I2IC: {
        int dfmt = (int)(bits(w, 12, 1) << 2 | bits(w, 8, 2)), sfmt = (int)(bits(w, 13, 1) << 2 | bits(w, 10, 2));
        int sel = (int)bits(w, 41, 2);
        uint32_t v = operand_b(t, in, 0);
        int ssigned = sfmt >= 4, ssize = sfmt & 3, dsigned = dfmt >= 4, dsize = dfmt & 3;
        if (ssize == 0) v = op3(t, ssigned ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, v, cu(t, 8u * sel), cu(t, 8));
        else if (ssize == 1) v = op3(t, ssigned ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, v, cu(t, 16u * (sel >> 1)), cu(t, 16));
        if (bit(w, 49)) v = as_u(t, ext1(t, t->t_i32, GLSLstd450SAbs, as_i(t, v)));
        if (bit(w, 45)) v = ineg(t, v);
        if (bit(w, 50) && dsize < 2) {                   /* saturate into the smaller type */
            int bitsz = dsize == 0 ? 8 : 16;
            if (dsigned) v = as_u(t, ext3(t, t->t_i32, GLSLstd450SClamp, as_i(t, v), ci(t, -(1 << (bitsz - 1))), ci(t, (1 << (bitsz - 1)) - 1)));
            else v = ssigned ? as_u(t, ext3(t, t->t_i32, GLSLstd450SClamp, as_i(t, v), ci(t, 0), ci(t, (1 << bitsz) - 1)))
                             : ext2(t, t->t_u32, GLSLstd450UMin, v, cu(t, (1u << bitsz) - 1));
        }
        if (dsize < 2) v = op3(t, dsigned ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, v, cu(t, 0), cu(t, dsize == 0 ? 8 : 16));
        set_u(t, rd, v);
        return 1;
    }

    /* ---- moves and selects ---- */
    case OP_MOVR: case OP_MOVI: case OP_MOVC: case OP_MOV32I:
        set_u(t, rd, operand_b(t, in, 0));
        if (bits(w, 16, 4) == PT && (in->op == OP_MOV32I || in->op == OP_MOVI))     /* unpredicated */
            set_known(t, rd, in->op == OP_MOV32I ? (uint32_t)(w >> 20) : (uint32_t)sext(bits(w, 20, 19) | bit(w, 56) << 19, 20));
        return 1;
    case OP_SELR: case OP_SELI: case OP_SELC:
        set_u(t, rd, select_(t, t->t_u32, get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42)), get_u(t, ra), operand_b(t, in, 0)));
        return 1;
    case OP_S2R: {
        uint32_t sr = bits(w, 20, 8);
        switch (sr) {
        case 0x00: case 0x11: set_u(t, rd, cu(t, 0)); return 1;     /* lane id, invocation id */
        case 0x12: set_u(t, rd, cu(t, 0x3F800000)); return 1;       /* Y direction: +1.0 */
        }
        fail(t, "S2R special register %#x", sr);
        return 1;
    }
    case OP_LDC: {
        int slot = (int)bits(w, 36, 5), size = (int)bits(w, 48, 3);
        int32_t imm = sext(bits(w, 20, 16), 16);
        if (bits(w, 44, 2)) { fail(t, "LDC address mode %u", bits(w, 44, 2)); return 1; }
        int words = size == 5 ? 2 : size == 6 ? 4 : 1;
        if (size < 4) { fail(t, "LDC of %d-bit data", size < 2 ? 8 : 16); return 1; }
        for (int i = 0; i < words; i++) {
            uint32_t v;
            if (ra == RZ) v = cbuf_u(t, slot, (uint32_t)imm + 4u * i);
            else v = cbuf_u_dyn(t, slot, iadd(t, get_u(t, ra), cu(t, (uint32_t)imm + 4u * i)));
            set_u(t, rd + i, v);
        }
        return 1;
    }

    /* ---- integer arithmetic ---- */
    case OP_IADDR: case OP_IADDI: case OP_IADDC: case OP_IADD32I: {
        int is32 = in->op == OP_IADD32I;
        int avg = (int)(is32 ? bits(w, 55, 2) : bits(w, 48, 2)), x = bit(w, is32 ? 53 : 43), cc = bit(w, is32 ? 52 : 47);
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0);
        if (avg == 2) a = ineg(t, a);
        if (avg == 1) b = ineg(t, b);
        uint32_t r = iadd(t, a, b);
        if (x) r = iadd(t, r, carry_u(t));
        if (avg == 3) r = iadd(t, r, cu(t, 1));
        if (cc) set_add_flags(t, r, a, b, x);
        set_u(t, rd, r);
        return 1;
    }
    case OP_ISCADDR: case OP_ISCADDI: case OP_ISCADDC: case OP_ISCADD32I: {
        int is32 = in->op == OP_ISCADD32I, avg = is32 ? 0 : (int)bits(w, 48, 2);
        uint32_t a = op2(t, SpvOpShiftLeftLogical, t->t_u32, get_u(t, ra), cu(t, bits(w, is32 ? 53 : 39, 5)));
        uint32_t b = operand_b(t, in, 0);
        if (avg == 2) a = ineg(t, a);
        if (avg == 1) b = ineg(t, b);
        set_u(t, rd, iadd(t, a, b));
        return 1;
    }
    case OP_XMADR: case OP_XMADI: case OP_XMADC: case OP_XMADRC: {
        int f = form_of(in->op), cbuf_form = f == FORM_C || f == FORM_RC;
        int high_a = bit(w, 53), asigned = bit(w, 48), bsigned = bit(w, 49);
        int high_b = f == FORM_I ? 0 : cbuf_form ? bit(w, 52) : bit(w, 35);
        int psl = cbuf_form ? (f == FORM_C ? bit(w, 55) : 0) : bit(w, 36);
        int mrg = cbuf_form ? (f == FORM_C ? bit(w, 56) : 0) : bit(w, 37);
        int x = cbuf_form ? bit(w, 54) : bit(w, 38);
        int mode = (int)(cbuf_form ? bits(w, 50, 2) : bits(w, 50, 3));    /* full, lo, hi, sfu, (bcc) */
        uint32_t b_raw = f == FORM_I ? cu(t, bits(w, 20, 16)) : operand_b(t, in, 0), c = operand_c(t, in);
        uint32_t a = half16(t, get_u(t, ra), high_a, asigned), b = half16(t, b_raw, high_b, bsigned);
        uint32_t r = op2(t, SpvOpIMul, t->t_u32, a, b);
        if (psl) r = op2(t, SpvOpShiftLeftLogical, t->t_u32, r, cu(t, 16));
        if (mode == 1) c = half16(t, c, 0, 0);
        else if (mode == 2) c = half16(t, c, 1, 0);
        else if (mode == 3) {
            uint32_t sa = op2(t, SpvOpShiftLeftLogical, t->t_u32, op2(t, SpvOpShiftRightLogical, t->t_u32, a, cu(t, 31)), cu(t, 16));
            uint32_t sb = op2(t, SpvOpShiftLeftLogical, t->t_u32, op2(t, SpvOpShiftRightLogical, t->t_u32, b, cu(t, 31)), cu(t, 16));
            c = op2(t, SpvOpISub, t->t_u32, c, iadd(t, sa, sb));
        } else if (mode == 4) {
            c = iadd(t, c, op2(t, SpvOpShiftLeftLogical, t->t_u32, b_raw, cu(t, 16)));
        }
        uint32_t product = r;
        r = x ? iadd(t, r, carry_u(t)) : iadd(t, r, c);
        if (bit(w, 47)) set_add_flags(t, r, product, c, x);
        if (mrg) r = op2(t, SpvOpBitwiseOr, t->t_u32, op2(t, SpvOpBitwiseAnd, t->t_u32, r, cu(t, 0xFFFF)),
                         op2(t, SpvOpShiftLeftLogical, t->t_u32, b_raw, cu(t, 16)));
        set_u(t, rd, r);
        return 1;
    }
    case OP_IMNMXR: case OP_IMNMXI: case OP_IMNMXC: {
        int is_signed = bit(w, 48);
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0);
        uint32_t is_min = get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42));
        uint32_t mn = is_signed ? as_u(t, ext2(t, t->t_i32, GLSLstd450SMin, as_i(t, a), as_i(t, b))) : ext2(t, t->t_u32, GLSLstd450UMin, a, b);
        uint32_t mx = is_signed ? as_u(t, ext2(t, t->t_i32, GLSLstd450SMax, as_i(t, a), as_i(t, b))) : ext2(t, t->t_u32, GLSLstd450UMax, a, b);
        set_u(t, rd, select_(t, t->t_u32, is_min, mn, mx));
        return 1;
    }
    case OP_ISETPR: case OP_ISETPI: case OP_ISETPC: case OP_ISETR: case OP_ISETI: case OP_ISETC: {
        if (bit(w, 43)) { fail(t, "extended ISETP/ISET"); return 1; }
        uint32_t res = icompare(t, (int)bits(w, 49, 3), bit(w, 48), get_u(t, ra), operand_b(t, in, 0));
        if (in->op == OP_ISETPR || in->op == OP_ISETPI || in->op == OP_ISETPC) {
            set_compare_preds(t, w, res);
        } else {
            uint32_t r = bool_op(t, (int)bits(w, 45, 2), res, get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42)));
            set_u(t, rd, select_(t, t->t_u32, r, cu(t, bit(w, 44) ? 0x3F800000u : 0xFFFFFFFFu), cu(t, 0)));
        }
        return 1;
    }
    case OP_PSETP: {
        uint32_t p1 = get_pred_operand(t, (int)bits(w, 12, 3), bit(w, 15));
        uint32_t p2 = get_pred_operand(t, (int)bits(w, 29, 3), bit(w, 32));
        uint32_t ab = bool_op(t, (int)bits(w, 24, 2), p1, p2);
        uint32_t src = get_pred_operand(t, (int)bits(w, 39, 3), bit(w, 42));
        int op = (int)bits(w, 45, 2);
        set_p(t, (int)bits(w, 3, 3), bool_op(t, op, ab, src));
        set_p(t, (int)bits(w, 0, 3), bool_op(t, op, op1(t, SpvOpLogicalNot, t->t_bool, ab), src));
        return 1;
    }

    /* ---- shifts and logic ---- */
    case OP_SHLR: case OP_SHLI: case OP_SHLC:
        set_u(t, rd, shift(t, SpvOpShiftLeftLogical, get_u(t, ra), operand_b(t, in, 0), bit(w, 39), 0));
        return 1;
    case OP_SHRR: case OP_SHRI: case OP_SHRC: {
        uint32_t a = get_u(t, ra), n = operand_b(t, in, 0);
        if (bit(w, 40)) a = op1(t, SpvOpBitReverse, t->t_u32, a);
        set_u(t, rd, bit(w, 48) ? shift(t, SpvOpShiftRightArithmetic, a, n, bit(w, 39), 1)
                                : shift(t, SpvOpShiftRightLogical, a, n, bit(w, 39), 0));
        return 1;
    }
    case OP_LOPR: case OP_LOPI: case OP_LOPC: {
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0);
        if (bit(w, 39)) a = inot(t, a);
        if (bit(w, 40)) b = inot(t, b);
        uint32_t r = logic(t, (int)bits(w, 41, 2), a, b);
        set_u(t, rd, r);
        logic_pred(t, (int)bits(w, 44, 2), (int)bits(w, 48, 3), r);
        return 1;
    }
    case OP_LOP32I: {
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0);
        if (bit(w, 55)) a = inot(t, a);
        if (bit(w, 56)) b = inot(t, b);
        set_u(t, rd, logic(t, (int)bits(w, 53, 2), a, b));
        return 1;
    }
    case OP_LOP3R: case OP_LOP3I: case OP_LOP3C: {
        /* any function of 3 inputs, as a truth table: bit (a<<2 | b<<1 | c) of the lut */
        uint32_t lut = in->op == OP_LOP3R ? bits(w, 28, 8) : bits(w, 48, 8);
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0), c = get_u(t, (int)bits(w, 39, 8));
        uint32_t na = inot(t, a), nb = inot(t, b), nc = inot(t, c), r = cu(t, 0);
        for (int i = 0; i < 8; i++) {
            if (!(lut >> i & 1)) continue;
            uint32_t term = op2(t, SpvOpBitwiseAnd, t->t_u32, i & 4 ? a : na, i & 2 ? b : nb);
            term = op2(t, SpvOpBitwiseAnd, t->t_u32, term, i & 1 ? c : nc);
            r = op2(t, SpvOpBitwiseOr, t->t_u32, r, term);
        }
        set_u(t, rd, r);
        if (in->op == OP_LOP3R) logic_pred(t, (int)bits(w, 36, 2), (int)bits(w, 48, 3), r);
        return 1;
    }
    case OP_BFER: case OP_BFEI: case OP_BFEC: {           /* B: position (bits 0-7), length (bits 8-15) */
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0);
        if (bit(w, 40)) a = op1(t, SpvOpBitReverse, t->t_u32, a);
        uint32_t pos = op2(t, SpvOpBitwiseAnd, t->t_u32, b, cu(t, 0xFF));
        uint32_t len = op3(t, SpvOpBitFieldUExtract, t->t_u32, b, cu(t, 8), cu(t, 8));
        uint32_t count = bf_clamp_count(t, pos, len);
        uint32_t safe_pos = ext2(t, t->t_u32, GLSLstd450UMin, pos, cu(t, 31));
        uint32_t r = bit(w, 48) ? as_u(t, op3(t, SpvOpBitFieldSExtract, t->t_i32, as_i(t, a), safe_pos, count))
                               : op3(t, SpvOpBitFieldUExtract, t->t_u32, a, safe_pos, count);
        set_u(t, rd, select_(t, t->t_u32, op2(t, SpvOpIEqual, t->t_bool, count, cu(t, 0)), cu(t, 0), r));
        return 1;
    }
    case OP_BFIR: case OP_BFII: case OP_BFIC: case OP_BFIRC: {  /* insert A into C at B's position/length */
        uint32_t a = get_u(t, ra), b = operand_b(t, in, 0), c = operand_c(t, in);
        uint32_t pos = op2(t, SpvOpBitwiseAnd, t->t_u32, b, cu(t, 0xFF));
        uint32_t len = op3(t, SpvOpBitFieldUExtract, t->t_u32, b, cu(t, 8), cu(t, 8));
        uint32_t count = bf_clamp_count(t, pos, len);
        uint32_t safe_pos = ext2(t, t->t_u32, GLSLstd450UMin, pos, cu(t, 31));
        uint32_t r = spv_id(&t->s);
        spv_op(&t->s, SEC_BODY, SpvOpBitFieldInsert, 6, t->t_u32, r, c, a, safe_pos, count);
        set_u(t, rd, r);
        return 1;
    }
    case OP_POPCR: case OP_POPCI: case OP_POPCC: {
        uint32_t b = operand_b(t, in, 0);
        if (bit(w, 40)) b = inot(t, b);
        set_u(t, rd, as_u(t, op1(t, SpvOpBitCount, t->t_i32, b)));
        return 1;
    }
    case OP_FLOR: case OP_FLOI: case OP_FLOC: {           /* position of the highest set bit, -1 if none */
        uint32_t b = operand_b(t, in, 0);
        if (bit(w, 40)) b = inot(t, b);
        uint32_t r = as_u(t, ext1(t, t->t_i32, bit(w, 48) ? GLSLstd450FindSMsb : GLSLstd450FindUMsb, b));
        if (bit(w, 41)) {                                 /* .SH: shift amount (31 - position) */
            uint32_t none = op2(t, SpvOpIEqual, t->t_bool, r, cu(t, 0xFFFFFFFF));
            r = select_(t, t->t_u32, none, r, op2(t, SpvOpISub, t->t_u32, cu(t, 31), r));
        }
        set_u(t, rd, r);
        return 1;
    }

    /* ---- nothing to do ---- */
    case OP_MEMBAR: case OP_CCTL: case OP_CCTLL:
        return 1;

    default:
        return 0;
    }
}
