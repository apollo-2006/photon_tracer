# photon_tracer
A CPU-based raytracer written from scratch in C++, built to explore computer graphics, vector mathematics, and physical rendering fundamentals. The engine calculates the intersection of light rays with 3D geometry to generate images entirely from scratch, with no external graphics libraries.

## Features
* **Core Raytracing Engine**: Implements foundational rendering mathematics including 3D vector operations (`vec3.hpp`), ray generation (`ray.hpp`), and camera positioning (`camera.hpp`).
* **Geometric Intersections**: Calculates precise ray-object intersections using a modular `hittable` interface, currently supporting accurate sphere rendering (`sphere.hpp`).
* **PPM Image Output**: Renders the final scene directly to a Portable Pixmap image file (`render.ppm`), handling raw image data manipulation without external graphics libraries.
* **Configurable Rendering**: Supports adjustable anti-aliasing and sampling (e.g., 50 rays per pixel) to balance performance and visual fidelity.

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

## Author
**Abir Deol**
