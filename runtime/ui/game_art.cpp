/*
 * game_art.cpp - the game's own logo for the start screen's title, read from the player's dump while the app runs and
 * drawn in the menus' colours. Nothing of the game is in this repository or the app: without game files the start
 * screen shows the name as text.
 *
 * The title screen's pictures are in romfs/region_common/ui/StartUp.arc: a SARC archive (a list of named files)
 * holding timg/__Combined.bntx, a BNTX texture container. Its "Logo_00^f" is the logo, 1092 x 546, two 8-bit
 * channels (R8G8): the letters' shape, and a soft edge around them (the game colours it itself). Like every Switch
 * texture it is block-linear: 64-byte x 8-row tiles ("GOBs") stacked in blocks, not row after row.
 * Here: RGBA, premultiplied (what the menu's renderer takes), the letters in the menus' gold (pale at the top, deeper
 * below) and the soft edge as a faint warm glow around them, like the egg's.
 */
#include "ui_internal.h"
#include <SDL3/SDL.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

uint32_t u16(const std::vector<uint8_t> &b, size_t o) { return o + 2 <= b.size() ? b[o] | b[o + 1] << 8 : 0; }
uint32_t u32(const std::vector<uint8_t> &b, size_t o) { return o + 4 <= b.size() ? u16(b, o) | u16(b, o + 2) << 16 : 0; }
uint64_t u64(const std::vector<uint8_t> &b, size_t o) { return u32(b, o) | (uint64_t)u32(b, o + 4) << 32; }

/* A file in a SARC archive: [start, end) in b, or false */
bool sarc_file(const std::vector<uint8_t> &b, const char *name, size_t &start, size_t &end) {
    if (b.size() < 0x20 || memcmp(b.data(), "SARC", 4) != 0) return false;
    size_t sfat = u16(b, 4), data = u32(b, 0xC);
    if (sfat + 12 > b.size() || memcmp(b.data() + sfat, "SFAT", 4) != 0) return false;
    uint32_t count = u16(b, sfat + 6);
    size_t names = sfat + 12 + 16 * (size_t)count + 8;  /* (past the SFNT header) */
    for (uint32_t i = 0; i < count; i++) {
        size_t node = sfat + 12 + 16 * (size_t)i;
        uint32_t attr = u32(b, node + 4);
        if (!(attr & 0x01000000)) continue;
        size_t at = names + (attr & 0xFFFF) * 4;
        if (at >= b.size() || strncmp((const char *)b.data() + at, name, b.size() - at) != 0) continue;
        start = data + u32(b, node + 8);
        end = data + u32(b, node + 12);
        return end <= b.size() && start < end;
    }
    return false;
}

/* The logo's two channels (letters, soft edge: 0..255 per pixel, interleaved, row after row), or false */
bool logo_channels(const char *romfs, std::vector<uint8_t> &shape, int &width, int &height) {
    std::string path = std::string(romfs) + "/region_common/ui/StartUp.arc";
    size_t size = 0;
    void *file = SDL_LoadFile(path.c_str(), &size);
    if (!file) return false;
    std::vector<uint8_t> arc((uint8_t *)file, (uint8_t *)file + size);
    SDL_free(file);
    size_t s, e;
    if (!sarc_file(arc, "timg/__Combined.bntx", s, e)) return false;
    std::vector<uint8_t> b(arc.begin() + (long)s, arc.begin() + (long)e);
    if (b.size() < 0x40 || memcmp(b.data(), "BNTX", 4) != 0 || memcmp(b.data() + 0x20, "NX  ", 4) != 0) return false;
    uint32_t count = u32(b, 0x24);
    uint64_t infos = u64(b, 0x28);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t t = u64(b, infos + 8 * i);         /* a BRTI: the texture's description */
        if (t + 0x78 > b.size() || memcmp(b.data() + t, "BRTI", 4) != 0) return false;
        uint64_t name = u64(b, t + 0x60);
        uint32_t len = u16(b, name);
        if (name + 2 + len > b.size() || len != 9 || memcmp(b.data() + name + 2, "Logo_00^f", 9) != 0) continue;
        uint32_t format = u32(b, t + 0x1C), w = u32(b, t + 0x24), h = u32(b, t + 0x28), block_log2 = u32(b, t + 0x34);
        uint64_t data = u64(b, u64(b, t + 0x70));
        if (format != 0x0901 || w == 0 || h == 0 || w > 4096 || h > 4096 || block_log2 > 5) return false;   /* (R8G8) */
        uint32_t rows = 8u << block_log2, gobs_x = (w * 2 + 63) / 64;
        uint64_t need = (uint64_t)((h + rows - 1) / rows) * gobs_x * (512u << block_log2);
        if (data + need > b.size()) return false;
        shape.assign((size_t)w * h * 2, 0);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t xb = x * 2, gy = y % 8, gx = xb % 64;   /* (byte x within the row; its place in the GOB) */
                uint64_t at = data + (uint64_t)(y / rows) * gobs_x * (512u << block_log2) + (uint64_t)(xb / 64) * (512u << block_log2)
                            + (uint64_t)((y % rows) / 8) * 512 + (gx / 32) * 256 + (gy / 2) * 64 + ((gx % 32) / 16) * 32
                            + (gy % 2) * 16 + (gx % 16);
                shape[((size_t)y * w + x) * 2] = b[at];
                shape[((size_t)y * w + x) * 2 + 1] = b[at + 1];
            }
        width = (int)w;
        height = (int)h;
        return true;
    }
    return false;
}

}  // namespace

bool game_logo_rgba(const char *romfs, std::vector<uint8_t> &rgba, int &width, int &height) {
    static std::string made_for;                     /* (made once per game folder) */
    static std::vector<uint8_t> made;
    static int made_w, made_h, made_ok;
    if (!romfs) return false;
    if (made_for != romfs) {
        made_for = romfs;
        std::vector<uint8_t> ch;
        made_ok = logo_channels(romfs, ch, made_w, made_h);
        made.assign(made_ok ? (size_t)made_w * made_h * 4 : 0, 0);
        for (int y = 0; made_ok && y < made_h; y++)
            for (int x = 0; x < made_w; x++) {
                size_t i = (size_t)y * made_w + x;
                float fill = ch[i * 2] / 255.0f, glow = ch[i * 2 + 1] / 255.0f * 0.35f * (1 - fill), v = (float)y / made_h;
                uint8_t *p = &made[i * 4];               /* (premultiplied: gold letters, pale at the top; gold glow) */
                p[0] = (uint8_t)(fill * (255 - 22 * v) + glow * 233 + 0.5f);
                p[1] = (uint8_t)(fill * (241 - 45 * v) + glow * 196 + 0.5f);
                p[2] = (uint8_t)(fill * (196 - 90 * v) + glow * 106 + 0.5f);
                p[3] = (uint8_t)((fill + glow) * 255 + 0.5f);
            }
    }
    rgba = made, width = made_w, height = made_h;
    return made_ok;
}
