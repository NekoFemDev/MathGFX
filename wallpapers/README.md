# MathGFX AMOLED Wallpapers — Schwarzschild Black Hole

Generated with `mathgfx-vk` (headless Vulkan compute) on Mali-G615.
Resolution: **2560×1440 (2K)**, **1440×2560 (vertical 2K)**
SSAA: **2×** (rendered at 2×, box-downsampled)
Frame rate: **60 FPS** for videos
Quality: **CRF 18** (near-lossless H.264)

## Why AMOLED?

- **Event horizon = pure #000000** — true black, zero pixel power
- **Accretion disk** — vibrant oranges/blues, high contrast, pops on OLED
- **Photon ring** — subtle glow at 2.598 rs, visible on dark backgrounds
- **No UI elements** — clean, full-screen

---

## Files

### Still Wallpapers (2560×1440, 2× SSAA)

| File | Time | Description |
|------|------|-------------|
| `bh_2k_t0p0.png` | 0.0 | Reference frame |
| `bh_2k_t1p57.png` | 1.57 | π/2 — disk rotated 90° |
| `bh_2k_t3p14.png` | 3.14 | π — disk rotated 180° |
| `bh_2k_t4p71.png` | 4.71 | 3π/2 — disk rotated 270° |
| `bh_2k_t6p28.png` | 6.28 | 2π — full rotation |
| `bh_2k_t7p85.png` | 7.85 | 5π/2 |
| `bh_2k_t9p42.png` | 9.42 | 3π |
| `bh_2k_t11p0.png` | 11.0 | Near period end |

Each ~2.8 MB PNG. Pick any for static wallpaper.

### Vertical Wallpapers (1440×2560, 2× SSAA)

| File | Time | Use Case |
|------|------|----------|
| `bh_vertical_2k_t0.png` | 0.0 | Lock screen / home |
| `bh_vertical_2k_t3.png` | 3.14 | Alternate phase |

### Video Loops (2560×1440, 60 FPS, H.264 MP4)

| File | Duration | Frames | Size | Description |
|------|----------|--------|------|-------------|
| `bh_2k_60fps_10s_loop1.mp4` | 10 s | 600 | 7.7 MB | Full disk rotation (t=0→12.5) |
| `bh_2k_60fps_10s_loop2.mp4` | 10 s | 600 | 7.8 MB | Phase offset (t=π→π+12.5) |
| `bh_2k_60fps_5s_dof.mp4` | 5 s | 300 | ~5 MB | Cinematic DoF (4 samples, aperture 0.3) |

**Loop seamlessly:** Disk rotation period ≈ 12.5 time units. Videos span exactly one period.

### Preview Thumbnails (540×960)

| File | Time |
|------|------|
| `preview_bh_t0p0.png` | 0.0 |
| `preview_bh_t3p14.png` | 3.14 |
| `preview_bh_t6p28.png` | 6.28 |

Quick reference for picking stills.

---

## Usage

### Android (Static)

1. Copy `.png` to device
2. Settings → Wallpaper → Gallery → Select image
3. **Disable "Wallpaper zoom" / "Motion effect"** for pixel-perfect fit

### Android (Live/Video Wallpaper)

**Option A: KLWP (Kustom Live Wallpaper)**
- Create preset → Add "Video" module → Select `.mp4`
- Set "Loop" = on, "Fit" = "Crop"

**Option B: Video Live Wallpaper (Play Store)**
- Select `.mp4` → Set as wallpaper

**Option C: Termux + `mpv` (root/not needed)**
```sh
mpv --loop --vo=gpu --gpu-context=android wallpapers/bh_2k_60fps_10s_loop1.mp4
```

### iOS

- Convert `.mp4` → `.mov` (HEVC) via Shortcuts or `ffmpeg`
- Use "Live Photo" apps (intoLive, etc.) or native iOS 16+ video wallpaper

### Desktop (Linux/macOS/Windows)

- **mpvpaper** (Wayland): `mpvpaper -o "--loop" "*" wallpapers/bh_2k_60fps_10s_loop1.mp4`
- **Wallpaper Engine** (Steam): Import `.mp4`
- **Lively Wallpaper** (Windows): Add video
- **GNOME/KDE**: Set as animated wallpaper via plugins

---

## Regenerate / Customize

```sh
cd /path/to/MathGFX

# Prerequisites
make                    # builds mathgfx-vk + compiles shader

# Custom still
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 2560 --height 1440 --ssaa 2 --time 2.0 \
    --out my_wallpaper.png

# Custom 60FPS video (N seconds)
SEC=15
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 2560 --height 1440 --ssaa 2 \
    --video my_${SEC}s.mp4 --frames $((SEC*60)) --fps 60 --crf 18 \
    --time 0

# With Depth of Field (cinematic, slower)
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 2560 --height 1440 --ssaa 2 --dof 8 --aperture 0.3 --focus 24 \
    --video my_dof.mp4 --frames 300 --fps 60 --crf 18 --time 0

# Vertical (phone portrait)
./build/mathgfx-vk --shader build/blackhole.spv \
    --width 1440 --height 2560 --ssaa 2 --time 0 \
    --out vertical.png
```

### Parameters

| Flag | Default | Description |
|------|---------|-------------|
| `--ssaa N` | 2 | Supersampling (1..4). 2 = 2× render → box downsample |
| `--dof N` | 1 | Depth of field samples (1 = off). 4-8 for video, 16-64 for stills |
| `--aperture A` | 0 | Lens radius in rs. 0.3-0.5 = cinematic |
| `--focus F` | 24 | Focus distance in rs. 24 = camera distance |
| `--crf N` | 20 | H.264 quality (0-51, lower=better). 18 = near-lossless |
| `--time T` | 0 | Animation time offset |

---

## Technical Details

**Physics model:**
- Schwarzschild metric (`rs = 1`, `c = 1`)
- Camera: `r = 24 rs`, inclination 12°, half-FOV 18°
- Leapfrog integration, 360 steps/ray
- Null geodesic: `a = -1.5 * h² * r_vec / r⁵`
- Disk: Keplerian `v ∝ r⁻¹ᐟ²`, Doppler beaming `D³`, gravitational redshift
- Photon ring at `b_c = 3√3/2 ≈ 2.598 rs`

**Rendering:**
- Headless Vulkan compute (no window/surface)
- GLSL compute shader, 8×8 workgroups, float32
- Storage buffer output: packed `0x00BBGGRR`
- Box-filter SSAA downsample on host
- H.264 via libx264 + custom ISO BMFF muxer (no ffmpeg)

**Performance (Mali-G615):**
- 2560×1440 2× SSAA still: ~0.18 s
- 2560×1440 60 FPS video (600 frames): ~3-4 min
- 2560×1440 60 FPS DoF (4 samples, 300 frames): ~4 min

---

## License

Generated content: **CC0 / Public Domain** — use freely.
MathGFX code: See repository license.