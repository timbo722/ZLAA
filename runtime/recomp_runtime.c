/*
 * recomp_runtime.c - support code shared by all recompiled programs.
 * Indirect-call lookup and error reporting. SVCs (OS calls) are in svc.c.
 */
#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
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

void recomp_call_addr(CPU *c, uint64_t addr) {
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
