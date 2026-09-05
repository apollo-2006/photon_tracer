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
* **Multithreaded.** The image is split into horizontal bands, one per hardware thread.
  The RNG is `thread_local`, so the workers never contend on it; the only shared mutable
  state is the progress counter.
* **PPM output.** Written as plain ASCII P3 with no image library involved.

## Scene

Two spheres: a 0.5-radius sphere at the origin, and a 100-radius sphere below it acting
as the ground plane. The background is a vertical blue-to-white gradient interpolated on
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

## Known limits

* **Spheres only.** No planes, triangles, or meshes, so no model loading.
* **One material.** Every surface is a 50%-albedo diffuse; no metals, glass, refraction,
  or emissive surfaces.
* **No light sources.** Illumination comes entirely from the sky gradient, which is why
  the scene reads as overcast.
* **Fixed camera.** No position, orientation, field of view, or depth of field controls.
* **No acceleration structure.** Every ray tests every object, which is fine for two
  spheres and quadratic for a real scene. A BVH is the next thing this needs.
* **Static work split.** Bands are handed out evenly up front, so a thread that draws a
  cheap band finishes early and idles.

## Author
**Abir Deol**
