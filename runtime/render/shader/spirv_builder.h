/*
 * spirv_builder.h - writing SPIR-V, Vulkan's shader format.
 *
 * A SPIR-V module is an array of 32-bit words: a 5-word header, then instructions. Each
 * instruction is (word count << 16 | opcode) followed by its operands, and most define a
 * numbered "id" that later instructions refer to. The module must be in a fixed order
 * (capabilities, extensions, ..., types and constants, functions), so we collect each part in
 * its own growable array (Section) and concatenate them at the end.
 */
#ifndef SPIRV_BUILDER_H
#define SPIRV_BUILDER_H

#include <stdint.h>
#include "spirv/spirv.h"

typedef struct {
    uint32_t *w;
    uint32_t n, cap;
} Section;

/* SEC_FUNC: OpFunction and its first OpLabel; SEC_VARS: the function's local variables (SPIR-V
 * wants them right after that label, but we only learn which we need while writing the body);
 * SEC_BODY: everything else. Concatenated in this order, that is one valid function. */
enum { SEC_CAPS, SEC_EXT, SEC_IMPORT, SEC_MEMMODEL, SEC_ENTRY, SEC_EXECMODE, SEC_DEBUG, SEC_DECO, SEC_TYPES,
       SEC_FUNC, SEC_VARS, SEC_BODY, SEC_COUNT };

typedef struct {
    Section sec[SEC_COUNT];
    uint32_t bound;                  /* next free id */
    struct Cached { uint32_t op, n, id, operands[8]; } *cache;   /* types and constants, to reuse */
    uint32_t ncache, cache_cap;
} Spv;

void spv_init(Spv *s);
void spv_free(Spv *s);
uint32_t spv_id(Spv *s);
/* Append an instruction: opcode and its operand words. */
void spv_inst(Spv *s, int section, SpvOp op, const uint32_t *operands, uint32_t n);
/* Same, with the operands as arguments (count them!): spv_op(s, SEC_FUNCS, SpvOpReturn, 0) */
void spv_op(Spv *s, int section, SpvOp op, uint32_t n, ...);
/* An instruction with a string operand (names, entry point), packed 4 chars per word. */
void spv_inst_str(Spv *s, int section, SpvOp op, const uint32_t *before, uint32_t nbefore, const char *str,
                  const uint32_t *after, uint32_t nafter);
/* A type or constant: returns the existing id if an identical one was made before. The
 * result id goes first in the instruction, except for result-type-first ops (constants). */
uint32_t spv_type(Spv *s, SpvOp op, const uint32_t *operands, uint32_t n);
uint32_t spv_constant(Spv *s, uint32_t type, uint32_t value);
uint32_t spv_bool_constant(Spv *s, uint32_t bool_type, int value);
uint32_t spv_composite_constant(Spv *s, uint32_t type, const uint32_t *ids, uint32_t n);
/* The finished module; the caller frees it. */
uint32_t *spv_finish(Spv *s, uint32_t *nwords);

#endif
