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
  directly and returns the nearer root inside the valid `t` range. Spheres and triangles
  are plain structs kept in one flat array per type (`geometry.hpp`), not objects behind
  a virtual call, and finding the hit is split from shading it: the point and normal are
  only worked out once, for the nearest hit.
* **Triangles and OBJ meshes.** `triangle.hpp` intersects with Moller-Trumbore, which
  solves for the hit distance and barycentric coordinates in one step. `obj.hpp` reads
  positions, texture coordinates, normals and faces from Wavefront OBJ text (polygons
  are split into fans) and scales the model into place. Where the file gives no normals,
  it computes smooth vertex normals by summing the area-weighted normals of the faces
  around each vertex, so a low-poly model shades smoothly. Which side a ray hit still
  comes from the true face normal.
* **Materials from MTL files, and textures.** An OBJ's `.mtl` files map onto the four
  materials: an emissive `Ke` makes a light, a dissolve below 1 or a glass illumination
  model makes glass with index `Ni`, `illum 3` makes metal tinted by `Ks` with a blur from
  the shininess `Ns`, and anything else is matte, colored by `Kd` times its `map_Kd`
  texture. Textures are sampled bilinearly in linear color. They may be PNG or PPM, read
  by `image_io.hpp` without an image library: a small inflate written from RFC 1951 and a
  PNG reader covering gray, RGB, palette and alpha images at 8 or 16 bits.
* **Materials.** Each sphere carries a material (`material.hpp`) that decides how a
  ray scatters and how much of each color channel survives:
  * *Lambertian:* scatters toward the normal plus a random unit vector, a
    cosine-weighted bounce, tinted by its albedo.
  * *Metal:* mirror reflection, blurred by a `fuzz` factor from 0 (mirror) to 1.
  * *Dielectric:* refracts by Snell's law, reflects under total internal reflection,
    and otherwise chooses between the two with Schlick's approximation. A sphere with a
    negative radius flips its normals inward, which makes a hollow glass shell.
  * *Light:* gives off a color from its front face and scatters nothing.

  A path is capped at 10 bounces so a ray trapped between surfaces terminates. From the
  third bounce on, Russian roulette ends paths that can only carry a little light: a path
  continues with probability equal to its brightest remaining channel, and survivors are
  scaled up to match, so the image is the same on average with about 13% fewer rays.
* **Lights and next event estimation.** A bounce only reaches a small light by luck, so
  at every matte surface the tracer also aims a shadow ray at one of the scene's light
  spheres, picked at random: at a direction drawn uniformly from the cone the sphere
  fills as seen from that point. If nothing blocks it, the light's emission is added,
  weighted by the Lambertian BRDF, the cosine at the surface and the probability of that
  direction. A light the next bounce happens to hit is then not counted again. In the
  room scene at 64 samples per pixel, this turns an image that is mostly noise into a
  clean one (`--no-nee` switches it off to compare), and with enough samples both
  converge to the same image to within 0.1%.
* **BVH.** `bvh.hpp` builds a binary tree of axis-aligned bounding boxes over the scene
  with the surface area heuristic: centroids are sorted into 16 bins per axis and each
  node splits where the expected cost of tracing through its two halves is lowest. That
  binary tree is then collapsed into a 4-wide one, whose nodes keep their four child boxes
  axis by axis, so one SIMD slab test (`simd4.hpp`: SSE natively, SIMD128 in
  WebAssembly) checks all four at once. A ray walks the flat node array with a small
  stack, nearest child first, skipping any box farther than the nearest hit so far. The
  ground sphere stays outside the tree: its box contains the whole scene, so every ray
  would enter it anyway and it would widen every box above it.
* **Instancing, a two-level BVH.** A mesh can be placed any number of times: each mesh
  has its own tree in its own space, and each instance is a transform, a box in a
  top-level tree, and optionally a material of its own. A ray that reaches an instance
  is moved into the mesh's space (the same `t` holds in both, since the transform is
  affine) and continues down the mesh's tree; a hit's normal comes back by the inverse
  transpose. It is the bottom-level/top-level split of Vulkan ray tracing and DXR.
  Instances have a top-level tree of their own rather than joining the main one, since
  checking every leaf for instances slowed scenes without any by 5-7%.
* **Single precision.** Geometry and color are `float` (`real` in `vec3.hpp`), except
  the ground sphere's intersection, which is `double`: in `float`, `|oc|^2 - r^2` for a
  sphere of radius 100 loses the few thousandths that separate a bounce from the surface
  it left, and the ground came out slightly darker.
* **Anti-aliasing by supersampling.** 50 rays per pixel, each jittered by a sub-pixel
  offset, averaged. This is what removes the stair-stepping on sphere edges.
