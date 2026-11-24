# Third-party notices

Polish's own code and assets are under the MIT licence (see `LICENSE`). It is used with, and
includes or downloads, the following:

| Component | Where | Licence |
| --- | --- | --- |
| [OBS Studio](https://github.com/obsproject/obs-studio) libobs | the plugin loads into OBS and links to libobs at run time; libobs and w32-pthreads headers are used to build it | GPL-2.0-or-later (OBS); Polish's MIT code is GPL-compatible |
| [ONNX Runtime](https://onnxruntime.ai) 1.32.0 | Windows and Linux packages: `obs-polish-onnxruntime.dll` / `libobs-polish-onnxruntime.so`, a renamed copy of Microsoft's release from NuGet (`Microsoft.ML.OnnxRuntime`); C API headers in `third_party/onnxruntime` | MIT; see `third_party/onnxruntime/LICENSE` and `ThirdPartyNotices.txt` |
| [MediaPipe](https://github.com/google-ai-edge/mediapipe) models: face detection (short range), face landmark (face mesh), selfie segmentation | `data/models/*.onnx`, converted from Google's official `.tflite` files; see `data/models/PROVENANCE.md` | Apache-2.0 |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | `third_party/stb` (image decoding in the plugin, PNG writing in the tests) | public domain or MIT, at your choice |
| [SIMDe](https://github.com/simd-everywhere/simde) | build-time headers required by libobs on ARM | MIT |
| Test portrait: Chris Hadfield, NASA | `tests/images/face.png` and the documentation images | public domain (NASA) |

Apple's Vision framework (macOS) is part of the operating system and is not distributed.
