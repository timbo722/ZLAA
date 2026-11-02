/*
 * pl.c - "pl:u", the shared system fonts. The game draws its text with them.
 *
 * The Switch's system fonts are Nintendo's and are not part of the game, so this project
 * never ships them. The font for each of the 6 slots comes from, in order:
 *   1. the player's own dump of that system font, in $LAA_FONT_DIR or
 *      ~/.local/share/LAARecomp/fonts (the .bfttf files from the console's system data)
 *   2. a font already installed on the host: $LAA_HOST_FONT, or DejaVu Sans on Linux, or
 *      Noto Serif on Android. It must have U+25A1 (white square): the game puts that glyph
 *      in its cache at startup and crashes if the font lacks it. Roboto doesn't have it.
 *
 * In shared memory each font is an 8-byte header (magic, size XOR key) then the TrueType
 * data. A .bfttf file is an 8-byte header (magic, size) then the TrueType data, every 32-bit
 * word read big-endian and XOR'd with a key; we decode it. switchbrew.org/wiki/Shared_Database_services
 * (optional: this project has no tool that extracts them, and never asks for keys)
 */
#include "services.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHARED_FONT_MEMORY_SIZE 0x1100000
#define FONT_TYPES 6
#define BFTTF_MAGIC 0x36F81A1Eu     /* first word of a .bfttf file */
#define TTF_MARK 0x7F9A0218u        /* what that word decodes to; magic ^ mark = the key */
#define FONT_KEY 0x49621806u

/* Standard, Chinese simplified, extended Chinese simplified, Chinese traditional, Korean,
 * Nintendo extended (button symbols) */
static const char *const bfttf_names[FONT_TYPES] = {
    "nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.bfttf",
    "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udjxh-db_zh-tw_003.bfttf",
    "nintendo_udsg-r_ko_003.bfttf", "nintendo_ext_003.bfttf",
};
static const char *host_fonts[] = {
    NULL,                                        /* $LAA_HOST_FONT */
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/system/fonts/NotoSerif-Regular.ttf",       /* Android: the only Latin font there with U+25A1 */
};

static KSharedMemory *font_shm;
static uint32_t font_offset[FONT_TYPES], font_size[FONT_TYPES];   /* offset: of the TrueType data */

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* .bfttf -> TrueType, in place. Returns the TrueType size, or 0 if it isn't a .bfttf. */
static size_t decode_bfttf(uint8_t *buf, size_t len) {
    uint32_t w0, w1;
    if (len < 8) return 0;
    memcpy(&w0, buf, 4);
    memcpy(&w1, buf + 4, 4);
    w0 = __builtin_bswap32(w0);                      /* the file's words are big-endian */
    w1 = __builtin_bswap32(w1);
    if (w0 != BFTTF_MAGIC) return 0;
    uint32_t key = w0 ^ TTF_MARK;
    size_t size = w1 ^ key;
    if (size > len - 8) return 0;
    /* XOR each word with the key, in the file's byte order: the TrueType data comes out as is */
    uint32_t key_bytes = __builtin_bswap32(key);
    for (size_t k = 8; k + 4 <= len; k += 4) {
        uint32_t w;
        memcpy(&w, buf + k, 4);
        w ^= key_bytes;
        memcpy(buf + k - 8, &w, 4);
    }
    return size;
}

