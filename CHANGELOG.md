# Changelog

## 0.5.0

- **47 effects** in a new *Effects* group, each with a strength and, where it has one, its own
  setting and colours:
  - Colour: grayscale, sepia, teal and orange, invert, colourize, duotone, bleach bypass,
    cross-process, golden hour, moonlight, cyberpunk neon, colour pass (keep one colour).
  - Light: HDR, bloom, solarize, posterize, threshold, light leak.
  - Blur: blur, motion blur, zoom blur, tilt-shift miniature, box blur, lens blur (bokeh),
    unsharp mask, denoise.
  - Retro: film grain, VHS, CRT scanlines, Super 8, old film, halftone, RGB split, dither, lomo.
  - Stylize: glitch, pixelate, fisheye, mirror and kaleidoscope, wave, twirl, emboss, neon
    outline, cartoon, oil paint, thermal, night vision, echo trails.
- Effects apply over everything else, stickers included, so a pixelated or thermal picture has a
  pixelated or thermal sticker.
- **Contrast** in the Colour group, and a vignette that can lighten the corners as well as darken
  them.
- 29 pixel checks in a real OBS on every platform, up from 24: every effect renders and changes
  the picture, and grayscale, invert, threshold, posterize, pixelate, mirror, blur, unsharp mask,
  outline, night vision and echo are each held to an exact property.

## 0.4.0

- **Eye makeup:** eyeshadow, eyeliner with a small wing, eyebrows (fill and define) and eye colour,
  each in any colour. They follow the eye and brow outlines from the face landmarks; eye colour
  finds the iris from the darkest part of each eye and keeps the pupil and reflections.
- **Foundation:** evens skin tone and softens redness and blotches while keeping light, shade and
  texture, towards a shade you choose.
- **Soft light:** a gentle glow from a blurred copy of the picture, and a soft light on the face.
- **Face shape:** V-shape jaw, chin length (shorter or longer), slim nose and fuller lips, alongside
  slim face and bigger eyes.
- **Fixed:** with smoothing on (the default) or any makeup, moving *Slim face* or *Bigger eyes*
  could turn the whole picture one colour, and blush, eye brightening, vignette and grain could
  land in the wrong place. libobs clears effect values after every pass, and the frame size was
  only set before the first; every pass now sets what it reads. A new test stacks effects the way
  people use them, so this cannot come back unnoticed.
- 24 pixel checks in a real OBS on every platform, up from 14: one for each new effect and one
  for stacked effects.

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
