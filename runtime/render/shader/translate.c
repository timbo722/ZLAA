/*
 * translate.c - Maxwell shader -> SPIR-V: setup, control flow, registers, inputs and outputs.
 * The instructions themselves are in alu.c and texture.c.
 *
 * Control flow. SPIR-V only allows "structured" control flow (if/else, loops, switch, each
 * with a declared merge point), while Maxwell code jumps freely. So:
 *   - a shader with no jumps is emitted straight;
 *   - otherwise we cut it into basic blocks and emit
 *         pc = 0; loop { switch (pc) { case 0: ...; pc = 3; break; case 1: ... } }
 *     which is structured whatever the jumps are. Each block ends by setting pc.
 * SSY/SYNC and PBK/BRK (the hardware's "jump back to the point pushed earlier") are resolved
 * here, once, by following the pushes along every path: each SYNC/BRK gets a fixed target.
 */
#include "translate_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ helpers */

void fail(T *t, const char *fmt, ...) {
    if (t->info->error[0]) return;                   /* keep the first error */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->info->error, sizeof t->info->error, fmt, ap);
    va_end(ap);
}

uint32_t ptr_type(T *t, SpvStorageClass sc, uint32_t type) {
    uint32_t o[2] = { sc, type };
    return spv_type(&t->s, SpvOpTypePointer, o, 2);
}
uint32_t cu(T *t, uint32_t v) { return spv_constant(&t->s, t->t_u32, v); }
uint32_t ci(T *t, int32_t v) { return spv_constant(&t->s, t->t_i32, (uint32_t)v); }
uint32_t cf(T *t, float v) { uint32_t u; memcpy(&u, &v, 4); return spv_constant(&t->s, t->t_f32, u); }
uint32_t cb(T *t, int v) { return spv_bool_constant(&t->s, t->t_bool, v); }

uint32_t op1(T *t, SpvOp op, uint32_t type, uint32_t a) {
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, op, 3, type, id, a);
    return id;
}
uint32_t op2(T *t, SpvOp op, uint32_t type, uint32_t a, uint32_t b) {
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, op, 4, type, id, a, b);
    return id;
}
uint32_t op3(T *t, SpvOp op, uint32_t type, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, op, 5, type, id, a, b, c);
    return id;
}
uint32_t ext1(T *t, uint32_t type, enum GLSLstd450 f, uint32_t a) {
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpExtInst, 5, type, id, t->glsl, (uint32_t)f, a);
    return id;
}
uint32_t ext2(T *t, uint32_t type, enum GLSLstd450 f, uint32_t a, uint32_t b) {
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpExtInst, 6, type, id, t->glsl, (uint32_t)f, a, b);
    return id;
}
uint32_t ext3(T *t, uint32_t type, enum GLSLstd450 f, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpExtInst, 7, type, id, t->glsl, (uint32_t)f, a, b, c);
    return id;
}
uint32_t bitcast(T *t, uint32_t type, uint32_t v) { return op1(t, SpvOpBitcast, type, v); }
uint32_t select_(T *t, uint32_t type, uint32_t cond, uint32_t a, uint32_t b) { return op3(t, SpvOpSelect, type, cond, a, b); }

uint32_t label(T *t, uint32_t id) {
    if (!id) id = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpLabel, 1, id);
    t->terminated = 0;
    return id;
}
void branch(T *t, uint32_t to) {
    if (t->terminated) return;
    spv_op(&t->s, SEC_BODY, SpvOpBranch, 1, to);
    t->terminated = 1;
}

/* a local variable of `type`, declared at the top of the function */
static uint32_t local_var(T *t, uint32_t type, uint32_t init) {
    uint32_t id = spv_id(&t->s);
    if (init) spv_op(&t->s, SEC_VARS, SpvOpVariable, 4, ptr_type(t, SpvStorageClassFunction, type), id,
                     SpvStorageClassFunction, init);
    else spv_op(&t->s, SEC_VARS, SpvOpVariable, 3, ptr_type(t, SpvStorageClassFunction, type), id,
                SpvStorageClassFunction);
    return id;
}

static uint32_t load(T *t, uint32_t type, uint32_t ptr) { return op1(t, SpvOpLoad, type, ptr); }
static void store(T *t, uint32_t ptr, uint32_t v) { spv_op(&t->s, SEC_BODY, SpvOpStore, 2, ptr, v); }

/* ================================================================ registers */

/* Registers start at 0: the hardware leaves them undefined, but a defined start is safer. */
static uint32_t reg_var(T *t, int r) {
    if (!t->reg[r]) t->reg[r] = local_var(t, t->t_u32, cu(t, 0));
    return t->reg[r];
}
/* (r > RZ: the next registers of a vector starting at RZ, e.g. a 64-bit store of RZ: zero too,
 * and they must not index past reg[]) */
uint32_t get_u(T *t, int r) { return r >= RZ ? cu(t, 0) : load(t, t->t_u32, reg_var(t, r)); }
uint32_t get_f(T *t, int r) { return r == RZ ? cf(t, 0.0f) : bitcast(t, t->t_f32, get_u(t, r)); }
void set_u(T *t, int r, uint32_t v) {
    if (r >= RZ) return;
    store(t, reg_var(t, r), v);
    t->known[r] = 0;
}
void set_known(T *t, int r, uint32_t value) {
    if (r >= RZ) return;
    t->known[r] = 1;
    t->known_value[r] = value;
}
void set_f(T *t, int r, uint32_t v) { if (r < RZ) set_u(t, r, bitcast(t, t->t_u32, v)); }

uint32_t get_p(T *t, int p) {
    if (p == PT) return cb(t, 1);
    if (!t->pred[p]) t->pred[p] = local_var(t, t->t_bool, cb(t, 0));
    return load(t, t->t_bool, t->pred[p]);
}
void set_p(T *t, int p, uint32_t v) {
    if (p == PT) return;
    if (!t->pred[p]) t->pred[p] = local_var(t, t->t_bool, cb(t, 0));
    store(t, t->pred[p], v);
}
uint32_t get_pred_operand(T *t, int p, int inv) {
    uint32_t v = get_p(t, p);
    return inv ? op1(t, SpvOpLogicalNot, t->t_bool, v) : v;
}

/* ================================================================ constant buffers */

/* c[slot] as a uniform buffer: struct { uvec4 data[4096]; } (64 KiB, the hardware's limit) */
static uint32_t cbuf_var(T *t, int slot) {
    if (t->cbuf[slot]) return t->cbuf[slot];
    uint32_t len = cu(t, 4096);
    uint32_t a[2] = { t->t_v4u, len };
    uint32_t arr = spv_type(&t->s, SpvOpTypeArray, a, 2);
    uint32_t strct = spv_type(&t->s, SpvOpTypeStruct, &arr, 1);
    if (!t->cbuf_decorated) {                             /* (the types are shared by every slot) */
        spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, arr, SpvDecorationArrayStride, 16);
        spv_op(&t->s, SEC_DECO, SpvOpMemberDecorate, 4, strct, 0, SpvDecorationOffset, 0);
        spv_op(&t->s, SEC_DECO, SpvOpDecorate, 2, strct, SpvDecorationBlock);
        t->cbuf_decorated = 1;
    }
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_TYPES, SpvOpVariable, 3, ptr_type(t, SpvStorageClassUniform, strct), id, SpvStorageClassUniform);
    spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, id, SpvDecorationDescriptorSet, 0);
    spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, id, SpvDecorationBinding,
           (uint32_t)t->stage * SHADER_STAGE_BINDINGS + SHADER_CBUF_BINDING + (uint32_t)slot);
    t->cbuf[slot] = id;
    t->info->cbuf_mask |= 1u << slot;
    return id;
}