static void load_fonts(void) {
    font_shm = kshm_new(SHARED_FONT_MEMORY_SIZE);
    const char *env = getenv("LAA_FONT_DIR"), *home = getenv("HOME");
    char dir[2048];
    if (env) snprintf(dir, sizeof dir, "%s", env);
    else snprintf(dir, sizeof dir, "%s/.local/share/LAARecomp/fonts", home ? home : ".");

    host_fonts[0] = getenv("LAA_HOST_FONT");

    uint32_t at = 0;
    int fallback_type = -1;                      /* slot already holding the host font */
    for (int t = 0; t < FONT_TYPES; t++) {
        char path[4096];
        size_t len = 0, size = 0;
        snprintf(path, sizeof path, "%s/%s", dir, bfttf_names[t]);
        uint8_t *buf = read_all(path, &len);
        if (buf) size = decode_bfttf(buf, len);
        if (!size) {
            free(buf);
            buf = NULL;
            if (fallback_type >= 0) {            /* same host font for every missing slot */
                font_offset[t] = font_offset[fallback_type];
                font_size[t] = font_size[fallback_type];
                continue;
            }
            for (size_t k = 0; !buf && k < sizeof host_fonts / sizeof *host_fonts; k++) {
                buf = host_fonts[k] ? read_all(host_fonts[k], &len) : NULL;
                if (buf) { snprintf(path, sizeof path, "%s", host_fonts[k]); size = len; }
            }
            if (!buf) { fprintf(stderr, "pl: no font found for slot %d\n", t); continue; }
            fallback_type = t;
        }
        if (at + 8 + size > SHARED_FONT_MEMORY_SIZE) { fprintf(stderr, "pl: fonts don't fit\n"); free(buf); break; }
        uint32_t header[2] = { 0x18029A7Fu ^ FONT_KEY, (uint32_t)size ^ FONT_KEY };
        memcpy(font_shm->host + at, header, 8);
        memcpy(font_shm->host + at + 8, buf, size);
        font_offset[t] = at + 8;
        font_size[t] = (uint32_t)size;
        at = (uint32_t)((at + 8 + size + 3) & ~3u);
        printf("pl: font slot %d: %s (%zu bytes)\n", t, path, size);
        free(buf);
    }
}

static void pl_dispatch(ServiceObject *self, IpcCall *call) {
    uint32_t t = ipc_in32(call, 0);
    if (t >= FONT_TYPES) t = 0;
    switch (call->cmd) {
    case 0:                                       /* RequestLoad(type) */
        break;
    case 1:                                       /* GetLoadState(type) -> 1 = loaded */
        ipc_out32(call, 1);
        break;
    case 2:                                       /* GetSize(type) */
        ipc_out32(call, font_size[t]);
        break;
    case 3:                                       /* GetSharedMemoryAddressOffset(type) */
        ipc_out32(call, font_offset[t]);
        break;
    case 4:                                       /* GetSharedMemoryNativeHandle */
        ipc_out_handle(call, kshm_handle(font_shm), 0);
        break;
    case 5: {                                     /* GetSharedFontInOrderOfPriority(language) */
        static const uint32_t order[FONT_TYPES] = { 0, 5, 1, 2, 3, 4 };
        IpcBuffer bt = ipc_out_buffer(call, 0), bo = ipc_out_buffer(call, 1), bs = ipc_out_buffer(call, 2);
        uint8_t *pt = ipc_guest_ptr(bt.addr, bt.size), *po = ipc_guest_ptr(bo.addr, bo.size),
                *ps = ipc_guest_ptr(bs.addr, bs.size);
        uint32_t n = 0;
        for (; n < FONT_TYPES && (n + 1) * 4 <= bt.size && (n + 1) * 4 <= bo.size && (n + 1) * 4 <= bs.size; n++) {
            if (pt) memcpy(pt + 4 * n, &order[n], 4);
            if (po) memcpy(po + 4 * n, &font_offset[order[n]], 4);
            if (ps) memcpy(ps + 4 * n, &font_size[order[n]], 4);
        }
        ipc_out32(call, 1);                       /* u8: all fonts loaded */
        ipc_out32(call, n);
        break;
    }
    default:
        ipc_unknown_command(self, call);
    }
}
static const ServiceType pl_type = { "pl:u", pl_dispatch, NULL };

ServiceObject *service_pl_create(void) {
    if (!font_shm) load_fonts();
    return ipc_new_object(&pl_type, NULL);
}
