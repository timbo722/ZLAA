/*
 * host/main.c - desktop (Linux) host program.
 *
 *   laa_host <exefs dir> [--romfs DIR] [--headless] [--dump-image FILE]
 *
 * Reserves guest memory, loads the game's modules from the player's own dump, opens a window
 * (SDL3 + Vulkan; --headless: none), then starts the game the way the Switch kernel would.
 * The game runs on its own threads; this thread handles the window's events.
 * Set LAA_TRACE_SVC=1 to log every SVC, LAA_TRACE_IPC=1 to log every service command.
 *
 * The RomFS is the extracted folder, by default "romfs" next to the ExeFS folder.
 * --dump-image writes the loaded modules' memory to FILE (and doesn't run anything) so
 * tests/check_loader.py can compare it byte for byte with what the recompiler saw.
 */
#include "recomp_runtime.h"
#include "guest_memory.h"
#include "nso_loader.h"
#include "kernel.h"
#include "romfs.h"
#include "hle.h"
#include "nvn/nvn.h"
#include "movie/movie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#ifdef LAA_HAVE_SDL
#include <SDL3/SDL.h>
#include "render/render.h"
#endif

#define LOAD_BASE 0x8000000ull   /* must match recomp.py --base (same as yuzu/Ryujinx) */

static int dump_image(const GameImage *img, const char *path) {
    uint64_t start = img->mods[0].base;
    uint64_t end = img->mods[img->count - 1].base + img->mods[img->count - 1].size;
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(guest_mem + start, 1, end - start, f) != end - start) {
        perror(path);
        if (f) fclose(f);
        return -1;
    }
    fclose(f);
    printf("Wrote guest memory 0x%" PRIx64 "..0x%" PRIx64 " to %s\n", start, end, path);
    return 0;
}

int main(int argc, char **argv) {
    const char *exefs = NULL, *romfs = NULL, *dump = NULL;
    int headless = 0;
    for (int k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--romfs") == 0 && k + 1 < argc) romfs = argv[++k];
        else if (strcmp(argv[k], "--headless") == 0) headless = 1;
        else if (strcmp(argv[k], "--dump-image") == 0 && k + 1 < argc) dump = argv[++k];
        else if (!exefs && argv[k][0] != '-') exefs = argv[k];
        else exefs = NULL, k = argc;   /* anything else: show usage */
    }
    if (!exefs) {
        fprintf(stderr, "usage: %s <exefs dir> [--romfs DIR] [--headless] [--dump-image FILE]\n", argv[0]);
        return 2;
    }
    if (gmem_reserve() != 0) return 1;

    GameImage img;
    if (nso_load_exefs(exefs, LOAD_BASE, &img) != 0) return 1;
    for (int k = 0; k < img.count; k++) {
        const GameModule *m = &img.mods[k];
        printf("  %-8s 0x%09" PRIx64 "  size 0x%08" PRIx64 "  build id ", m->name, m->base, m->size);
        for (int b = 0; b < 8; b++) printf("%02X", m->build_id[b]);
        printf("...\n");
    }
    printf("Loaded %d modules; %" PRIu64 " recompiled functions linked\n", img.count, recomp_func_count);

    if (dump) return dump_image(&img, dump) != 0;

    char romfs_default[4096];
    if (!romfs) {
        snprintf(romfs_default, sizeof romfs_default, "%s/../romfs", exefs);
        romfs = romfs_default;
    }
    if (romfs_open_dir(romfs) != 0) return 1;
    if (kernel_init(exefs, &img) != 0) return 1;
    hle_init(&kproc.image);
    nvn_init();
    movie_init(&kproc.image);             /* the game's graphics calls now come to runtime/nvn */

    void *window = NULL;
#ifdef LAA_HAVE_SDL
    if (!headless) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            fprintf(stderr, "No window: %s\n", SDL_GetError());
        } else {
            window = SDL_CreateWindow("LAARecomp", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (!window) fprintf(stderr, "No window: %s\n", SDL_GetError());
            else if (render_init(window) != 0) { SDL_DestroyWindow(window); window = NULL; }
        }
    }
#else
    (void)headless;
#endif
    if (!window) printf("Running without a window\n");
    if (kernel_start_main() != 0) return 1;

    for (;;) {
#ifdef LAA_HAVE_SDL
        SDL_Event e;
        while (window && SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) exit(0);
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) render_resized();
        }
#endif
        if (kernel_wait_main(window ? 10 : 1000)) break;   /* the game ended */
    }
    printf("kernel: main thread exited\n");
    return 0;
}