uint32_t cbuf_u(T *t, int slot, uint32_t byte_offset) {
    if (slot > 17) { fail(t, "constant buffer %d", slot); return cu(t, 0); }
    if (slot == 1 && byte_offset + 4 <= t->c1_size) {   /* the shader's own constants: known now */
        uint32_t v;
        memcpy(&v, t->c1 + byte_offset, 4);
        return cu(t, v);
    }
    uint32_t var = cbuf_var(t, slot);
    if (byte_offset + 4 > t->info->cbuf_size[slot]) t->info->cbuf_size[slot] = byte_offset + 4;
    uint32_t chain = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpAccessChain, 6, ptr_type(t, SpvStorageClassUniform, t->t_u32), chain, var,
           cu(t, 0), cu(t, byte_offset / 16), cu(t, (byte_offset / 4) & 3));
    return load(t, t->t_u32, chain);
}

uint32_t cbuf_u_dyn(T *t, int slot, uint32_t offset) {
    if (slot > 17) { fail(t, "constant buffer %d", slot); return cu(t, 0); }
    uint32_t var = cbuf_var(t, slot);
    t->info->cbuf_size[slot] = 0x10000;                   /* could read anywhere */
    uint32_t vec = op2(t, SpvOpShiftRightLogical, t->t_u32, offset, cu(t, 4));
    vec = op2(t, SpvOpBitwiseAnd, t->t_u32, vec, cu(t, 4095));
    uint32_t comp = op2(t, SpvOpBitwiseAnd, t->t_u32, op2(t, SpvOpShiftRightLogical, t->t_u32, offset, cu(t, 2)), cu(t, 3));
    uint32_t chain = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpAccessChain, 6, ptr_type(t, SpvStorageClassUniform, t->t_u32), chain, var,
           cu(t, 0), vec, comp);
    return load(t, t->t_u32, chain);
}

/* ================================================================ operand forms */

int form_of(MaxwellOp op) {
    const char *n = NULL;
    for (size_t i = 0; i < sizeof maxwell_ops / sizeof *maxwell_ops; i++)
        if (maxwell_ops[i].op == op) { n = maxwell_ops[i].name; break; }
    if (!n) return FORM_OTHER;
    size_t len = strlen(n);
    if (len > 3 && strcmp(n + len - 3, "32I") == 0) return FORM_32I;
    if (len > 2 && strcmp(n + len - 2, "RC") == 0) return FORM_RC;
    switch (n[len - 1]) {
    case 'R': return FORM_R;
    case 'I': return FORM_I;
    case 'C': return FORM_C;
    }
    return FORM_OTHER;
}

uint32_t operand_b(T *t, const Inst *in, int float_imm) {
    uint64_t w = in->w;
    switch (form_of(in->op)) {
    case FORM_R:   return get_u(t, (int)bits(w, 20, 8));
    case FORM_RC:  return get_u(t, (int)bits(w, 39, 8));
    case FORM_C:   return cbuf_u(t, (int)bits(w, 34, 5), bits(w, 20, 14) * 4);
    case FORM_32I: return cu(t, (uint32_t)(w >> 20));
    case FORM_I: {
        uint32_t imm = bits(w, 20, 19) | bit(w, 56) << 19;
        return cu(t, float_imm ? imm << 12 : (uint32_t)sext(imm, 20));
    }
    }
    fail(t, "operand B of an instruction without one");
    return cu(t, 0);
}

uint32_t operand_c(T *t, const Inst *in) {
    uint64_t w = in->w;
    if (form_of(in->op) == FORM_RC) return cbuf_u(t, (int)bits(w, 34, 5), bits(w, 20, 14) * 4);
    return get_u(t, (int)bits(w, 39, 8));
}

/* ================================================================ inputs and outputs */

static uint32_t io_var(T *t, SpvStorageClass sc, uint32_t type) {
    uint32_t id = spv_id(&t->s);
    if (sc == SpvStorageClassOutput && t->stage == SHADER_VERTEX && type == t->t_v4f) {
        /* vertex outputs start as (0, 0, 0, 1), as on the GPU: shaders often write only some
         * components (e.g. a full-screen pass sets position x, y and leaves w = 1) */
        uint32_t c[4] = { cf(t, 0.0f), cf(t, 0.0f), cf(t, 0.0f), cf(t, 1.0f) };
        spv_op(&t->s, SEC_TYPES, SpvOpVariable, 4, ptr_type(t, sc, type), id, sc,
               spv_composite_constant(&t->s, t->t_v4f, c, 4));
    } else {
        spv_op(&t->s, SEC_TYPES, SpvOpVariable, 3, ptr_type(t, sc, type), id, sc);
    }
    if (t->ninterface < 64) t->interface[t->ninterface++] = id;
    return id;
}
static uint32_t builtin_var(T *t, SpvStorageClass sc, uint32_t type, SpvBuiltIn b) {
    uint32_t id = io_var(t, sc, type);
    spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, id, SpvDecorationBuiltIn, b);
    return id;
}

/* generic attribute i: vec4 at location i */
static uint32_t attr_var(T *t, int output, int i) {
    uint32_t *slot = output ? &t->out_attr[i] : &t->in_attr[i];
    if (*slot) return *slot;
    *slot = io_var(t, output ? SpvStorageClassOutput : SpvStorageClassInput, t->t_v4f);
    spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, *slot, SpvDecorationLocation, (uint32_t)i);
    if (!output && t->stage == SHADER_FRAGMENT) {
        /* how the rasterizer interpolates it: from the shader header's input map */
        uint32_t imap = (t->sph[6 + i / 4] >> (8 * (i % 4))) & 0xFF;
        uint32_t type = imap & 3 ? imap & 3 : imap >> 2 & 3 ? imap >> 2 & 3 : imap >> 4 & 3 ? imap >> 4 & 3 : imap >> 6;
        if (type == 1) spv_op(&t->s, SEC_DECO, SpvOpDecorate, 2, *slot, SpvDecorationFlat);
        if (type == 3 && !getenv("LAA_NO_NOPERSPECTIVE")) spv_op(&t->s, SEC_DECO, SpvOpDecorate, 2, *slot, SpvDecorationNoPerspective);
    }
    if (output) t->info->outputs |= 1u << i;
    else t->info->inputs |= 1u << i;
    return *slot;
}

static uint32_t component_ptr(T *t, SpvStorageClass sc, uint32_t vec_var, uint32_t comp) {
    uint32_t chain = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpAccessChain, 4, ptr_type(t, sc, t->t_f32), chain, vec_var, cu(t, comp));
    return chain;
}

/* This invocation's lane in its subgroup (the GPU runs 32 of them together as a "warp") */
uint32_t subgroup_lane(T *t) {
    if (!t->in_lane) {
        t->in_lane = builtin_var(t, SpvStorageClassInput, t->t_u32, SpvBuiltInSubgroupLocalInvocationId);
        if (t->stage == SHADER_FRAGMENT) spv_op(&t->s, SEC_DECO, SpvOpDecorate, 2, t->in_lane, SpvDecorationFlat);
    }
    return load(t, t->t_u32, t->in_lane);
}

