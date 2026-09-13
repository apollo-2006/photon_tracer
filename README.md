# photon_tracer
A CPU-based raytracer written from scratch in C++, built to explore computer graphics, vector mathematics, and physical rendering fundamentals. The engine calculates the intersection of light rays with 3D geometry to generate images entirely from scratch, with no external graphics libraries.

## How it works

* **Rays, not triangles.** Nothing is projected or rasterized. For every pixel the
  renderer casts a ray out through the viewport and asks the world what it hit, which is
  why shadows and bounced light fall out of the algorithm instead of being added on top.
* **Analytic sphere intersection.** `sphere.hpp` solves the ray-sphere quadratic
  directly and returns the nearer root inside the valid `t` range. The `hittable`
  interface (`hittable.hpp`) keeps the intersection test behind a virtual call, so the
  world is just a list of things that know how to be hit.
* **Diffuse bounce.** On a hit, a new ray is fired toward a random point in the unit
  sphere above the surface and its result is halved, which is Lambertian scattering with
  a 50% albedo. Recursion is capped at 10 bounces so a ray trapped between surfaces
  terminates.
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

Two spheres: a 0.5-radius sphere at `(0, 0, -1)`, one unit in front of the camera, and a
100-radius sphere below it acting as the ground plane. The background is a vertical blue-to-white gradient interpolated on
the ray direction.

## Build & Run
```bash
# Clone the repository
git clone https://github.com/apollo-2006/photon_tracer.git
cd photon_tracer

# Compile the project
make

# Run the raytracer (outputs to render.ppm)
./photon_tracer
```

Renders 1920x1080 at 50 samples per pixel. The output is a ~24 MB ASCII PPM; most image
viewers open it directly, or convert it with `magick render.ppm render.png`.

Resolution, sample count and bounce depth are constants at the top of `src/main.cpp`.

## Performance

The renderer prints its own timing when it finishes. Ryzen 9 5900XT (16 cores, 32
threads), g++ 16 `-O3`, 1920x1080, 50 samples per pixel, up to 10 bounces, best of three:

| | |
|---|---|
| render | **0.51 s** |
| rays traced | 184.6M (103.7M primary, the rest bounces) |
| throughput | **~360M rays/s** |
| whole run, including the 24 MB PPM write | 0.67 s |

Handing out rows from a shared counter instead of fixed bands took the whole run from
0.98 s to 0.67 s on the same machine, with identical output statistics. CPU time went up
(13.3 s to 15.9 s) because threads that used to finish their band of sky and exit now
keep working, which is the point: parallel speedup went from about 14x to 24x.

## Known limits

* **Spheres only.** No planes, triangles, or meshes, so no model loading.
* **One material.** Every surface is a 50%-albedo diffuse; no metals, glass, refraction,
  or emissive surfaces.
* **No light sources.** Illumination comes entirely from the sky gradient, which is why
  the scene reads as overcast.
* **Fixed camera.** No position, orientation, field of view, or depth of field controls.
* **No acceleration structure.** Every ray tests every object, so cost grows linearly with
  object count per ray. Fine for two spheres, hopeless for a mesh. A BVH is the next thing
  this needs.
* **Row granularity.** Work is claimed a whole row at a time, so one expensive row still
  runs on a single thread.

## Author
**Abir Deol**
