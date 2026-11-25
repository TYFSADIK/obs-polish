"""Builds the README images from renders in tests/out (made by the plugin itself in libobs):
the before/after animation, the background gallery and the social preview card.

    python3 tools/make_docs_images.py      (after tests/run_tests.py and the demo renders below)

Demo renders, with render-test (see tests/render_test.c):
    demo_before.png   settings "none"
    demo_after.png    {"smooth":60,"brighten":35,"lipstick":35,"blush":30,"look":"golden",
                       "look_strength":70,"bg_mode":1,"bg_blur":70}
    bg_blur, bg_color, bgp_studio, bgp_bokeh, bgp_sunset, bgimg    the background modes
    doc_<name>.png    the face and eye galleries, default colours, every other effect off:
                      foundation 100, soft_light 70, vline 100 + chin 40, nose_slim 100, lip_fill 100,
                      eyeshadow 100, eyeliner 100, brows 100, eye_color 100, and doc_eyes_all with
                      eyeshadow 60, eyeliner 70, brows 50, eye_color 60
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "tests" / "out"
DOCS = ROOT / "docs" / "img"
BOLD = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
REGULAR = "/System/Library/Fonts/Supplemental/Arial.ttf"


def font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default(size)


def label(im, text, f, pad=10):
    d = ImageDraw.Draw(im)
    w = d.textlength(text, font=f)
    d.rounded_rectangle([pad, pad, pad + w + 18, pad + f.size + 12], 8, fill=(0, 0, 0))
    d.text((pad + 9, pad + 4), text, font=f, fill="white")
    return im


def square(name, size):
    return Image.open(OUT / name).convert("RGB").crop((150, 40, 810, 700)).resize((size, size), Image.LANCZOS)


def demo():
    a, b = square("demo_before.png", 480), square("demo_after.png", 480)
    f = font(BOLD, 19)
    frames, n = [], 40
    for i in range(n):
        t = 0.5 - 0.5 * math.cos(2 * math.pi * i / n)
        x = int(32 + t * 416)
        fr = a.copy()
        fr.paste(b.crop((x, 0, 480, 480)), (x, 0))
        d = ImageDraw.Draw(fr)
        d.line([(x, 0), (x, 480)], fill="white", width=3)
        d.ellipse([x - 13, 227, x + 13, 253], fill="white")
        d.polygon([(x - 7, 240), (x - 1, 234), (x - 1, 246)], fill="black")
        d.polygon([(x + 7, 240), (x + 1, 234), (x + 1, 246)], fill="black")
        label(fr, "Original", f)
        w = d.textlength("Polish", font=f)
        d.rounded_rectangle([480 - 10 - w - 18, 10, 480 - 10, 10 + f.size + 12], 8, fill=(0, 0, 0))
        d.text((480 - 10 - w - 9, 14), "Polish", font=f, fill="white")
        frames.append(fr)
    durations = [70] * n
    durations[0] = durations[n // 2] = 1000
    frames[0].save(DOCS / "demo.webp", save_all=True, append_images=frames[1:], duration=durations, loop=0,
                   quality=82, method=6)
    return a, b


def backgrounds():
    f = font(BOLD, 22)
    tiles = [("bg_blur.png", "Blur"), ("bg_color.png", "Colour"), ("bgp_studio.png", "Studio"),
             ("bgp_bokeh.png", "Bokeh"), ("bgp_sunset.png", "Sunset"), ("bgimg.png", "Your image")]
    ims = [label(Image.open(OUT / n).convert("RGB").crop((0, 0, 960, 1000)).resize((300, 312), Image.LANCZOS), t, f)
           for n, t in tiles]
    o = Image.new("RGB", (3 * 300 + 16, 2 * 312 + 8), "white")
    for k, im in enumerate(ims):
        o.paste(im, ((k % 3) * 308, (k // 3) * 320))
    o.save(DOCS / "background.jpg", quality=86)


FACE = (380, 180, 580, 380)  # the test portrait's face, in its 960 x 1200 frame
EYES = (415, 200, 545, 265)  # and its eyes


def gallery(tiles, box, tile_w, cols, out):
    f = font(BOLD, 18)
    bw, bh = box[2] - box[0], box[3] - box[1]
    tile_h = int(tile_w * bh / bw)
    ims = [label(Image.open(OUT / n).convert("RGB").crop(box).resize((tile_w, tile_h), Image.LANCZOS), t, f) for n, t in tiles]
    rows = (len(ims) + cols - 1) // cols
    o = Image.new("RGB", (cols * tile_w + (cols - 1) * 8, rows * tile_h + (rows - 1) * 8), "white")
    for k, im in enumerate(ims):
        o.paste(im, ((k % cols) * (tile_w + 8), (k // cols) * (tile_h + 8)))
    o.save(DOCS / out, quality=86)


def faces():
    gallery([("baseline.png", "Original"), ("doc_foundation.png", "Foundation"), ("doc_soft_light.png", "Soft light"),
             ("doc_vline.png", "V-shape jaw, longer chin"), ("doc_nose.png", "Slim nose"), ("doc_lips.png", "Fuller lips")],
            FACE, 300, 3, "face.jpg")
    gallery([("baseline.png", "Original"), ("doc_eyeshadow.png", "Eyeshadow"), ("doc_eyeliner.png", "Eyeliner"),
             ("doc_brows.png", "Eyebrows"), ("doc_eye_color.png", "Eye colour"), ("doc_eyes_all.png", "All together")],
            EYES, 300, 3, "eyes.jpg")


def social(a, b):
    o = Image.new("RGB", (1280, 640), (22, 24, 30))
    a, b = a.resize((560, 560), Image.LANCZOS), b.resize((560, 560), Image.LANCZOS)
    fr = a.copy()
    fr.paste(b.crop((280, 0, 560, 560)), (280, 0))
    ImageDraw.Draw(fr).line([(280, 0), (280, 560)], fill="white", width=4)
    o.paste(fr, (40, 40))
    d = ImageDraw.Draw(o)
    big, mid, reg = font(BOLD, 72), font(BOLD, 30), font(REGULAR, 28)
    x = 650
    d.text((x, 120), "Polish", font=big, fill="white")
    d.text((x, 200), "for OBS Studio", font=mid, fill=(170, 180, 200))
    y = 270
    for line in ["Skin, makeup, eyes and face shape", "17 looks, background blur & replace", "Face-tracked stickers"]:
        d.text((x, y), line, font=reg, fill=(225, 228, 235))
        y += 44
    y += 20
    for i, tag in enumerate(["Windows", "macOS", "Linux", "Free", "No watermark"]):
        w = d.textlength(tag, font=mid)
        d.rounded_rectangle([x, y, x + w + 28, y + 48], 12, fill=(52, 120, 246) if i < 3 else (40, 160, 90))
        d.text((x + 14, y + 7), tag, font=mid, fill="white")
        x += w + 40
        if i == 2:
            x, y = 650, y + 62
    o.save(DOCS / "social-preview.png", optimize=True)


if __name__ == "__main__":
    a, b = demo()
    backgrounds()
    faces()
    social(a, b)
    print("docs images written")
