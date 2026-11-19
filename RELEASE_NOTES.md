# MathGFX v0.1.0

First public release: a software procedural renderer with its own
ASM-like scene language, a CPU VM, a per-scene Vulkan compute path, and
a built-in video pipeline without ffmpeg.

## Highlights

- **MGF DSL**: canvas, double registers, expressions, `if/elif/else`,
  `repeat/break`, `y_loop`/`x_loop`, `time_loop`, `noise2`/`fbm2`,
  `save_frame`.
- **CPU renderer/VM** with pthreads row splitting, PNG/JPEG output and
  deterministic multithreaded renders.
- **Vulkan compute**: generic headless host (`mathgfx-vk`), per-scene
  SPIR-V shaders for the black hole and the DoF bokeh demo.
- **Honest thin-lens DoF**: aperture sampling and focus pull, no
  screen-space blur.
- **Video without ffmpeg**: H.264/MP4 via libx264 + a small ISO BMFF
  muxer; MJPEG/AVI fallback; `mathgfx video` and `mathgfx mux`.
- **Examples**: gradient, plasma, Mandelbrot, value noise, SDF cat,
  black-hole geodesic render, procedural Earth and a looping cloud
  animation; Vulkan black-hole and DoF demos with previews.
- **Windows x86_64 release** (MinGW-w64) with binaries, shaders, examples
  and required runtime DLLs; **Linux x86_64** tarball.

## Assets

- `mathgfx-windows-x86_64.zip` — Windows binaries + DLLs + shaders +
  examples. See `README-WINDOWS.md`.
- `mathgfx-linux-x86_64.tar.gz` — Linux binaries + shaders + examples.
  Requires system `libx264` and Vulkan loader/driver for the relevant
  features.
- `SHA256SUMS` — checksums.

## Quick checks

```sh
mathgfx.exe render examples\gradient.mgf out\gradient.png
mathgfx-vk.exe --shader build\blackhole.spv --list-gpus
mathgfx-vk.exe --shader build\blackhole.spv --width 1920 --height 1080 --ssaa 2 --out out\blackhole_vk.png
```

## Known limitations

- No audio; MP4 is H.264 only; no HEVC/VP9/AV1 or hardware encoder.
- Vulkan shaders are per-effect/hand-written; there is no general
  MGF-to-GPU compiler.
- The CPU VM is an interpreter (no JIT).
- Windows Vulkan output requires a Vulkan-capable GPU driver; the CPU
  renderer does not.
- x264 is GPL; see `THIRD_PARTY_NOTICES.md`.

## Verification

- `make test` — CPU scenes, threads, PNG/AVI/MP4 structure.
- `make test-vk` — Vulkan PNG/MP4/AVI on a device with Vulkan.
- `make lint` — shell scene linter.
