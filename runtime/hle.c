/*
 * hle.c - fake function addresses and replaced guest functions. See hle.h.
 */
#include "hle.h"
#include "kernel.h"
#include "guest_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

/* ---- fake addresses: HLE_BASE + 16 * slot ---- */
static struct { char name[96]; HleFunc fn; } slots[HLE_MAX];
static int nslots;
static pthread_mutex_t hle_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread uint64_t calling;          /* which fake address is running (for the stub) */

/* ---- replaced guest functions: small open-addressing hash table ---- */
#define REPLACE_CAP 1024
static struct { uint64_t addr; HleFunc fn; } replaced[REPLACE_CAP];
static const GameImage *image;
int hle_trace;

static void unimplemented(CPU *c) {
    fprintf(stderr, "\nhle: %s is not implemented yet\n", hle_name(calling));
    (void)c;
    kernel_dump_state();
    exit(1);
}

/* LAA_TRACE_NVN: print the call, then run it */
static void traced_call(CPU *c) {
    uint64_t k = (calling - HLE_BASE) / 16;
    fprintf(stderr, "hle: %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ")\n",
            slots[k].name, c->x[0], c->x[1], c->x[2], c->x[3]);
    (slots[k].fn ? slots[k].fn : unimplemented)(c);
}

static RecompFunc override(uint64_t addr) {
    if (addr - HLE_BASE < 16ull * HLE_MAX) {
        uint64_t k = (addr - HLE_BASE) / 16;
        if (k >= (uint64_t)nslots) return NULL;
        calling = addr;
        if (hle_trace) return traced_call;
        return slots[k].fn ? slots[k].fn : unimplemented;
    }
    for (uint64_t k = (addr >> 2) & (REPLACE_CAP - 1); replaced[k].addr; k = (k + 1) & (REPLACE_CAP - 1))
        if (replaced[k].addr == addr) return replaced[k].fn;
    return NULL;
}

/* nn::diag::detail::AbortImpl(condition, function, file, line): the SDK's assert failure.
 * Say what failed before stopping (the original would just call svcBreak). */
static const char *guest_cstr(uint64_t a) {
    if (!a || a >= guest_mem_size) return "?";
    const char *s = (const char *)guest_mem + a;
    return memchr(s, 0, 512) ? s : "?";
}
static void abort_impl(CPU *c) {
    fprintf(stderr, "\ngame abort: %s\n  in %s\n  at %s:%d\n", guest_cstr(c->x[0]), guest_cstr(c->x[1]),
            guest_cstr(c->x[2]), (int)c->x[3]);
    kernel_dump_state();
    exit(1);
}

/* LAA_SPY=sym1,sym2,...: print each call to these guest functions (arguments, result, and
 * the first 32 bytes at the first argument afterwards: often an output struct). Calls made
 * through a pointer or an import only (see hle.h). Every 60th call, to keep it readable. */
#define MAX_SPIES 16
static struct { uint64_t addr; char name[128]; uint64_t calls; } spies[MAX_SPIES];
static int nspies;
static void spy(CPU *c, int k) {
    uint64_t in[4];
    memcpy(in, c->x, sizeof in);
    recomp_lookup(spies[k].addr)(c);
    uint32_t w2 = 0;                                 /* (e.g. buttons in an input state) */
    if (in[0] && in[0] + 32 < guest_mem_size) memcpy(&w2, guest_mem + in[0] + 8, 4);
    if (spies[k].calls++ % 60 && !w2) return;
    fprintf(stderr, "spy: %s(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ") -> %#" PRIx64 " [", spies[k].name, in[0], in[1], in[2], c->x[0]);
    for (int i = 0; i < 8 && in[0] && in[0] + 32 < guest_mem_size; i++) {
        uint32_t w;
        memcpy(&w, guest_mem + in[0] + 4 * i, 4);
        fprintf(stderr, " %08x", w);
    }
    fprintf(stderr, " ]\n");
}
#define SPY(n) static void spy_##n(CPU *c) { spy(c, n); }
SPY(0) SPY(1) SPY(2) SPY(3) SPY(4) SPY(5) SPY(6) SPY(7) SPY(8) SPY(9) SPY(10) SPY(11) SPY(12) SPY(13) SPY(14) SPY(15)
static const HleFunc spy_fns[MAX_SPIES] = { spy_0, spy_1, spy_2, spy_3, spy_4, spy_5, spy_6, spy_7, spy_8, spy_9,
                                            spy_10, spy_11, spy_12, spy_13, spy_14, spy_15 };

static void install_spies(void) {
    const char *list = getenv("LAA_SPY");
    while (list && *list && nspies < MAX_SPIES) {
        size_t n = strcspn(list, ",");
        snprintf(spies[nspies].name, sizeof spies[nspies].name, "%.*s", (int)n, list);
        spies[nspies].addr = nso_find_symbol(image, spies[nspies].name);
        if (spies[nspies].addr) hle_replace(spies[nspies].addr, spy_fns[nspies]), nspies++;
        else fprintf(stderr, "spy: no symbol %s\n", spies[nspies].name);
        list += n + (list[n] == ',');
    }
}

void hle_init(const GameImage *img) {
    image = img;
    recomp_override = override;
    hle_replace_symbol("_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", abort_impl);
    install_spies();
}

uint64_t hle_function(const char *name, HleFunc fn) {
    pthread_mutex_lock(&hle_lock);
    if (nslots == HLE_MAX) { fprintf(stderr, "hle: too many functions\n"); exit(1); }
    int k = nslots++;
    snprintf(slots[k].name, sizeof slots[k].name, "%s", name);
    slots[k].fn = fn;
    pthread_mutex_unlock(&hle_lock);
    return HLE_BASE + 16ull * (uint64_t)k;
}

const char *hle_name(uint64_t addr) {
    uint64_t k = (addr - HLE_BASE) / 16;
    return addr - HLE_BASE < 16ull * HLE_MAX && k < (uint64_t)nslots ? slots[k].name : NULL;
}

uint64_t hle_call(CPU *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    recomp_call_addr(c, fn);
    uint64_t result = c->x[0];
    memcpy(c->x, saved, sizeof saved);
    return result;
}

uint64_t hle_alloc(uint64_t size) {
    size = (size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1);
    uint64_t addr = kmem_map_free(KMAP_BASE, KMAP_BASE + KMAP_SIZE, size, MEM_CODE_DATA, PERM_RW);
    if (!addr) { fprintf(stderr, "hle: out of guest memory (%#" PRIx64 " bytes)\n", size); exit(1); }
    return addr;
}

int hle_replace(uint64_t guest_addr, HleFunc fn) {
    uint64_t k = (guest_addr >> 2) & (REPLACE_CAP - 1);
    for (int n = 0; n < REPLACE_CAP; n++, k = (k + 1) & (REPLACE_CAP - 1)) {
        if (!replaced[k].addr || replaced[k].addr == guest_addr) {
            replaced[k].fn = fn;
            replaced[k].addr = guest_addr;
            recomp_set_target(guest_addr, fn);
            return 0;
        }
    }
    return -1;
}

int hle_replace_symbol(const char *symbol, HleFunc fn) {
    uint64_t a = nso_find_symbol(image, symbol);
    if (!a) {
        fprintf(stderr, "hle: symbol %s not found\n", symbol);
        return -1;
    }
    return hle_replace(a, fn);
}
