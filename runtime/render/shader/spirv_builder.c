/*
 * spirv_builder.c - see spirv_builder.h.
 */
#include "spirv_builder.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void spv_init(Spv *s) {
    memset(s, 0, sizeof *s);
    s->bound = 1;
}

void spv_free(Spv *s) {
    for (int i = 0; i < SEC_COUNT; i++) free(s->sec[i].w);
    free(s->cache);
    memset(s, 0, sizeof *s);
}

uint32_t spv_id(Spv *s) { return s->bound++; }

static void put(Section *sec, uint32_t w) {
    if (sec->n == sec->cap) {
        sec->cap = sec->cap ? sec->cap * 2 : 256;
        sec->w = realloc(sec->w, sec->cap * sizeof *sec->w);
        if (!sec->w) { fprintf(stderr, "spirv: out of memory\n"); exit(1); }
    }
    sec->w[sec->n++] = w;
}

void spv_inst(Spv *s, int section, SpvOp op, const uint32_t *operands, uint32_t n) {
    Section *sec = &s->sec[section];
    put(sec, (n + 1) << 16 | (uint32_t)op);
    for (uint32_t i = 0; i < n; i++) put(sec, operands[i]);
}

void spv_op(Spv *s, int section, SpvOp op, uint32_t n, ...) {
    uint32_t w[32];
    va_list ap;
    va_start(ap, n);
    for (uint32_t i = 0; i < n && i < 32; i++) w[i] = va_arg(ap, uint32_t);
    va_end(ap);
    spv_inst(s, section, op, w, n);
}

void spv_inst_str(Spv *s, int section, SpvOp op, const uint32_t *before, uint32_t nbefore, const char *str,
                  const uint32_t *after, uint32_t nafter) {
    uint32_t len = (uint32_t)strlen(str), nstr = len / 4 + 1;      /* always room for the terminating 0 */
    Section *sec = &s->sec[section];
    put(sec, (1 + nbefore + nstr + nafter) << 16 | (uint32_t)op);
    for (uint32_t i = 0; i < nbefore; i++) put(sec, before[i]);
    for (uint32_t i = 0; i < nstr; i++) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4; b++) {
            uint32_t k = i * 4 + b;
            if (k < len) word |= (uint32_t)(uint8_t)str[k] << (8 * b);
        }
        put(sec, word);
    }
    for (uint32_t i = 0; i < nafter; i++) put(sec, after[i]);
}

/* Look for an identical type/constant made before. key: op + operands (without result id). */
static uint32_t cached(Spv *s, uint32_t op, const uint32_t *operands, uint32_t n) {
    if (n > 8) return 0;
    for (uint32_t i = 0; i < s->ncache; i++) {
        struct Cached *c = &s->cache[i];
        if (c->op == op && c->n == n && memcmp(c->operands, operands, n * 4) == 0) return c->id;
    }
    return 0;
}

static void remember(Spv *s, uint32_t op, const uint32_t *operands, uint32_t n, uint32_t id) {
    if (n > 8) return;
    if (s->ncache == s->cache_cap) {
        s->cache_cap = s->cache_cap ? s->cache_cap * 2 : 128;
        s->cache = realloc(s->cache, s->cache_cap * sizeof *s->cache);
    }
    struct Cached *c = &s->cache[s->ncache++];
    c->op = op;
    c->n = n;
    c->id = id;
    memcpy(c->operands, operands, n * 4);
}

uint32_t spv_type(Spv *s, SpvOp op, const uint32_t *operands, uint32_t n) {
    uint32_t id = cached(s, op, operands, n);
    if (id) return id;
    id = spv_id(s);
    uint32_t w[16];
    w[0] = id;
    memcpy(w + 1, operands, n * 4);
    spv_inst(s, SEC_TYPES, op, w, n + 1);
    remember(s, op, operands, n, id);
    return id;
}

/* constants: OpConstant <type> <id> <value>; the cache key is (type, value) */
uint32_t spv_constant(Spv *s, uint32_t type, uint32_t value) {
    uint32_t key[2] = { type, value };
    uint32_t id = cached(s, SpvOpConstant, key, 2);
    if (id) return id;
    id = spv_id(s);
    spv_op(s, SEC_TYPES, SpvOpConstant, 3, type, id, value);
    remember(s, SpvOpConstant, key, 2, id);
    return id;
}

uint32_t spv_bool_constant(Spv *s, uint32_t bool_type, int value) {
    SpvOp op = value ? SpvOpConstantTrue : SpvOpConstantFalse;
    uint32_t id = cached(s, op, &bool_type, 1);
    if (id) return id;
    id = spv_id(s);
    spv_op(s, SEC_TYPES, op, 2, bool_type, id);
    remember(s, op, &bool_type, 1, id);
    return id;
}

uint32_t spv_composite_constant(Spv *s, uint32_t type, const uint32_t *ids, uint32_t n) {
    uint32_t key[8] = { type };
    memcpy(key + 1, ids, n * 4);
    uint32_t id = cached(s, SpvOpConstantComposite, key, n + 1);
    if (id) return id;
    id = spv_id(s);
    uint32_t w[16] = { type, id };
    memcpy(w + 2, ids, n * 4);
    spv_inst(s, SEC_TYPES, SpvOpConstantComposite, w, n + 2);
    remember(s, SpvOpConstantComposite, key, n + 1, id);
    return id;
}

uint32_t *spv_finish(Spv *s, uint32_t *nwords) {
    uint32_t total = 5;
    for (int i = 0; i < SEC_COUNT; i++) total += s->sec[i].n;
    uint32_t *out = malloc(total * 4);
    out[0] = SpvMagicNumber;
    out[1] = 0x00010300;                             /* SPIR-V 1.3 (Vulkan 1.1+) */
    out[2] = 0;                                      /* generator */
    out[3] = s->bound;
    out[4] = 0;
    uint32_t at = 5;
    for (int i = 0; i < SEC_COUNT; i++) {
        memcpy(out + at, s->sec[i].w, s->sec[i].n * 4);
        at += s->sec[i].n;
    }
    *nwords = total;
    return out;
}
