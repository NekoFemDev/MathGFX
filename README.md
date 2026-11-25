# MathGFX

Software procedural renderer: custom ASM-like DSL → bytecode → VM →
RGB8 frame → PNG. Primary path is CPU; heavy scenes can be offloaded to
headless Vulkan compute (one shader per effect). Video is written via
built-in H.264/MP4 (libx264 + custom MP4 muxer) or MJPEG/AVI fallback
— no ffmpeg required. No diffusion models — just math.

`Project.md` — original concept. This README describes what is
implemented in v0.1.

---

## Quick Start

```sh
make
./build/mathgfx render examples/gradient.mgf out/gradient.png
```

Six scenes at once:

```sh
make examples
# out/gradient.png  out/plasma.png  out/mandelbrot.png  out/noise.png  out/cat.png  out/earth.png
```

Black hole separately (360 geodesic integration steps per pixel):

```sh
make blackhole          # out/blackhole_960.png, 960x540, ~40 s
make blackhole-hq       # CPU 8 threads: 1920x1080 -> 1280x720 + 960x540, ~50 s
make blackhole-vk       # Vulkan Mali: 1920x1080 2x SSAA -> out/blackhole_vk.png, ~0.5 s
make blackhole-vk-video # Vulkan Mali: 5 s 1920x1080 H.264 MP4 -> out/blackhole_vk.mp4, ~10 s
```

Video from scene or PNG sequence:

```sh
./build/mathgfx video tests/anim.mgf out/anim.mp4 --fps 25 --crf 20
./build/mathgfx mux -o out/anim.mp4 --fps 25 --crf 20 out/frame_*.png
# fallback without x264: .avi + MJPEG
./build/mathgfx video tests/anim.mgf out/anim.avi --fps 25
```

Scene check without rendering:

```sh
./build/mathgfx check examples/*.mgf
```

Tests:

```sh
make test
```

---

## What's Implemented

- Lexer, parser, and single-pass DSL-to-bytecode compiler.
- Register VM: `r0..r15`, `double`, separate expression stack.
- Expressions with precedence, unary minus, comparisons, `^`.
- Built-in variables `X Y U V T W H PI`.
- Math: `sin cos tan sqrt abs floor ceil fract min max pow mod
  atan2 clamp lerp smoothstep noise2 fbm2`.
- `if / elif / else / end` blocks, `repeat / break` loops,
  `y_loop`, `x_loop`, `time_loop`.
- `set_pixel`, `pixel`, `save_frame` with `%d` / `%03d`.
- PNG via `stb_image_write.h` (single-header, no external deps).
- pthreads: `y_loop` splits by rows, `--threads N` (default: CPU count);
  `save_frame` stays single-threaded.
- Headless Vulkan compute: `src/vk_shader.c` — universal host,
  `examples/vulkan/<name>/<name>.comp` — one shader per effect.
  `mathgfx-vk --shader build/<name>.spv`; no window or surface.
  On Mali-G615 1080p 2×SSAA for black hole — ~0.1 s GPU, ~0.5 s total.
- Built-in video: H.264/MP4 via `libx264` + minimal ISO BMFF muxer
  (`src/mp4.c`), and MJPEG/AVI fallback (`src/avi.c`).
  `src/mux.c` stitches PNG sequence, `mathgfx video` renders scene
  directly into writer. No ffmpeg.
- Eight example scenes (SDF cat, black hole, Earth, cyclic cloud
  animation) and smoke tests including PNG structure validation.
- POSIX sh scene linter `tools/scene-lint.sh`.

---

## MGF Language

Scene files are `.mgf`. Comments: `;` or `//` to end of line.
Identifiers are case-sensitive: `r0`, `X`, `sin`, `set_pixel`.

### canvas

```asm
canvas 1280, 720
```

Mandatory, one per scene. Width and height are integer literals in
`1..16384`; size can be overridden from CLI (`--width`, `--height`)
— useful for quick previews and tests.

### Variables

