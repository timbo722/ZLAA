/*
 * translate_internal.h - shared by the Maxwell -> SPIR-V translator's files.
 *
 * Model: every Maxwell register R0..R254 becomes a SPIR-V local variable holding a uint (RZ,
 * register 255, always reads 0); float operations bitcast. Predicates P0..P6 are bool
 * variables (PT, 7, is always true). An instruction "@P0 ..." runs inside an if (P0).
 */
#ifndef TRANSLATE_INTERNAL_H
#define TRANSLATE_INTERNAL_H

#include "shader.h"
#include "spirv_builder.h"
#include "maxwell_ops.h"
#include "../../../third_party/spirv/GLSL.std.450.h"
#include <stdint.h>

#define RZ 255
#define PT 7

typedef struct {
    uint32_t pc;                     /* byte offset from the start of the code */
    uint64_t w;                      /* the 64-bit instruction */
    MaxwellOp op;
    int32_t target;                  /* resolved branch/sync/break target pc, -1 if none */
    int32_t *targets;                /* BRX: all possible targets (pcs, later block numbers) */
    int ntargets;
    int leader;                      /* first instruction of a basic block */
} Inst;

typedef struct {
    /* input */
    const uint8_t *code;             /* first instruction word (after the header, if any) */
    uint32_t code_size;
    const uint8_t *c1;               /* the shader's constants (c1) */
    uint32_t c1_size;
    int stage, options;
    uint32_t sph[20];                /* shader program header (graphics stages) */
    ShaderInfo *info;

    /* the program */
    Inst *inst;
    uint32_t ninst, nblocks;
    int *block_of;                   /* instruction index -> block number (leaders only) */

    /* SPIR-V */
    Spv s;
    uint32_t glsl;                   /* GLSL.std.450 extended instructions */
    uint32_t t_void, t_bool, t_u32, t_i32, t_f32, t_v2f, t_v3f, t_v4f, t_v4u, t_v2i, t_v3i, t_v4i, t_v2u;
    uint32_t reg[256], pred[8];      /* local variables, made on first use */
    uint32_t cc_zero, cc_sign, cc_carry, cc_overflow;
    uint32_t pc_var;                 /* dispatch loop: the next block to run */
    uint32_t cbuf[18];               /* uniform buffer variables */
    int cbuf_decorated;              /* their types got their decorations */
    int uses_pointers;               /* global memory (LDG/STG): 64-bit physical pointers */
    uint32_t in_attr[32], out_attr[32];
    uint32_t in_position, in_front_facing, in_vertex_index, in_instance_index, frag_coord;
    uint32_t out_position, out_point_size, out_layer, out_viewport, out_clip[8], out_color[8], out_depth;
    uint32_t tex_var[SHADER_MAX_TEXTURES];
    uint32_t interface[64], ninterface;   /* entry point's Input/Output variables */
    uint32_t loop_continue;          /* dispatch loop's continue block (0: straight-line code) */
    uint32_t switch_merge;
    int terminated;                  /* the current block already ended (return/kill/branch) */
    uint8_t known[256];              /* register holds a known constant (in this block)... */
    uint32_t known_value[256];       /* ...this one */
    char *error;
} T;

/* ---- types, constants, instructions (translate.c) ---- */
uint32_t ptr_type(T *t, SpvStorageClass sc, uint32_t type);
uint32_t cu(T *t, uint32_t v);                 /* uint constant */
uint32_t ci(T *t, int32_t v);                  /* int constant */
uint32_t cf(T *t, float v);                    /* float constant */
uint32_t cb(T *t, int v);                      /* bool constant */
/* an instruction with a result: returns the new id */
uint32_t op1(T *t, SpvOp op, uint32_t type, uint32_t a);
uint32_t op2(T *t, SpvOp op, uint32_t type, uint32_t a, uint32_t b);
uint32_t op3(T *t, SpvOp op, uint32_t type, uint32_t a, uint32_t b, uint32_t c);
uint32_t ext1(T *t, uint32_t type, enum GLSLstd450 f, uint32_t a);
uint32_t ext2(T *t, uint32_t type, enum GLSLstd450 f, uint32_t a, uint32_t b);
uint32_t ext3(T *t, uint32_t type, enum GLSLstd450 f, uint32_t a, uint32_t b, uint32_t c);
uint32_t bitcast(T *t, uint32_t type, uint32_t v);
uint32_t select_(T *t, uint32_t type, uint32_t cond, uint32_t a, uint32_t b);
uint32_t label(T *t, uint32_t id);             /* start a block (id 0: new); returns its id */
void branch(T *t, uint32_t to);
void fail(T *t, const char *fmt, ...);

/* ---- registers and predicates ---- */
uint32_t get_u(T *t, int r);
uint32_t get_f(T *t, int r);
void set_u(T *t, int r, uint32_t v);
void set_known(T *t, int r, uint32_t value);           /* r now holds this constant */
void set_f(T *t, int r, uint32_t v);
uint32_t get_p(T *t, int p);
void set_p(T *t, int p, uint32_t v);
uint32_t get_pred_operand(T *t, int p, int inv);     /* predicate, maybe inverted */
uint32_t cbuf_u(T *t, int slot, uint32_t byte_offset);
uint32_t cbuf_u_dyn(T *t, int slot, uint32_t byte_offset_id);

/* ---- instruction fields (bit ranges of the 64-bit word) ---- */
static inline uint32_t bits(uint64_t w, int lo, int n) { return (uint32_t)((w >> lo) & ((1ull << n) - 1)); }
static inline int bit(uint64_t w, int b) { return (int)((w >> b) & 1); }
static inline int32_t sext(uint32_t v, int n) { return (int32_t)(v << (32 - n)) >> (32 - n); }

/* operand forms of an instruction family member (from its MaxwellOp name suffix) */
enum { FORM_R, FORM_I, FORM_C, FORM_RC, FORM_32I, FORM_OTHER };
int form_of(MaxwellOp op);
/* operand B / C as a uint id, for the forms above; `float_imm`: an I-form immediate is the
 * top 20 bits of a float (imm << 12) rather than a signed integer */
uint32_t operand_b(T *t, const Inst *in, int float_imm);
uint32_t operand_c(T *t, const Inst *in);

/* ---- per-instruction emitters ---- */
int emit_alu(T *t, const Inst *in);              /* alu.c: 1 if handled */
int emit_texture(T *t, const Inst *in);          /* texture.c: 1 if handled */
int emit_io(T *t, const Inst *in);               /* translate.c: attributes, IPA */
int emit_memory(T *t, const Inst *in);           /* translate.c: LDG / STG (global memory) */
void emit_outputs_at_exit(T *t);                 /* fragment: colors/depth from registers */

#endif
