"""
make_menu_art.py - paint the start screen's background (res/ui/images): night over Koholint.

    python tools/make_menu_art.py res/ui/images        (Pillow, NumPy, OpenCV)

Our own picture, drawn from plain shapes here (none of the game's art): a starry sky with a faint band of the Milky
Way and a thin moon, Mt. Tamaranch with the Wind Fish's egg on top, the island below it (palms, a few houses with
lit windows), a far ridge, and the sea with the egg's light and the island mirrored in it. The left third stays dark
for the title and the menu. Two files: night.png (everything) and glow.png (the egg's glow alone, transparent: the
start screen fades it in and out over night.png). Same seed, same picture.
"""
import math
import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw

W, H = 1920, 1080
HORIZON = int(H * 0.70)
EGG = (1560, 318)                    # the egg's bottom middle, on the mountain's top
rng = np.random.default_rng(1993)


def rgb(h):
    return np.array([int(h[i:i + 2], 16) for i in (1, 3, 5)], np.float32) / 255


def mix(a, b, t):
    return a + (b - a) * t


def lerp_rows(stops, n):
    """A vertical gradient: stops = [(0..1, colour)]; returns n x 3."""
    ys = np.linspace(0, 1, n)
    out = np.zeros((n, 3), np.float32)
    for i in range(3):
        out[:, i] = np.interp(ys, [s[0] for s in stops], [s[1][i] for s in stops])
    return out


def blur(a, r):
    """Gaussian blur of a float H x W array (sigma r)."""
    return cv2.GaussianBlur(a.astype(np.float32), (0, 0), r)


def smooth_noise(cols, rows, seed_rng=None):
    """Smooth random blobs over the picture: a cols x rows grid of random values, scaled up (bicubic); -1..1."""
    g = (seed_rng or rng).random((rows, cols)).astype(np.float32) * 2 - 1
    return np.clip(cv2.resize(g, (W, H), interpolation=cv2.INTER_CUBIC), -1, 1)


def mask(draw_fn, ss=3):
    """A shape's coverage (H x W, 0..1), drawn at ss x the size and shrunk: smooth edges."""
    img = Image.new("L", (W * ss, H * ss), 0)
    draw_fn(ImageDraw.Draw(img), ss)
    return np.asarray(img.resize((W, H), Image.LANCZOS), np.float32) / 255


def over(base, colour, alpha):
    """Paint colour (3 or H x W x 3) over base with alpha (H x W)."""
    a = alpha[..., None]
    return base * (1 - a) + colour * a


def add(base, colour, amount):
    return base + colour * amount[..., None]


