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

void hle_init(const GameImage *img) {
    image = img;
    recomp_override = override;
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