/* compute: gl_LocalInvocationID (thread in its workgroup) / gl_WorkGroupID, one component */
uint32_t compute_id(T *t, int workgroup, int component) {
    uint32_t *var = workgroup ? &t->in_workgroup_id : &t->in_local_id;
    if (!t->t_v3u) t->t_v3u = spv_type(&t->s, SpvOpTypeVector, (uint32_t[]){ t->t_u32, 3 }, 2);
    if (!*var) *var = builtin_var(t, SpvStorageClassInput, t->t_v3u, workgroup ? SpvBuiltInWorkgroupId : SpvBuiltInLocalInvocationId);
    uint32_t chain = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpAccessChain, 4, ptr_type(t, SpvStorageClassInput, t->t_u32), chain, *var, cu(t, (uint32_t)component));
    return load(t, t->t_u32, chain);
}

/* The fragment shader's gl_FragCoord (x, y, z, 1/w) */
static uint32_t frag_coord(T *t) {
    if (!t->frag_coord) t->frag_coord = builtin_var(t, SpvStorageClassInput, t->t_v4f, SpvBuiltInFragCoord);
    return t->frag_coord;
}

/* Read the 32-bit attribute at address `addr` (input side): as uint bits */
static uint32_t read_attribute(T *t, uint32_t addr) {
    if (addr >= 0x80 && addr < 0x280) {
        uint32_t v = load(t, t->t_f32, component_ptr(t, SpvStorageClassInput, attr_var(t, 0, (int)(addr - 0x80) / 16), (addr / 4) & 3));
        return bitcast(t, t->t_u32, v);
    }
    if (t->stage == SHADER_FRAGMENT && addr >= 0x70 && addr < 0x80)
        return bitcast(t, t->t_u32, load(t, t->t_f32, component_ptr(t, SpvStorageClassInput, frag_coord(t), (addr / 4) & 3)));
    if (addr == 0x2fc || addr == 0x2f8) {                 /* vertex id / instance id */
        uint32_t *v = addr == 0x2fc ? &t->in_vertex_index : &t->in_instance_index;
        if (!*v) *v = builtin_var(t, SpvStorageClassInput, t->t_i32, addr == 0x2fc ? SpvBuiltInVertexIndex : SpvBuiltInInstanceIndex);
        uint32_t id = load(t, t->t_i32, *v);
        if (addr == 0x2f8) {
            /* Vulkan's instance index counts from the draw's first instance, Maxwell's from 0
             * (the text renderer draws its glyphs as instances, starting further along) */
            if (!t->in_base_instance) t->in_base_instance = builtin_var(t, SpvStorageClassInput, t->t_i32, SpvBuiltInBaseInstance);
            id = op2(t, SpvOpISub, t->t_i32, id, load(t, t->t_i32, t->in_base_instance));
        }
        return bitcast(t, t->t_u32, id);
    }
    if (addr == 0x3fc) {                                  /* front facing: -1 (all bits) or 0 */
        if (!t->in_front_facing) t->in_front_facing = builtin_var(t, SpvStorageClassInput, t->t_bool, SpvBuiltInFrontFacing);
        return select_(t, t->t_u32, load(t, t->t_bool, t->in_front_facing), cu(t, 0xFFFFFFFF), cu(t, 0));
    }
    fail(t, "read of attribute %#x", addr);
    return cu(t, 0);
}

static void write_attribute(T *t, uint32_t addr, uint32_t value_u) {
    uint32_t f = bitcast(t, t->t_f32, value_u);
    if (addr >= 0x70 && addr < 0x80) {
        if (!t->out_position) t->out_position = builtin_var(t, SpvStorageClassOutput, t->t_v4f, SpvBuiltInPosition);
        store(t, component_ptr(t, SpvStorageClassOutput, t->out_position, (addr / 4) & 3), f);
    } else if (addr >= 0x80 && addr < 0x280) {
        store(t, component_ptr(t, SpvStorageClassOutput, attr_var(t, 1, (int)(addr - 0x80) / 16), (addr / 4) & 3), f);
    } else if (addr == 0x6c) {
        if (!t->out_point_size) t->out_point_size = builtin_var(t, SpvStorageClassOutput, t->t_f32, SpvBuiltInPointSize);
        store(t, t->out_point_size, f);
    } else if (addr == 0x64 || addr == 0x68) {
        uint32_t *v = addr == 0x64 ? &t->out_layer : &t->out_viewport;
        if (!*v) *v = builtin_var(t, SpvStorageClassOutput, t->t_i32, addr == 0x64 ? SpvBuiltInLayer : SpvBuiltInViewportIndex);
        store(t, *v, bitcast(t, t->t_i32, value_u));
    } else if (addr >= 0x2c0 && addr < 0x2e0) {
        fail(t, "clip distance output");
    } else {
        fail(t, "write of attribute %#x", addr);
    }
}

/* ALD / AST / IPA */
int emit_io(T *t, const Inst *in) {
    uint64_t w = in->w;
    switch (in->op) {
    case OP_ALD: {                                        /* Rd.. = a[Ra + imm11], 1-4 words */
        int rd = (int)bits(w, 0, 8), ra = (int)bits(w, 8, 8), n = (int)bits(w, 47, 2) + 1;
        if (ra != RZ || bit(w, 31)) { fail(t, "indexed or patch ALD"); return 1; }
        for (int i = 0; i < n; i++) set_u(t, rd + i, read_attribute(t, bits(w, 20, 11) + 4u * i));
        return 1;
    }
    case OP_AST: {                                        /* a[Ra + imm11] = Rb.. */
        int rb = (int)bits(w, 0, 8), ra = (int)bits(w, 8, 8), n = (int)bits(w, 47, 2) + 1;
        if (ra != RZ || bit(w, 31)) { fail(t, "indexed or patch AST"); return 1; }
        for (int i = 0; i < n; i++) write_attribute(t, bits(w, 20, 11) + 4u * i, get_u(t, rb + i));
        return 1;
    }
    case OP_IPA: {
        int rd = (int)bits(w, 0, 8);
        uint32_t addr = bits(w, 28, 10);
        if (bit(w, 38)) { fail(t, "indexed IPA"); return 1; }
        uint32_t v = bitcast(t, t->t_f32, read_attribute(t, addr));
        /* The hardware interpolates attr/w; Vulkan gives the perspective-correct attr. Undo
         * that for perspective inputs: the shader multiplies by w itself (IPA.MUL below). */
        if (addr >= 0x80 && addr < 0x280) {
            int i = (int)(addr - 0x80) / 16;
            uint32_t imap = (t->sph[6 + i / 4] >> (8 * (i % 4))) & 0xFF, type = 0;
            for (int c = 0; c < 4 && !type; c++) type = (imap >> (2 * c)) & 3;
            if (type == 2) v = op2(t, SpvOpFMul, t->t_f32, v, load(t, t->t_f32, component_ptr(t, SpvStorageClassInput, frag_coord(t), 3)));
        }
        if (bits(w, 54, 2) == 1 && !(addr >= 0x70 && addr < 0x80)) v = op2(t, SpvOpFMul, t->t_f32, v, get_f(t, (int)bits(w, 20, 8)));
        if (bit(w, 51)) v = ext3(t, t->t_f32, GLSLstd450FClamp, v, cf(t, 0.0f), cf(t, 1.0f));
        set_f(t, rd, v);
        return 1;
    }
    default:
        return 0;
    }
}

