#!/bin/sh
# MathGFX smoke tests. Run from anywhere; paths are anchored to the repo root.
set -eu
cd "$(dirname "$0")/.."

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe ;;
    *) EXE= ;;
esac
BIN="build/mathgfx$EXE"

if command -v python3 >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1; then
    PY=python
else
    echo "FAIL: python3/python not found" >&2
    exit 1
fi
OUT=out/test
rm -rf "$OUT"
mkdir -p "$OUT"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

echo "== check =="
"$BIN" check examples/*.mgf

echo "== stills =="
for scene in gradient plasma noise mandelbrot cat blackhole; do
    "$BIN" render "examples/$scene.mgf" "$OUT/$scene.png" --width 64 --height 36
    "$PY" tests/check_png.py "$OUT/$scene.png" 64 36
done

echo "== branching =="
"$BIN" render tests/branches.mgf "$OUT/branches.png"
"$PY" tests/check_png.py "$OUT/branches.png" 16 16
# one pixel per branch: first/elif/else-if/else
"$PY" tests/check_pixels.py "$OUT/branches.png" \
    1 1 255 24 128 \
    5 1 191 24 128 \
    9 1 128 24 128 \
    13 1 64 24 128

echo "== save_frame / time_loop =="
"$BIN" render tests/anim.mgf
i=0
while [ "$i" -lt 3 ]; do
    name=$(printf 'out/test/frame_%03d.png' "$i")
    [ -f "$name" ] || fail "missing $name"
    "$PY" tests/check_png.py "$name" 32 32
    i=$((i + 1))
done
[ ! -e out/test/frame_003.png ] || fail "time_loop produced too many frames"

echo "== video =="
"$BIN" video tests/anim.mgf "$OUT/anim.mp4" --fps 25 --crf 20
"$PY" tests/check_mp4.py "$OUT/anim.mp4" 3 32 32
"$BIN" mux -o "$OUT/mux.mp4" --fps 25 --crf 20 "$OUT"/frame_*.png
"$PY" tests/check_mp4.py "$OUT/mux.mp4" 3 32 32
"$BIN" video tests/anim.mgf "$OUT/anim.avi" --fps 25
"$PY" tests/check_avi.py "$OUT/anim.avi" 3 32 32
"$BIN" mux -o "$OUT/mux.avi" --fps 25 "$OUT"/frame_*.png
"$PY" tests/check_avi.py "$OUT/mux.avi" 3 32 32

echo "== threads =="
"$BIN" render examples/gradient.mgf "$OUT/t1.png" --width 64 --height 36 --threads 1
"$BIN" render examples/gradient.mgf "$OUT/t8.png" --width 64 --height 36 --threads 8
cmp "$OUT/t1.png" "$OUT/t8.png" || fail "1-thread and 8-thread renders differ"

echo "== error handling =="
if "$BIN" check tests/bad_syntax.mgf >/dev/null 2>&1; then
    fail "bad_syntax.mgf was accepted"
fi
if "$BIN" render tests/bad_time.mgf >/dev/null 2>&1; then
    fail "bad_time.mgf was accepted"
fi

echo "== override =="
"$BIN" render examples/gradient.mgf "$OUT/override.png" --width 17 --height 9
"$PY" tests/check_png.py "$OUT/override.png" 17 9

echo "ALL TESTS PASSED"
