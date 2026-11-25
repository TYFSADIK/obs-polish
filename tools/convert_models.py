"""Converts Google's official MediaPipe models to ONNX and proves the conversion is faithful.

    python tools/convert_models.py      (needs tensorflow, tf2onnx, onnxruntime, ai-edge-litert, numpy, pillow)

For each model: download the .tflite from Google's MediaPipe asset bucket, check its SHA-256,
convert with tf2onnx, then run the original (LiteRT) and the converted model (ONNX Runtime) on
the test portrait and on random inputs and report the largest difference. Writes
data/models/*.onnx and data/models/PROVENANCE.md.
"""
import hashlib
import subprocess
import sys
import tempfile
import urllib.request
from datetime import date
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:  # older name
    from tflite_runtime.interpreter import Interpreter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data" / "models"
BASE = "https://storage.googleapis.com/mediapipe-assets/"
MODELS = {
    # name: (sha256 of the official .tflite, input size, input range)
    "face_detection_short_range": ("3bc182eb9f33925d9e58b5c8d59308a760f4adea8f282370e428c51212c26633", 128, (-1, 1)),
    "face_landmark": ("1055cb9d4a9ca8b8c688902a3a5194311138ba256bcc94e336d8373a5f30c814", 192, (0, 1)),
}
# Google's selfie segmentation uses a MediaPipe-only operator (Convolution2DTransposeBias) that no
# converter handles; onnx-community publishes an ONNX re-implementation with Google's weights.
SEG_URL = "https://huggingface.co/onnx-community/mediapipe_selfie_segmentation/resolve/main/onnx/model.onnx"
SEG_SHA = "3241ac4ad8aa35bdaf33946776db29f7c283a413aa0b0dacb9483594b4531aad"


def inputs(size, lo, hi):
    face = Image.open(ROOT / "tests" / "images" / "face.png").convert("RGB")
    w, h = face.size
    crops = [face.crop((330, 120, 630, 420)), face]  # the face, and the whole frame
    out = [np.asarray(c.resize((size, size), Image.BILINEAR), np.float32) / 255.0 for c in crops]
    rng = np.random.default_rng(0)
    out += [rng.random((size, size, 3), dtype=np.float32) for _ in range(3)]
    return [(x * (hi - lo) + lo)[None] for x in out]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    import tf2onnx
    for name, (sha, size, (lo, hi)) in MODELS.items():
        with tempfile.TemporaryDirectory() as tmp:
            tfl = Path(tmp) / f"{name}.tflite"
            urllib.request.urlretrieve(BASE + tfl.name, tfl)
            digest = hashlib.sha256(tfl.read_bytes()).hexdigest()
            if digest != sha:
                sys.exit(f"{name}: checksum {digest} does not match the pinned {sha}")
            onnx_path = OUT / f"{name}.onnx"
            subprocess.run([sys.executable, "-m", "tf2onnx.convert", "--tflite", str(tfl), "--output", str(onnx_path),
                            "--opset", "15"], check=True, capture_output=True)

            lite = Interpreter(model_path=str(tfl))
            lite.allocate_tensors()
            sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
            worst = 0.0
            for x in inputs(size, lo, hi):
                lite.set_tensor(lite.get_input_details()[0]["index"], x)
                lite.invoke()
                ref = {tuple(d["shape"]): lite.get_tensor(d["index"]) for d in lite.get_output_details()}
                got = sess.run(None, {sess.get_inputs()[0].name: x})
                for g in got:
                    r = ref.get(tuple(g.shape)) if tuple(g.shape) in ref else next(v for v in ref.values() if v.size == g.size)
                    scale = max(1.0, float(np.abs(r).max()))
                    worst = max(worst, float(np.abs(g.reshape(r.shape) - r).max()) / scale)
            rows.append((name, digest, onnx_path.stat().st_size, worst))
            print(f"{name}: {onnx_path.stat().st_size} bytes, largest relative difference from the original {worst:.2e}")

    seg = OUT / "selfie_segmentation.onnx"
    urllib.request.urlretrieve(SEG_URL, seg)
    digest = hashlib.sha256(seg.read_bytes()).hexdigest()
    if digest != SEG_SHA:
        sys.exit(f"selfie_segmentation: checksum {digest} does not match the pinned {SEG_SHA}")
    print(f"selfie_segmentation: {seg.stat().st_size} bytes from onnx-community")

    lines = [
        "# Where these models come from",
        "",
        "Google's MediaPipe models (Apache License 2.0), downloaded from Google's MediaPipe asset bucket",
        f"(`{BASE}`), checked against the SHA-256 below, and converted to ONNX with",
        f"[tf2onnx](https://github.com/onnx/tensorflow-onnx) {tf2onnx.__version__} (opset 15) by `tools/convert_models.py`",
        f"on {date.today().isoformat()}. The script then runs the original and the converted model on the test",
        "portrait and on random inputs; the last column is the largest difference between them, relative",
        "to the output's range.",
        "",
        "| Model | Original .tflite SHA-256 | ONNX bytes | Largest difference |",
        "| --- | --- | --- | --- |",
    ]
    lines += [f"| {n} | `{d}` | {b:,} | {w:.1e} |" for n, d, b, w in rows]
    lines += ["", "`selfie_segmentation.onnx` is different: Google's model uses a MediaPipe-only operator",
              "(Convolution2DTransposeBias) that no converter supports, so this is",
              f"[onnx-community's ONNX version]({SEG_URL.replace('/resolve/main/onnx/model.onnx', '')}) of the same model",
              f"(Apache License 2.0, re-implemented with Google's weights), downloaded and checked against SHA-256 `{SEG_SHA}`.",
              "It is checked by the background tests instead of an output comparison."]
    lines += ["", "Model cards: [face detection](https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20(Short%20Range).pdf),",
              "[face mesh](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Face%20Mesh.pdf),",
              "[selfie segmentation](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf).", ""]
    (OUT / "PROVENANCE.md").write_text("\n".join(lines))


if __name__ == "__main__":
    main()