| Name | Value |
|------|-------|
| `X`, `Y` | current pixel coordinates, `0..W-1`, `0..H-1` |
| `U`, `V` | `(X + 0.5) / W`, `(Y + 0.5) / H` — pixel centers in `[0,1]` |
| `T` | current time; `0` for static scene |
| `W`, `H` | canvas size |
| `PI` | 3.141592653589793 |

### Registers

`r0..r15`, values are `double`. Registers **are not reset per pixel**:
they are global VM variables. If a register is used in a pixel, it must
be explicitly set (or it will hold the value from the previous pixel).
Initial value is `0.0`.

```asm
mov r0, U
mul r0, 2.0          ; r0 = r0 * 2.0
add r0, 1.0
```

### Expressions

Instruction operands can be expressions — the compiler lowers them to
temporary stack slots:

```asm
mov r0, 0.5 + 0.5 * sin(U * 8.0 + T * 1.3)
```

Precedence (low → high):

1. comparisons: `< <= > >= == !=` (result `1.0` / `0.0`)
2. `+ -`
3. `* / %`
4. unary `+ -`
5. `^` (right-associative)

`-2^2` = `-(2^2)` = `-4`. `%` is remainder (`fmod`).

Functions:

| Function | Arity | Result |
|----------|-------|--------|
| `sin cos tan` | 1 | radians |
| `sqrt abs floor ceil fract` | 1 | `fract(x) = x - floor(x)` |
| `min(a,b) max(a,b)` | 2 | — |
| `pow(a,b) mod(a,b)` | 2 | — |
| `atan2(y,x)` | 2 | radians |
| `clamp(x,a,b)` | 3 | `min(max(x,a),b)` |
| `lerp(a,b,t)` | 3 | `a + (b-a)*t` |
| `smoothstep(a,b,t)` | 3 | smooth interpolation `a..b` |
| `noise2(x,y)` | 2 | value noise, `[0,1)` |
| `fbm2(x,y,octaves)` | 3 | fbm, `[0,1]`; octaves 1..10 |

Commas inside function calls are required: `min(a, b)`.

### Pixels

```asm
set_pixel X, Y, r0, r1, r2   ; explicit coordinates
pixel r0, r1, r2             ; current X, Y
```

Coordinates are `floor(x)`, `floor(y)`. Out-of-bounds is ignored.
Color is three `double`, clamped to `[0,1]` and rounded to 8-bit.
Alpha not yet supported.

### Loops

```asm
y_loop
  x_loop
    ...
  end
end
```

`y_loop` iterates `Y = 0..H-1`, `x_loop` iterates `X = 0..W-1`.
Alternative: `pixel` inside `x_loop`.

`repeat`:

```asm
repeat 64
  ...
  if r7 > 4.0
    break
  end
end
```

Counter is evaluated once, floored, capped at 10,000,000.
`break` works in `repeat`, `y_loop`, `x_loop`, `time_loop`.

`time_loop`:

```asm
time_loop 0.0, 10.0, 0.04   ; from (inclusive), to (exclusive), step
  ...
  save_frame "out/frame_%04d.png"
end
```

`T` runs from `from` with `step` while `T < to`. `step` must be `> 0`.
Without `save_frame` only the last frame remains in memory.

For static scenes time can be set manually:

```asm
time 1.25
```

### Conditionals

```asm
if U < 0.5
  mov r0, 1.0
elif U < 0.75
  mov r0, 0.5
else if U < 0.9
  mov r0, 0.25
else
  mov r0, 0.0
end
```

`elif` and `else if` are equivalent; the whole chain closes with one
`end`. A regular nested `if` on a new line requires its own `end`.

### save_frame

```asm
save_frame "out/frame_%04d.png"
```

Writes current frame to PNG. `%d`, `%02d`, `%04d` are replaced by frame
number (`save_frame` counter, starting at 0). Directories are created
automatically. Legacy form `save_frame T, "path"` is also accepted — the
expression before the string is ignored.

If the scene does not call `save_frame`, CLI writes the final frame to
`out.png` (or the path passed as second argument to `render`).

---

## Example Scene

`examples/plasma.mgf`:

