#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) exe=.exe ;;
    *) exe= ;;
esac
bin="$root/build/mathgfx-vk$exe"
spv="$root/build/dof_demo.spv"
[ -x "$bin" ] || { echo "run make first" >&2; exit 1; }
[ -f "$spv" ] || { echo "run make first" >&2; exit 1; }
mkdir -p "$root/out"
exec "$bin" --shader "$spv" --width 1920 --height 1080 --ssaa 1 --gpu 0 \
    --dof 256 --aperture 0.32 --focus 5.0 --out "$root/out/dof_demo.png"
