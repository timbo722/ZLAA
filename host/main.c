/*
 * host/main.c - desktop (Linux) host program.
 *
 * Phase 3a: this only proves that all the generated game code compiled and linked.
 * The linker had to find every f_XXXX the game calls; if one were missing, the build
 * would have failed. Loading the ExeFS and calling the entry point come next (3b).
 */
#include "recomp_runtime.h"
#include <stdio.h>
#include <inttypes.h>

int main(void) {
    /* recomp_lookup() binary-searches the table, so it must be sorted by address */
    for (uint64_t k = 1; k < recomp_func_count; k++) {
        if (recomp_func_table[k - 1].addr >= recomp_func_table[k].addr) {
            fprintf(stderr, "function table not sorted at entry %" PRIu64 "\n", k);
            return 1;
        }
    }
    printf("Linked %" PRIu64 " recompiled functions (0x%" PRIx64 " .. 0x%" PRIx64 ")\n",
           recomp_func_count, recomp_func_table[0].addr, recomp_func_table[recomp_func_count - 1].addr);
    printf("Lookup of 0x%" PRIx64 ": %s\n", recomp_func_table[0].addr,
           recomp_lookup(recomp_func_table[0].addr) ? "ok" : "MISSING");
    return 0;
}
