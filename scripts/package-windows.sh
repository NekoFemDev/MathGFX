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

# Explicit runtime DLLs that ldd does not always resolve on the CI image.
for dll in vulkan-1.dll libvulkan-1.dll libwinpthread-1.dll \
           libgcc_s_seh-1.dll libstdc++-6.dll libssp-0.dll; do
    [ -f "/mingw64/bin/$dll" ] && cp "/mingw64/bin/$dll" "dist/$name/"
done

if command -v pacman >/dev/null 2>&1; then
    for f in $(pacman -Ql mingw-w64-x86_64-x264 2>/dev/null | awk '{print $2}' | \
               grep -Ei '/(copying|license)'); do
        [ -f "$f" ] && cp "$f" "dist/$name/X264_LICENSE"
    done
fi
for f in /mingw64/share/licenses/vulkan-loader/*; do
    [ -f "$f" ] && cp "$f" "dist/$name/VULKAN_LOADER_LICENSE"
done

(cd dist && zip -r "$name.zip" "$name" >/dev/null)
ls -lh "dist/$name.zip"
