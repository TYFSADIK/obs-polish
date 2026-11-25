# Changelog

## 0.3.0

- **Windows (x64 and ARM64) and Linux (x86_64 and ARM64)**, alongside macOS (Apple silicon and
  Intel). Windows and Linux use Google's MediaPipe face detection, face mesh and selfie
  segmentation models on ONNX Runtime; macOS keeps Apple's Vision framework.
- **Works in OBS 30, 31, 32 and 33.** 0.1.0 and 0.2.0 were built against OBS 33's libobs, which
  older OBS versions refuse to load, and used an image API that only exists in OBS 33. The plugin
  is now built against libobs 30.0 and decodes images itself.
- Works with OBS's new Metal renderer on macOS as well as OpenGL.
- Background blur rebuilt as a dual-filter chain (half-size steps down, bilinear steps up): it
  renders the same on Direct3D, OpenGL and Metal.
- With every effect off, or compare held, the filter steps aside: exactly what OBS shows without
  it, at no cost.
- Stickers blend with straight alpha, so their edges are no longer darkened.
- The settings show why face analysis is unavailable instead of silently doing nothing.
- Fixed a crash when the lips outline had more points than expected.
- Tests run on every platform in CI, each inside that platform's own OBS.

## 0.2.0

- Makeup: lipstick and blush in any colour, teeth whitening, eye brightening.
- 9 more looks in Portrait, Food, Landscape and Modern groups (17 in all); vignette and film grain.
- Built-in backdrops: Studio, Gradient, Bokeh lights, Sunset. 4 more stickers (10 in all).

## 0.1.0

- First release: skin smoothing, brightening, sharpening, face shape, 8 looks, background blur,
  colour or image, 6 stickers, compare, Tools menu, 8 languages. macOS only.
