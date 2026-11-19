# Vulkan examples

`mathgfx-vk` is a generic headless Vulkan compute host. Each effect is a
separate shader in its own directory; the host only knows the SPIR-V
interface:

- `binding 0`: `uint pixels[]`, packed `0x00BBGGRR`;
- push constants: `uint width, height; float time; uint frame;
  float aperture, focus; uint samples, seed`;
- local size 8×8.

Build the host and shaders from the repo root:

```sh
make
```

Then run an example script, or use the Makefile target from the root.

Run the four main renders at once:

```sh
./run_all.sh
```

| Example | Script | Output |
|---------|--------|--------|
| black hole still | `blackhole/run_still.sh` | `out/blackhole_vk.png` |
| black hole 5 s video | `blackhole/run_video.sh` | `out/blackhole_vk.mp4` |
| black hole DoF still | `blackhole/run_dof.sh` | `out/blackhole_dof.png` |
| black hole DoF video | `blackhole/run_dof_video.sh` | `out/blackhole_dof.mp4` |
| DoF bokeh still | `dof_demo/run_still.sh` | `out/dof_demo.png` |
| DoF bokeh far focus | `dof_demo/run_focus9.sh` | `out/dof_demo_focus9.png` |
| DoF motion + focus pull | `dof_demo/run_video.sh` | `out/dof_demo.mp4` |

Each script resolves the repository root from its own path, so it works
from any current directory once `make` has built the binaries.

Makefile shortcuts (`make blackhole-vk`, `make blackhole-vk-video`,
`make blackhole-dof`, `make dof-demo`, `make dof-demo-video`) call the
same scripts.