```asm
canvas 1280, 720

y_loop
  x_loop
    mov r0, 0.5 + 0.5 * sin(U * 8.0 + T * 1.3 + noise2(U * 3.0, V * 3.0) * 2.0)
    mov r1, 0.5 + 0.5 * cos(V * 9.0 - T * 0.9 + fbm2(U * 2.0, V * 2.0, 4.0) * 3.0)
    mov r2, 0.5 + 0.5 * sin((U + V) * 7.0 + T * 0.7)
    set_pixel X, Y, r0, r1, r2
  end
end
```

`examples/cat.mgf` — cat portrait from SDF masks: ellipses (head, body,
eyes), triangles (ears, nose), capsules (whiskers, mouth), analytical
antialiasing via `r9 = 1.5 / H`, stripes and fur noise. Not just a
"gradient" — a full composition at ~1800 instructions per pixel.

`examples/earth.mgf` — procedural Earth with atmosphere, biomes, clouds,
and night lights; `examples/earth_clouds.mgf` — 12-second cyclic cloud
flyover (CPU `time_loop` + `save_frame`, `make earth-video`).

---

## Black Hole

`examples/blackhole.mgf` — Schwarzschild black hole raytracing
(`rs = 1`, `c = 1`). Per pixel: camera at `r = 24 rs`, inclination 12°
above disk plane, vertical half-FOV 18°.

Ray is integrated with leapfrog step using null geodesic equation in
Cartesian form:

```text
a = -1.5 * h^2 * r_vec / r^5        h = |r x v| = const
```

Equivalent to `u'' + u = 3 M u^2` with `M = 0.5`. Termination
conditions:

- `r < 1` — horizon, black;
- crossing `z = 0` plane in ring `3..12 rs` — opaque thin disk;
- `r > 40` and ray moving outward — procedural background (stars + nebula).

Disk: Keplerian rotation, Doppler factor
`D = 1 / (1 - beta * v_los)`, `beta = sqrt(0.5 / r)`, gravitational
redshift `sqrt(1 - 1/r)`, brightness `~D^3`, fbm turbulence, hot inner
glow and soft outer edge. Disk is infinitely thin and optically thick:
first plane intersection is taken; higher-order images are not traced
individually. Photon ring `b_c = 3*sqrt(3)/2 ~ 2.598 rs` is additionally
boosted by a smooth component — otherwise stepwise sampling leaves it
dotted.

360 steps per ray, ~920 instructions in scene.

- CPU: 1920×1080 — ~154 s single-thread, ~45–50 s on 8 threads (pthreads
  by rows). `make blackhole-hq` does CPU render + Lanczos downscale to
  1280×720/960×540.
- Vulkan compute (Mali-G615): 1920×1080 2×SSAA — ~0.1 s GPU, ~0.5 s
  total. Output: `out/blackhole_vk.png`.
  `examples/vulkan/blackhole/blackhole.comp` — same model, hand-ported
  to GLSL float32; no hardware RT cores, this is compute raytracing.
- Video: `make blackhole-vk-video` renders 5 s 1920×1080 (125 frames,
  25 fps) through same shader with `time`, writes
  `out/blackhole_vk.mp4` (~9.2 s, ~1.7 MB, H.264). MJPEG/AVI fallback —
  `make blackhole-vk-video-avi` (~9.5 s, ~15 MB).

---

## Vulkan: One Shader Per Scene

`mathgfx-vk` intentionally does not compile MGF and does not pretend to
be a general GPU backend. Each heavy scene is a separate compute shader
(`blackhole.comp`, `dof_demo.comp`, ...):

```
examples/vulkan/blackhole/blackhole.comp  ->  build/blackhole.spv  ->  mathgfx-vk --shader ...
```

Shader interface:

- `binding 0`: storage buffer `uint pixels[]`, packed `0x00BBGGRR`;
- push constants (16 bytes): `uint width, height; float time; uint frame`;
- local group 8×8, one invocation per pixel.

Add a new effect:

```sh
# 1. write examples/vulkan/foo/foo.comp
# 2. compile SPIR-V
glslangValidator -V -o build/foo.spv examples/vulkan/foo/foo.comp
# 3. run with same host
./build/mathgfx-vk --shader build/foo.spv --width 1920 --height 1080 \
    --out out/foo.png
```

