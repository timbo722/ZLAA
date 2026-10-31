/*
 * game_lib.h - the game's recompiled code is a library of its own, built from the player's
 * own dump (never shipped with the app): libgame-<main build id>.so. See game_lib.c.
 */
#ifndef GAME_LIB_H
#define GAME_LIB_H

#include <stddef.h>
#include <stdint.h>

/* Read the build id of <exefs_dir>/main, open the game library made from it in lib_dir
 * (copying it there first from import_dir if another one was put there; import_dir may be
 * NULL), check it was built against this runtime's recomp_runtime.h, and hand its tables to
 * the runtime. 0 = ok; otherwise it says why on stderr. */
int game_lib_load(const char *exefs_dir, const char *lib_dir, const char *import_dir);

/* "f_<guest address>" (written into buf) if pc is inside the game library's code, else NULL;
 * *offset (if not NULL) = pc's distance from that function's start. (The game's functions
 * are hidden symbols there, so dladdr can't name them.) */
const char *game_lib_symbol(uintptr_t pc, char *buf, size_t size, unsigned long *offset);

#endif
