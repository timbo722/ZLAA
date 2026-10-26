/*
 * trace_symbols.c - LAA_TRACE_SYMBOLS="sym1,sym2,...": log calls to guest functions by name.
 *
 * Each named (exported or imported) function is replaced by a wrapper that prints its first
 * arguments, runs the original, and prints what it returned. Like every hle_replace, only calls
 * through a pointer or an import are seen, not direct calls inside the same module. For
 * debugging, e.g. which font-engine calls the game makes and whether they fail:
 *     LAA_TRACE_SYMBOLS=_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt
 */
#include "trace_symbols.h"
#include "hle.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRACED 16

static struct { char name[160]; uint64_t original; } traced[MAX_TRACED];
static int ntraced;

static void wrapper(CPU *c, int k) {
    fprintf(stderr, "trace: %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ")\n", traced[k].name,
            c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]);
    recomp_lookup(traced[k].original)(c);
    fprintf(stderr, "trace: %s -> %#" PRIx64 "\n", traced[k].name, c->x[0]);
}
#define W(k) static void wrapper_##k(CPU *c) { wrapper(c, k); }
W(0) W(1) W(2) W(3) W(4) W(5) W(6) W(7) W(8) W(9) W(10) W(11) W(12) W(13) W(14) W(15)
static const HleFunc wrappers[MAX_TRACED] = { wrapper_0, wrapper_1, wrapper_2, wrapper_3, wrapper_4, wrapper_5,
    wrapper_6, wrapper_7, wrapper_8, wrapper_9, wrapper_10, wrapper_11, wrapper_12, wrapper_13, wrapper_14, wrapper_15 };

void trace_symbols_init(const GameImage *img) {
    const char *list = getenv("LAA_TRACE_SYMBOLS");
    while (list && *list && ntraced < MAX_TRACED) {
        size_t n = strcspn(list, ",");
        if (n && n < sizeof traced[0].name) {
            memcpy(traced[ntraced].name, list, n);
            traced[ntraced].name[n] = 0;
            uint64_t a = nso_find_symbol(img, traced[ntraced].name);
            if (!a) fprintf(stderr, "trace: no symbol %s\n", traced[ntraced].name);
            else if (hle_replace(a, wrappers[ntraced]) == 0) traced[ntraced++].original = a;
        }
        list += n;
        if (*list == ',') list++;
    }
}