For the two current shaders Makefile has explicit rules
(`build/blackhole.spv`, `build/dof_demo.spv`). For a new effect just
compile SPIR-V via `glslangValidator` and run the same
`mathgfx-vk --shader build/foo.spv ...`. Host does not change.

---

## DoF: True Thin Lens

`mathgfx-vk` does real Depth of Field, not screen-space blur:

```sh
--dof N        lens samples per pixel
--aperture A   aperture radius in rs (0 = pinhole)
--focus F      focus distance in rs
```

For each sample, ray starts at random point on aperture disk and aims
at focal plane; then same geodesic, disk, and photon ring. No depth
buffer, no post-blur.

```sh
# 1080p, 64 samples, soft cinematic DoF
make blackhole-dof

# manually
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 1920 --height 1080 --ssaa 1 --gpu 0 \
    --dof 64 --aperture 0.4 --focus 24 --out out/blackhole_dof.png
```

Result: `out/blackhole_dof.png` (64 samples, aperture 0.4, focus
24 rs, ~1.8 s GPU / ~2.5 s total). `out/blackhole_dof_wide.png` —
aperture 0.8 variant, stronger blur.

Physical caveat: on astrophysical scales DoF is near zero; `aperture
0.4–1.0 rs` at `focus 24 rs` is a cinematic camera, not a real
telescope. Camera model is still honest: thin lens, N independent rays,
no fake post-process. For video 8–16 samples give soft background with
some noise; for stills 64–128 is better. Video target:
`make blackhole-dof-video` (8 samples, 125 frames, ~32 s).

---

## DoF Demo: Bokeh Spheres

A dedicated example where DoF is immediately visible:

```sh
make dof-demo        # out/dof_demo.png, 1920x1080, 256 samples, ~1.5 s
make dof-demo-video  # out/dof_demo.mp4, 120 frames, 32 samples, focus pull, ~19 s
```

Scene: sharp amber sphere on focal plane, receding row of glossy
spheres, emissive orbs at different depths (their images become bokeh),
glossy floor with grid, contact shadows and reflections. Camera is the
same honest thin lens: `--dof 256 --aperture 0.32 --focus 5.0`. No depth
buffer, no post-blur.

In video, spheres and orbs move while focus smoothly pulls from front
sphere to background and back (`--focus-end 9.0`). 120 frames, 32
samples, 1920×1080, H.264, ~9.7 MB, ~19 s. Stop frame at far focus:
`out/dof_demo_focus9.png`.

Parameters chosen so front sphere is sharp, background soft; floor grid
gives clear depth cue, orbs produce large bokeh. Unlike black hole
(where DoF is near zero on astrophysical scales), here the effect is
the star of the frame.

Measurements: 256 samples 1080p — ~0.8 s GPU / ~1.5 s total; video 32
samples, 125 frames — 18.7 s (GPU 12.4 s, readback 1.5 s, encode 4.8 s),
~11 MB.

---

## Video Without ffmpeg

Two built-in video paths:

- **H.264/MP4** — `libx264` + custom ISO BMFF/MP4 muxer
  (`src/mp4.c`); quality via `--crf` (0..51, lower is better).
- **MJPEG/AVI** — JPEG frames via `stb_image_write` + RIFF/AVI muxer
  (`src/avi.c`); quality via `--quality` (1..100).

No `ffmpeg`, `popen`, or external containers. MP4 needs `libx264`
(`pkg install libx264`); AVI path works without it.

```sh
# 1. MP4 from scene with time_loop + save_frame (recommended for Android)
./build/mathgfx video tests/anim.mgf out/anim.mp4 --fps 25 --crf 20

# 2. Stitch PNG sequence into MP4
./build/mathgfx mux -o out/anim.mp4 --fps 25 --crf 20 out/frame_*.png

# 3. Vulkan animation: time = --time + frame/fps
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 1920 --height 1080 --ssaa 1 --gpu 0 \
    --video out/blackhole_vk.mp4 --frames 125 --fps 25 --crf 20

# fallback: AVI/MJPEG
./build/mathgfx video tests/anim.mgf out/anim.avi --fps 25 --quality 90
```

