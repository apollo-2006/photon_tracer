# photon_tracer

[![demo](https://github.com/apollo-2006/photon_tracer/actions/workflows/pages.yml/badge.svg)](https://github.com/apollo-2006/photon_tracer/actions/workflows/pages.yml)
[![license: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**[Render it in your browser →](https://apollo-2006.github.io/photon_tracer/)** The
renderer is compiled to WebAssembly and runs in one Web Worker per thread, with controls
for resolution, samples, bounces and worker count, and a switch between the current row
scheduling and the old fixed bands so you can time the difference yourself.

A CPU-based raytracer written from scratch in C++, built to explore computer graphics, vector mathematics, and physical rendering fundamentals. The engine calculates the intersection of light rays with 3D geometry to generate images entirely from scratch, with no external graphics libraries.

## How it works

* **Rays, not triangles.** Nothing is projected or rasterized. For every pixel the
  renderer casts a ray out through the viewport and asks the world what it hit, which is
  why shadows and bounced light fall out of the algorithm instead of being added on top.
* **Analytic sphere intersection.** `sphere.hpp` solves the ray-sphere quadratic
  directly and returns the nearer root inside the valid `t` range. The `hittable`
  interface (`hittable.hpp`) keeps the intersection test behind a virtual call, so the
  world is just a list of things that know how to be hit.
* **Triangles and OBJ meshes.** `triangle.hpp` intersects with Moller-Trumbore, which
  solves for the hit distance and barycentric coordinates in one step. `obj.hpp` reads
  vertex positions and faces from Wavefront OBJ text (polygons are split into fans),
  scales the model into place, and computes smooth vertex normals by summing the
  area-weighted normals of the faces around each vertex, so a low-poly model shades
  smoothly. Which side a ray hit still comes from the true face normal.
* **Materials.** Each sphere carries a material (`material.hpp`) that decides how a
  ray scatters and how much of each color channel survives:
  * *Lambertian:* scatters toward the normal plus a random unit vector, a
    cosine-weighted bounce, tinted by its albedo.
  * *Metal:* mirror reflection, blurred by a `fuzz` factor from 0 (mirror) to 1.
  * *Dielectric:* refracts by Snell's law, reflects under total internal reflection,
    and otherwise chooses between the two with Schlick's approximation. A sphere with a
    negative radius flips its normals inward, which makes a hollow glass shell.

  Recursion is capped at 10 bounces so a ray trapped between surfaces terminates.
* **BVH.** `bvh.hpp` builds a binary tree of axis-aligned bounding boxes over the scene,
  splitting each node at the median along the longest axis. A ray only descends into
  boxes it crosses (`aabb.hpp`, a slab test), so it tests a handful of spheres instead of
  all of them. The right subtree is searched only for hits closer than the left's.
* **Anti-aliasing by supersampling.** 50 rays per pixel, each jittered by a random
  sub-pixel offset, averaged. This is what removes the stair-stepping on sphere edges.
* **Gamma correction.** Output is square-rooted before writing, an approximation of sRGB
  that keeps midtones from looking too dark.
* **Multithreaded.** One worker per hardware thread, each claiming the next unrendered row
  from a shared atomic counter. Rows are not split into fixed bands up front, because the
  top of the frame is sky (one miss per sample) and the bottom is ground (several
  bounces), so fixed bands leave the sky threads idle. The RNG and the ray counter are
  `thread_local`, so workers never contend on them.
* **PPM output.** Written as plain ASCII P3 with no image library involved.

## Scene

Three 0.5-radius spheres one unit in front of the camera: hollow glass at `(-1, 0, -1)`,
matte blue at `(0, 0, -1)` and fuzzy gold metal at `(1, 0, -1)`, on a 100-radius matte
sphere acting as the ground plane. The background is a vertical blue-to-white gradient interpolated on
the ray direction.

`--field` adds about 400 small spheres around them (random matte, metal and glass, from
a fixed seed) and views the scene from above with a look-at camera, so the field spreads
out instead of bunching up at the horizon.

`--mesh` loads the Utah teapot (`models/teapot.obj`, 6,320 triangles, from
[common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models)) in
polished copper between a glass and a matte sphere. `--obj PATH` puts any other OBJ model
in its place.

## Build & Run
```bash
# Clone the repository
git clone https://github.com/apollo-2006/photon_tracer.git
cd photon_tracer

# Compile the project
make

# Run the raytracer (outputs to render.ppm)
./photon_tracer
./photon_tracer --field             # the 400-sphere scene
./photon_tracer --field --no-bvh    # the same, testing every sphere per ray
./photon_tracer --mesh              # the Utah teapot
./photon_tracer --obj model.obj     # your own model in the teapot's place
./photon_tracer --spp 10            # fewer samples per pixel
./photon_tracer --threads 8         # fewer than every hardware thread
```

Renders 1920x1080 at 50 samples per pixel. The output is a ~24 MB ASCII PPM; most image
viewers open it directly, or convert it with `magick render.ppm render.png`.

The BVH is on by default for the field and the mesh and off for the five-sphere scene, where the
ground sphere's box covers nearly every ray and the tree is pure overhead; `--bvh` and
`--no-bvh` force it either way. Resolution and bounce depth are constants at the top of `src/main.cpp`.
The tracing itself (`ray_color`, the scenes, and `render_row`) lives in
`include/renderer.hpp`, shared with the web build.

## Performance

The renderer prints its own timing when it finishes. Ryzen 9 5900XT (16 cores, 32
threads), g++ 16 `-O3`, 1920x1080, 50 samples per pixel, up to 10 bounces, best of three:

| | |
|---|---|
| render | **0.49 s** |
| rays traced | 184.6M (103.7M primary, the rest bounces) |
| throughput | **~375M rays/s** |
| whole run, including the 24 MB PPM write | 0.64 s |

Handing out rows from a shared counter instead of fixed bands took the whole run from
0.98 s to 0.67 s on the same machine, with identical output statistics. CPU time went up
(13.3 s to 15.9 s) because threads that used to finish their band of sky and exit now
keep working, which is the point: parallel speedup went from about 14x to 24x.

In the browser demo, the same render at 100 samples per pixel across 32 Web Workers takes
1.15 s at about 320M rays/s, and 1.73 s with fixed bands. WebAssembly gets within about
15% of the native build's throughput here, since the inner loop is plain double-precision
arithmetic.

### BVH

Same machine and settings, native build:

| scene | list | BVH |
|---|---|---|
| materials, 5 spheres | **1.39 s**, 238M rays/s | 1.79 s, 186M rays/s |
| field, ~400 spheres | 22.0 s, 15M rays/s | **5.2 s**, 67M rays/s |
| mesh, 6,320 triangles, 4 spp, 8 threads | 77.4 s, 0.21M rays/s | **0.67 s**, 23M rays/s |

The BVH makes the field 4.3x faster and the teapot 115x faster: without it, every ray
tests all 6,320 triangles. On five spheres it is slower, since the ground is a
100-radius sphere whose box contains everything, so every ray pays for the tree walk and
still tests nearly every sphere. The table above predates materials: glass and metal rays
bounce further before they escape, so the materials scene traces more rays per pixel.

## Web demo

`web/tracer_web.cpp` exposes `render_row_linear()` and a scene/BVH switch to JavaScript and `web/build.sh` compiles it
with Emscripten. The page runs one module instance per Web Worker and hands rows out
from the main thread, which is the native scheduler with messages in place of an atomic
counter; GitHub Pages cannot send the headers `SharedArrayBuffer` needs for real threads.
Rendering is progressive by default: passes of 1, 1, 2, 4... samples per pixel, summed
per pixel in linear color on the page and gamma corrected for display, so a noisy full
frame appears almost at once and then refines. The page also switches between the three
scenes and turns the BVH on and off, with the rays/s figure to compare. For the teapot,
each worker fetches `teapot.obj` and copies it into its module's memory. Any control can
be set from the URL, so a view can be linked: `?scene=2&threads=8&spp=50`.
GitHub Actions builds the native renderer and the demo and publishes it to Pages on every
push to `main`.

```bash
web/build.sh                          # needs em++ on PATH
python3 -m http.server -d web/dist    # then open http://localhost:8000
```

## Known limits

* **Positions only from OBJ.** Texture coordinates, file normals and `.mtl` materials are
  ignored; a whole model gets one material.
* **No emissive materials.** Surfaces can be matte, metal or glass, but none give off
  light.
* **No light sources.** Illumination comes entirely from the sky gradient, which is why
  the scene reads as overcast.
* **Scene-fixed cameras.** Each scene has a look-at camera with a field of view, but
  there are no controls for it and no depth of field.
* **Median-split BVH.** Built by splitting at the median, not with a surface area
  heuristic, so the tree is balanced but its boxes are not the tightest they could be.
* **Row granularity.** Work is claimed a whole row at a time, so one expensive row still
  runs on a single thread.

## License

MIT. See [LICENSE](LICENSE).

## Author

**Abir Deol** · [abirdeol.tech](https://abirdeol.tech)
