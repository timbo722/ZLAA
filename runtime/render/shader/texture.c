/*
 * texture.c - Maxwell texture instructions -> SPIR-V.
 *
 * A texture instruction names its texture by an index into the texture constant buffer, where
 * the driver keeps 8-byte handles (texture id | sampler id << 20). We give each (index, kind)
 * a combined image sampler binding and record the handle's offset in ShaderInfo, so the
 * renderer can look the handle up and bind the right image and sampler for each draw.
 *
 * Coordinates come from consecutive registers starting at Ra, extra operands (LOD, depth
 * reference, offsets) from Rb onward. 1D textures are sampled as 2D with y = 0.
 */
#include "translate_internal.h"

typedef struct { int dim, array, shadow, ms; } Kind;

static uint32_t tex_variable(T *t, uint32_t tid, Kind k, uint32_t *image_type_out) {
    uint32_t dim = k.dim == TEXDIM_1D || k.dim == TEXDIM_2D ? SpvDim2D : k.dim == TEXDIM_3D ? SpvDim3D
                 : k.dim == TEXDIM_CUBE ? SpvDimCube : SpvDimBuffer;
    uint32_t it[7] = { t->t_f32, dim, (uint32_t)k.shadow, (uint32_t)k.array, (uint32_t)k.ms, 1, SpvImageFormatUnknown };
    uint32_t image_type = spv_type(&t->s, SpvOpTypeImage, it, 7);
    *image_type_out = image_type;
    ShaderInfo *info = t->info;
    for (uint32_t i = 0; i < info->ntextures; i++) {
        ShaderTexture *e = &info->textures[i];
        if (e->handle_offset == tid * 4 && e->dim == (k.dim == TEXDIM_1D ? TEXDIM_2D : k.dim) && e->array == k.array &&
            e->shadow == k.shadow && e->ms == k.ms)
            return t->tex_var[i];
    }
    if (info->ntextures == SHADER_MAX_TEXTURES) { fail(t, "too many textures"); return 0; }
    uint32_t i = info->ntextures++;
    info->textures[i] = (ShaderTexture){ tid * 4, (uint8_t)(k.dim == TEXDIM_1D ? TEXDIM_2D : k.dim), (uint8_t)k.array,
                                         (uint8_t)k.shadow, (uint8_t)k.ms };
    uint32_t sampled = spv_type(&t->s, SpvOpTypeSampledImage, &image_type, 1);
    uint32_t id = spv_id(&t->s);
    spv_op(&t->s, SEC_TYPES, SpvOpVariable, 3, ptr_type(t, SpvStorageClassUniformConstant, sampled), id,
           SpvStorageClassUniformConstant);
    spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, id, SpvDecorationDescriptorSet, 0);
    spv_op(&t->s, SEC_DECO, SpvOpDecorate, 3, id, SpvDecorationBinding,
           (uint32_t)t->stage * SHADER_STAGE_BINDINGS + SHADER_TEXTURE_BINDING + i);
    t->tex_var[i] = id;
    return id;
}

/* Register reader that walks Ra / Rb upwards; RZ (and past it) reads 0 */
typedef struct { int next; } RegWalk;
static uint32_t walk(T *t, RegWalk *r) {
    if (r->next >= RZ) return cu(t, 0);
    return get_u(t, r->next++);
}

enum { F_LOD = 1, F_BIAS = 2, F_DREF = 4, F_GATHER = 8, F_FETCH = 16, F_OFFSET = 32, F_LOD_ZERO = 64 };

typedef struct {
    Kind kind;
    int flags;
    uint32_t coords[4]; int ncoords;     /* float bits (int for fetch); the array layer last */
    uint32_t lod, dref, offset, comp;    /* uint ids */
    int offset_reg;                      /* register the packed offsets came from */
} Sample;

static uint32_t vec(T *t, uint32_t elem_type, const uint32_t *c, int n) {
    if (n == 1) return c[0];
    uint32_t vt_ops[2] = { elem_type, (uint32_t)n };
    uint32_t vt = spv_type(&t->s, SpvOpTypeVector, vt_ops, 2);
    uint32_t id = spv_id(&t->s), w[6] = { vt, id };
    for (int i = 0; i < n; i++) w[2 + i] = c[i];
    spv_inst(&t->s, SEC_BODY, SpvOpCompositeConstruct, w, 2 + (uint32_t)n);
    return id;
}