For `mathgfx video` the `save_frame` path is ignored — frame goes to
writer, not PNG.

Why AVI might open black: file was valid MJPEG/AVI (frames readable),
but Android players often don't support MJPEG/AVI. MP4/H.264 plays
natively. Sizes for same 5 s 1080p clip: MP4 — ~1.7 MB, AVI/MJPEG — ~15 MB.

Limitations: no audio; HEVC/VP9/AV1 and hardware video encoder not yet
connected. MJPEG remains fallback and works well for VLC/mpv.

---

## CLI

```
mathgfx render <scene.mgf> [out.png] [--width N] [--height N] [--threads N] [--ssaa N]
mathgfx check <scene.mgf> [more.mgf ...]
mathgfx video <scene.mgf> <out.mp4|out.avi> [--fps N] [--crf N]
                [--quality N] [--width N] [--height N]
mathgfx mux -o <out.mp4|out.avi> [--fps N] [--crf N] [--quality N]
                <frame.png> ...
mathgfx --version

mathgfx-vk [--shader build/blackhole.spv] [--width N] [--height N]
           [--ssaa N] [--gpu N] [--time SECONDS]
           [--dof N] [--aperture A] [--focus F]
           [--out out/blackhole_vk.png]
           [--video out.mp4|out.avi] [--frames N] [--fps N]
           [--crf N] [--quality N] [--list-gpus]
```

`render` prints time and thread count. Without `--threads` uses `nproc`
(up to 64); scenes with `save_frame` and without `y_loop` render
single-threaded. `check` compiles scene and shows size, instruction
count, and string constants.

`video` renders `time_loop` + `save_frame` directly into writer:
`save_frame` paths are ignored, frames go to MP4 or AVI by extension.
`mux` stitches ready PNGs into same format. `mathgfx-vk` — separate
headless Vulkan compute host; runs any compatible SPIR-V (`--shader`),
`--ssaa N` renders at N× resolution and box-downsamples on readback.
With `--video` it renders N frames with changing `time` and writes MP4
or AVI by extension.

---

## Architecture

```
scene.mgf
    │
    ▼
lexer.c ── tokens ──▶ parser.c ── Instr[] ──▶ vm.c ── RGB8 ──┬──▶ stb_image_write ──▶ PNG
                              (one pass)         │           │
                                                 │           └──▶ video.c ──┬──▶ avi.c ──▶ AVI
                                                 │                          └──▶ mp4.c ──▶ MP4
                                                 └── save_frame / time_loop
```

- `src/lexer.c` — tokens, `line:col` positions, string literals.
- `src/parser.c` — recursive descent, emits bytecode directly
  (no AST), patches block jumps.
- `src/vm.c` — `double` registers, expression stack, loops, noise,
  pixel/frame write, pthreads by rows, frame-sink for video.
- `src/png.c` — PNG/JPEG via `third_party/stb_image_write.h`,
  `mkdir -p` for frame paths.
- `src/avi.c` — RIFF/AVI muxer and JPEG frame encoding.
- `src/mp4.c` — H.264 via libx264 + ISO BMFF/MP4 muxer.
- `src/video.c` — writer selection by `.avi`/`.mp4` extension.
- `src/mux.c` — PNG sequence → AVI/MP4 (stb_image).
- `src/main.c` — CLI including `video` and `mux`.

GPU path for black hole:

```
examples/vulkan/blackhole/blackhole.comp ── glslangValidator ──▶ build/blackhole.spv
        (float32 port of blackhole.mgf)                       │
                                                            ▼
                                               mathgfx-vk --shader build/blackhole.spv
                                                            │
                                                            ▼
                                           Vulkan compute -> readback -> box SSAA
                                                            │
                                                            ▼
                                               PNG / MP4 / AVI
```

