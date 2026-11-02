"""
make_icon_font.py - cut Google's Material Symbols Rounded font (Apache 2.0) down to the icons the menus use.

    pip install fonttools
    python tools/make_icon_font.py <MaterialSymbolsRounded[FILL,GRAD,opsz,wght].ttf> res/ui/fonts/MaterialSymbolsRounded.ttf

The full font (github.com/google/material-design-icons, variablefont/) is 15 MB: thousands of icons, and every
weight and fill as a variable font. The menus need a dozen icons at one style, ~10 KB. Add an icon's code
point to ICONS (fonts.google.com/icons shows them) and run this again.
"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

ICONS = {
    0xE002: "warning", 0xE5CA: "check", 0xE5CD: "close", 0xE897: "lock", 0xE5CB: "chevron_left",
    0xE5CC: "chevron_right", 0xE8B5: "schedule", 0xE863: "autorenew", 0xE887: "help", 0xE88E: "info",
    0xE8B8: "settings", 0xE037: "play_arrow", 0xE9BA: "logout", 0xE2C7: "folder", 0xE5C8: "arrow_forward",
}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    font = TTFont(src)
    # one style: regular weight, outlined, normal grade, 24 px optical size (as the menus' text icons)
    font = instancer.instantiateVariableFont(font, {"wght": 400, "FILL": 0, "GRAD": 0, "opsz": 24})
    options = subset.Options()
    options.layout_features = []                     # no ligatures: icons are chosen by code point
    options.name_IDs = ["*"]                         # keep the names (family, copyright, license)
    sub = subset.Subsetter(options)
    sub.populate(unicodes=list(ICONS))
    sub.subset(font)
    font.save(dst)


if __name__ == "__main__":
    main()