/* ---- global memory: LDG / STG ----
 * Addresses are 64-bit GPU addresses (from c0, where the renderer puts the device address of
 * each storage buffer the game bound). We read and write through SPIR-V physical pointers.
 * An address is kept as a uvec2 (low word, high word), not a 64-bit integer: some GPUs (the
 * Thor's) have no 64-bit integers in shaders, and a uvec2 can be bitcast to a pointer directly. */

/* the 64-bit address a + (hi:lo), with the carry from the low word into the high word */
static uint32_t add64(T *t, uint32_t a, uint32_t lo, uint32_t hi) {
    uint32_t t_carry = spv_type(&t->s, SpvOpTypeStruct, (uint32_t[]){ t->t_u32, t->t_u32 }, 2);
    uint32_t sum = op2(t, SpvOpIAddCarry, t_carry, op2(t, SpvOpCompositeExtract, t->t_u32, a, 0), lo);
    uint32_t new_lo = op2(t, SpvOpCompositeExtract, t->t_u32, sum, 0);
    uint32_t carry = op2(t, SpvOpCompositeExtract, t->t_u32, sum, 1);
    uint32_t new_hi = op2(t, SpvOpIAdd, t->t_u32, op2(t, SpvOpCompositeExtract, t->t_u32, a, 1),
                          op2(t, SpvOpIAdd, t->t_u32, hi, carry));
    return op2(t, SpvOpCompositeConstruct, t->t_v2u, new_lo, new_hi);
}

static uint32_t address(T *t, const Inst *in) {
    int generic = in->op == OP_LD || in->op == OP_ST;   /* LD/ST: 32-bit offset, E at bit 52 */
    int ra = (int)bits(in->w, 8, 8), wide = bit(in->w, generic ? 52 : 45);
    uint32_t hi = wide && ra != RZ ? get_u(t, ra + 1) : cu(t, 0);
    uint32_t a = op2(t, SpvOpCompositeConstruct, t->t_v2u, get_u(t, ra), hi);
    int32_t off = generic ? (int32_t)bits(in->w, 20, 32) : sext(bits(in->w, 20, 24), 24);
    t->uses_pointers = 1;
    return add64(t, a, cu(t, (uint32_t)off), cu(t, off < 0 ? 0xFFFFFFFFu : 0));   /* offset sign-extended */
}

/* pointer to the uint at a 64-bit address (+ byte offset) */
static uint32_t word_ptr(T *t, uint32_t addr, uint32_t byte_offset) {
    if (byte_offset) addr = add64(t, addr, cu(t, byte_offset), cu(t, 0));
    return op1(t, SpvOpBitcast, ptr_type(t, SpvStorageClassPhysicalStorageBuffer, t->t_u32), addr);
}

/* ---- local memory: LDL / STL ----
 * Each invocation's own scratch memory (spilled registers, small arrays indexed by a register).
 * It becomes an array of uints in the function; its size is in the shader header (graphics; for
 * compute, a guess). An index past the end is clamped: SPIR-V has no out-of-bounds accesses. */
static uint32_t local_word_ptr(T *t, uint32_t byte_addr, uint32_t word) {
    if (!t->local_mem) {
        uint32_t bytes = t->stage == SHADER_COMPUTE ? 1024 : t->sph[1] & 0xFFFFFF;
        t->local_words = bytes < 16 ? 4 : (bytes + 3) / 4;
        uint32_t arr = spv_type(&t->s, SpvOpTypeArray, (uint32_t[]){ t->t_u32, cu(t, t->local_words) }, 2);
        t->local_mem = local_var(t, arr, 0);
    }
    uint32_t index = op2(t, SpvOpIAdd, t->t_u32, op2(t, SpvOpShiftRightLogical, t->t_u32, byte_addr, cu(t, 2)), cu(t, word));
    index = ext2(t, t->t_u32, GLSLstd450UMin, index, cu(t, t->local_words - 1));
    return op2(t, SpvOpAccessChain, ptr_type(t, SpvStorageClassFunction, t->t_u32), t->local_mem, index);
}

static int emit_local(T *t, const Inst *in) {
    uint64_t w = in->w;
    int rd = (int)bits(w, 0, 8), ra = (int)bits(w, 8, 8), size = (int)bits(w, 48, 3);   /* U8 S8 U16 S16 B32 B64 B128 */
    uint32_t addr = op2(t, SpvOpIAdd, t->t_u32, get_u(t, ra), cu(t, (uint32_t)sext(bits(w, 20, 24), 24)));
    int words = size == 5 ? 2 : size == 6 || size == 7 ? 4 : 1;
    if (size < 4) {                                  /* 8/16 bits: part of the word at the address */
        uint32_t p = local_word_ptr(t, addr, 0);
        uint32_t shift = op2(t, SpvOpShiftLeftLogical, t->t_u32, op2(t, SpvOpBitwiseAnd, t->t_u32, addr, cu(t, 3)), cu(t, 3));
        uint32_t count = cu(t, size < 2 ? 8 : 16);
        if (in->op == OP_LDL)
            set_u(t, rd, op3(t, size & 1 ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, load(t, t->t_u32, p), shift, count));
        else {
            uint32_t v = spv_id(&t->s);
            spv_op(&t->s, SEC_BODY, SpvOpBitFieldInsert, 6, t->t_u32, v, load(t, t->t_u32, p), get_u(t, rd), shift, count);
            store(t, p, v);
        }
        return 1;
    }
    for (int i = 0; i < words; i++) {
        uint32_t p = local_word_ptr(t, addr, (uint32_t)i);
        if (in->op == OP_LDL) set_u(t, rd + i, load(t, t->t_u32, p));
        else store(t, p, get_u(t, rd + i));
    }
    return 1;
}

