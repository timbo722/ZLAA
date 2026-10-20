/*
 * nso_loader.h - load the game's ExeFS (rtld, main, subsdk*, sdk) into guest memory.
 *
 * This does at startup what the Switch kernel does: place each module at its address,
 * exactly as nso_loader.py does (same order, same addresses), but without relocating:
 * rtld, the game's own dynamic linker, does that when it runs.
 * The game files are read from the player's own dump; nothing is embedded in the build.
 */
#ifndef NSO_LOADER_H
#define NSO_LOADER_H

#include <stdint.h>

#define MAX_MODULES 13           /* rtld, main, subsdk0..9, sdk */

typedef struct {
    char     name[16];
    uint64_t base, size;         /* guest address and total size (incl. .bss, page aligned) */
    uint64_t text_off, text_size;
    uint64_t ro_off, ro_size;
    uint64_t data_off, data_size, bss_size;
    uint8_t  build_id[32];
} GameModule;

typedef struct {
    GameModule mods[MAX_MODULES];
    int count;
} GameImage;

/* Loads every module found in exefs_dir starting at `base`. Checks that they are the
 * same files the code was generated from. Needs gmem_reserve() first. 0 on success. */
int nso_load_exefs(const char *exefs_dir, uint64_t base, GameImage *out);

#endif
