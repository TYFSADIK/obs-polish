"""Generates every bundled asset: colour-look LUTs and stickers. Everything is drawn here from
code, so the repository contains no third-party art.

    python3 tools/make_assets.py            (needs numpy and pillow)

LUT layout: 512 x 512 PNG, 8 x 8 tiles of 64 x 64; tile index = blue; x = red; y = green (down).
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1] / "data"


# ---------------------------------------------------------------- looks

def identity():
    i = np.arange(64) / 63.0
    r = np.tile(i, (64, 1))
    g = r.T
    tiles = []
    for b in range(64):
        tiles.append(np.dstack([r, g, np.full((64, 64), b / 63.0)]))
    return tiles


def luma(c):
    return c[..., 0] * 0.299 + c[..., 1] * 0.587 + c[..., 2] * 0.114


def sat(c, k):
    y = luma(c)[..., None]
    return y + (c - y) * k


def curve(x, lift=0.0, gain=1.0, gamma=1.0, contrast=0.0):
    x = np.clip(x * gain + lift * (1 - x), 0, 1) ** gamma
    if contrast:
        x = x + contrast * (x - 0.5) * (1 - np.abs(2 * x - 1))     # gentle S-curve
    return x


def split_tone(c, shadow, highlight, amount):
    y = luma(c)[..., None]
    sh = np.array(shadow) * (1 - y) ** 2
    hi = np.array(highlight) * y ** 2
    return c + amount * (sh + hi)


LOOKS = {
    "warm": lambda c: c * [1.06, 1.0, 0.90],
    "cool": lambda c: c * [0.93, 1.0, 1.07],
    "golden": lambda c: split_tone(sat(curve(c, contrast=0.15), 1.08), [0.02, 0.0, -0.04], [0.10, 0.05, -0.08], 0.7),
    "vivid": lambda c: sat(curve(c, contrast=0.35), 1.35),
    "pastel": lambda c: sat(curve(c, lift=0.10, contrast=-0.15), 0.75) * [1.02, 1.0, 1.03],
    "tealorange": lambda c: split_tone(sat(curve(c, contrast=0.2), 1.1), [-0.06, 0.02, 0.08], [0.08, 0.02, -0.06], 1.0),
    "mono": lambda c: np.repeat(curve(luma(c), contrast=0.25)[..., None], 3, axis=2),
    "film": lambda c: split_tone(sat(curve(c, lift=0.06, gain=0.96, contrast=0.2), 0.85), [0.0, 0.03, 0.04], [0.05, 0.03, -0.02], 1.0),
}


def write_luts():
    out = ROOT / "luts"
    out.mkdir(parents=True, exist_ok=True)
    tiles = identity()
    for name, fn in LOOKS.items():
        img = np.zeros((512, 512, 3))
        for b, t in enumerate(tiles):
            r, c = divmod(b, 8)
            img[r * 64:(r + 1) * 64, c * 64:(c + 1) * 64] = np.clip(fn(t.copy()), 0, 1)
        Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(out / f"{name}.png", optimize=True)
    # the identity LUT is used by the tests: applying it must change nothing
    img = np.zeros((512, 512, 3))
    for b, t in enumerate(tiles):
        r, c = divmod(b, 8)
        img[r * 64:(r + 1) * 64, c * 64:(c + 1) * 64] = t
    Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(Path(__file__).resolve().parents[1] / "tests" / "identity_lut.png")


# ---------------------------------------------------------------- stickers

S = 4    # supersampling


def canvas(w, h):
    return Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))


def finish(img, name, shadow=True):
    w, h = img.size
    if shadow:
        a = img.split()[3].filter(ImageFilter.GaussianBlur(6 * S))
        sh = Image.new("RGBA", img.size, (0, 0, 0, 0))
        sh.putalpha(a.point(lambda v: int(v * 0.35)))
        base = Image.new("RGBA", img.size, (0, 0, 0, 0))
        base.alpha_composite(sh, (0, 3 * S))
        base.alpha_composite(img)
        img = base
    img = img.resize((w // S, h // S), Image.LANCZOS)
    img.save(ROOT / "stickers" / f"{name}.png", optimize=True)


def sunglasses():
    w, h = 600, 200
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    k = S
    for cx in (150, 450):
        d.rounded_rectangle([(cx - 120) * k, 40 * k, (cx + 120) * k, 170 * k], radius=55 * k, fill=(20, 20, 26, 255))
        d.rounded_rectangle([(cx - 104) * k, 54 * k, (cx + 104) * k, 156 * k], radius=44 * k, fill=(40, 46, 70, 235))
        d.ellipse([(cx - 70) * k, 62 * k, (cx - 20) * k, 92 * k], fill=(255, 255, 255, 70))
    d.rounded_rectangle([262 * k, 70 * k, 338 * k, 92 * k], radius=10 * k, fill=(20, 20, 26, 255))
    d.line([10 * k, 60 * k, 34 * k, 56 * k], fill=(20, 20, 26, 255), width=14 * k)
    d.line([590 * k, 60 * k, 566 * k, 56 * k], fill=(20, 20, 26, 255), width=14 * k)
    finish(im, "sunglasses")


def crown():
    w, h = 520, 300
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    k = S
    pts = [(40, 270), (40, 110), (140, 190), (260, 50), (380, 190), (480, 110), (480, 270)]
    d.polygon([(x * k, y * k) for x, y in pts], fill=(240, 186, 40, 255))
    d.polygon([(x * k, y * k) for x, y in [(40, 270), (40, 230), (480, 230), (480, 270)]], fill=(214, 150, 20, 255))
    for x, y in [(40, 104), (260, 44), (480, 104)]:
        d.ellipse([(x - 22) * k, (y - 22) * k, (x + 22) * k, (y + 22) * k], fill=(250, 214, 90, 255))
    for x, c in [(130, (220, 40, 70)), (260, (40, 150, 230)), (390, (220, 40, 70))]:
        d.ellipse([(x - 20) * k, 230 * k, (x + 20) * k, 270 * k], fill=c + (255,))
    finish(im, "crown")


def catears():
    w, h = 700, 300
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    k = S
    for side in (-1, 1):
        cx = 350 + side * 235
        outer = [(cx - 110, 290), (cx + side * 40, 20), (cx + 110, 290)]
        inner = [(cx - 65, 270), (cx + side * 30, 85), (cx + 65, 270)]
        d.polygon([(x * k, y * k) for x, y in outer], fill=(70, 60, 64, 255))
        d.polygon([(x * k, y * k) for x, y in inner], fill=(246, 160, 180, 255))
    finish(im, "catears")


def halo():
    w, h = 520, 200
    im = canvas(w, h)
    glow = canvas(w, h)
    ImageDraw.Draw(glow).ellipse([40 * S, 50 * S, 480 * S, 150 * S], outline=(255, 236, 150, 200), width=40 * S)
    glow = glow.filter(ImageFilter.GaussianBlur(14 * S))
    im.alpha_composite(glow)
    ImageDraw.Draw(im).ellipse([40 * S, 50 * S, 480 * S, 150 * S], outline=(255, 222, 90, 255), width=18 * S)
    finish(im, "halo", shadow=False)


def heart(d, cx, cy, r, color):
    k = S
    pts = []
    for i in range(120):
        t = 2 * math.pi * i / 120
        x = 16 * math.sin(t) ** 3
        y = -(13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t))
        pts.append(((cx + x * r / 16) * k, (cy + y * r / 16) * k))
    d.polygon(pts, fill=color)


def hearts():
    w, h = 800, 300
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    for cx, cy, r, c in [(110, 170, 70, (236, 60, 100)), (260, 90, 50, (255, 120, 150)), (400, 60, 40, (236, 60, 100)),
                         (540, 90, 50, (255, 120, 150)), (690, 170, 70, (236, 60, 100))]:
        heart(d, cx, cy, r, c + (255,))
    finish(im, "hearts")


def partyhat():
    w, h = 360, 420
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    k = S
    d.polygon([(40 * k, 400 * k), (180 * k, 40 * k), (320 * k, 400 * k)], fill=(70, 120, 230, 255))
    for i, c in enumerate([(250, 200, 60), (240, 90, 120), (250, 200, 60), (240, 90, 120)]):
        y0 = 120 + i * 75
        half = (y0 - 40) / 360 * 140
        half2 = (y0 + 30 - 40) / 360 * 140
        d.polygon([((180 - half) * k, y0 * k), ((180 + half) * k, y0 * k), ((180 + half2) * k, (y0 + 30) * k),
                   ((180 - half2) * k, (y0 + 30) * k)], fill=c + (255,))
    d.ellipse([150 * k, 10 * k, 210 * k, 70 * k], fill=(250, 240, 240, 255))
    finish(im, "partyhat")


def write_stickers():
    (ROOT / "stickers").mkdir(parents=True, exist_ok=True)
    for fn in (sunglasses, crown, catears, halo, hearts, partyhat):
        fn()


if __name__ == "__main__":
    write_luts()
    write_stickers()
    print("assets written to", ROOT)
