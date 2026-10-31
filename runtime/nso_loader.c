/*
 * nso_loader.c - read NSO files and decompress them into guest memory.
 * Follows recompiler/nso_loader.py step by step; see there (and switchbrew.org/wiki/NSO)
 * for what the formats mean. Every offset read from the files is bounds-checked, so a
 * damaged dump gives an error message instead of a crash.
 */
#include "nso_loader.h"
#include "guest_memory.h"
#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const MODULE_ORDER[MAX_MODULES] = {
    "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
    "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk",
};

#define FAIL(...) do { fprintf(stderr, "loader: " __VA_ARGS__); fputc('\n', stderr); goto fail; } while (0)

/* Little-endian reads from a byte buffer (the Switch and our hosts are all little-endian) */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1)) != NULL) {
            if (fread(buf, 1, (size_t)n, f) == (size_t)n) *len = (size_t)n;
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

/* LZ4 block format: a series of (literal bytes to copy, then a match = copy `len` bytes
 * from `offset` bytes back in the output). Returns 0 if exactly dst_len bytes came out. */
static int lz4_decompress(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    size_t i = 0, o = 0;
    while (i < src_len) {
        uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do { if (i >= src_len) return -1; b = src[i++]; lit += b; } while (b == 255);
        }
        if (lit > src_len - i || lit > dst_len - o) return -1;
        memcpy(dst + o, src + i, lit);
        i += lit; o += lit;
        if (i >= src_len) break;                 /* the last sequence has only literals */
        if (src_len - i < 2) return -1;
        size_t offset = src[i] | ((size_t)src[i + 1] << 8);
        i += 2;
        size_t len = token & 15;
        if (len == 15) {
            uint8_t b;
            do { if (i >= src_len) return -1; b = src[i++]; len += b; } while (b == 255);
        }
        len += 4;
        if (offset == 0 || offset > o || len > dst_len - o) return -1;
        for (size_t k = 0; k < len; k++, o++)    /* byte by byte: a match may overlap itself */
            dst[o] = dst[o - offset];
    }
    return o == dst_len ? 0 : -1;
}

int nso_load_exefs(const char *exefs_dir, uint64_t base, GameImage *out) {
    uint8_t *file = NULL;
    memset(out, 0, sizeof *out);
    uint64_t addr = base;

    /* ---- 1. read every module and place it in memory, in the fixed order ---- */
    for (int k = 0; k < MAX_MODULES; k++) {
        char path[4096];
        size_t len = 0;
        snprintf(path, sizeof path, "%s/%s", exefs_dir, MODULE_ORDER[k]);
        file = read_file(path, &len);
        if (!file) continue;                           /* most games have no subsdk2..9 */
        GameModule *m = &out->mods[out->count];
        snprintf(m->name, sizeof m->name, "%s", MODULE_ORDER[k]);
        if (len < 0x100 || memcmp(file, "NSO0", 4) != 0) FAIL("%s is not an NSO file", path);
        uint32_t flags = rd32(file + 0x0C);
        if (flags & 0x80) FAIL("%s uses zstd compression (firmware 22+ format), not supported", path);

        /* header: text at 0x10, rodata at 0x20, data at 0x30 = (file offset, memory offset, size) */
        uint64_t *offs[3] = { &m->text_off, &m->ro_off, &m->data_off };
        uint64_t *sizes[3] = { &m->text_size, &m->ro_size, &m->data_size };
        for (int s = 0; s < 3; s++) {
            *offs[s] = rd32(file + 0x14 + 0x10 * s);
            *sizes[s] = rd32(file + 0x18 + 0x10 * s);
        }
        m->bss_size = rd32(file + 0x3C);
        memcpy(m->build_id, file + 0x40, 32);
        m->dynstr_off = rd32(file + 0x90);
        m->dynsym_off = rd32(file + 0x98);
        m->dynsym_size = rd32(file + 0x9C);
        m->base = addr;
        m->size = (m->data_off + m->data_size + m->bss_size + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
        if (m->ro_off < m->text_off + m->text_size || m->data_off < m->ro_off + m->ro_size)
            FAIL("%s: segments overlap", path);
        if (gmem_protect(m->base, m->size, GMEM_READ | GMEM_WRITE) != 0) goto fail;

        for (int s = 0; s < 3; s++) {
            uint32_t file_off = rd32(file + 0x10 + 0x10 * s), comp = rd32(file + 0x60 + 4 * s);
            uint8_t *dst = guest_mem + m->base + *offs[s];
            int compressed = (flags >> s) & 1;
            uint64_t stored = compressed ? comp : *sizes[s];
            if (file_off > len || stored > len - file_off) FAIL("%s is truncated", path);
            if (compressed ? lz4_decompress(file + file_off, comp, dst, *sizes[s]) != 0
                           : (memcpy(dst, file + file_off, *sizes[s]), 0))
                FAIL("%s: segment %d failed to decompress (damaged dump?)", path, s);
        }
        free(file);
        file = NULL;
        addr += m->size;
        out->count++;
    }
    if (out->count == 0) FAIL("no NSO modules (main, sdk, rtld, ...) found in %s", exefs_dir);

    /* ---- 2. are these the files the code was generated from? ---- */
    int same = out->count == (int)game_module_count;
    for (int k = 0; same && k < out->count; k++) {
        const GameModule *m = &out->mods[k];
        const RecompModuleInfo *g = &game_modules[k];
        same = strcmp(m->name, g->name) == 0 && m->base == g->base && m->size == g->size &&
               memcmp(m->build_id, g->build_id, 32) == 0;
    }
    if (!same) {
        fprintf(stderr, "loader: the ExeFS in %s is not the one the game library was generated from.\n"
                        "        Expected:", exefs_dir);
        for (uint64_t k = 0; k < game_module_count; k++) fprintf(stderr, " %s", game_modules[k].name);
        fprintf(stderr, " (main build id ");
        for (uint64_t k = 0; k < game_module_count; k++)
            if (strcmp(game_modules[k].name, "main") == 0)
                for (int b = 0; b < 8; b++) fprintf(stderr, "%02X", game_modules[k].build_id[b]);
        fprintf(stderr, "...). Rebuild the game library from this dump.\n");
        goto fail;
    }

    /* No relocations here: like the real kernel we leave that to rtld, which runs first.
     * (rtld binds imports lazily and checks that the slots still hold their original
     * values, so they must not be filled in ahead of time.) kernel_init() sets the
     * final access rights. */
    return 0;

fail:
    free(file);
    return -1;
}

/* Elf64_Sym (24 bytes): name offset (4), info (1), other (1), section (2), value (8), size (8).
 * Section 0 means "not defined here" (an import). */
uint64_t nso_find_symbol(const GameImage *img, const char *name) {
    size_t len = strlen(name);
    for (int k = 0; k < img->count; k++) {
        const GameModule *m = &img->mods[k];
        uint64_t sym = m->base + m->ro_off + m->dynsym_off, str = m->base + m->ro_off + m->dynstr_off;
        for (uint64_t i = 0; i < m->dynsym_size / 24; i++) {
            const uint8_t *e = guest_mem + sym + 24 * i;
            uint16_t shndx;
            uint64_t value;
            memcpy(&shndx, e + 6, 2);
            memcpy(&value, e + 8, 8);
            const char *s = (const char *)guest_mem + str + rd32(e);
            if (shndx != 0 && strncmp(s, name, len + 1) == 0) return m->base + value;
        }
    }
    return 0;
}