- `examples/vulkan/blackhole/blackhole.comp` — GLSL compute 8×8, float32;
  hand port of same model: geodesics, disk, photon ring. Not generated
  from `.mgf` — separate GPU backend, not a general DSL compiler.
- `src/vk_shader.c` — universal headless Vulkan host: instance →
  physical device → compute queue → storage buffer → dispatch → readback
  → PNG or AVI. Knows nothing about black hole; specific effect set via
  `--shader`.
- Build requires `vulkan-loader`, `vulkan-headers`, `glslangValidator`
  (Termux: `pkg install vulkan-loader vulkan-headers vulkan-tools glslang`).
  For Mali uses `vulkan-wrapper-android`; for software fallback —
  `mesa-vulkan-icd-swrast` (lavapipe).
- `--gpu` selects physical device; `--list-gpus` shows Mali and lavapipe.

Why C core:

- Per-pixel math needs `double`, `sin/cos/sqrt` and predictable speed;
  C gives this without fixed-point layers.
- Core stays small and portable: C99 + `-lm` + `-pthread`.
- Vulkan backend is a separate binary, doesn't drag software path.

Tooling is plain POSIX sh: `tools/scene-lint.sh` checks `canvas` and
block balance without interpreter or external deps.

---

## SH Linter

```sh
make lint
# or
sh tools/scene-lint.sh examples
sh tools/scene-lint.sh tests
```

Checks each `*.mgf`:

- has `canvas`;
- balance of opening blocks (`if`, `repeat`, `y_loop`, `x_loop`,
  `time_loop`) and `end`;
- prints `lines`, `blocks`, `pixels`, `canvas` and `OK/WARN`.

Exit code `0` if all scenes clean, else `1`.

---

## Tests

```sh
make test
```

`tests/run.sh`:

- compiles all examples;
- renders them at 64×36 and validates PNG via `tests/check_png.py`
  (signature, IHDR, zlib stream, exact decompressed row lengths);
- checks colors of all four `if/elif/else if/else` branches via
  `tests/check_pixels.py` (full PNG unfilter, RGB compare);
- verifies 1-thread and 8-thread renders produce byte-identical PNG
  (row determinism);
- checks `time_loop` + `save_frame` (exactly 3 frames `frame_000..002`);
- checks `mathgfx video` and `mathgfx mux`:
  - MP4: `ftyp`/`moov`/`mdat`, `avc1`/`avcC`, SPS/PPS, `stsz`/`stco`/
    `stts`/`stsc`/`stss`, NAL lengths — `tests/check_mp4.py`;
  - AVI: RIFF/AVI, MJPG, `movi`, `idx1`, JPEG decode via
    `tests/check_avi.py` (PIL);
- verifies broken scenes (`bad_syntax`, `bad_time`) are rejected;
- checks CLI size override.

Separate: `make test-vk` — Vulkan PNG + Vulkan MP4 + Vulkan AVI.

---

## Performance

Measurements on Termux/aarch64 (Mali-G615, 8 CPU), including
PNG/JPEG/H.264 encoding. CPU numbers are single-threaded unless noted:

| Scene | Size | Time |
|-------|------|------|
| gradient | 1280×720 | ~0.10 s |
| plasma | 1280×720 | ~0.38 s |
| noise | 1280×720 | ~0.66 s |
| mandelbrot (96 iter) | 960×540 | ~1.7 s |
| cat (SDF portrait) | 1280×720 | ~9.8 s |
| earth (planet) | 1280×720 | ~2.4 s (8 threads) |
| blackhole (360 steps/ray) | 640×360 | ~17 s (1 thread) |
| blackhole | 960×540 | ~40 s (1 thread) / ~12 s (8) |
| blackhole | 1920×1080 | ~154 s (1 thread) / ~45–50 s (8) |
| blackhole Vulkan 2×SSAA | 1920×1080 | ~0.1 s GPU / ~0.5 s total |
| blackhole Vulkan video, 125 frames, MP4 | 1920×1080 | ~9.2 s, ~1.7 MB |
| blackhole Vulkan video, 125 frames, AVI | 1920×1080 | ~9.5 s, ~15 MB |
| blackhole Vulkan DoF, 64 samples | 1920×1080 | ~1.8 s GPU / ~2.5 s total |
| blackhole Vulkan DoF video, 8 samples | 1920×1080 | ~32 s (125 frames) |
| DoF demo still, 256 samples | 1920×1080 | ~0.8 s GPU / ~1.5 s total |
| DoF demo video, 32 samples, focus pull | 1920×1080 | ~19 s (120 frames), ~9.7 MB |

