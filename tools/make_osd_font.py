"""
make_osd_font.py - the on-screen overlays' letters (FPS counter, shader compilation): runtime/render/osd_font.h.

    python tools/make_osd_font.py res/ui/fonts/Nunito-ExtraBold.ttf runtime/render/osd_font.h      (Pillow)

The overlays are drawn into a small texture by the CPU (runtime/render/render.c), without a font engine: this cuts
the few characters they need out of Nunito (SIL Open Font License, as the menus use it) at three pixel sizes, as
8-bit coverage, with where each sits on the baseline and how far it advances.
"""
import sys

from PIL import Image, ImageDraw, ImageFont

SIZES = (12, 17, 26)
CHARS = "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.:%/-"


def main():
    ttf, out = sys.argv[1], sys.argv[2]
    lines = ["/* osd_font.h - made by tools/make_osd_font.py from Nunito ExtraBold (SIL Open Font License): the",
             " * overlays' characters at %s px, 8-bit coverage. Don't edit; run the tool again. */" % ", ".join(map(str, SIZES)),
             "#include <stdint.h>", "",
             "typedef struct { int8_t x0, y0; uint8_t w, h, advance; uint32_t offset; } OsdGlyph;   /* (x0, y0: from the pen"
             " on the baseline) */",
             "typedef struct { int size, ascent; const OsdGlyph *glyphs; const uint8_t *bits; } OsdFont;   /* glyphs[c - 32] */", ""]
    fonts = []
    for size in SIZES:
        font = ImageFont.truetype(ttf, size)
        ascent, _ = font.getmetrics()
        glyphs, bits = [], bytearray()
        for code in range(32, 127):
            ch = chr(code)
            if ch not in CHARS:
                glyphs.append((0, 0, 0, 0, 0, 0))
                continue
            l, t, r, b = font.getbbox(ch, anchor="ls")
            w, h = max(r - l, 0), max(b - t, 0)
            img = Image.new("L", (max(w, 1), max(h, 1)), 0)
            ImageDraw.Draw(img).text((-l, -t), ch, font=font, fill=255, anchor="ls")
            glyphs.append((l, t, w, h, round(font.getlength(ch)), len(bits)))
            if w and h:
                bits += img.tobytes()
        lines.append("static const OsdGlyph osd_glyphs_%d[95] = {" % size)
        for i in range(0, 95, 4):
            lines.append("    " + " ".join("{ %d, %d, %d, %d, %d, %d }," % g for g in glyphs[i:i + 4]))
        lines.append("};")
        lines.append("static const uint8_t osd_bits_%d[%d] = {" % (size, max(len(bits), 1)))
        for i in range(0, len(bits), 24):
            lines.append("    " + "".join("%d," % v for v in bits[i:i + 24]))
        lines.append("};")
        fonts.append("{ %d, %d, osd_glyphs_%d, osd_bits_%d }" % (size, ascent, size, size))
    lines.append("static const OsdFont osd_fonts[%d] = { %s };" % (len(SIZES), ", ".join(fonts)))
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
