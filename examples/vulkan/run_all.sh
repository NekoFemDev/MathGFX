#!/bin/sh
# Render the main Vulkan examples (still + animation).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
sh "$root/examples/vulkan/blackhole/run_still.sh"
sh "$root/examples/vulkan/blackhole/run_video.sh"
sh "$root/examples/vulkan/dof_demo/run_still.sh"
sh "$root/examples/vulkan/dof_demo/run_video.sh"