/* Run the sample and return a vec4 of results as float bits... as a vec4 of floats */
static uint32_t do_sample(T *t, uint32_t tid, Sample *s) {
    uint32_t image_type;
    uint32_t var = tex_variable(t, tid, s->kind, &image_type);
    if (!var) return 0;
    uint32_t sampled_type = spv_type(&t->s, SpvOpTypeSampledImage, &image_type, 1);
    uint32_t sampled = op1(t, SpvOpLoad, sampled_type, var);
    int fetch = s->flags & F_FETCH;
    /* coordinates: floats (or ints for fetch); 1D gets y = 0 */
    uint32_t c[5];
    int n = 0, nc = s->ncoords;
    int layer_last = s->kind.array;
    for (int i = 0; i < nc - layer_last; i++) {
        c[n++] = fetch ? bitcast(t, t->t_i32, s->coords[i]) : bitcast(t, t->t_f32, s->coords[i]);
        if (s->kind.dim == TEXDIM_1D && i == 0) c[n++] = fetch ? ci(t, 0) : cf(t, 0.0f);
    }
    if (layer_last) {                     /* the layer is an integer register for both */
        uint32_t layer = s->coords[nc - 1];
        c[n++] = fetch ? bitcast(t, t->t_i32, layer) : op1(t, SpvOpConvertUToF, t->t_f32, op2(t, SpvOpBitwiseAnd, t->t_u32, layer, cu(t, 0xFFFF)));
    }
    uint32_t coord = vec(t, fetch ? t->t_i32 : t->t_f32, c, n);

    /* image operands */
    uint32_t mask = 0, ops[4];
    int nops = 0;
    int implicit = !(s->flags & (F_LOD | F_LOD_ZERO)) && t->stage == SHADER_FRAGMENT && !fetch;
    if (s->flags & F_BIAS && implicit) { mask |= SpvImageOperandsBiasMask; ops[nops++] = bitcast(t, t->t_f32, s->lod); }
    if (!implicit && !(s->flags & F_GATHER)) {
        mask |= SpvImageOperandsLodMask;
        if (fetch) ops[nops++] = s->flags & F_LOD ? bitcast(t, t->t_i32, s->lod) : ci(t, 0);
        else ops[nops++] = s->flags & F_LOD ? bitcast(t, t->t_f32, s->lod) : cf(t, 0.0f);
    }
    if (s->flags & F_OFFSET) {
        /* packed 4-bit signed offsets (6-bit for gathers), one per coordinate. Vulkan wants
         * them constant (ConstOffset) except for gathers; usually they are. */
        int width = s->flags & F_GATHER ? 6 : 4, stride = s->flags & F_GATHER ? 8 : 4;
        int no = s->kind.dim == TEXDIM_3D ? 3 : 2;
        uint32_t o[3];
        if (s->offset_reg < RZ && t->known[s->offset_reg]) {
            for (int i = 0; i < no; i++)
                o[i] = ci(t, (s->kind.dim == TEXDIM_1D && i == 1) ? 0 : sext(t->known_value[s->offset_reg] >> (i * stride), width));
            uint32_t vt_ops[2] = { t->t_i32, (uint32_t)no };
            mask |= SpvImageOperandsConstOffsetMask;
            ops[nops++] = spv_composite_constant(&t->s, spv_type(&t->s, SpvOpTypeVector, vt_ops, 2), o, (uint32_t)no);
        } else {
            for (int i = 0; i < no; i++)
                o[i] = (s->kind.dim == TEXDIM_1D && i == 1) ? ci(t, 0)
                     : op3(t, SpvOpBitFieldSExtract, t->t_i32, bitcast(t, t->t_i32, s->offset), cu(t, (uint32_t)(i * stride)), cu(t, (uint32_t)width));
            uint32_t ov = vec(t, t->t_i32, o, no);
            if (s->flags & F_GATHER || fetch) {
                mask |= SpvImageOperandsOffsetMask;
                ops[nops++] = ov;
                spv_op(&t->s, SEC_CAPS, SpvOpCapability, 1, SpvCapabilityImageGatherExtended);
            } else {                                 /* move the coordinates by offset / size */
                uint32_t image = op1(t, SpvOpImage, image_type, sampled);
                uint32_t vt_ops[2] = { t->t_i32, (uint32_t)no }, vf_ops[2] = { t->t_f32, (uint32_t)no };
                uint32_t vi = spv_type(&t->s, SpvOpTypeVector, vt_ops, 2), vf = spv_type(&t->s, SpvOpTypeVector, vf_ops, 2);
                uint32_t size = op2(t, SpvOpImageQuerySizeLod, vi, image, ci(t, 0));
                uint32_t delta = op2(t, SpvOpFDiv, vf, op1(t, SpvOpConvertSToF, vf, ov), op1(t, SpvOpConvertSToF, vf, size));
                for (int i = 0; i < no; i++)
                    c[i] = op2(t, SpvOpFAdd, t->t_f32, c[i], op2(t, SpvOpCompositeExtract, t->t_f32, delta, (uint32_t)i));
                coord = vec(t, t->t_f32, c, n);
            }
        }
    }

    uint32_t result = spv_id(&t->s), w[16];
    int nw = 0;
    SpvOp op;
    uint32_t rtype = t->t_v4f;
    if (s->flags & F_GATHER) {
        op = s->kind.shadow ? SpvOpImageDrefGather : SpvOpImageGather;
        w[nw++] = rtype; w[nw++] = result; w[nw++] = sampled; w[nw++] = coord;
        w[nw++] = s->kind.shadow ? bitcast(t, t->t_f32, s->dref) : s->comp;
    } else if (fetch) {
        op = SpvOpImageFetch;
        uint32_t image = op1(t, SpvOpImage, image_type, sampled);
        w[nw++] = rtype; w[nw++] = result; w[nw++] = image; w[nw++] = coord;
    } else if (s->kind.shadow) {
        op = implicit ? SpvOpImageSampleDrefImplicitLod : SpvOpImageSampleDrefExplicitLod;
        rtype = t->t_f32;
        w[nw++] = rtype; w[nw++] = result; w[nw++] = sampled; w[nw++] = coord; w[nw++] = bitcast(t, t->t_f32, s->dref);
    } else {
        op = implicit ? SpvOpImageSampleImplicitLod : SpvOpImageSampleExplicitLod;
        w[nw++] = rtype; w[nw++] = result; w[nw++] = sampled; w[nw++] = coord;
    }
    if (mask) {
        w[nw++] = mask;
        for (int i = 0; i < nops; i++) w[nw++] = ops[i];
    }
    spv_inst(&t->s, SEC_BODY, op, w, (uint32_t)nw);
    if (rtype == t->t_f32) {                             /* depth compare: (r, 0, 0, 1) */
        uint32_t parts[4] = { result, cf(t, 0.0f), cf(t, 0.0f), cf(t, 1.0f) };
        result = vec(t, t->t_f32, parts, 4);
    }
    return result;
}

