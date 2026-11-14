"""
make_cjk_fonts.py - make the game's stand-in fonts for Chinese, Korean and Japanese (res/fonts) from free originals.

    pip install fonttools
    python tools/make_cjk_fonts.py <folder with the originals> <Unihan_Readings.txt> res/fonts

The game draws its text with the Switch's system fonts (runtime/services/pl.c). They are Nintendo's, so a player who
hasn't imported their own gets stand-ins: a Latin font from the device, and for these languages the fonts made here.
The originals are Noto Sans SC / TC / KR / JP (SIL Open Font License; github.com/google/fonts, ofl/notosans*/
NotoSans*[wght].ttf: TrueType outlines, like the Switch's fonts). Each is one variable font of ~10-18 MB with every
weight and tens of thousands of characters; the regular weight is cut out and only the characters in everyday use are
kept: each language's standard character set (GB 2312 + China's 8105 general standard characters, Big5, KS X 1001 without
hanja + every Hangul syllable, JIS X 0208), plus Latin, punctuation and kana for all of them.
Unihan_Readings.txt (unicode.org/Public/UCD/latest/ucd/Unihan.zip) lists the 8105 characters (kTGHZ2013).
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer


def double_byte(codec, lead, trail):
    """Every character of a two-byte legacy character set (decoding each code with Python's codec)"""
    chars = set()
    for a in lead:
        for b in trail:
            try:
                ch = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                chars.add(ord(ch))
    return chars


def general_standard(unihan):
    """China's Table of General Standard Chinese Characters (2013): the 8105 characters in kTGHZ2013"""
    chars = set()
    for line in open(unihan, encoding="utf-8"):
        if "\tkTGHZ2013\t" in line:
            chars.add(int(line.split("\t")[0][2:], 16))
    return chars


COMMON = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180)) \
    | set(range(0x2000, 0x2070)) | set(range(0x2190, 0x2200)) | set(range(0x2460, 0x2500)) \
    | set(range(0x25A0, 0x2700)) | set(range(0x3000, 0x3100)) | set(range(0xFF00, 0xFFF0))
# (U+25A1, the white square, must be there: the game puts it in its glyph cache at startup)


def main():
    src, unihan, out = sys.argv[1], sys.argv[2], sys.argv[3]
    sets = {
        "SC": double_byte("gb2312", range(0xA1, 0xF8), range(0xA1, 0xFF)) | general_standard(unihan),
        "TC": double_byte("big5", range(0xA1, 0xFA), list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))),
        "KR": double_byte("euc_kr", range(0xA1, 0xC9), range(0xA1, 0xFF)) | set(range(0xAC00, 0xD7A4)),   # (no hanja)
        "JP": double_byte("euc_jp", range(0xA1, 0xF5), range(0xA1, 0xFF)),
    }
    os.makedirs(out, exist_ok=True)
    for name, chars in sets.items():
        font = instancer.instantiateVariableFont(TTFont(os.path.join(src, "NotoSans%s.ttf" % name)), {"wght": 400})
        options = subset.Options()
        options.name_IDs = ["*"]                     # keep the names (family, copyright, license)
        options.layout_features = ["*"]              # (vertical forms, locl: small, and the game may use them)
        options.notdef_outline = True
        options.hinting = False                      # (the game's engine hints by itself: SetAutoHint)
        sub = subset.Subsetter(options)
        sub.populate(unicodes=sorted(chars | COMMON))
        sub.subset(font)
        path = os.path.join(out, "NotoSans%s-Regular.ttf" % name)
        font.save(path)
        print("%s: %d characters, %d KB" % (path, len(font.getBestCmap()), os.path.getsize(path) // 1024))


if __name__ == "__main__":
    main()
