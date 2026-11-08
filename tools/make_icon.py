"""
make_icon.py - ZLAA's app icon, drawn from plain shapes (none of the game's art), in the start screen's look: a
night sky, a dark mountain with a gold edge, a glowing egg on its top.

    python tools/make_icon.py res/android docs/icon.png        (Pillow, NumPy)

Writes an Android adaptive icon (the launcher crops it to its own shape): mipmap-xxxhdpi/ic_launcher_background.png,
ic_launcher_foreground.png and ic_launcher_monochrome.png (Android 13's themed icons) at 432 x 432 (108 dp), and
mipmap-anydpi-v26/ic_launcher.xml, which puts them together; and a 512 x 512 picture with rounded corners for the
README and the GitHub release.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

S = 432                      # a layer: 108 dp at xxxhdpi; what a launcher always shows is the middle 66 dp circle
SS = 4                       # drawn this many times larger, then shrunk: smooth edges
rng = np.random.default_rng(7)


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def shape(draw_fn, size=S):
    img = Image.new("L", (size * SS, size * SS), 0)
    draw_fn(ImageDraw.Draw(img), SS)
    return img.resize((size, size), Image.LANCZOS)


def mountain_points(ss):
    # a peak with a small flat top for the egg, shoulders either side, the base below the layer's bottom
    pts = [(-20, 470), (40, 372), (95, 334), (140, 316), (174, 282), (196, 264), (236, 264), (260, 286), (300, 314),
           (345, 334), (392, 366), (452, 470)]
    return [(x * ss, y * ss) for x, y in pts]


def egg(draw, ss, cx, bottom, a, b):
    pts = []
    for i in range(240):
        t = 2 * np.pi * i / 240
        px = a * np.cos(t) * (1 - 0.14 * np.sin(t))
        py = -b * np.sin(t)
        pts.append(((cx + px) * ss, (bottom - b + py) * ss))
    draw.polygon(pts, fill=255)


def background():
    y, x = np.mgrid[0:S, 0:S].astype(np.float32) / S
    top, bottom = np.array(rgb("#06070B"), np.float32), np.array(rgb("#22201A"), np.float32)
    img = top + (bottom - top) * (y[..., None] ** 1.3)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.52) ** 2)
    img += np.array(rgb("#5A4818"), np.float32) * np.exp(-(r / 0.34) ** 2)[..., None]   # the egg's warm light
    for _ in range(55):                             # stars, more near the top
        sx, sy = rng.uniform(0, S), rng.uniform(0, S) ** 1.4 / S ** 0.4
        b = rng.uniform(90, 255) * (1 - sy / S) ** 0.5
        ix, iy = int(sx), int(sy)
        img[iy, ix] = np.maximum(img[iy, ix], b)
    img += (rng.random((S, S, 1)) - 0.5) * 2      # dither: no banding
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


def foreground():
    out = np.zeros((S, S, 4), np.float32)

    def over(colour, alpha):
        a = np.asarray(alpha, np.float32)[..., None] / 255
        out[..., :3] = out[..., :3] * (1 - a) + np.array(colour, np.float32) * a
        out[..., 3:] = out[..., 3:] * (1 - a) + 255 * a

    # the glow around the egg (its own soft layer)
    glow = shape(lambda d, ss: d.ellipse([(216 - 84) * ss, (204 - 84) * ss, (216 + 84) * ss, (204 + 84) * ss], fill=255))
    over(rgb("#FEE685"), np.asarray(glow.filter(ImageFilter.GaussianBlur(36)), np.float32) * 0.75)
    # the mountain: dark, its ridge lit gold
    m = shape(lambda d, ss: d.polygon(mountain_points(ss), fill=255))
    over(rgb("#1A1C14"), m)
    edge = shape(lambda d, ss: d.line(mountain_points(ss)[1:-1], fill=255, width=5 * ss, joint="curve"))
    lit = np.asarray(edge, np.float32) * np.clip(1.15 - np.abs(np.arange(S) - 216) / 190, 0, 1)[None, :]
    over(rgb("#E9C46A"), lit)
    # the egg, lit from above left, a thin gold edge
    e = shape(lambda d, ss: egg(d, ss, 216, 268, 45, 61))
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    shade = np.clip(1 - 0.28 * np.sqrt(((xx - 200) / 64) ** 2 + ((yy - 180) / 78) ** 2), 0.66, 1)
    egg_col = np.stack([np.full((S, S), c, np.float32) for c in rgb("#F7F0DD")], -1) * shade[..., None]
    a = np.asarray(e, np.float32)[..., None] / 255
    out[..., :3] = out[..., :3] * (1 - a) + egg_col * a
    out[..., 3:] = out[..., 3:] * (1 - a) + 255 * a
    ring = np.clip(np.asarray(e, np.float32) - np.asarray(e.filter(ImageFilter.MinFilter(5)), np.float32), 0, 255)
    over(rgb("#E9C46A"), ring * 0.8)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA"), m, e


def main():
    res, picture = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(res, "mipmap-xxxhdpi"), exist_ok=True)
    os.makedirs(os.path.join(res, "mipmap-anydpi-v26"), exist_ok=True)
    bg = background()
    fg, m, e = foreground()
    bg.save(os.path.join(res, "mipmap-xxxhdpi", "ic_launcher_background.png"), optimize=True)
    fg.save(os.path.join(res, "mipmap-xxxhdpi", "ic_launcher_foreground.png"), optimize=True)
    # themed icons: one colour, the shapes only (the launcher tints it)
    mono = Image.new("LA", (S, S), (255, 0))
    mono.putalpha(Image.fromarray(np.maximum(np.asarray(m) * 0.55, np.asarray(e)).astype(np.uint8), "L"))
    mono.save(os.path.join(res, "mipmap-xxxhdpi", "ic_launcher_monochrome.png"), optimize=True)
    with open(os.path.join(res, "mipmap-anydpi-v26", "ic_launcher.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@mipmap/ic_launcher_background" />\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
                '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
                '</adaptive-icon>\n')
    # the picture: the middle 72 dp of the layers (what most launchers show), rounded corners, 512 x 512
    full = Image.alpha_composite(bg.convert("RGBA"), fg)
    crop = full.crop((72, 72, S - 72, S - 72)).resize((512, 512), Image.LANCZOS)
    corners = shape(lambda d, ss: d.rounded_rectangle([0, 0, 512 * ss - 1, 512 * ss - 1], radius=112 * ss, fill=255), 512)
    crop.putalpha(corners)
    os.makedirs(os.path.dirname(os.path.abspath(picture)), exist_ok=True)
    crop.save(picture, optimize=True)


if __name__ == "__main__":
    main()