int emit_memory(T *t, const Inst *in) {
    uint64_t w = in->w;
    int rd = (int)bits(w, 0, 8);
    if (in->op == OP_LDL || in->op == OP_STL) return emit_local(t, in);
    /* LDG/STG: global memory. LD/ST: "generic" addresses, which in these shaders are global too */
    if (in->op != OP_LDG && in->op != OP_STG && in->op != OP_LD && in->op != OP_ST) return 0;
    int generic = in->op == OP_LD || in->op == OP_ST;
    int size = (int)bits(w, generic ? 53 : 48, 3);      /* U8 S8 U16 S16 B32 B64 B128 */
    if (generic && bits(w, 58, 3) != 7) { fail(t, "LD/ST with a predicate operand"); return 1; }
    int load_op = in->op == OP_LDG || in->op == OP_LD;
    uint32_t a = address(t, in);
    int words = size == 5 ? 2 : size == 6 || size == 7 ? 4 : 1;
    if (load_op) {
        if (size < 4) {                                  /* small: the aligned word, then the bits */
            uint32_t lo = op2(t, SpvOpCompositeExtract, t->t_u32, a, 0);
            uint32_t aligned = op2(t, SpvOpCompositeConstruct, t->t_v2u, op2(t, SpvOpBitwiseAnd, t->t_u32, lo, cu(t, ~3u)),
                                   op2(t, SpvOpCompositeExtract, t->t_u32, a, 1));
            uint32_t v = spv_id(&t->s);
            spv_op(&t->s, SEC_BODY, SpvOpLoad, 5, t->t_u32, v, word_ptr(t, aligned, 0), SpvMemoryAccessAlignedMask, 4);
            uint32_t shift = op2(t, SpvOpShiftLeftLogical, t->t_u32, op2(t, SpvOpBitwiseAnd, t->t_u32, lo, cu(t, 3)), cu(t, 3));
            v = op3(t, size & 1 ? SpvOpBitFieldSExtract : SpvOpBitFieldUExtract, t->t_u32, v, shift, cu(t, size < 2 ? 8 : 16));
            set_u(t, rd, v);
            return 1;
        }
        for (int i = 0; i < words; i++) {
            uint32_t v = spv_id(&t->s);
            spv_op(&t->s, SEC_BODY, SpvOpLoad, 5, t->t_u32, v, word_ptr(t, a, 4u * i), SpvMemoryAccessAlignedMask, 4);
            set_u(t, rd + i, v);
        }
        return 1;
    }
    if (size < 4) { fail(t, "store of %d-bit data", size < 2 ? 8 : 16); return 1; }
    for (int i = 0; i < words; i++)
        spv_op(&t->s, SEC_BODY, SpvOpStore, 4, word_ptr(t, a, 4u * i), get_u(t, rd + i), SpvMemoryAccessAlignedMask, 4);
    return 1;
}

/* Fragment shaders return their colors in registers: each written render target takes 4
 * registers (R0-R3, then R4-R7, ...), depth comes after (see shader.h's header notes). */
uint64_t shader_capture_address;

void emit_outputs_at_exit(T *t) {
    if (t->stage == SHADER_VERTEX && (t->options & SHADER_DEBUG_CAPTURE) && shader_capture_address) {
        uint32_t slot = op2(t, SpvOpIAdd, t->t_u32, op2(t, SpvOpShiftLeftLogical, t->t_u32, read_attribute(t, 0x2f8), cu(t, 2)),
                            op2(t, SpvOpBitwiseAnd, t->t_u32, read_attribute(t, 0x2fc), cu(t, 3)));
        slot = ext2(t, t->t_u32, GLSLstd450UMin, slot, cu(t, 255));
        uint32_t base = op2(t, SpvOpCompositeConstruct, t->t_v2u, cu(t, (uint32_t)shader_capture_address),
                            cu(t, (uint32_t)(shader_capture_address >> 32)));
        base = add64(t, base, op2(t, SpvOpShiftLeftLogical, t->t_u32, slot, cu(t, 5)), cu(t, 0));
        t->uses_pointers = 1;
        for (int k = 0; k < 8; k++) {
            uint32_t var = k < 4 ? t->out_position : t->out_attr[1];
            uint32_t v = var ? load(t, t->t_f32, component_ptr(t, SpvStorageClassOutput, var, (uint32_t)(k & 3))) : cf(t, -1.0f);
            spv_op(&t->s, SEC_BODY, SpvOpStore, 4, word_ptr(t, base, 4u * (uint32_t)k), bitcast(t, t->t_u32, v),
                   SpvMemoryAccessAlignedMask, 4);
        }
    }
    if (t->stage == SHADER_VERTEX && (t->options & SHADER_GL_DEPTH) && t->out_position) {
        uint32_t zp = component_ptr(t, SpvStorageClassOutput, t->out_position, 2);
        uint32_t z = load(t, t->t_f32, zp);
        uint32_t w = load(t, t->t_f32, component_ptr(t, SpvStorageClassOutput, t->out_position, 3));
        store(t, zp, op2(t, SpvOpFMul, t->t_f32, op2(t, SpvOpFAdd, t->t_f32, z, w), cf(t, 0.5f)));
    }
    if (t->stage != SHADER_FRAGMENT) return;
    uint32_t omap = t->sph[18];
    int base = 0;
    for (int rt = 0; rt < 8; rt++) {
        if (!(omap >> (4 * rt) & 0xF)) continue;
        if (!t->out_color[rt]) {
            t->out_color[rt] = io_var(t, SpvStorageClassOutput, t->t_v4f);
            spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, t->out_color[rt], SpvDecorationLocation, (uint32_t)rt);
            t->info->color_outputs |= 1u << rt;
        }
        /* (debugging: 0.5, or the input attribute given in the options' upper bits) */
        int debug_input = (t->options >> 8) - 1;
        uint32_t debug_vec = t->options & SHADER_DEBUG_CONST && debug_input >= 0 ? load(t, t->t_v4f, attr_var(t, 0, debug_input)) : 0;
        for (int c = 0; c < 4; c++)
            if (omap >> (4 * rt + c) & 1)
                store(t, component_ptr(t, SpvStorageClassOutput, t->out_color[rt], (uint32_t)c),
                      !(t->options & SHADER_DEBUG_CONST) ? get_f(t, base + c)
                      : debug_vec ? op2(t, SpvOpCompositeExtract, t->t_f32, debug_vec, (uint32_t)c) : cf(t, 0.5f));
        base += 4;
    }
    if (t->sph[19] & 2) {
        if (!t->out_depth) t->out_depth = builtin_var(t, SpvStorageClassOutput, t->t_f32, SpvBuiltInFragDepth);
        int popcount = 0;
        for (uint32_t m = omap; m; m &= m - 1) popcount++;
        store(t, t->out_depth, get_f(t, popcount + 1));
        t->info->writes_depth = 1;
    }
}

/* ================================================================ decoding and control flow */

static MaxwellOp decode(uint64_t w) {
    for (size_t i = 0; i < sizeof maxwell_ops / sizeof *maxwell_ops; i++)
        if ((w & maxwell_ops[i].mask) == maxwell_ops[i].value) return maxwell_ops[i].op;
    return OP_INVALID;
}
static const char *op_name(MaxwellOp op) {
    for (size_t i = 0; i < sizeof maxwell_ops / sizeof *maxwell_ops; i++)
        if (maxwell_ops[i].op == op) return maxwell_ops[i].name;
    return "?";
}

static int is_sched(uint32_t pc) { return pc % 32 == 0; }
static int predicate_always(uint64_t w) { return bits(w, 16, 3) == PT && !bit(w, 19); }
/* A jump target; one that lands on a scheduling word means the instruction after it */
static int32_t fix_target(int32_t target) { return is_sched((uint32_t)target) ? target + 8 : target; }
static int32_t rel_target(uint64_t w, uint32_t pc) { return fix_target((int32_t)pc + 8 + sext(bits(w, 20, 24), 24)); }

/* BRX Rx is a switch statement's jump table:
 *     IMNMX.U32 Rx, Rx, bound;  SHL Rx, Rx, 2;  LDC Rx, c[1][Rx + base];  BRX Rx
 * so its targets are the (bound + 1) offsets in the shader's constants at `base`. */
