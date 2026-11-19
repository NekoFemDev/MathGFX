#!/bin/sh
# Build a Linux x86_64 release tree and tarball. Run on the build host.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
make
name=mathgfx-linux-x86_64
rm -rf "dist/$name" "dist/$name.tar.gz"
mkdir -p "dist/$name/build" "dist/$name/examples"
cp build/mathgfx build/mathgfx-vk "dist/$name/"
cp build/blackhole.spv build/dof_demo.spv "dist/$name/build/"
cp README.md Project.md THIRD_PARTY_NOTICES.md "dist/$name/"
cp examples/*.mgf "dist/$name/examples/"
cp -r examples/vulkan "dist/$name/examples/"
cp tests/anim.mgf "dist/$name/examples/anim.mgf"
tar -czf "dist/$name.tar.gz" -C dist "$name"
ls -lh "dist/$name.tar.gz"
