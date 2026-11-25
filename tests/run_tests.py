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
NOISE = 0


def render(name, settings, frames=25, image=FACE, switch_to=None):
    out = OUT / f"{name}.png"
    arg = "none" if settings is None else json.dumps(settings)
    env = dict(os.environ)
    if switch_to:
        env["POLISH_SWITCH_IMAGE"] = str(switch_to)
    r = subprocess.run([str(TOOL), str(PLUGIN_BIN), str(PLUGIN_DATA), str(image), str(out), arg, str(frames), "--json"],
                       capture_output=True, text=True, timeout=600, env=env)
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
    again, _ = render("baseline2", None, frames=5)
    raw = np.asarray(Image.open(FACE).convert("RGB")).astype(np.int32)
    # Real GPUs render the same frame identically every time. Some virtual ones (CI's macOS VMs)
    # do not; "exact" checks then allow exactly the GPU's own run-to-run difference, no more.
    global NOISE
    NOISE = int(np.abs(again - src).max())
    print(f"      this GPU draws the image within {np.abs(src - raw).max()} levels of the file; "
          f"two identical renders differ by {NOISE}")
    h, w = src.shape[:2]

    # 1. with every effect off, the filter must not change a single pixel
    off, _ = render("off", OFF, frames=5)
    check("passthrough", np.abs(off - src).max() <= NOISE, f"max difference {np.abs(off - src).max()}")

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
    # an identity lookup table must change nothing; 2 levels of interpolation rounding allowed.
    # POLISH_LUT_TOLERANCE widens it for emulated GPUs (CI's macOS VM: OpenGL translated to Metal).
    lut_tol = int(os.environ.get("POLISH_LUT_TOLERANCE", "2"))
    check("identity look", np.abs(ident - src).max() <= lut_tol + NOISE, f"max difference {np.abs(ident - src).max()}")
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
    cmp_, _ = render("compare", {"compare": True, "smooth": 100, "look": "mono", "lipstick": 100, "vignette": 100,
                                 "foundation": 100, "soft_light": 100, "eyeliner": 100, "nose_slim": 100}, frames=5)
    check("compare", np.abs(cmp_ - src).max() <= NOISE, f"max difference {np.abs(cmp_ - src).max()}")

    # 13. effects stacked the way people use them: each pass after the first must still know the
    # frame size and where the face is (libobs clears effect parameters after every technique)
    stk, _ = render("stacked", {"smooth": 45, "brighten": 20, "slim": 60, "eyes": 30, "blush": 60, "vignette": 60})
    flat = stk.reshape(-1, 3).std(axis=0).mean()
    corner_d = region(src, (0, 0, 80, 80)).mean() - region(stk, (0, 0, 80, 80)).mean()
    face_d = np.abs(region(stk, face_box) - region(src, face_box)).mean()
    check("stacked effects", flat > 20 and corner_d > 10 and 1 < face_d < 30,
          f"picture spread {flat:.1f}, corner darker by {corner_d:.1f}, face changed by {face_d:.1f} on average")

    def changed_px(img, thr=6):
        return np.nonzero(np.abs(img - src).max(axis=2) > thr)

    eye_row = (f["eye_l"][1] + f["eye_r"][1]) / 2 * H
    chin_row = f["chin"][1] * H
    face_w = fw * W

    # 14. foundation evens skin colour on the face and leaves the background alone
    fd, _ = render("foundation", {**OFF, "foundation": 100})
    cheeks = (int(f["eye_l"][0] * W), int(eye_row + 0.25 * (chin_row - eye_row)), int(f["eye_r"][0] * W), int(eye_row + 0.45 * (chin_row - eye_row)))

    def chroma_spread(img):
        px = region(img, cheeks).reshape(-1, 3).astype(float)
        return (px - px.mean(axis=1, keepdims=True)).std(axis=0).mean()
    c0, c1 = chroma_spread(src), chroma_spread(fd)
    bg_change = np.abs(region(fd, corner) - region(src, corner)).max()
    check("foundation", c1 < c0 * 0.9 and bg_change == 0, f"cheek colour spread {c0:.2f} -> {c1:.2f}, background change {bg_change}")

    # 15. soft light lifts the picture gently, more on the face
    sl, _ = render("soft_light", {**OFF, "soft_light": 100})
    lift_all = sl.mean() - src.mean()
    lift_face = region(sl, face_box).mean() - region(src, face_box).mean()
    check("soft light", 2 < lift_all < 35 and lift_face > lift_all * 0.8, f"brightness +{lift_all:.1f} overall, +{lift_face:.1f} on the face")

    # 16. V-shape jaw and a longer chin move the lower face only
    vl, _ = render("vline_chin", {**OFF, "vline": 100, "chin": 50})
    ys, xs = changed_px(vl)
    check("v-shape and chin", len(ys) > 200 and np.median(ys) > eye_row and ys.min() > eye_row - 0.1 * fh * H
          and np.abs(xs - (face_box[0] + face_box[2]) / 2).max() < face_w,
          f"{len(ys)} pixels, rows {ys.min() if len(ys) else 0}-{ys.max() if len(ys) else 0} (eyes {eye_row:.0f}, chin {chin_row:.0f})")

    # 17. slim nose changes the middle of the face, between the eyes and the mouth
    no, info = render("nose", {**OFF, "nose_slim": 100})
    ys, xs = changed_px(no)
    nose_x = (info["nose"][0][0] + info["nose"][1][0]) / 2 * W
    check("slim nose", len(ys) > 30 and eye_row - 0.05 * fh * H < np.median(ys) < chin_row and abs(np.median(xs) - nose_x) < 0.15 * face_w,
          f"{len(ys)} pixels around ({np.median(xs) if len(xs) else 0:.0f}, {np.median(ys) if len(ys) else 0:.0f}), nose at x {nose_x:.0f}")

    # 18. fuller lips change pixels around the mouth only
    lf, _ = render("lip_fill", {**OFF, "lip_fill": 100})
    ys, xs = changed_px(lf)
    check("fuller lips", len(ys) > 30 and abs(np.median(ys) - mouth_y) < 0.25 * fh * H and ys.min() > eye_row,
          f"{len(ys)} pixels around row {np.median(ys) if len(ys) else 0:.0f} (mouth about {mouth_y:.0f})")

    # 19. eye makeup: shadow above the eyes, liner darker at the lash line, brows darker above,
    # iris colour inside the iris
    es, info = render("eyeshadow", {**OFF, "eyeshadow": 100, "eyeshadow_color": 0xFFC8508C})  # ABGR violet
    ys, xs = changed_px(es)
    check("eyeshadow", info["eyes"][0] > 0 and len(ys) > 40 and np.median(ys) < eye_row and np.median(ys) > eye_row - 0.3 * fh * H,
          f"eye mask {info['eyes']}, {len(ys)} pixels, median row {np.median(ys) if len(ys) else 0:.0f} (eyes {eye_row:.0f})")
    el, _ = render("eyeliner", {**OFF, "eyeliner": 100})
    ys, xs = changed_px(el)
    darker = (src[ys, xs].mean() - el[ys, xs].mean()) if len(ys) else 0
    check("eyeliner", len(ys) > 30 and abs(np.median(ys) - eye_row) < 0.12 * fh * H and darker > 8,
          f"{len(ys)} pixels around row {np.median(ys) if len(ys) else 0:.0f} (eyes {eye_row:.0f}), darker by {darker:.1f}")
    bw, info = render("brows", {**OFF, "brows": 100})
    ys, xs = changed_px(bw)
    darker = (src[ys, xs].mean() - bw[ys, xs].mean()) if len(ys) else 0
    check("eyebrows", info.get("brows", 0) > 0 and len(ys) > 40 and np.median(ys) < eye_row and darker > 5,
          f"{len(ys)} pixels, median row {np.median(ys) if len(ys) else 0:.0f} (eyes {eye_row:.0f}), darker by {darker:.1f}")
    ec, info = render("eye_colour", {**OFF, "eye_color": 100, "iris_color": 0xFF30B040})  # ABGR green
    ys, xs = changed_px(ec)  # 6 levels, like every other check: emulated GPUs round a few levels off
    ok_inside = True
    for x, y in zip(xs, ys):
        if min(np.hypot(x - ix * W, y - iy * H) - 1.6 * ir * W for ix, iy, ir in info["iris"]) > 2:
            ok_inside = False
            break
    greener = (ec[ys, xs, 1] - ec[ys, xs, 2]).mean() - (src[ys, xs, 1] - src[ys, xs, 2]).mean() if len(ys) else 0
    check("eye colour", len(ys) > 10 and ok_inside and greener > 3,
          f"{len(ys)} pixels, all inside the irises: {ok_inside}, green-blue +{greener:.1f}")

    # 20. effects: each one changes the picture without breaking it, and those with an exact
    # property are held to it
    STYLES = ["grayscale", "sepia", "tealorange", "invert", "tint", "duotone", "bleach", "crossprocess", "golden", "moonlight",
              "neon", "colorpass", "hdr", "bloom", "solarize", "posterize", "threshold", "lightleak", "blur", "motionblur",
              "zoomblur", "tiltshift", "boxblur", "lensblur", "unsharp", "denoise", "filmgrain", "vhs", "crt", "super8", "oldfilm",
              "halftone", "rgbsplit", "dither", "lomo", "glitch", "pixelate", "fisheye", "mirror", "wave", "twirl", "emboss",
              "edges", "cartoon", "oilpaint", "thermal", "nightvision"]
    broken, unchanged = [], []
    renders = {}
    for st in STYLES:
        im, _ = render(f"fx_{st}", {**OFF, "style": st}, frames=8)
        renders[st] = im
        if im.reshape(-1, 3).std(axis=0).mean() < 8:
            broken.append(st)
        # denoise and colour pass barely change a clean, mostly white and grey test picture
        if np.abs(im - src).mean() < (0.5 if st in ("denoise", "colorpass") else 3):
            unchanged.append(st)
    check("every effect renders", not broken and not unchanged,
          f"{len(STYLES)} effects; flat: {broken or 'none'}; unchanged: {unchanged or 'none'}")
    g = renders["grayscale"]
    inv = renders["invert"]
    th = renders["threshold"]
    mid = ((th > 12) & (th < 243)).mean()
    post_levels = len(np.unique(renders["posterize"][..., 1]))
    px = renders["pixelate"]
    same_next = (np.abs(np.diff(px, axis=1)).max(axis=2) == 0).mean()
    check("colour effects exact", np.abs(g[..., 0] - g[..., 2]).max() <= 2 and np.abs(inv - (255 - src)).max() <= 3 + NOISE
          and mid < 0.002 and post_levels <= 14,
          f"grayscale red-blue max {np.abs(g[..., 0] - g[..., 2]).max()}, invert off by {np.abs(inv - (255 - src)).max()}, "
          f"threshold grey pixels {mid:.4f}, posterize green levels {post_levels}")
    mir, _ = render("fx_mirror2", {**OFF, "style": "mirror", "style_param": 0}, frames=5)
    sym = np.abs(mir - mir[:, ::-1]).mean()
    blur_d, sharp_d = detail(region(renders["blur"], skin)), detail(region(renders["unsharp"], skin))
    ed = renders["edges"].mean()
    nv = renders["nightvision"].reshape(-1, 3).mean(axis=0)
    check("geometry and detail effects", same_next > 0.9 and sym < 2 and blur_d < d0 * 0.6 and sharp_d > d0 * 1.1 and ed < 70
          and nv[1] > nv[0] * 2 and nv[1] > nv[2] * 2,
          f"pixelate same-as-next {same_next:.2f}, mirror asymmetry {sym:.2f}, forehead detail {d0:.2f} blur {blur_d:.2f} "
          f"unsharp {sharp_d:.2f}, outline brightness {ed:.0f}, night vision {nv.round()}")
    # echo: halfway through, the picture is replaced by its mirror image (same size); with echo the
    # result still holds the first picture, without it, none of it
    mirror_img = OUT / "face_mirror.png"
    Image.fromarray(raw[:, ::-1].astype(np.uint8)).save(mirror_img)
    no_echo, _ = render("fx_echo_off", {**OFF, "vignette": 1}, frames=7, switch_to=mirror_img)
    echo, _ = render("fx_echo", {**OFF, "style": "echo", "style_param": 100}, frames=7, switch_to=mirror_img)
    after = no_echo  # what the second picture looks like through OBS
    check("echo trails", np.abs(echo - after).mean() > 10 and np.abs(echo - src).mean() < np.abs(after - src).mean() * 0.8,
          f"echo differs from the new picture by {np.abs(echo - after).mean():.1f}; "
          f"distance to the old picture {np.abs(echo - src).mean():.1f} vs {np.abs(after - src).mean():.1f} without echo")
    # contrast, a vignette that lightens, and an effect at strength 0 doing nothing
    ct, _ = render("contrast", {**OFF, "contrast": 50}, frames=5)
    vl, _ = render("vignette_light", {**OFF, "vignette": -100}, frames=5)
    z, _ = render("fx_zero", {**OFF, "style": "pixelate", "style_amount": 0}, frames=5)
    corner_l = region(vl, (0, 0, 80, 80)).mean() - region(src, (0, 0, 80, 80)).mean()
    check("contrast, light vignette, strength 0", ct.std() > src.std() * 1.15 and corner_l > 20 and np.abs(z - src).max() <= NOISE,
          f"spread {src.std():.1f} -> {ct.std():.1f}, corner lighter by {corner_l:.1f}, strength 0 max difference {np.abs(z - src).max()}")

    failed = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
