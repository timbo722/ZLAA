"""
make_fonts.py - make the menus' fonts (res/ui/fonts) from the free originals.

    pip install fonttools
    python tools/make_fonts.py <Nunito[wght].ttf> <MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf> res/ui/fonts

Nunito (SIL Open Font License; github.com/google/fonts, ofl/nunito) is one variable font: every weight in one file.
RmlUi picks a font file per weight, so three fixed weights are cut from it: Regular, Bold, ExtraBold.
Material Symbols Rounded (Apache 2.0; github.com/google/material-design-icons, variablefont/) is 15 MB: thousands of
icons in every weight and fill. The menus need a few at one style (~4 KB). Add an icon's code point to ICONS
(fonts.google.com/icons shows them) and run this again.
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

WEIGHTS = {"Regular": 400, "Bold": 700, "ExtraBold": 800}

ICONS = {
    0xE002: "warning", 0xE5CA: "check", 0xE5CD: "close", 0xE897: "lock", 0xE5CB: "chevron_left",
    0xE5CC: "chevron_right", 0xE8B5: "schedule", 0xE863: "autorenew", 0xE887: "help", 0xE88E: "info",
    0xE8B8: "settings", 0xE037: "play_arrow", 0xE9BA: "logout", 0xE2C7: "folder", 0xE5C8: "arrow_forward",
    0xE034: "pause", 0xE050: "volume_up", 0xE30C: "tv", 0xE061: "fiber_manual_record",
}


def main():
    nunito, icons, out = sys.argv[1], sys.argv[2], sys.argv[3]
    for name, weight in WEIGHTS.items():
        font = instancer.instantiateVariableFont(TTFont(nunito), {"wght": weight})
        font["OS/2"].usWeightClass = weight            # (how RmlUi tells the files apart)
        font.save(os.path.join(out, "Nunito-%s.ttf" % name))

    # one icon style: regular weight, outlined, normal grade, 24 px optical size
    font = instancer.instantiateVariableFont(TTFont(icons), {"wght": 400, "FILL": 0, "GRAD": 0, "opsz": 24})
    options = subset.Options()
    options.layout_features = []                     # no ligatures: icons are chosen by code point
    options.name_IDs = ["*"]                         # keep the names (family, copyright, license)
    sub = subset.Subsetter(options)
    sub.populate(unicodes=list(ICONS))
    sub.subset(font)
    font.save(os.path.join(out, "MaterialSymbolsRounded.ttf"))


if __name__ == "__main__":
    main()
