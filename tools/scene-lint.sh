#!/bin/sh
# scene-lint.sh — structural linter for .mgf scenes.
#
# Checks that every scene has a canvas and that if/repeat/y_loop/x_loop/
# time_loop blocks balance their `end`. Prints lines / blocks / pixels.
#
# usage: sh tools/scene-lint.sh [dir]      (default: examples)
# exit:  0 = clean, 1 = warnings, 2 = no scenes
set -u

dir=${1:-examples}
status=0
found=0

for f in "$dir"/*.mgf; do
    [ -e "$f" ] || continue
    found=1
    awk -v file="$f" '
        BEGIN { lines = 0; open = 0; ends = 0; pixels = 0; canvas = 0 }
        {
            lines++
            line = $0
            sub(/^[ \t]+/, "", line)
            if (line == "" || line ~ /^;/ || line ~ /^\/\//)
                next
            n = split(line, a, /[ \t]+/)
            w = a[1]
            if (w == "canvas")
                canvas = 1
            else if (w == "if" || w == "repeat" || w == "y_loop" ||
                     w == "x_loop" || w == "time_loop")
                open++
            else if (w == "end")
                ends++
            else if (w == "set_pixel" || w == "pixel")
                pixels++
        }
        END {
            ok = (canvas == 1 && open == ends)
            printf "%s lines=%d blocks=%d/%d pixels=%d canvas=%s -> %s\n",
                   file, lines, open, ends, pixels,
                   (canvas ? "yes" : "no"), (ok ? "OK" : "WARN")
            exit (ok ? 0 : 1)
        }
    ' "$f" || status=1
done

if [ "$found" -eq 0 ]; then
    echo "scene-lint: no .mgf files in $dir" >&2
    exit 2
fi

if [ "$status" -ne 0 ]; then
    echo "scene-lint: warnings found" >&2
else
    echo "scene-lint: all scenes OK"
fi
exit "$status"