static int brx_targets(T *t, uint32_t brx_pc, uint64_t brx) {
    int reg = (int)bits(brx, 8, 8), stage = 0;
    uint32_t base = 0, count = 0;
    /* (the three may be spread out among other instructions: up to 32 back, following the register) */
    for (uint32_t pc = brx_pc; pc >= 8 && pc + 256 > brx_pc && stage < 3; ) {
        pc -= 8;
        if (is_sched(pc)) continue;
        uint64_t w;
        memcpy(&w, t->code + pc, 8);
        if ((int)bits(w, 0, 8) != reg) continue;      /* not the instruction that wrote reg */
        MaxwellOp op = OP_INVALID;
        for (size_t i = 0; i < sizeof maxwell_ops / sizeof *maxwell_ops; i++)
            if ((w & maxwell_ops[i].mask) == maxwell_ops[i].value) { op = maxwell_ops[i].op; break; }
        if (stage == 0 && op == OP_LDC && bits(w, 36, 5) == 1 && !bits(w, 44, 2)) base = bits(w, 20, 16);
        else if (stage == 1 && op == OP_SHLI && (bits(w, 20, 19) | bit(w, 56) << 19) == 2) {}
        else if (stage == 2 && op == OP_IMNMXI && !bit(w, 48)) count = (bits(w, 20, 19) | bit(w, 56) << 19) + 1;
        else return -1;
        reg = (int)bits(w, 8, 8);
        stage++;
    }
    if (stage != 3 || count > 256) return -1;
    Inst *in = &t->inst[t->ninst - 1];
    in->targets = malloc(count * sizeof *in->targets);
    in->ntargets = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (base + 4 * i + 4 > t->c1_size) return -1;
        uint32_t off;
        memcpy(&off, t->c1 + base + 4 * i, 4);
        int32_t target = fix_target((int32_t)brx_pc + 8 + sext(bits(brx, 20, 24), 24) + (int32_t)off);
        int dup = 0;
        for (int k = 0; k < in->ntargets; k++) dup |= in->targets[k] == target;
        if (!dup) in->targets[in->ntargets++] = target;
    }
    return 0;
}

/* The hardware's reconvergence stack, for resolving SYNC / BRK / CONT: a short list of
 * (kind, target) pushed by SSY / PBK / PCNT. */
typedef struct { uint8_t n; uint8_t kind[8]; int32_t target[8]; } Stack;

#define MAX_INST 16384
typedef struct { uint32_t pc; Stack st; } Work;

