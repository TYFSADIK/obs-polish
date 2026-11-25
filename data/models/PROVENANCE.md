# Where these models come from

Google's MediaPipe models (Apache License 2.0), downloaded from Google's MediaPipe asset bucket
(`https://storage.googleapis.com/mediapipe-assets/`), checked against the SHA-256 below, and converted to ONNX with
[tf2onnx](https://github.com/onnx/tensorflow-onnx) 1.17.0 (opset 15) by `tools/convert_models.py`
on 2026-11-24. The script then runs the original and the converted model on the test
portrait and on random inputs; the last column is the largest difference between them, relative
to the output's range.

| Model | Original .tflite SHA-256 | ONNX bytes | Largest difference |
| --- | --- | --- | --- |
| face_detection_short_range | `3bc182eb9f33925d9e58b5c8d59308a760f4adea8f282370e428c51212c26633` | 418,528 | 1.2e-06 |
| face_landmark | `1055cb9d4a9ca8b8c688902a3a5194311138ba256bcc94e336d8373a5f30c814` | 2,429,085 | 1.0e-06 |

`selfie_segmentation.onnx` is different: Google's model uses a MediaPipe-only operator
(Convolution2DTransposeBias) that no converter supports, so this is
[onnx-community's ONNX version](https://huggingface.co/onnx-community/mediapipe_selfie_segmentation) of the same model
(Apache License 2.0, re-implemented with Google's weights), downloaded and checked against SHA-256 `3241ac4ad8aa35bdaf33946776db29f7c283a413aa0b0dacb9483594b4531aad`.
It is checked by the background tests instead of an output comparison.

Model cards: [face detection](https://storage.googleapis.com/mediapipe-assets/MediaPipe%20BlazeFace%20Model%20Card%20(Short%20Range).pdf),
[face mesh](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Face%20Mesh.pdf),
[selfie segmentation](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf).
