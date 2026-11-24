"""Renders the test portrait through the filter inside a real libobs and checks the results.

    python3 tests/run_tests.py [--install DIR]      (needs numpy and pillow, and OBS installed)

By default the plugin is taken from the build tree for this platform; --install points at an
installed layout instead (DIR/obs-polish.plugin on macOS, DIR/obs-polish/{bin/64bit,data} elsewhere).
render-test reads where OBS's own pieces are from POLISH_* environment variables (see
tests/render_test.c). Writes the renders to tests/out/ so they can be looked at.
"""
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
FACE = ROOT / "tests" / "images" / "face.png"
OUT = ROOT / "tests" / "out"
OFF = {"smooth": 0, "brighten": 0, "sharpen": 0}
SYSTEM = platform.system()


def plugin_paths():
    install = sys.argv[sys.argv.index("--install") + 1] if "--install" in sys.argv else None
    build = Path(os.environ.get("POLISH_BUILD", ROOT / "build"))
    if SYSTEM == "Darwin":
        bundle = Path(install) / "obs-polish.plugin" if install else build / "obs-polish.plugin"
        return bundle / "Contents" / "MacOS" / "obs-polish", bundle / "Contents" / "Resources"
    if install:
        base = Path(install) / "obs-polish"
        name = "obs-polish.dll" if SYSTEM == "Windows" else "obs-polish.so"
        return base / "bin" / "64bit" / name, base / "data"
    if SYSTEM == "Windows":
        cfg = os.environ.get("POLISH_CONFIG", "Release")
        return build / cfg / "obs-polish.dll", build / "data"
    return build / "obs-polish.so", build / "data"


PLUGIN_BIN, PLUGIN_DATA = plugin_paths()
TOOL = Path(os.environ.get("POLISH_RENDER_TEST", ROOT / "build" / ("Release/render-test.exe" if SYSTEM == "Windows" else "render-test")))

results = []


def render(name, settings, frames=25, image=FACE):
    out = OUT / f"{name}.png"
    arg = "none" if settings is None else json.dumps(settings)
    r = subprocess.run([str(TOOL), str(PLUGIN_BIN), str(PLUGIN_DATA), str(image), str(out), arg, str(frames), "--json"],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise RuntimeError(f"{name}: render-test exited {r.returncode}\n{r.stderr[-2000:]}")
    info = next((json.loads(l) for l in r.stdout.splitlines() if l.startswith("{")), {})
    return np.asarray(Image.open(out).convert("RGB")).astype(np.int32), info


def check(name, ok, detail):
    results.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}: {detail}")


def region(img, box):
    x0, y0, x1, y1 = box
    return img[y0:y1, x0:x1]