def radial(cx, cy, rx, ry=None):
    ry = ry or rx
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    return np.sqrt(((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2)


def ridge(x0, x1, base, heights, rough, seed_scale):
    """A ridge line: points from x0 to x1, y = base - height profile + small noise."""
    xs = np.arange(x0, x1 + 1, 4, dtype=np.float32)
    ys = np.full_like(xs, base)
    for cx, h, w in heights:
        ys -= h * np.exp(-((xs - cx) / w) ** 2)
    n = np.zeros_like(xs)
    for f, a in ((1 / 90, 1.0), (1 / 37, 0.5), (1 / 13, 0.25)):
        n += a * np.sin(xs * f * seed_scale + rng.uniform(0, 6.28))
    return xs, ys + n * rough


def poly_mask(points):
    def d(dr, ss):
        dr.polygon([(x * ss, y * ss) for x, y in points], fill=255)
    return mask(d)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)

    # ---- sky: near black at the top, a warm dusk-olive haze at the horizon ----
    sky = lerp_rows([(0, rgb("#05060A")), (0.45, rgb("#0A0C10")), (0.62, rgb("#15160F")), (0.70, rgb("#22201A")),
                     (1, rgb("#22201A"))], H)
    img = np.repeat(sky[:, None, :], W, 1)
    # a warm haze under the egg, and a cooler one far left
    img = add(img, rgb("#3A2F14"), 0.55 * np.exp(-radial(EGG[0], HORIZON - 60, 760, 330) ** 2))
    img = add(img, rgb("#101A24"), 0.6 * np.exp(-radial(250, HORIZON - 40, 700, 260) ** 2))

    # the Milky Way: a faint band from top left to the right, mottled by large soft blobs
    d = (y - (0.08 * H + (x / W) * 0.30 * H)) / (0.17 * H)
    mottle = 0.6 * smooth_noise(14, 8) + 0.4 * smooth_noise(40, 22)
    band = np.clip(np.exp(-d ** 2) * (0.75 + 0.35 * mottle), 0, 1) * (y < HORIZON)
    img = add(img, rgb("#5C6070"), 0.09 * band)

    # stars: many faint, a few bright with a soft halo; more in the band, fewer near the horizon
    stars = np.zeros((H, W), np.float32)
    n = 2600
    sx = rng.uniform(0, W, n)
    sy = rng.uniform(0, HORIZON, n) ** 1.15 / HORIZON ** 0.15
    for px, py in zip(sx, sy):
        ix, iy = int(px), int(py)
        if iy >= HORIZON - 30:
            continue
        keep = 0.35 + 0.65 * band[iy, ix]
        if rng.random() > keep:
            continue
        b = rng.random() ** 3.2
        stars[iy, ix] = max(stars[iy, ix], 0.25 + 0.75 * b)
    halo = blur(np.where(stars > 0.8, stars, 0), 2.2) * 3.0
    fade = np.clip((HORIZON - 40 - y) / 260, 0, 1)
    star_col = rgb("#FFF4D6")
    img = add(img, star_col, np.clip(stars + halo, 0, 1) * fade * 0.95)

    # the moon: a thin crescent with a soft halo, between the title and the mountain
    mx, my, mr = 905, 150, 30
    img = add(img, rgb("#B8B49A"), 0.10 * np.exp(-(radial(mx, my, mr * 6)) ** 2))
    disc = np.clip((mr - np.sqrt((x - mx) ** 2 + (y - my) ** 2)) + 0.5, 0, 1)
    bite = np.clip((mr * 0.92 - np.sqrt((x - mx - mr * 0.42) ** 2 + (y - my + mr * 0.18) ** 2)) + 0.5, 0, 1)
    img = over(img, rgb("#F3ECD2"), disc * (1 - bite))

    # long thin clouds: soft-edged streaks, warmer near the egg
    clouds = np.zeros((H, W), np.float32)
    edge_n = smooth_noise(60, 34)
    for cx, cy, w, h in ((1250, 292, 560, 13), (1500, 262, 300, 8), (620, 430, 640, 12), (1730, 470, 380, 10),
                         (330, 250, 420, 8), (1020, 540, 760, 10)):
        clouds += np.exp(-((x - cx) / w) ** 4 - ((y - cy + 6 * edge_n) / h) ** 2)
    clouds = np.clip(clouds * (0.65 + 0.45 * edge_n), 0, 1)
    lit = np.exp(-radial(EGG[0], EGG[1] - 60, 760, 420) ** 2)
    img = over(img, mix(rgb("#141519"), rgb("#5A4D30"), lit[..., None]), clouds * 0.8)

    # ---- the far ridge, cool and faint ----
    xs, ys = ridge(620, W, HORIZON, [(900, 40, 140), (1150, 75, 180), (1800, 70, 220), (720, 22, 80)], 5, 1.0)
    far = poly_mask([(620, HORIZON)] + list(zip(xs, ys)) + [(W, HORIZON)])
    img = over(img, rgb("#161A1D"), far)

    # ---- Mt. Tamaranch: peaks (the egg on the highest, its top a little flat), steep then gentler slopes ----
    top = EGG[1] + 3
    xs = np.arange(980, W + 41, 2, dtype=np.float32)
    peaks = ((EGG[0], top, 0.20, 46), (1300, 455, 0.22, 10), (1835, 405, 0.20, 14), (1140, 585, 0.25, 6))
    ys = np.full_like(xs, float(HORIZON))
    for cx, py, k, flat in peaks:
        dd = np.maximum(np.abs(xs - cx) - flat, 0)
        ys = np.minimum(ys, py + k * dd ** 1.22)
    rough = sum(a * np.sin(xs * f + rng.uniform(0, 6.28)) for f, a in ((1 / 61, 5), (1 / 23, 2.5), (1 / 9, 1.2)))
    ys = np.minimum(ys + rough * np.clip(np.abs(xs - EGG[0]) / 80 - 0.6, 0, 1), HORIZON)
    mountain = poly_mask([(980, HORIZON)] + list(zip(xs, ys)) + [(W + 40, HORIZON)])
    # rock: ridges and gullies running down from the summit (a 1-D noise over the angle seen from just above
    # it), darker lower down; the egg lights the top
    rays = cv2.GaussianBlur(rng.random((1, 720)).astype(np.float32), (0, 0), 1.6)[0]
    rays = (rays - rays.min()) / (rays.max() - rays.min())
    ang = np.arctan2(x - EGG[0], y - top + 120)
    streak = rays[np.clip(((ang + 1.6) / 3.2 * 719).astype(int), 0, 719)]
    light = np.exp(-radial(EGG[0], EGG[1] + 20, 230, 200) ** 2)
    low = np.clip((y - 300) / (HORIZON - 300), 0, 1)
    m_col = mix(rgb("#121410"), rgb("#3A3320"), light[..., None]) * (0.80 + 0.32 * streak[..., None])
    m_col *= (1 - 0.40 * low)[..., None]
    img = over(img, m_col, mountain)
    ridge_line = np.clip(mountain - np.roll(mountain, 2, 0), 0, 1)
    img = add(img, rgb("#E9C46A"), ridge_line * (0.12 + 1.2 * light))

    # ---- the island in front: low hills, palms, houses ----
    xs, ys = ridge(760, W + 40, HORIZON + 6, [(900, 40, 120), (1120, 55, 170), (1420, 70, 200), (1760, 50, 180)],
                   4, 2.2)
    land = poly_mask([(760, HORIZON + 6)] + list(zip(xs, ys)) + [(W + 40, HORIZON + 6)])

    def land_y(px):
        return float(np.interp(px, xs, ys))

    def palms(dr, ss):
        for px, hgt, lean in ((880, 120, -0.25), (930, 95, 0.18), (1290, 135, -0.12), (1345, 105, 0.3),
                              (1700, 125, -0.2), (1755, 92, 0.22)):
            by = land_y(px) + 4
            pts = []
            for i in range(25):
                t = i / 24
                pts.append((px + lean * hgt * t ** 1.6 * 1.4, by - hgt * t))
            for i in range(24):
                (x0, y0), (x1, y1) = pts[i], pts[i + 1]
                wdt = 6.5 - 3.2 * (i / 24)
                dr.line([(x0 * ss, y0 * ss), (x1 * ss, y1 * ss)], fill=255, width=int(wdt * ss))
            tx, ty = pts[-1]
            for ang in (-170, -140, -110, -70, -40, -10, 20, 200):
                a = math.radians(ang)
                ln = hgt * rng.uniform(0.42, 0.55)
                frond = []
                for i in range(13):
                    t = i / 12
                    fx = tx + math.cos(a) * ln * t
                    fy = ty + math.sin(a) * ln * t + (ln * 0.55) * t ** 2       # droops
                    frond.append((fx, fy))
                for i in range(12):
                    (x0, y0), (x1, y1) = frond[i], frond[i + 1]
                    wdt = 7 * math.sin(math.pi * (i + 0.5) / 12) + 1
                    dr.line([(x0 * ss, y0 * ss), (x1 * ss, y1 * ss)], fill=255, width=int(wdt * ss))
            dr.ellipse([(tx - 6) * ss, (ty - 6) * ss, (tx + 6) * ss, (ty + 6) * ss], fill=255)

    houses = [(1040, 46, 34), (1100, 38, 28), (1165, 52, 36), (1560, 40, 30), (1610, 34, 26)]

    def huts(dr, ss):
        for hx, hw, hh in houses:
            by = land_y(hx) + 6
            dr.rectangle([(hx - hw / 2) * ss, (by - hh) * ss, (hx + hw / 2) * ss, by * ss], fill=255)
            dr.polygon([((hx - hw / 2 - 7) * ss, (by - hh + 2) * ss), (hx * ss, (by - hh - hw * 0.45) * ss),
                        ((hx + hw / 2 + 7) * ss, (by - hh + 2) * ss)], fill=255)
            cx = hx + hw * 0.22
            dr.rectangle([(cx - 3) * ss, (by - hh - hw * 0.45) * ss, (cx + 3) * ss, (by - hh - 2) * ss], fill=255)

    fg = np.clip(land + mask(palms) + mask(huts), 0, 1)
    img = over(img, rgb("#050604"), fg)

    # lit windows, each with a small warm glow
    win = np.zeros((H, W), np.float32)
    for hx, hw, hh in houses:
        by = land_y(hx) + 6
        for wx in ((hx - hw * 0.22,), (hx - hw * 0.2, hx + hw * 0.2))[hw > 40]:
            if rng.random() < 0.2:
                continue
            x0, y0 = int(wx - 4), int(by - hh * 0.62)
            win[y0:y0 + 9, x0:x0 + 7] = 1
    img = add(img, rgb("#FFB85A"), np.clip(blur(win, 7) * 3.5, 0, 0.8))
    img = over(img, rgb("#FFD38A"), win)

    # ---- the egg: smooth, lit from above left, a thin gold edge; its glow ----
    ex, eb = EGG
    a_, b_ = 52, 70                                   # half width, half height

    def egg(dr, ss):
        pts = []
        for i in range(200):
            t = 2 * math.pi * i / 200
            px = a_ * math.cos(t) * (1 - 0.14 * math.sin(t))
            py = -b_ * math.sin(t)
            pts.append(((ex + px) * ss, (eb - b_ + py) * ss))
        dr.polygon(pts, fill=255)
    egg_m = mask(egg, 4)
    shade = np.clip(1 - 0.35 * radial(ex - 18, eb - b_ * 1.45, a_ * 1.6, b_ * 1.6), 0.55, 1)
    egg_col = rgb("#F6EFDC") * shade[..., None]
    glow = np.exp(-radial(ex, eb - b_, 150) ** 2) * 0.55 + np.exp(-radial(ex, eb - b_, 420, 360) ** 2) * 0.35
    glow_col = rgb("#FEE685")
    img = add(img, glow_col, glow * 0.35 * (1 - egg_m))
    img = over(img, egg_col, egg_m)
    egg_edge = np.clip(egg_m - blur(egg_m, 1.2) * 0.9, 0, 1)
    img = add(img, rgb("#E9C46A"), egg_edge * 0.4)

    # ---- the sea ----
    sea = lerp_rows([(0, rgb("#121D20")), (0.08, rgb("#0D171A")), (0.45, rgb("#070C0E")), (1, rgb("#030506"))],
                    H - HORIZON)
    sea_img = np.repeat(sea[:, None, :], W, 1)
    sy_, sx_ = np.mgrid[HORIZON:H, 0:W].astype(np.float32)
    # gentle swells: darker and lighter bands that bunch up towards the horizon
    depth = (sy_ - HORIZON) / (H - HORIZON)
    swell = np.sin((1 / (depth + 0.04)) * 9 + np.sin(sx_ / 140) * 0.6)
    sea_img *= (1 + 0.10 * swell * (1 - depth))[..., None]
    img[HORIZON:] = sea_img

    # the island mirrored in the water, wobbling and dark
    mirror_rows = np.arange(HORIZON, H)
    src_rows = np.clip(2 * HORIZON - mirror_rows + 10, 0, HORIZON - 1)
    wob = (np.sin(mirror_rows / 3.1) * 3 + np.sin(mirror_rows / 7.7) * 2).astype(int)
    refl = np.zeros((H - HORIZON, W), np.float32)
    shapes = np.clip(fg + mountain * 0.9 + far * 0.6, 0, 1)
    for i, (r, s) in enumerate(zip(src_rows, wob)):
        refl[i] = np.roll(shapes[r], s)
    refl *= np.clip(1 - depth * 1.6, 0, 1)
    img[HORIZON:] = over(img[HORIZON:], rgb("#040607"), refl * 0.55)

    # the egg's light on the water: short gold dashes in a column that widens towards us
    glints = np.zeros((H - HORIZON, W), np.float32)
    for _ in range(1500):
        dy = rng.random() ** 1.4
        row = int(dy * (H - HORIZON - 1))
        spread = 30 + dy * 260
        cx = ex + rng.normal(0, spread * 0.45)
        ln = 6 + dy * 40 * rng.random()
        b = (1 - dy) ** 1.2 * rng.uniform(0.3, 1.0)
        x0, x1 = int(max(cx - ln / 2, 0)), int(min(cx + ln / 2, W))
        glints[row, x0:x1] = np.maximum(glints[row, x0:x1], b)
    # and the moon's, fainter and silver
    silver = np.zeros_like(glints)
    for _ in range(400):
        dy = rng.random() ** 1.2
        row = int(dy * (H - HORIZON - 1))
        cx = mx + rng.normal(0, 18 + dy * 120)
        ln = 4 + dy * 26 * rng.random()
        b = (1 - dy) * rng.uniform(0.2, 0.6)
        x0, x1 = int(max(cx - ln / 2, 0)), int(min(cx + ln / 2, W))
        silver[row, x0:x1] = np.maximum(silver[row, x0:x1], b)
    gl = blur(glints, 0.8) * 1.4
    img[HORIZON:] = add(img[HORIZON:], rgb("#F2C46A"), np.clip(gl, 0, 1) * 0.85)
    img[HORIZON:] = add(img[HORIZON:], rgb("#C9CFD6"), np.clip(blur(silver, 0.8), 0, 1) * 0.35)
    # the line where the land meets the water, faintly lit
    shore = np.clip(blur(fg, 2)[HORIZON - 4:HORIZON + 8] * 0.4, 0, 0.4)
    img[HORIZON - 4:HORIZON + 8] = add(img[HORIZON - 4:HORIZON + 8], rgb("#6E6650"), shore * 0.25)

    # ---- dark on the left, behind the title and the menu; a soft vignette ----
    left = np.clip(1 - x / (W * 0.48), 0, 1) ** 1.5
    img *= (1 - 0.55 * left)[..., None]
    vig = np.clip(radial(W * 0.58, H * 0.48, W * 0.75, H * 0.75) - 0.55, 0, 1)
    img *= (1 - 0.5 * vig)[..., None]

    # dither (no banding in the dark gradients), then 8 bits
    img = img + (rng.random((H, W, 1)).astype(np.float32) - 0.5) / 255
    Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(
        os.path.join(out, "night.png"), optimize=True)

    # glow.png: the egg's halo alone (premultiplied when loaded), faded in and out by the start screen
    g = np.exp(-radial(ex, eb - b_, 110) ** 2) * 0.55 + np.exp(-radial(ex, eb - b_, 300, 260) ** 2) * 0.28
    g *= (1 - egg_m)
    rgba = np.zeros((H, W, 4), np.float32)
    rgba[..., :3] = rgb("#FEE685")
    rgba[..., 3] = np.clip(g + (rng.random((H, W)) - 0.5) / 255, 0, 1)
    Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(out, "glow.png"), optimize=True)


if __name__ == "__main__":
    main()