* **Well-spread samples.** The pixel offset, the direction of each matte bounce and the
  point aimed at on a light come, for the first two bounces, from shuffled Owen-scrambled
  Sobol points (`sampler.hpp`, after Burley 2020) rather than independent random
  numbers, so each new sample lands where earlier ones left room. Scrambling is per pixel
  and per dimension pair, and the sequence is indexed by each pixel's sample number, so
  progressive passes continue it. At 16 samples per pixel it cuts error against a
  converged image by about 17% on the materials and teapot scenes for 8-10% more time,
  about 1.3x less time for the same error. The room gains little (3-4%): most of its noise
  is caustics under the glass, which only bounces that find the lamp can light.
* **Denoising.** `--denoise` (and the demo's denoise switch, on by default) filters the
  image with an edge-avoiding a-trous wavelet filter steered by each pixel's noise, as in
  SVGF (`denoise.hpp`). Alongside its color, each pixel records the albedo and normal of
  what its rays first hit and its mean squared luminance. The filter divides the albedo
  out, blurs the remaining lighting over five passes of a 5x5 kernel spreading from 1 to
  16 pixels, weights each tap down where normals turn, albedo changes, or brightness
  differs by more than the pixel's noise explains, then multiplies the albedo back in. On
  the room at 16 samples per pixel it cuts error against a converged image from 16.6 to
  6.1 display levels, below the 11.5 of 64 undenoised samples; the gain shrinks as samples
  grow, since the filter's own blur starts to count. It knows only what the first ray
  hit, so what a mirror or glass shows is smoothed along with its noise.
* **Gamma correction.** Output is square-rooted before writing, an approximation of sRGB
  that keeps midtones from looking too dark.
* **Multithreaded.** One worker per hardware thread, each claiming the next unrendered row
  from a shared atomic counter. Rows are not split into fixed bands up front, because the
  top of the frame is sky (one miss per sample) and the bottom is ground (several
  bounces), so fixed bands leave the sky threads idle. The RNG (xoshiro256+) and the ray
  counter are `thread_local`, so workers never contend on them.
* **PPM output.** Written as binary P6, a short text header and then the pixel bytes,
  with no image library involved.
* **Reproducible renders.** Each row's random numbers are seeded from the render's seed,
  the row and the sample number, so the same `--seed` gives the same image, byte for
  byte, however many threads render it.

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

`--obj models/spot/spot.obj --turn 150` puts Keenan Crane's cow Spot (public domain,
5,856 triangles) in the teapot's place, textured through its MTL file, turned to face the
camera.

`--crowd` is a grid of 20 x 20 instanced teapots, 2.5 million triangles, each turned,
sized and colored at random, on a wide ground; `--crowd N` makes it N x N, `--obj PATH`
makes it of another model, and `--flatten` writes every copy's triangles out into one
tree instead of instancing, for comparison. `models/fetch.sh` downloads the Stanford bunny
(69,451 triangles), which is not redistributed here: `--crowd 40 --obj
models/stanford-bunny.obj` is 1,600 bunnies, 111 million triangles in 95 MB, built in
0.09 s and rendered at 960x540 and 32 spp in half a second.