/* Find every instruction reachable from the start, and the target of every jump. */
static int discover(T *t) {
    uint32_t limit = t->code_size;
    int32_t *index = calloc(limit / 8 + 1, sizeof *index);    /* pc/8 -> instruction index + 1 */
    Work *work = malloc(sizeof(Work) * 4096);
    t->inst = calloc(MAX_INST, sizeof *t->inst);
    int nwork = 0;
    work[nwork++] = (Work){ 0, { 0 } };
    while (nwork && !t->info->error[0]) {
        Work cur = work[--nwork];
        uint32_t pc = cur.pc;
        Stack st = cur.st;
        for (;;) {
            if (is_sched(pc)) pc += 8;
            if (pc + 8 > limit) { fail(t, "code runs past its end at %#x", pc); break; }
            if (index[pc / 8]) break;                        /* already seen */
            if (t->ninst == MAX_INST) { fail(t, "shader too long"); break; }
            uint64_t w;
            memcpy(&w, t->code + pc, 8);
            Inst *in = &t->inst[t->ninst];
            *in = (Inst){ .pc = pc, .w = w, .op = decode(w), .target = -1 };
            index[pc / 8] = (int32_t)++t->ninst;
            int always = predicate_always(w), next = 1;
            switch (in->op) {
            case OP_SSY: case OP_PBK: case OP_PCNT:
                if (st.n < 8) {
                    st.kind[st.n] = (uint8_t)in->op;
                    st.target[st.n++] = rel_target(w, pc);
                }
                in->target = rel_target(w, pc);
                if (nwork < 4096) work[nwork++] = (Work){ (uint32_t)in->target, st };
                break;
            case OP_BRA:
                if (bit(w, 5)) { fail(t, "BRA with constant-buffer target"); next = 0; break; }
                in->target = rel_target(w, pc);
                if (nwork < 4096) work[nwork++] = (Work){ (uint32_t)in->target, st };
                next = !always;
                break;
            case OP_SYNC: case OP_BRK: case OP_CONT: {
                MaxwellOp want = in->op == OP_SYNC ? OP_SSY : in->op == OP_BRK ? OP_PBK : OP_PCNT;
                Stack popped = st;
                while (popped.n && popped.kind[popped.n - 1] != want) popped.n--;
                if (!popped.n) { fail(t, "%s at %#x without a matching push", op_name(in->op), pc); next = 0; break; }
                in->target = popped.target[--popped.n];
                if (nwork < 4096) work[nwork++] = (Work){ (uint32_t)in->target, popped };
                next = !always;
                break;
            }
            case OP_EXIT: case OP_KIL:
                next = !always || in->op == OP_KIL;
                break;
            case OP_BRX:
                if (brx_targets(t, pc, w) != 0) { fail(t, "BRX at %#x isn't a recognised jump table", pc); next = 0; break; }
                for (int k = 0; k < in->ntargets; k++)
                    if (nwork < 4096) work[nwork++] = (Work){ (uint32_t)in->targets[k], st };
                next = !always;
                break;
            case OP_JMX: case OP_CAL: case OP_RET: case OP_JMP: case OP_JCAL:
                fail(t, "%s (indirect jumps and calls) not supported", op_name(in->op));
                next = 0;
                break;
            case OP_INVALID:
                fail(t, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
                next = 0;
                break;
            default:
                break;
            }
            if (in->op == OP_EXIT && always) break;
            if (!next) break;
            pc += 8;
        }
    }
    free(work);
    if (t->info->error[0]) { free(index); return -1; }

    /* sort by address, then mark block leaders: the start, jump targets, and whatever follows a jump */
    for (uint32_t i = 1; i < t->ninst; i++)
        for (uint32_t j = i; j > 0 && t->inst[j - 1].pc > t->inst[j].pc; j--) {
            Inst tmp = t->inst[j]; t->inst[j] = t->inst[j - 1]; t->inst[j - 1] = tmp;
        }
    memset(index, 0, (limit / 8 + 1) * sizeof *index);
    for (uint32_t i = 0; i < t->ninst; i++) index[t->inst[i].pc / 8] = (int32_t)i + 1;
    t->inst[0].leader = 1;
    for (uint32_t i = 0; i < t->ninst; i++) {
        Inst *in = &t->inst[i];
        int jumps = in->op == OP_BRA || in->op == OP_SYNC || in->op == OP_BRK || in->op == OP_CONT || in->op == OP_BRX;
        for (int k = 0; k < in->ntargets; k++) {
            int32_t at = index[in->targets[k] / 8];
            if (at) t->inst[at - 1].leader = 1;
            else { fail(t, "jump table entry %#x isn't code", in->targets[k]); break; }
        }
        if (jumps && in->target >= 0) {
            int32_t k = index[in->target / 8];
            if (k) t->inst[k - 1].leader = 1;
            else { fail(t, "jump to %#x, which isn't code", in->target); break; }
        }
        if ((jumps || (in->op == OP_EXIT && predicate_always(in->w))) && i + 1 < t->ninst) t->inst[i + 1].leader = 1;
    }
    /* a block also starts where the instruction before isn't the one just before it in memory */
    for (uint32_t i = 1; i < t->ninst; i++) {
        uint32_t prev = t->inst[i - 1].pc + 8;
        if (is_sched(prev)) prev += 8;
        if (prev != t->inst[i].pc) t->inst[i].leader = 1;
    }
    t->block_of = calloc(t->ninst, sizeof *t->block_of);
    for (uint32_t i = 0; i < t->ninst; i++) {
        if (t->inst[i].leader) t->nblocks++;
        t->block_of[i] = (int)t->nblocks - 1;
    }
    /* targets as block numbers */
    for (uint32_t i = 0; i < t->ninst; i++) {
        if (t->inst[i].target >= 0 && index[t->inst[i].target / 8])
            t->inst[i].target = t->block_of[index[t->inst[i].target / 8] - 1];
        for (int k = 0; k < t->inst[i].ntargets; k++)
            t->inst[i].targets[k] = t->block_of[index[t->inst[i].targets[k] / 8] - 1];
    }
    free(index);
    return t->info->error[0] ? -1 : 0;
}

/* ================================================================ instructions */

static void set_pc(T *t, uint32_t block) { store(t, t->pc_var, cu(t, block)); }

/* Instructions that end a block or leave the shader */
static int emit_flow(T *t, const Inst *in, uint32_t next_block) {
    switch (in->op) {
    case OP_EXIT:
        emit_outputs_at_exit(t);
        spv_op(&t->s, SEC_BODY, SpvOpReturn, 0);
        t->terminated = 1;
        return 1;
    case OP_KIL:
        if (t->options & SHADER_DEBUG_CONST) return 1;     /* (debugging: draw every pixel) */
        spv_op(&t->s, SEC_BODY, SpvOpKill, 0);
        t->info->discards = 1;
        t->terminated = 1;
        return 1;
    case OP_BRX: {                                        /* jump table: pick the block by address */
        uint32_t addr = op2(t, SpvOpIAdd, t->t_u32, get_u(t, (int)bits(in->w, 8, 8)),
                            cu(t, (uint32_t)((int32_t)in->pc + 8 + sext(bits(in->w, 20, 24), 24))));
        uint32_t merge = spv_id(&t->s), deflt = spv_id(&t->s);
        uint32_t *w = malloc((2 + 2 * (size_t)in->ntargets) * 4), *labels = malloc((size_t)in->ntargets * 4);
        w[0] = addr;
        w[1] = deflt;
        for (int k = 0; k < in->ntargets; k++) {
            labels[k] = spv_id(&t->s);
            /* the case value is the target's address; find it from the block's first instruction */
            uint32_t target_pc = 0;
            for (uint32_t i = 0; i < t->ninst; i++)
                if (t->inst[i].leader && t->block_of[i] == in->targets[k]) { target_pc = t->inst[i].pc; break; }
            w[2 + 2 * k] = target_pc;
            w[3 + 2 * k] = labels[k];
        }
        spv_op(&t->s, SEC_BODY, SpvOpSelectionMerge, 2, merge, SpvSelectionControlMaskNone);
        spv_inst(&t->s, SEC_BODY, SpvOpSwitch, w, 2 + 2 * (uint32_t)in->ntargets);
        t->terminated = 1;
        for (int k = 0; k < in->ntargets; k++) {
            label(t, labels[k]);
            set_pc(t, (uint32_t)in->targets[k]);
            branch(t, merge);
        }
        label(t, deflt);
        set_pc(t, next_block);
        branch(t, merge);
        label(t, merge);
        branch(t, t->switch_merge);
        free(w);
        free(labels);
        return 1;
    }
    case OP_BRA: case OP_SYNC: case OP_BRK: case OP_CONT:
        if (!t->loop_continue) { fail(t, "jump in straight-line code"); return 1; }
        (void)next_block;
        set_pc(t, (uint32_t)in->target);
        branch(t, t->switch_merge);
        return 1;
    case OP_SSY: case OP_PBK: case OP_PCNT:               /* resolved when discovering */
    case OP_NOP: case OP_DEPBAR:
        return 1;
    default:
        return 0;
    }
}

static void emit_one(T *t, const Inst *in, uint32_t next_block) {
    if (emit_flow(t, in, next_block) || emit_io(t, in) || emit_memory(t, in) || emit_alu(t, in) || emit_half(t, in) ||
        emit_texture(t, in)) return;
    fail(t, "%s (%016llx) not translated yet", op_name(in->op), (unsigned long long)in->w);
}

/* An instruction, inside "if (predicate)" unless it always runs */
static void emit_predicated(T *t, const Inst *in, uint32_t next_block) {
    int p = (int)bits(in->w, 16, 3), inv = bit(in->w, 19);
    if (p == PT && !inv) { emit_one(t, in, next_block); return; }
    if (p == PT && inv) return;                          /* @!PT: never runs */
    uint32_t cond = get_pred_operand(t, p, inv);
    uint32_t then = spv_id(&t->s), merge = spv_id(&t->s);
    spv_op(&t->s, SEC_BODY, SpvOpSelectionMerge, 2, merge, SpvSelectionControlMaskNone);
    spv_op(&t->s, SEC_BODY, SpvOpBranchConditional, 3, cond, then, merge);
    label(t, then);
    emit_one(t, in, next_block);
    branch(t, merge);
    label(t, merge);
}

static void emit_program(T *t) {
    if (t->nblocks == 1) {                               /* straight-line code */
        for (uint32_t i = 0; i < t->ninst && !t->terminated; i++) emit_predicated(t, &t->inst[i], 0);
        if (!t->terminated) { emit_outputs_at_exit(t); spv_op(&t->s, SEC_BODY, SpvOpReturn, 0); }
        return;
    }
    /* pc = 0; loop { switch (pc) { case n: block n } } */
    t->pc_var = local_var(t, t->t_u32, cu(t, 0));
    uint32_t header = spv_id(&t->s), dispatch = spv_id(&t->s), merge = spv_id(&t->s);
    t->loop_continue = spv_id(&t->s);
    t->switch_merge = spv_id(&t->s);
    uint32_t *case_label = malloc(t->nblocks * sizeof *case_label);
    for (uint32_t b = 0; b < t->nblocks; b++) case_label[b] = spv_id(&t->s);
    branch(t, header);
    label(t, header);
    spv_op(&t->s, SEC_BODY, SpvOpLoopMerge, 3, merge, t->loop_continue, SpvLoopControlMaskNone);
    branch(t, dispatch);
    label(t, dispatch);
    uint32_t pc = load(t, t->t_u32, t->pc_var);
    spv_op(&t->s, SEC_BODY, SpvOpSelectionMerge, 2, t->switch_merge, SpvSelectionControlMaskNone);
    {
        uint32_t *w = malloc((2 + 2 * t->nblocks) * 4);
        w[0] = pc;
        w[1] = t->switch_merge;
        for (uint32_t b = 0; b < t->nblocks; b++) { w[2 + 2 * b] = b; w[3 + 2 * b] = case_label[b]; }
        spv_inst(&t->s, SEC_BODY, SpvOpSwitch, w, 2 + 2 * t->nblocks);
        free(w);
        t->terminated = 1;
    }
    for (uint32_t i = 0; i < t->ninst; i++) {
        const Inst *in = &t->inst[i];
        uint32_t b = (uint32_t)t->block_of[i];
        if (in->leader) {
            label(t, case_label[b]);
            memset(t->known, 0, sizeof t->known);        /* constants don't survive jumps */
        }
        if (t->terminated) continue;                     /* rest of a block after an exit */
        int last = i + 1 == t->ninst || t->inst[i + 1].leader;
        emit_predicated(t, in, b + 1);
        if (last && !t->terminated) {                    /* fall through into the next block */
            if (b + 1 < t->nblocks) { set_pc(t, b + 1); branch(t, t->switch_merge); }
            else { emit_outputs_at_exit(t); spv_op(&t->s, SEC_BODY, SpvOpReturn, 0); t->terminated = 1; }
        }
    }
    free(case_label);
    label(t, t->switch_merge);
    branch(t, t->loop_continue);
    label(t, t->loop_continue);
    branch(t, header);
    label(t, merge);
    spv_op(&t->s, SEC_BODY, SpvOpUnreachable, 0);
    t->terminated = 1;
}

/* ================================================================ module */

static void setup_types(T *t) {
    Spv *s = &t->s;
    t->t_void = spv_type(s, SpvOpTypeVoid, NULL, 0);
    t->t_bool = spv_type(s, SpvOpTypeBool, NULL, 0);
    uint32_t u[2] = { 32, 0 }, i[2] = { 32, 1 }, f[1] = { 32 };
    t->t_u32 = spv_type(s, SpvOpTypeInt, u, 2);
    t->t_i32 = spv_type(s, SpvOpTypeInt, i, 2);
    t->t_f32 = spv_type(s, SpvOpTypeFloat, f, 1);
    uint32_t v[2];
    v[0] = t->t_f32; v[1] = 2; t->t_v2f = spv_type(s, SpvOpTypeVector, v, 2);
    v[1] = 3; t->t_v3f = spv_type(s, SpvOpTypeVector, v, 2);
    v[1] = 4; t->t_v4f = spv_type(s, SpvOpTypeVector, v, 2);
    v[0] = t->t_u32; v[1] = 4; t->t_v4u = spv_type(s, SpvOpTypeVector, v, 2);
    v[1] = 2; t->t_v2u = spv_type(s, SpvOpTypeVector, v, 2);
    v[0] = t->t_i32; v[1] = 2; t->t_v2i = spv_type(s, SpvOpTypeVector, v, 2);
    v[1] = 3; t->t_v3i = spv_type(s, SpvOpTypeVector, v, 2);
    v[1] = 4; t->t_v4i = spv_type(s, SpvOpTypeVector, v, 2);
}

void shader_constants(const uint8_t *control, uint32_t *offset, uint32_t *size) {
    memcpy(offset, control + 0x700, 4);
    memcpy(size, control + 0x6FC, 4);
}

int shader_translate(const uint8_t *code, size_t size, int stage, const uint8_t *constants,
                     uint32_t constants_size, const uint8_t *control, int options, uint32_t extra_outputs,
                     ShaderInfo *info) {
    memset(info, 0, sizeof *info);
    info->stage = stage;
    for (int i = 0; i < 3; i++) info->local_size[i] = 1;
    if (stage == SHADER_COMPUTE && control)          /* workgroup size: control + 0x748, 0x74C, 0x750 */
        for (int i = 0; i < 3; i++) {
            uint32_t v;
            memcpy(&v, control + 0x748 + 4 * i, 4);
            info->local_size[i] = v ? v : 1;
        }
    T *t = calloc(1, sizeof *t);
    t->info = info;
    t->stage = stage;
    t->options = options;
    t->c1 = constants;
    t->c1_size = constants_size;
    uint32_t header = stage == SHADER_COMPUTE ? 0 : 0x80;
    if (size < header + 32) { fail(t, "too short"); free(t); return -1; }
    if (header) memcpy(t->sph, code + 0x30, sizeof t->sph);
    t->code = code + header;
    t->code_size = (uint32_t)(size - header);
    if (stage != SHADER_VERTEX && stage != SHADER_FRAGMENT && stage != SHADER_COMPUTE)
        fail(t, "only vertex, fragment and compute shaders so far");

    spv_init(&t->s);
    if (!info->error[0] && discover(t) == 0) {
        Spv *s = &t->s;
        spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityShader);
        spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityImageQuery);
        t->glsl = spv_id(s);
        spv_inst_str(s, SEC_IMPORT, SpvOpExtInstImport, &t->glsl, 1, "GLSL.std.450", NULL, 0);
        setup_types(t);
        uint32_t fn_type = spv_type(s, SpvOpTypeFunction, &t->t_void, 1);
        uint32_t main_fn = spv_id(s);
        spv_op(s, SEC_FUNC, SpvOpFunction, 4, t->t_void, main_fn, SpvFunctionControlMaskNone, fn_type);
        spv_op(s, SEC_FUNC, SpvOpLabel, 1, spv_id(s));
        emit_program(t);
        if (stage == SHADER_VERTEX)                       /* never written: they keep (0, 0, 0, 1) */
            for (uint32_t m = extra_outputs & ~info->outputs; m; m &= m - 1) attr_var(t, 1, __builtin_ctz(m));
        spv_op(s, SEC_BODY, SpvOpFunctionEnd, 0);
        if (t->in_lane || t->uses_quad || t->uses_shuffle)
            spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityGroupNonUniform);
        if (t->uses_quad) spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityGroupNonUniformQuad);
        if (t->in_base_instance) spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityDrawParameters);
        if (t->uses_shuffle) spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityGroupNonUniformShuffle);
        if (t->uses_pointers) {                           /* (decided while translating) */
            spv_op(s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityPhysicalStorageBufferAddresses);
            spv_inst_str(s, SEC_EXT, SpvOpExtension, NULL, 0, "SPV_KHR_physical_storage_buffer", NULL, 0);
            spv_op(s, SEC_MEMMODEL, SpvOpMemoryModel, 2, SpvAddressingModelPhysicalStorageBuffer64, SpvMemoryModelGLSL450);
        } else {
            spv_op(s, SEC_MEMMODEL, SpvOpMemoryModel, 2, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
        }
        uint32_t model = stage == SHADER_VERTEX ? SpvExecutionModelVertex
                       : stage == SHADER_COMPUTE ? SpvExecutionModelGLCompute : SpvExecutionModelFragment;
        uint32_t before[2] = { model, main_fn };
        spv_inst_str(s, SEC_ENTRY, SpvOpEntryPoint, before, 2, "main", t->interface, t->ninterface);
        if (stage == SHADER_COMPUTE)
            spv_op(s, SEC_EXECMODE, SpvOpExecutionMode, 5, main_fn, SpvExecutionModeLocalSize,
                   info->local_size[0], info->local_size[1], info->local_size[2]);
        if (stage == SHADER_FRAGMENT) {
            spv_op(s, SEC_EXECMODE, SpvOpExecutionMode, 2, main_fn, SpvExecutionModeOriginUpperLeft);
            if (info->writes_depth) spv_op(s, SEC_EXECMODE, SpvOpExecutionMode, 2, main_fn, SpvExecutionModeDepthReplacing);
        }
        if (!info->error[0]) info->spirv = spv_finish(s, &info->words);
    }
    spv_free(&t->s);
    for (uint32_t i = 0; i < t->ninst; i++) free(t->inst[i].targets);
    free(t->inst);
    free(t->block_of);
    free(t);
    return info->error[0] ? -1 : 0;
}

void shader_free(ShaderInfo *info) {
    free(info->spirv);
    info->spirv = NULL;
}