CPU bottlenecks: bytecode interpretation per pixel and PNG/JPEG
compression. pthreads already enabled; next step — built-in CPU SSAA.
Vulkan remains per-scene shaders, not a general DSL backend.

---

## Limitations v0.1

- Video: H.264/MP4 and MJPEG/AVI only. No audio, HEVC/VP9/AV1, or
  hardware video encoder yet; MJPEG remains fallback.
- No PNG texture loading/sampling (stb_image decoder exists for `mux`,
  but `sample(u,v)` not yet exposed in DSL).
- No alpha channel, HDR, or gamma correction.
- pthreads exists, but `save_frame`/video path is single-threaded.
- No scene include files or string variables.
- No JIT.
- Vulkan host is universal over SPIR-V, but shaders are written per
  effect; no general MGF→GPU compiler. GPU code uses float32, no
  float64 or hardware RT.
- **CPU vs GPU precision mismatch:** CPU VM uses `double` (64-bit);
  Vulkan shaders use `float` (32-bit). This causes visible differences
  in precision-sensitive scenes (e.g., black hole geodesics near the
  photon ring). For exact cross-validation, use `--fp32` (planned) or
  compare visually. The DSL has no type annotations; all math is `double`
  on CPU and `float` on GPU.

---

## Roadmap

1. More built-in formats: audio, HEVC/VP9/AV1, GIF/APNG.
2. Built-in SSAA in CPU path (currently external: 2× render + Lanczos).
3. `sample(u, v)` for PNG textures (stb_image).
4. More math: `log exp sign step smoothstep` for vectors, gradient
   noise, curl noise.
5. JIT bytecode compilation to C or direct threaded-code interpreter.
6. New per-scene Vulkan shaders: Kerr, jets, planets, nebulae.
7. Extended shell tooling: scene generator, statistics, palette checks.

---

## Structure

```
MathGFX/
├── include/mgfx.h            public API, opcodes, limits
├── src/
│   ├── internal.h            tokens and lexer
│   ├── lexer.c
│   ├── parser.c              parser + bytecode compiler
│   ├── vm.c                  VM, pthreads, frame-sink
│   ├── png.c                 PNG/JPEG + mkdir -p
│   ├── avi.c / avi.h         MJPEG AVI muxer
│   ├── mp4.c / mp4.h         H.264/MP4 (libx264 + muxer)
│   ├── video.c / video.h     AVI/MP4 writer selection
│   ├── mux.c / mux.h         PNG sequence -> AVI/MP4
│   ├── vk_shader.c           headless Vulkan compute host
│   ├── stb_write_impl.c      stb_image_write implementation
│   └── main.c                CLI (render/check/video/mux)
├── examples/
│   ├── *.mgf                 CPU DSL scenes
│   └── vulkan/
│       ├── README.md
│       ├── run_all.sh
│       ├── blackhole/        blackhole.comp + run_*.sh + preview.jpg
│       └── dof_demo/         dof_demo.comp + run_*.sh + preview*.jpg
├── third_party/stb_image_write.h
├── third_party/stb_image.h
├── tests/
│   ├── run.sh
│   ├── check_png.py
│   ├── check_pixels.py
│   ├── check_avi.py
│   ├── check_mp4.py
│   ├── anim.mgf
│   ├── branches.mgf
│   ├── bad_syntax.mgf
│   └── bad_time.mgf
├── tools/scene-lint.sh       POSIX-sh scene linter
├── Makefile
├── README.md
└── Project.md
```

`stb_image_write.h` and `stb_image.h` — public domain / MIT (Sean
Barrett), in `third_party/` as single-header. H.264 encoder — system
`libx264` (`pkg install libx264`).