/* write the enabled components of a vec4 result to consecutive registers */
static void write_packed(T *t, uint32_t res, int mask, int rd) {
    for (int c = 0, k = 0; c < 4; c++)
        if (mask >> c & 1) set_f(t, rd == RZ ? RZ : rd + k++, op2(t, SpvOpCompositeExtract, t->t_f32, res, (uint32_t)c));
}

/* TEXS/TLDS/TLD4S: components go to Rd, Rd+1 (first two) and Rd2, Rd2+1 (the rest) */
static void write_split(T *t, uint32_t res, int mask, int rd, int rd2) {
    for (int c = 0, k = 0; c < 4; c++) {
        if (!(mask >> c & 1)) continue;
        int base = k < 2 ? rd : rd2;
        if (base != RZ) set_f(t, base + (k & 1), op2(t, SpvOpCompositeExtract, t->t_f32, res, (uint32_t)c));
        k++;
    }
}

static const int texs_mask[2][8] = {
    { 0x1, 0x2, 0x4, 0x8, 0x3, 0x9, 0xA, 0xC },
    { 0x7, 0xB, 0xD, 0xE, 0xF, 0x0, 0x0, 0x0 },
};

/* TEX / TLD4 dimension field -> kind */
static Kind tex_dim(uint32_t d) {
    static const Kind k[8] = {
        { TEXDIM_1D, 0, 0, 0 }, { TEXDIM_1D, 1, 0, 0 }, { TEXDIM_2D, 0, 0, 0 }, { TEXDIM_2D, 1, 0, 0 },
        { TEXDIM_3D, 0, 0, 0 }, { TEXDIM_3D, 1, 0, 0 }, { TEXDIM_CUBE, 0, 0, 0 }, { TEXDIM_CUBE, 1, 0, 0 },
    };
    return k[d & 7];
}
static int dim_coords(int dim) { return dim == TEXDIM_1D ? 1 : dim == TEXDIM_2D ? 2 : 3; }

