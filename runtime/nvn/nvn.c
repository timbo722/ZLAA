/*
 * nvn.c - NVN, the Switch's graphics API, implemented on the host.
 *
 * The game never links to NVN functions directly. It calls nvnBootstrapLoader (exported by
 * the sdk module) to get nvnDeviceGetProcAddress, then looks up every NVN function by name
 * and calls it through a pointer. We replace nvnBootstrapLoader, so every name the game asks
 * for resolves to one of our C functions (hle.c gives each a fake guest address). The real
 * NVN driver in the sdk module is then never used, and neither is the null GPU behind nvdrv.
 */
#include "nvn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nvn_trace;

/* name -> implementation; names not listed stop the program when the game calls them */
static const NvnEntry *const tables[] = {
    nvn_device_functions, nvn_memory_functions, nvn_texture_functions,
    nvn_state_functions, nvn_command_functions, NULL,
};

#define NVN_OP_NAME(name) #name,
const char *const nvn_command_names[] = { NVN_COMMANDS(NVN_OP_NAME) };

static uint64_t proc_address(const char *name) {
    static struct { char name[96]; uint64_t addr; } cache[1024];
    static int ncache;
    for (int k = 0; k < ncache; k++)
        if (strcmp(cache[k].name, name) == 0) return cache[k].addr;
    HleFunc fn = NULL;
    for (int t = 0; tables[t] && !fn; t++)
        for (const NvnEntry *e = tables[t]; e->name; e++)
            if (strcmp(e->name, name) == 0) { fn = e->fn; break; }
    if (strcmp(name, "nvnDeviceGetProcAddress") == 0) fn = nvn_get_proc_address;
    uint64_t addr = hle_function(name, fn);
    if (ncache < 1024) {
        snprintf(cache[ncache].name, sizeof cache[ncache].name, "%s", name);
        cache[ncache++].addr = addr;
    }
    return addr;
}

/* void *nvnDeviceGetProcAddress(const NVNdevice *, const char *name) */
void nvn_get_proc_address(CPU *c) {
    const char *name = guest_str(c->x[1]);
    c->x[0] = name && *name ? proc_address(name) : 0;
}

/* void *nvnBootstrapLoader(const char *name) */
static void bootstrap_loader(CPU *c) {
    const char *name = guest_str(c->x[0]);
    c->x[0] = name && *name ? proc_address(name) : 0;
}

void nvn_init(void) {
    nvn_trace = getenv("LAA_TRACE_NVN") ? atoi(getenv("LAA_TRACE_NVN")) : 0;   /* 1 calls, 2 + commands */
    hle_trace = nvn_trace;
    hle_replace_symbol("nvnBootstrapLoader", bootstrap_loader);
}
