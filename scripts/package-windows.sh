#!/bin/sh
# Build a Windows x86_64 release tree and zip. Run inside MSYS2 MINGW64.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
make
name=mathgfx-windows-x86_64
rm -rf "dist/$name" "dist/$name.zip"
mkdir -p "dist/$name/build" "dist/$name/examples"
cp build/mathgfx.exe build/mathgfx-vk.exe "dist/$name/"
cp build/blackhole.spv build/dof_demo.spv "dist/$name/build/"
cp README.md README-WINDOWS.md Project.md THIRD_PARTY_NOTICES.md "dist/$name/"
cp examples/*.mgf "dist/$name/examples/"
cp -r examples/vulkan "dist/$name/examples/"
cp tests/anim.mgf "dist/$name/examples/anim.mgf"

{
    ldd build/mathgfx.exe || true
    ldd build/mathgfx-vk.exe || true
} | sed -n 's#.*=> \(/mingw64/[^ ]*\).*#\1#p' | sort -u | while IFS= read -r dll; do
    [ -f "$dll" ] && cp "$dll" "dist/$name/"
done

for f in /mingw64/share/licenses/x264/* /mingw64/share/licenses/libx264/*; do
    [ -f "$f" ] && cp "$f" "dist/$name/X264_LICENSE"
done
for f in /mingw64/share/licenses/vulkan-loader/*; do
    [ -f "$f" ] && cp "$f" "dist/$name/VULKAN_LOADER_LICENSE"
done

(cd dist && zip -r "$name.zip" "$name" >/dev/null)
ls -lh "dist/$name.zip"
