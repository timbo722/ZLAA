/*
 * recomp_runtime.c - support code shared by all recompiled programs.
 * Indirect-call lookup and error reporting. SVCs (OS calls) are in svc.c.
 */
#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <time.h>

/* recomp_func_table is sorted by address, so binary search it. */
RecompFunc recomp_lookup(uint64_t addr) {
    uint64_t lo = 0, hi = recomp_func_count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (recomp_func_table[mid].addr == addr) return recomp_func_table[mid].fn;
        if (recomp_func_table[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

RecompFunc (*recomp_override)(uint64_t addr);

/* Indirect calls look the function up in a hash table of all of them (address -> C function),
 * built on first use: one or two probes, where the binary search above takes ~17 through 2 MB
 * of table (indirect calls were 18% of the game's main thread). Replacements (hle_replace) are
 * put in it too; addresses it doesn't have (HLE functions) go to recomp_override. */
#define DISPATCH_BITS 19                     /* 512K slots for ~150K functions */
static struct { uint64_t addr; RecompFunc fn; } *dispatch;
static pthread_once_t dispatch_once = PTHREAD_ONCE_INIT;

static inline uint64_t dispatch_slot(uint64_t addr) { return (addr >> 2) * 0x9E3779B97F4A7C15ull >> (64 - DISPATCH_BITS); }

static void dispatch_put(uint64_t addr, RecompFunc fn) {
    uint64_t k = dispatch_slot(addr);
    while (dispatch[k].addr && dispatch[k].addr != addr) k = (k + 1) & ((1u << DISPATCH_BITS) - 1);
    dispatch[k].fn = fn;
    dispatch[k].addr = addr;
}

static int dispatch_ready;

static void dispatch_build(void) {
    dispatch = calloc(1u << DISPATCH_BITS, sizeof *dispatch);
    if (!dispatch) { fprintf(stderr, "recomp: out of memory\n"); abort(); }
    for (uint64_t i = 0; i < recomp_func_count; i++) dispatch_put(recomp_func_table[i].addr, recomp_func_table[i].fn);
    __atomic_store_n(&dispatch_ready, 1, __ATOMIC_RELEASE);
}

void recomp_set_target(uint64_t addr, RecompFunc fn) {
    pthread_once(&dispatch_once, dispatch_build);
    dispatch_put(addr, fn);
}

void recomp_call_addr(CPU *c, uint64_t addr) {
    /* (a flag once it is built: pthread_once on every call was ~1% of the main thread) */
    if (__builtin_expect(!__atomic_load_n(&dispatch_ready, __ATOMIC_ACQUIRE), 0)) pthread_once(&dispatch_once, dispatch_build);
    for (uint64_t k = dispatch_slot(addr); dispatch[k].addr; k = (k + 1) & ((1u << DISPATCH_BITS) - 1))
        if (dispatch[k].addr == addr) { dispatch[k].fn(c); return; }
    RecompFunc fn = recomp_override ? recomp_override(addr) : NULL;
    if (!fn) fn = recomp_lookup(addr);
    if (!fn) {
        fprintf(stderr, "recomp: indirect call to unknown address 0x%" PRIx64 "\n", addr);
        abort();
    }
    fn(c);
}

void recomp_unimplemented(CPU *c, uint64_t pc, uint32_t insn) {
    (void)c;
    fprintf(stderr, "recomp: unimplemented instruction 0x%08" PRIx32 " at 0x%" PRIx64 "\n", insn, pc);
    abort();
}

void recomp_bad_jump(CPU *c, uint64_t pc, uint64_t target) {
    (void)c;
    fprintf(stderr, "recomp: jump from 0x%" PRIx64 " to 0x%" PRIx64 " has no known destination\n", pc, target);
    abort();
}

void recomp_trap(CPU *c, uint64_t pc, uint32_t insn) {
    (void)c;
    fprintf(stderr, "recomp: trap instruction 0x%08" PRIx32 " at 0x%" PRIx64 "\n", insn, pc);
    abort();
}

void recomp_mem_fault(CPU *c, uint64_t addr, int size) {
    fprintf(stderr, "recomp: %d-byte access at 0x%" PRIx64 " is outside guest memory (size 0x%" PRIx64 ")\n",
            size, addr, c->mem_size);
    abort();
}

/* CNTPCT_EL0: the Switch's system counter ticks at 19.2 MHz. */
uint64_t recomp_cntpct(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 19200000ull + (uint64_t)ts.tv_nsec * 192 / 10000;
}
