# DoF bokeh demo

![DoF preview](preview.jpg)

Far focus:

![far focus](preview_focus9.jpg)

`dof_demo.comp` is a purpose-built thin-lens DoF scene: a sharp amber
sphere on the focus plane, a receding row of glossy spheres, emissive
orbs that become bokeh, and a glossy grid floor with contact shadows and
reflections.

The animation is inside the shader:

- the amber sphere sways on the focus plane;
- the background spheres drift in X/Z with different phases;
- the 12 emissive orbs float in X/Y/Z, so the bokeh shifts;
- the host sweeps focus from 5.0 to 9.0 and back (`--focus-end 9.0`).

No depth buffer and no post blur: every sample starts on the aperture
disk and aims at the focus plane.

## Scripts

```sh
./run_still.sh    # 256 samples, focus 5.0 -> out/dof_demo.png
./run_focus9.sh   # 256 samples, focus 9.0 -> out/dof_demo_focus9.png
./run_video.sh    # 32 samples, 120 frames, focus pull -> out/dof_demo.mp4
```

From the repo root: `make dof-demo`, `make dof-demo-video`.

The 4.8-second video is ~9.7 MB at CRF 20, 1920x1080, 25 fps. With CRF 23
it drops to ~5 MB; with 125 frames and CRF 20 it was ~11 MB.
