"""Generates every bundled asset: colour-look LUTs, stickers and background presets. Everything is drawn here from
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
    # portrait
    "rosy": lambda c: split_tone(curve(c, lift=0.03, contrast=0.05), [0.03, 0.0, 0.02], [0.06, 0.01, 0.03], 0.8) * [1.02, 0.99, 1.0],
    "fresh": lambda c: sat(curve(c, lift=0.04, gain=1.03), 1.08) * [0.99, 1.01, 1.02],
    "porcelain": lambda c: sat(curve(c, lift=0.08, gain=1.04, contrast=-0.05), 0.8) * [1.0, 1.0, 1.03],
    # food
    "tasty": lambda c: split_tone(sat(curve(c, contrast=0.25), 1.25), [0.0, 0.0, -0.02], [0.06, 0.03, -0.05], 0.9),
    # landscape
    "lush": lambda c: green_boost(sat(curve(c, contrast=0.2), 1.15)),
    "sunset": lambda c: split_tone(sat(curve(c, contrast=0.2), 1.12), [-0.02, -0.02, 0.06], [0.14, 0.05, -0.08], 0.9),
    # modern
    "cinematic": lambda c: split_tone(sat(curve(c, lift=0.04, gain=0.95, contrast=0.3), 0.9), [-0.04, 0.03, 0.07], [0.07, 0.03, -0.04], 1.0),
    "noir": lambda c: np.repeat(curve(luma(c), lift=-0.0, gamma=1.15, contrast=0.55)[..., None], 3, axis=2),
    "vintage": lambda c: split_tone(sat(curve(c, lift=0.1, gain=0.92, contrast=0.1), 0.7), [0.04, 0.02, -0.02], [0.08, 0.05, -0.06], 1.0),
}


def green_boost(c):
    """more saturation in greens and blues (foliage, sky), less on skin"""
    g = np.clip((c[..., 1] - c[..., 0]) * 3, 0, 1)[..., None]
    b = np.clip((c[..., 2] - c[..., 0]) * 3, 0, 1)[..., None]
    return sat(c, 1.0) * (1 - 0.6 * (g + b) / 2) + sat(c, 1.5) * (0.6 * (g + b) / 2)


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


def star(d, cx, cy, r, color, k=S):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.45
        pts.append(((cx + rr * math.cos(a)) * k, (cy + rr * math.sin(a)) * k))
    d.polygon(pts, fill=color)


def starglasses():
    w, h = 640, 240
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    for cx in (160, 480):
        star(d, cx, 125, 120, (255, 205, 40, 255))
        star(d, cx, 125, 84, (255, 120, 170, 235))
    d.rounded_rectangle([270 * S, 100 * S, 370 * S, 122 * S], radius=10 * S, fill=(255, 205, 40, 255))
    finish(im, "starglasses")


def bunnyears():
    w, h = 520, 520
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    for cx, tilt in ((180, 10), (340, -10)):  # PIL rotates counter-clockwise: lean the tops apart
        ear = canvas(w, h)
        e = ImageDraw.Draw(ear)
        e.ellipse([(cx - 70) * S, 20 * S, (cx + 70) * S, 500 * S], fill=(250, 250, 252, 255))
        e.ellipse([(cx - 38) * S, 70 * S, (cx + 38) * S, 450 * S], fill=(250, 175, 195, 255))
        ear = ear.rotate(tilt, center=(cx * S, 480 * S), resample=Image.BICUBIC)
        im.alpha_composite(ear)
    finish(im, "bunnyears")


def devilhorns():
    w, h = 640, 260
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    for side in (-1, 1):
        base = 320 + side * 215
        pts = [(base - 55, 250), (base + side * 70, 10), (base + 55, 250)]
        tip = [(base - 55, 250), (base + side * 25, 120), (base + side * 70, 10), (base + 55, 250)]
        d.polygon([(x * S, y * S) for x, y in tip], fill=(200, 30, 40, 255))
        d.polygon([(x * S, y * S) for x, y in [(base - 20, 250), (base + side * 50, 60), (base + 15, 250)]], fill=(235, 70, 70, 255))
    finish(im, "devilhorns")


def flowercrown():
    w, h = 720, 240
    im = canvas(w, h)
    d = ImageDraw.Draw(im)
    pts = [(60 + i * 600 / 20, 170 - 70 * math.sin(math.pi * i / 20)) for i in range(21)]
    d.line([(x * S, y * S) for x, y in pts], fill=(70, 130, 60, 255), width=16 * S)
    for i, (x, y) in enumerate(pts[::3]):
        col = [(255, 140, 170), (255, 210, 90), (200, 150, 255), (255, 255, 255)][i % 4]
        for k in range(5):
            a = 2 * math.pi * k / 5 + i
            px, py = x + 26 * math.cos(a), y + 26 * math.sin(a)
            d.ellipse([(px - 22) * S, (py - 22) * S, (px + 22) * S, (py + 22) * S], fill=col + (255,))
        d.ellipse([(x - 14) * S, (y - 14) * S, (x + 14) * S, (y + 14) * S], fill=(250, 190, 60, 255))
    for x, y in pts[1::3]:
        d.ellipse([(x - 26) * S, (y - 12) * S, (x + 26) * S, (y + 12) * S], fill=(90, 160, 80, 255))
    finish(im, "flowercrown")


def write_stickers():
    (ROOT / "stickers").mkdir(parents=True, exist_ok=True)
    for fn in (sunglasses, crown, catears, halo, hearts, partyhat, starglasses, bunnyears, devilhorns, flowercrown):
        fn()


# ---------------------------------------------------------------- background presets

def write_backgrounds():
    out = ROOT / "backgrounds"
    out.mkdir(parents=True, exist_ok=True)
    w, h = 1920, 1080
    y, x = np.mgrid[0:h, 0:w] / h
    rng = np.random.default_rng(7)

    def save(arr, name, blur=0):
        im = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8))
        if blur:
            im = im.filter(ImageFilter.GaussianBlur(blur))
        im.save(out / f"{name}.jpg", quality=88)

    # studio: a grey backdrop lit from the middle
    r = np.hypot(x - w / h / 2, y - 0.45)
    v = 150 - 85 * np.clip(r / 1.1, 0, 1) ** 1.4
    save(np.dstack([v, v, v * 1.03]), "studio")
    # gradient: soft blue to peach
    t = np.clip((x / (w / h) + y) / 2, 0, 1)[..., None]
    save(np.array([45, 75, 130]) + (np.array([230, 160, 130]) - [45, 75, 130]) * t, "gradient")
    # bokeh: out-of-focus lights over dark blue
    base = Image.fromarray(np.dstack([np.full((h, w), 18), np.full((h, w), 22), np.full((h, w), 40)]).astype(np.uint8))
    d = ImageDraw.Draw(base, "RGBA")
    for _ in range(90):
        cx, cy, rr = rng.uniform(0, w), rng.uniform(0, h), rng.uniform(25, 110)
        col = [(255, 200, 120), (255, 150, 170), (140, 190, 255), (255, 240, 200)][rng.integers(4)]
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=col + (int(rng.uniform(40, 120)),))
    base.filter(ImageFilter.GaussianBlur(10)).save(out / "bokeh.jpg", quality=88)
    # sunset: sky gradient with a low sun
    sky = np.array([60, 60, 120]) + (np.array([255, 150, 90]) - [60, 60, 120]) * np.clip(y * 1.4, 0, 1)[..., None]
    sun = np.exp(-(((x - w / h * 0.62) ** 2 + (y - 0.72) ** 2) / 0.01))[..., None] * np.array([255, 220, 150]) * 0.9
    ground = (y > 0.78)[..., None] * (np.array([35, 25, 40]) - sky)
    save(sky + sun + ground, "sunset", blur=2)


if __name__ == "__main__":
    write_luts()
    write_stickers()
    write_backgrounds()
    print("assets written to", ROOT)