int emit_texture(T *t, const Inst *in) {
    uint64_t w = in->w;
    int rd = (int)bits(w, 0, 8);
    RegWalk a = { (int)bits(w, 8, 8) }, b = { (int)bits(w, 20, 8) };
    uint32_t tid = bits(w, 36, 13);
    Sample s = { 0 };
    switch (in->op) {
    case OP_TEXS: {
        int target = (int)bits(w, 53, 4), rd2 = (int)bits(w, 28, 8);
        if (rd == RZ && rd2 == RZ) return 1;
        switch (target) {
        case 0: s.kind = (Kind){ TEXDIM_1D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.flags = F_LOD_ZERO; break;
        case 1: s.kind = (Kind){ TEXDIM_2D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &b); break;
        case 2: s.kind = (Kind){ TEXDIM_2D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &b); s.flags = F_LOD_ZERO; break;
        case 3: s.kind = (Kind){ TEXDIM_2D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.lod = walk(t, &b); s.flags = F_LOD; break;
        case 4: s.kind = (Kind){ TEXDIM_2D, 0, 1, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.dref = walk(t, &b); s.flags = F_DREF; break;
        case 5: s.kind = (Kind){ TEXDIM_2D, 0, 1, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.lod = walk(t, &b); s.dref = walk(t, &b); s.flags = F_DREF | F_LOD; break;
        case 6: s.kind = (Kind){ TEXDIM_2D, 0, 1, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.dref = walk(t, &b); s.flags = F_DREF | F_LOD_ZERO; break;
        case 7: case 8: case 9: {                         /* arrays: Ra = layer, x; Rb = y (, dref) */
            uint32_t layer = walk(t, &a);
            s.kind = (Kind){ TEXDIM_2D, 1, target == 9, 0 };
            s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &b); s.coords[s.ncoords++] = layer;
            if (target == 9) { s.dref = walk(t, &b); s.flags |= F_DREF; }
            if (target != 7) s.flags |= F_LOD_ZERO;
            break;
        }
        case 10: case 11: s.kind = (Kind){ TEXDIM_3D, 0, 0, 0 };
                s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &b);
                if (target == 11) s.flags = F_LOD_ZERO;
                break;
        case 12: case 13: s.kind = (Kind){ TEXDIM_CUBE, 0, 0, 0 };
                s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &b);
                if (target == 13) { s.lod = walk(t, &b); s.flags = F_LOD; }
                break;
        default: fail(t, "TEXS target %d", target); return 1;
        }
        uint32_t res = do_sample(t, tid, &s);
        if (res) write_split(t, res, texs_mask[rd2 == RZ ? 0 : 1][bits(w, 50, 3)], rd, rd2);
        return 1;
    }
    case OP_TLDS: {
        int target = (int)bits(w, 53, 4), rd2 = (int)bits(w, 28, 8);
        s.flags = F_FETCH;
        switch (target) {
        case 0: s.kind = (Kind){ TEXDIM_1D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); break;
        case 1: s.kind = (Kind){ TEXDIM_1D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.lod = walk(t, &b); s.flags |= F_LOD; break;
        case 2: s.kind = (Kind){ TEXDIM_2D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &b); break;
        case 4: s.kind = (Kind){ TEXDIM_2D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.offset_reg = b.next; s.offset = walk(t, &b); s.flags |= F_OFFSET; break;
        case 5: case 0xC:
                s.kind = (Kind){ TEXDIM_2D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.lod = walk(t, &b); s.flags |= F_LOD;
                if (target == 0xC) { s.offset_reg = b.next; s.offset = walk(t, &b); s.flags |= F_OFFSET; }
                break;
        case 7: s.kind = (Kind){ TEXDIM_3D, 0, 0, 0 }; s.coords[s.ncoords++] = walk(t, &a); s.coords[s.ncoords++] = walk(t, &a);
                s.coords[s.ncoords++] = walk(t, &b); break;
        case 8: { s.kind = (Kind){ TEXDIM_2D, 1, 0, 0 };
                uint32_t layer = walk(t, &a);
                s.coords[s.ncoords++] = walk(t, &b); s.coords[s.ncoords++] = walk(t, &b); s.coords[s.ncoords++] = layer;
                break; }
        default: fail(t, "TLDS target %d", target); return 1;
        }
        uint32_t res = do_sample(t, tid, &s);
        if (res) write_split(t, res, texs_mask[rd2 == RZ ? 0 : 1][bits(w, 50, 3)], rd, rd2);
        return 1;
    }
    case OP_TLD4S: {
        int rd2 = (int)bits(w, 28, 8), dc = bit(w, 50), aoffi = bit(w, 51);
        s.kind = (Kind){ TEXDIM_2D, 0, dc, 0 };
        s.flags = F_GATHER;
        s.coords[s.ncoords++] = walk(t, &a);
        s.coords[s.ncoords++] = dc || aoffi ? walk(t, &a) : walk(t, &b);
        if (aoffi) { s.offset_reg = b.next; s.offset = walk(t, &b); s.flags |= F_OFFSET; }
        if (dc) { s.dref = walk(t, &b); s.flags |= F_DREF; }
        s.comp = cu(t, bits(w, 52, 2));
        uint32_t res = do_sample(t, tid, &s);
        if (res) write_split(t, res, rd2 == RZ ? 0x3 : 0xF, rd, rd2);
        return 1;
    }
    case OP_TEX: case OP_TLD4: case OP_TLD: {
        if (rd == RZ) return 1;
        int is_tld4 = in->op == OP_TLD4, is_tld = in->op == OP_TLD;
        s.kind = tex_dim(bits(w, 28, 3));
        if (bit(w, 50)) s.kind.shadow = 1;               /* depth compare */
        int lod_mode = is_tld ? (bit(w, 55) ? 3 : 1) : is_tld4 ? 0 : (int)bits(w, 55, 3);
        int offset = is_tld4 ? bits(w, 54, 2) == 1 : bit(w, is_tld ? 35 : 54);
        if (is_tld) { s.kind.shadow = 0; s.flags = F_FETCH; }
        if (is_tld4) { s.flags = F_GATHER; s.comp = cu(t, bits(w, 56, 2)); }
        uint32_t layer = s.kind.array ? walk(t, &a) : 0;
        for (int i = 0; i < dim_coords(s.kind.dim); i++) s.coords[s.ncoords++] = walk(t, &a);
        if (s.kind.array) s.coords[s.ncoords++] = layer;
        uint32_t lod = lod_mode > 1 ? walk(t, &b) : 0;
        if (offset) { s.offset_reg = b.next; s.offset = walk(t, &b); s.flags |= F_OFFSET; }
        if (s.kind.shadow) { s.dref = walk(t, &b); s.flags |= F_DREF; }
        if (lod_mode == 1) s.flags |= F_LOD_ZERO;                       /* LZ */
        else if (lod_mode == 3 || lod_mode == 7) { s.lod = lod; s.flags |= F_LOD; }   /* LL, LLA */
        else if (lod_mode == 2 || lod_mode == 6) { s.lod = lod; s.flags |= F_BIAS; }  /* LB, LBA */
        uint32_t res = do_sample(t, tid, &s);
        if (res) write_packed(t, res, (int)bits(w, 31, 4), rd);
        return 1;
    }
    case OP_TXQ: {                                        /* size query: width, height, depth, levels */
        uint32_t query = bits(w, 22, 6);
        if (query != 1) { fail(t, "TXQ query %u", query); return 1; }
        Kind k = { TEXDIM_2D, 0, 0, 0 };
        uint32_t image_type;
        uint32_t var = tex_variable(t, tid, k, &image_type);
        uint32_t sampled_type = spv_type(&t->s, SpvOpTypeSampledImage, &image_type, 1);
        uint32_t image = op1(t, SpvOpImage, image_type, op1(t, SpvOpLoad, sampled_type, var));
        uint32_t size = op2(t, SpvOpImageQuerySizeLod, t->t_v2i, image, bitcast(t, t->t_i32, walk(t, &a)));
        uint32_t levels = op1(t, SpvOpImageQueryLevels, t->t_i32, image);
        uint32_t vals[4] = { op2(t, SpvOpCompositeExtract, t->t_i32, size, 0), op2(t, SpvOpCompositeExtract, t->t_i32, size, 1),
                             ci(t, 1), levels };
        for (int c = 0, k2 = 0; c < 4; c++)
            if (bits(w, 31, 4) >> c & 1) set_u(t, rd + k2++, bitcast(t, t->t_u32, vals[c]));
        return 1;
    }
    default:
        return 0;
    }
}