`--room` is a closed box, red on the left and green on the right, lit only by a small
sphere lamp under the ceiling, with glass, metal and matte spheres on the floor. No sky
reaches in, so all of its light comes through next event estimation or, for caustics
under the glass, through bounces that find the lamp.

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
./photon_tracer --room              # a closed room lit by one small lamp
./photon_tracer --crowd             # 400 instanced teapots; --crowd N for N x N, --flatten to compare
models/fetch.sh && ./photon_tracer --crowd 40 --obj models/stanford-bunny.obj  # 1,600 bunnies
./photon_tracer --room --no-nee     # the same without sampling the lamp directly
./photon_tracer --obj model.obj     # your own model in the teapot's place, with its .mtl materials
./photon_tracer --obj models/spot/spot.obj --turn 150  # a textured cow, turned to face the camera
./photon_tracer --spp 10            # fewer samples per pixel
./photon_tracer --threads 8         # fewer than every hardware thread
./photon_tracer --width 480         # a smaller 16:9 image
./photon_tracer --seed 7 --out a.ppm  # the same image every time, written to a.ppm
./photon_tracer --out a.pfm         # linear floats (Portable Float Map) instead of display bytes
./photon_tracer --room --spp 16 --denoise  # few samples, then the denoiser
make test                           # unit tests, then render regression tests (~3 CPU-minutes)
```

Renders 1920x1080 at 50 samples per pixel. The output is a 6 MB binary PPM; most image
viewers open it directly, or convert it with `magick render.ppm render.png`.

The BVH is on by default for the field and the mesh and off for the five-sphere scene, where
four small spheres are cheaper to test than a tree is to walk; `--bvh` and
`--no-bvh` force it either way. Resolution and bounce depth are constants at the top of `src/main.cpp`.
The tracing itself (`ray_color`, the scenes, and `render_row`) lives in
`include/renderer.hpp`, shared with the web build.

## Performance

`bench/native.sh` times every scene natively, best of three with a fixed seed so each run
traces the same rays, and prints a table; `bench/wasm.sh` does the same for the
WebAssembly build on one thread under Node (it needs `em++` and `node`).

The renderer prints its own timing when it finishes. Ryzen 9 5900XT (16 cores, 32
threads), g++ 16 `-O3`, 1920x1080, 50 samples per pixel, up to 10 bounces, best of three:

| | |
|---|---|
| render | **0.49 s** |
| rays traced | 184.6M (103.7M primary, the rest bounces) |
| throughput | **~375M rays/s** |
| whole run, including the PPM write (then 24 MB of ASCII P3) | 0.64 s |

Handing out rows from a shared counter instead of fixed bands took the whole run from
0.98 s to 0.67 s on the same machine, with identical output statistics. CPU time went up
(13.3 s to 15.9 s) because threads that used to finish their band of sky and exit now
keep working, which is the point: parallel speedup went from about 14x to 24x.

The browser figures that were here (1.15 s at 100 samples per pixel) are replaced by
the ones under *Flat BVH, float and a faster RNG* below.

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
bounce further before they escape, so the materials scene traces more rays per pixel. It
also predates the SAH BVH and moving the ground out of the tree, below.

### Faster tracing

Same machine, 1920x1080, 50 samples per pixel (4 for the teapot), 32 threads, up to 10
bounces, best of three, measured on the same day. "Before" is the renderer with the
pointer-based median-split BVH, `double` and `mt19937`. The first round replaced those
with a flat SAH BVH, `float` and xoshiro256+; the second added the 4-wide SIMD BVH,
Russian roulette and an inlined RNG.

| native | before | first round | second round | |
|---|---|---|---|---|
| materials, list | 1.46 s | 0.90 s | **0.68 s** | 2.1x |
| materials, BVH | 1.86 s | 1.15 s | 0.91 s | 2.0x |
| field, BVH | 4.77 s | 1.42 s | **0.96 s** | 5.0x |
| teapot, BVH | 0.30 s | 0.080 s | **0.063 s** | 4.8x |

Where it came from, one change at a time:

* **RNG.** `mt19937` through `uniform_real_distribution` was about 9% of a profile;
  xoshiro256+ took the materials scene from 1.46 s to 1.09 s. Declaring its per-thread
  state so that `random_real()` inlines, without a check that the thread's copy was
  constructed on every call, took it from 0.90 s to 0.77 s.
* **Flat SAH BVH, ground outside it.** Walking the tree was 74% of the field's profile.
  The field went from 4.40 s to 1.44 s and the teapot from 0.28 s to 0.078 s.
* **`float`.** 5-10% natively, more in WebAssembly.
* **4-wide SIMD BVH.** Together with the RNG change, the field went from 1.42 s to
  1.15 s. In WebAssembly (single-threaded, Node) it took the field from 0.47 s to 0.30 s
  and the teapot from 0.128 s to 0.094 s.
* **Russian roulette** from the third bounce: 13% fewer rays, and 6-17% less time (least on the teapot, whose copper reflects most of the light and keeps paths bright).
  Starting at the second bounce saved only 5% more and visibly added noise.

Renders match the old ones to within the noise between two runs of the old one, per pixel
and in mean brightness.

Instancing against writing every copy out (`--flatten`), for the 400-teapot crowd at
960x540 and 32 spp:

| crowd | built in | peak memory | render |
|---|---|---|---|
| instanced | 7.5 ms | 14 MB | 0.52 s |
| flattened | 1.5 s | 631 MB | 0.43 s |

One tree over every triangle is tighter than 400 overlapping instance boxes, so the
flattened crowd renders 17% faster, but at 45 times the memory and 200 times the build
time; 1,600 bunnies flattened would need about 14 GB.

In the browser demo, Chrome, 32 Web Workers, same settings, progressive:

| browser | before | first round | second round | |
|---|---|---|---|---|
| materials, list | 3.07 s | 1.09 s | **1.07 s** | 2.9x |
| materials, BVH | 3.69 s | 1.33 s | 1.24 s | 3.0x |
| field, BVH | 6.65 s | 1.88 s | **1.34 s** | 5.0x |
| teapot, BVH | 5.57 s | 1.34 s | **1.09 s** | 5.1x |

Much of the first round's browser gain came from the page, not the tracing: with one row
in flight per worker, workers sat idle for 78% of a progressive render waiting for the
main thread to send the next row, and the main thread spent about a second drawing each
returned row to the canvas separately. The page now keeps two rows in flight per worker
and draws once per animation frame. The "before" and first-round columns were measured
with the window in the background, where the browser skips animation frames (the old
page drew every row regardless); the second round was measured in front, drawing
included. With progressive rendering off, workers are now busy 91% of the materials
render, so the browser is limited by WebAssembly's tracing speed rather than the page.
`-msimd128 -flto` in `web/build.sh` are worth 10-20%, and SIMD128 is what the 4-wide
BVH's box test runs on.

The page now also turns the BVH off by default for the five-sphere scene, as the native
renderer does, which makes the first render a visitor sees 1.07 s instead of 1.24 s.

## Tests

`make test` first runs two unit tests: `tests/obj_test.cpp` feeds the OBJ and MTL reader
small files (quads, negative indices, file normals, texture coordinates, every material
kind, a texture beside the MTL), and `tests/image_test.py` encodes PNGs of every supported
kind with Python's zlib, covering all five row filters and stored, fixed and dynamic
deflate blocks, and checks that `image_io.hpp` decodes them to the same pixels.

A path traced image is noisy, so a new render never matches a reference byte for byte,
and the noise differs across the frame: sky is exact, glass is not. `tests/render_test.py`
compares statistics instead. `tests/reference.json` holds, for every 20x20 block of each
scene at 960x540 and 200 samples per pixel, the mean linear color and how much that mean
varies between seeds, measured from 16 renders. `make test` renders each scene once with a
seed the reference did not use and fails if any block, row of blocks, or the whole image
is further from the reference than that noise explains. It also checks that one seed
gives identical bytes on one thread and on three. Colors are compared in linear light,
from the renderer's PFM output, because averaging after gamma and clamping biases noisy
pixels.

It is sensitive enough to catch the precision bug from moving to `float`, which
darkened the ground by about 0.02 of a display level on average: with it put back, the
whole-image z-scores are -9 to -13 against a limit of 5, while six seeds of correct
code stay within +/-2.6. It also found that the field scene came out differently under
GCC and clang. The crowd is checked twice, instanced and flattened against the same
reference, which tests the instance transforms: with instance normals left in object
space, it fails by z-scores in the thousands. After an intended change to how scenes look, rebuild the reference with
`tests/render_test.py reference`, which takes under a minute on 32 threads.

CI runs the tests on every push and pull request, and before each deploy of the demo,
and writes both benchmarks to the run's summary.

## Web demo

`web/tracer_web.cpp` exposes `render_row_linear()` and a scene/BVH switch to JavaScript and `web/build.sh` compiles it
with Emscripten. The page runs one module instance per Web Worker and hands rows out
from the main thread, which is the native scheduler with messages in place of an atomic
counter; GitHub Pages cannot send the headers `SharedArrayBuffer` needs for real threads.
Rendering is progressive by default: passes of 1, 1, 2, 4... samples per pixel, summed
per pixel in linear color on the page and gamma corrected for display, so a noisy full
frame appears almost at once and then refines. Each worker has two rows in flight, so
it never waits on the page for its next one, and the canvas is drawn once per animation
frame rather than once per row. With denoising on, the workers also return each row's
albedo, normal and squared luminance, and after each progressive pass a separate worker
(`denoise.js`) filters the whole frame, which then replaces the noisy one; at 960x540 that
takes about 0.7 s in WebAssembly on one thread, so it trails the render slightly. The page also switches between the three
scenes (the crowd included) and turns the BVH on and off, with the rays/s figure to compare. For the teapot,
each worker fetches `teapot.obj` and copies it into its module's memory. Any control can
be set from the URL, so a view can be linked: `?scene=2&threads=8&spp=50`.
GitHub Actions builds the native renderer and the demo and publishes it to Pages on every
push to `main`.

```bash
web/build.sh                          # needs em++ on PATH
python3 -m http.server -d web/dist    # then open http://localhost:8000
```

## Known limits

* **Only diffuse textures.** From MTL, only `map_Kd` is read: no bump, normal, specular or
  alpha maps, and PNG textures must not be interlaced. JPEG is not supported. The browser
  demo loads no MTL files at all.
* **Only spheres are sampled as lights.** An emissive triangle (a light made from a mesh)
  still lights the scene, but only through bounces that happen to hit it, so it is noisy.
  And light is sampled only from matte surfaces: caustics through glass, and light off
  metal, still depend on luck.
* **Scene-fixed cameras.** Each scene has a look-at camera with a field of view, but
  there are no controls for it and no depth of field.
* **Row granularity.** Work is claimed a whole row at a time, so one expensive row still
  runs on a single thread.

## License

MIT. See [LICENSE](LICENSE).

## Author

**Abir Deol** · [abirdeol.tech](https://abirdeol.tech)
