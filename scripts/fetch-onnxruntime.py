"""Fetches the ONNX Runtime shared library for one platform from Microsoft's NuGet package
(Microsoft.ML.OnnxRuntime), checks the package's SHA-256, and prints the library's path.

    python3 scripts/fetch-onnxruntime.py win-x64 | win-arm64 | linux-x64 | linux-arm64 | osx-arm64

The package (about 180 MB, every platform) is cached in deps/cache.
"""
import hashlib
import sys
import urllib.request
import zipfile
from pathlib import Path

VERSION = "1.32.0"
SHA256 = "a9150bc2992cbbb6722070da9910e1ffa989a8bfc67bdd5bb8e745ca755641db"
URL = f"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/{VERSION}/microsoft.ml.onnxruntime.{VERSION}.nupkg"
LIBS = {"win-x64": "onnxruntime.dll", "win-arm64": "onnxruntime.dll", "linux-x64": "libonnxruntime.so",
        "linux-arm64": "libonnxruntime.so", "osx-arm64": "libonnxruntime.dylib"}

root = Path(__file__).resolve().parents[1]
rid = sys.argv[1] if len(sys.argv) > 1 else ""
if rid not in LIBS:
    sys.exit(f"usage: fetch-onnxruntime.py {{{'|'.join(LIBS)}}}")
cache = root / "deps" / "cache" / f"onnxruntime-{VERSION}.nupkg"
out = root / "deps" / "onnxruntime" / rid / LIBS[rid]
if not out.exists():
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        print(f"downloading {URL}", file=sys.stderr)
        urllib.request.urlretrieve(URL, cache)
    digest = hashlib.sha256(cache.read_bytes()).hexdigest()
    if digest != SHA256:
        cache.unlink()
        sys.exit(f"checksum mismatch for {cache.name}: {digest}")
    with zipfile.ZipFile(cache) as z:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(z.read(f"runtimes/{rid}/native/{LIBS[rid]}"))
print(out)