def main():
    OUT.mkdir(exist_ok=True)
    # The baseline is what OBS shows with no filter at all. Comparing against it, not the image
    # file, separates the filter's effect from how a GPU or driver draws the image.
    src, _ = render("baseline", None, frames=5)
    raw = np.asarray(Image.open(FACE).convert("RGB")).astype(np.int32)
    print(f"      this GPU draws the image within {np.abs(src - raw).max()} levels of the file")
    h, w = src.shape[:2]

    # 1. with every effect off, the filter must not change a single pixel
    off, _ = render("off", OFF, frames=5)
    check("passthrough", np.abs(off - src).max() == 0, f"max difference {np.abs(off - src).max()}")

    # 2. the face is found, with the eyes above the chin and roughly level
    _, info = render("default", {})
    print(f"      backend: {info.get('backend')} {info.get('error') or ''}")
    f = info["face"][0] if info.get("faces") else None
    ok = bool(f) and f["eye_l"][0] < f["eye_r"][0] and f["eye_l"][1] < f["chin"][1] and abs(f["roll"]) < 0.2
    check("face tracking", ok, f"{info.get('faces')} face(s), analysis {info.get('face_ms')} ms")
    if not ok:
        print("\nno face found: the remaining checks need one")
        sys.exit(1)
    W, H = w, h
    fx0, fy0, fw, fh = f["box"]
    face_box = (int(fx0 * W), int(fy0 * H), int((fx0 + fw) * W), int((fy0 + fh) * H))

    # 3. smoothing reduces fine detail on the face and leaves the background alone
    sm, _ = render("smooth", {"smooth": 100, "brighten": 0, "sharpen": 0})

    def detail(img):
        g = img.mean(axis=2)
        return np.abs(np.diff(g, axis=1)).mean()

    # measured on forehead skin: eyes, brows and hair are edges the filter is meant to keep
    eye_y = int(min(f["eye_l"][1], f["eye_r"][1]) * H)
    skin = (int(f["eye_l"][0] * W), face_box[1] + 8, int(f["eye_r"][0] * W), eye_y - 25)
    d0, d1 = detail(region(src, skin)), detail(region(sm, skin))
    corner = (0, H - 200, 200, H)
    bg_change = np.abs(region(sm, corner) - region(src, corner)).max()
    check("smoothing", d1 < d0 * 0.8 and bg_change == 0,
          f"forehead detail {d0:.2f} -> {d1:.2f}, background corner max change {bg_change}")

    # 4. brightening raises skin luminance without shifting hue much
    br, _ = render("brighten", {"smooth": 0, "brighten": 100, "sharpen": 0})
    a, b = region(src, face_box).reshape(-1, 3), region(br, face_box).reshape(-1, 3)
    lum = (b.mean() - a.mean())
    ratio0, ratio1 = a[:, 0].mean() / a[:, 2].mean(), b[:, 0].mean() / b[:, 2].mean()
    check("brighten", lum > 5 and abs(ratio1 - ratio0) < 0.15, f"luminance +{lum:.1f}, red/blue {ratio0:.2f} -> {ratio1:.2f}")

    # 5. face shape changes only pixels near the face, and by a bounded amount
    sh, _ = render("shape", {**OFF, "slim": 100, "eyes": 100})
    changed = np.abs(sh - src).max(axis=2) > 6
    ys, xs = np.nonzero(changed)
    cx, cy = (face_box[0] + face_box[2]) / 2, (face_box[1] + face_box[3]) / 2
    far = np.hypot(xs - cx, ys - cy).max() if len(xs) else 0
    check("face shape", 0 < len(xs) and far < 1.2 * max(face_box[2] - face_box[0], face_box[3] - face_box[1]),
          f"{len(xs)} pixels changed, farthest {far:.0f} px from the face centre")

    # 6. background blur: person kept, background changed; solid colour fills the background
    bl, info = render("bg_blur", {**OFF, "bg_mode": 1, "bg_blur": 80}, frames=30)
    flag = (20, 300, 120, 700)       # the flag on the left, behind the subject
    person = face_box
    check("background blur", info["mask"][0] > 0 and detail(region(bl, flag)) < 0.5 * detail(region(src, flag))
          and np.abs(region(bl, person) - region(src, person)).mean() < 4,
          f"mask {info['mask']}, flag detail {detail(region(src, flag)):.2f} -> {detail(region(bl, flag)):.2f}")
    col, _ = render("bg_color", {**OFF, "bg_mode": 3, "bg_color": 0xFF00FF00}, frames=30)
    patch = region(col, (20, 900, 120, 1100)).reshape(-1, 3).mean(axis=0)
    check("background colour", patch[1] > 230 and patch[0] < 25 and patch[2] < 25, f"background patch {patch.round()}")

    # 7. looks: the identity LUT changes nothing; mono removes colour
    lut_dir = PLUGIN_DATA / "luts"
    shutil.copy(ROOT / "tests" / "identity_lut.png", lut_dir / "test-identity.png")
    try:
        ident, _ = render("look_identity", {**OFF, "look": "test-identity", "look_strength": 100}, frames=5)
    finally:
        os.remove(lut_dir / "test-identity.png")
    check("identity look", np.abs(ident - src).max() <= 2, f"max difference {np.abs(ident - src).max()}")
    mono, _ = render("look_mono", {**OFF, "look": "mono", "look_strength": 100}, frames=5)
    chroma = np.abs(mono[..., 0] - mono[..., 2]).mean()
    check("mono look", chroma < 3, f"mean red-blue difference {chroma:.2f}")

    # 8. a sticker is drawn above the eyes, and nowhere near the bottom of the frame
    st, _ = render("sticker", {**OFF, "sticker": "crown"})
    changed = np.abs(st - src).max(axis=2) > 20
    ys, xs = np.nonzero(changed)
    eye_y = f["eye_l"][1] * H
    check("sticker", len(ys) > 500 and np.median(ys) < eye_y and ys.max() < H * 0.5,
          f"{len(ys)} pixels, median row {np.median(ys):.0f} (eyes at {eye_y:.0f})")

    # 9. lipstick changes only pixels around the mouth, and makes them redder
    lp, info = render("lipstick", {**OFF, "lipstick": 100})
    changed = np.abs(lp - src).max(axis=2) > 6
    ys, xs = np.nonzero(changed)
    mouth_y = (f["chin"][1] * H + max(f["eye_l"][1], f["eye_r"][1]) * H) / 2 + 0.15 * (f["chin"][1] - f["eye_l"][1]) * H
    redder = (lp[changed][:, 0] - lp[changed][:, 2]).mean() - (src[changed][:, 0] - src[changed][:, 2]).mean() if len(ys) else 0
    check("lipstick", info["mouth"][0] > 0 and len(ys) > 50 and abs(np.median(ys) - mouth_y) < 0.25 * fh * H
          and ys.min() > max(f["eye_l"][1], f["eye_r"][1]) * H and redder > 5,
          f"mouth mask {info['mouth']}, {len(ys)} pixels around row {np.median(ys):.0f} (mouth about {mouth_y:.0f}), red-blue +{redder:.1f}")

    # 10. vignette darkens the corners, not the centre
    vg, _ = render("vignette", {**OFF, "vignette": 100}, frames=5)
    corner_d = region(src, (0, 0, 80, 80)).mean() - region(vg, (0, 0, 80, 80)).mean()
    centre_d = np.abs(region(src, (430, 550, 530, 650)) - region(vg, (430, 550, 530, 650))).mean()
    check("vignette", corner_d > 20 and centre_d < 1, f"corner darker by {corner_d:.1f}, centre change {centre_d:.2f}")

    # 11. a built-in background preset replaces the background
    bp, _ = render("bg_preset", {**OFF, "bg_mode": 2, "bg_preset": "bokeh"}, frames=30)
    flag_change = np.abs(region(bp, flag) - region(src, flag)).mean()
    check("background preset", flag_change > 40 and np.abs(region(bp, person) - region(src, person)).mean() < 4,
          f"flag region changed by {flag_change:.1f} on average; face unchanged")

    # 12. compare shows the original
    cmp_, _ = render("compare", {"compare": True, "smooth": 100, "look": "mono", "lipstick": 100, "vignette": 100}, frames=5)
    check("compare", np.abs(cmp_ - src).max() == 0, f"max difference {np.abs(cmp_ - src).max()}")

    failed = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
