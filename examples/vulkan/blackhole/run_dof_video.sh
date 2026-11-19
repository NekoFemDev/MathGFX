#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) exe=.exe ;;
    *) exe= ;;
esac
bin="$root/build/mathgfx-vk$exe"
spv="$root/build/blackhole.spv"
[ -x "$bin" ] || { echo "run make first" >&2; exit 1; }
[ -f "$spv" ] || { echo "run make first" >&2; exit 1; }
mkdir -p "$root/out"
exec "$bin" --shader "$spv" --width 1920 --height 1080 --ssaa 1 --gpu 0 \
    --dof 8 --aperture 0.4 --focus 24 \
    --video "$root/out/blackhole_dof.mp4" --frames 125 --fps 25 --crf 20
