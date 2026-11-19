# Third-party notices

MathGFX bundles or links the following third-party components.

## stb_image / stb_image_write

- Files: `third_party/stb_image.h`, `third_party/stb_image_write.h`
- Author: Sean Barrett and contributors
- License: public domain / MIT (dual). See the header comments.

## libx264 (H.264 encoder)

- Used by `mathgfx`/`mathgfx-vk` for `.mp4` output.
- License: GNU GPL v2 (or commercial). The Windows release includes the
  runtime DLL and the `X264_LICENSE` file. Source: https://www.videolan.org/developers/x264.html
- If you redistribute a binary linked with x264, comply with the GPL
  (source availability and notices).

## Vulkan loader

- Windows release includes `vulkan-1.dll` from the MSYS2 Vulkan loader
  package; license: Apache License 2.0. A copy is included as
  `VULKAN_LOADER_LICENSE` when available.

## MinGW-w64 runtime

- Windows binaries may include `libgcc_s_seh-1.dll`,
  `libwinpthread-1.dll` and related runtime DLLs from the MinGW-w64
  toolchain; these are distributed under the GCC runtime exception /
  permissive terms.

## MathGFX

The MathGFX source itself is published in this repository. Add the
license of your choice before redistributing if you need explicit terms.
