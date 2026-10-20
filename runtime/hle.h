/*
 * hle.h - high-level emulation: run C code in place of a guest function.
 *
 * Two ways in, both through indirect calls (BLR / calls through a pointer or an import):
 *   hle_function()  makes a fake guest address that, when called, runs a C function.
 *                   E.g. our nvnDeviceGetProcAddress hands these out for NVN functions.
 *   hle_replace()   makes calls to an existing guest function run C instead.
 *                   (Only calls that go through a pointer; direct BLs inside the same
 *                   module still reach the recompiled code.)
 * A C function gets the CPU registers: arguments in c->x[0..7] / c->V[0..7], result in c->x[0].
 */
#ifndef HLE_H
#define HLE_H

#include "recomp_runtime.h"
#include "nso_loader.h"

typedef void (*HleFunc)(CPU *c);

/* Fake addresses live here: never mapped, so reading or writing them faults. */
#define HLE_BASE 0x0E00000000ull
#define HLE_MAX  8192

extern int hle_trace;                      /* print every call to a fake address */

void hle_init(const GameImage *img);
/* A new fake address for `fn` (named for error messages). fn == NULL: stop with the name
 * when the game calls it (a function we haven't written yet). */
uint64_t hle_function(const char *name, HleFunc fn);
int hle_replace(uint64_t guest_addr, HleFunc fn);
int hle_replace_symbol(const char *symbol, HleFunc fn);   /* 0 on success */
/* Name of the fake function at addr, or NULL. */
const char *hle_name(uint64_t addr);

/* Call a guest function from C on this thread's CPU: up to 4 integer arguments, returns x0.
 * The CPU's registers are restored afterwards, so the caller's arguments stay readable. */
uint64_t hle_call(CPU *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
/* Guest memory for HLE code (zeroed, page-granular, never freed). Exits if out of space. */
uint64_t hle_alloc(uint64_t size);

#endif
