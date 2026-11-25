#!/bin/sh
# MathGFX AMOLED Wallpaper Generator
# Generates 2K (2560x1440) black hole wallpapers and 60FPS video loops
# Optimized for AMOLED: true blacks, vibrant accretion disk

set -e

BIN="./build/mathgfx-vk"
SHADER="build/blackhole.spv"
OUTDIR="wallpapers"
WIDTH=2560
HEIGHT=1440
SSAA=2

# AMOLED-friendly: pure black horizon, vibrant disk colors
# Time values chosen for visually distinct disk rotations
TIMES="0.0 1.57 3.14 4.71 6.28 7.85 9.42 11.0"

mkdir -p "$OUTDIR"

echo "=== MathGFX AMOLED Wallpaper Generator ==="
echo "Resolution: ${WIDTH}x${HEIGHT} (2K)"
echo "SSAA: ${SSAA}x"
echo "Output: ${OUTDIR}/"
echo

# Check binary and shader
if [ ! -x "$BIN" ]; then
    echo "Error: $BIN not found. Run 'make' first."
    exit 1
fi
if [ ! -f "$SHADER" ]; then
    echo "Error: $SHADER not found. Run 'make' first."
    exit 1
fi

# Generate still wallpapers at different time offsets
echo "--- Generating still wallpapers ---"
for t in $TIMES; do
    outfile="${OUTDIR}/bh_2k_t$(echo $t | sed 's/\./p/').png"
    echo "  Time $t -> $outfile"
    "$BIN" --shader "$SHADER" --width $WIDTH --height $HEIGHT --ssaa $SSAA --time "$t" --out "$outfile"
done

# Generate 60FPS video loops (10 seconds = 600 frames)
echo
echo "--- Generating 60FPS video loops (10s = 600 frames) ---"
# Loop 1: Full disk rotation (period ~12.5 time units for one rotation)
# 600 frames at 60fps = 10s. Time step = 12.5/600 = 0.02083 per frame
# We'll use time_loop equivalent: start at 0, step 0.02083, 600 frames
"$BIN" --shader "$SHADER" --width $WIDTH --height $HEIGHT --ssaa $SSAA \
    --video "${OUTDIR}/bh_2k_60fps_10s_loop1.mp4" --frames 600 --fps 60 --crf 18 \
    --time 0 2>&1 | tail -1

# Loop 2: Different starting phase (quarter rotation offset)
"$BIN" --shader "$SHADER" --width $WIDTH --height $HEIGHT --ssaa $SSAA \
    --video "${OUTDIR}/bh_2k_60fps_10s_loop2.mp4" --frames 600 --fps 60 --crf 18 \
    --time 3.14 2>&1 | tail -1

# Loop 3: With subtle DoF (cinematic, 8 samples for speed)
echo
echo "--- Generating cinematic DoF variant (8 samples) ---"
"$BIN" --shader "$SHADER" --width $WIDTH --height $HEIGHT --ssaa $SSAA \
    --dof 8 --aperture 0.3 --focus 24 \
    --video "${OUTDIR}/bh_2k_60fps_10s_dof.mp4" --frames 600 --fps 60 --crf 18 \
    --time 0 2>&1 | tail -1

# Loop 4: Vertical orientation for phone lock screens (1440x2560)
echo
echo "--- Generating vertical 2K (1440x2560) ---"
"$BIN" --shader "$SHADER" --width 1440 --height 2560 --ssaa $SSAA --time 0 \
    --out "${OUTDIR}/bh_vertical_2k_t0.png"
"$BIN" --shader "$SHADER" --width 1440 --height 2560 --ssaa $SSAA --time 3.14 \
    --out "${OUTDIR}/bh_vertical_2k_t3.png"

# Generate thumbnail previews (540x960 for quick viewing)
echo
echo "--- Generating preview thumbnails (540x960) ---"
for t in 0.0 3.14 6.28; do
    outfile="${OUTDIR}/preview_bh_t$(echo $t | sed 's/\./p/').png"
    "$BIN" --shader "$SHADER" --width 540 --height 960 --ssaa 1 --time "$t" --out "$outfile"
done

echo
echo "=== Done! Files in ${OUTDIR}/ ==="
ls -la "$OUTDIR"/
echo
echo "Wallpaper tips for AMOLED:"
echo "  - Event horizon is pure #000000 (true black, zero power)"
echo "  - Accretion disk uses vibrant oranges/blues (pop on AMOLED)"
echo "  - 60FPS videos: set as live wallpaper via KLWP, Video Live Wallpaper, etc."
echo "  - CRF 18 = near-lossless quality, ~5-15MB per 10s loop"