# MathGFX for Windows (x86_64)

This archive is a MinGW-w64 build. Run the binaries from the extracted
folder; the required runtime DLLs are next to the `.exe` files.

## Contents

```text
mathgfx.exe          CPU renderer + PNG/JPEG + H.264/MP4 + MJPEG/AVI
mathgfx-vk.exe       headless Vulkan compute host
build\blackhole.spv  black-hole shader
build\dof_demo.spv   DoF bokeh shader
examples\            MGF scenes + Vulkan examples/previews
examples\anim.mgf    small 75-frame CPU animation scene
```

## Quick start (cmd.exe / PowerShell)

```bat
mathgfx.exe render examples\gradient.mgf out\gradient.png
mathgfx.exe render examples\blackhole.mgf out\blackhole.png --width 1920 --height 1080
mathgfx.exe video examples\anim.mgf out\anim.mp4 --fps 25 --crf 20

mathgfx-vk.exe --shader build\blackhole.spv --width 1920 --height 1080 ^
  --ssaa 2 --out out\blackhole_vk.png

mathgfx-vk.exe --shader build\blackhole.spv --width 1920 --height 1080 ^
  --video out\blackhole_vk.mp4 --frames 125 --fps 25 --crf 20

mathgfx-vk.exe --shader build\dof_demo.spv --width 1920 --height 1080 ^
  --dof 32 --aperture 0.32 --focus 5.0 --focus-end 9.0 ^
  --video out\dof_demo.mp4 --frames 120 --fps 25 --crf 20
```

In PowerShell use backticks for line continuation, or put each command on
one line. `out\` and other parent directories are created automatically.

## Vulkan

`mathgfx-vk.exe` needs a Vulkan 1.1+ capable GPU driver. The Vulkan
loader (`vulkan-1.dll`) is included; the actual ICD comes from the GPU
driver. If `mathgfx-vk.exe --list-gpus` shows no devices, update the GPU
driver. The CPU `mathgfx.exe` works without Vulkan.

`--gpu N` selects a physical device; `--list-gpus` prints them. The
shader path is relative to the current directory, so run from the
extracted root or pass an absolute path.

## Video

- `.mp4` uses H.264 via the bundled `libx264` DLL (GPL — see
  `X264_LICENSE` and `THIRD_PARTY_NOTICES.md`).
- `.avi` uses the built-in MJPEG path and does not need x264.
- `mathgfx.exe video` renders `time_loop` + `save_frame` frames directly
  into the encoder; `mathgfx.exe mux` stitches PNG frames.

## Notes

- The `examples\vulkan\*.sh` helpers are POSIX shell scripts; they run
  under MSYS2/Git Bash. On plain cmd/PowerShell use the `.exe` commands
  above.
- No audio, HEVC/VP9/AV1 or hardware video encoder yet.
- Source and issue tracker: the MathGFX GitHub repository.
