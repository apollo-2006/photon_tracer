#pragma once
// The per-pixel work, shared by the native renderer (src/main.cpp) and the
// browser build (web/tracer_web.cpp). Neither owns a copy of the tracing code.
#include "vec3.hpp"
#include "ray.hpp"
#include "geometry.hpp"
#include "obj.hpp"
#include "camera.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

// Rays traced by this thread, primary and bounced. Summed after the render for
// the rays/s figure; thread_local so counting costs no synchronization.
inline thread_local long long rays_traced = 0;

// Bounces every path gets before Russian roulette may end it.
constexpr int roulette_after = 3;

// Follows one path for up to depth rays. throughput is how much of the light
// arriving along the current ray still reaches the camera: the product of the
// attenuations so far.
inline color ray_color(ray r, const geometry& world, int depth) {
    color throughput(1, 1, 1);
    for (int bounce = 0; bounce < depth; ++bounce) {
        ++rays_traced;

        hit_record rec;
        if (!world.hit(r, real(0.001), 1000, rec)) {
            vec3 unit_direction = r.direction().normalize();
            real t = real(0.5) * (unit_direction.y() + 1);
            return throughput * (color(1, 1, 1) * (1 - t) + color(0.5, 0.7, 1.0) * t);
        }

        ray scattered;
        color attenuation;
        if (!rec.mat->scatter(r, rec, attenuation, scattered)) return color(0, 0, 0);
        throughput = throughput * attenuation;
        r = scattered;

        // Russian roulette: a path that can only carry a little light on keeps
        // going with probability p, its brightest channel, and survivors are
        // scaled by 1/p. The image comes out the same on average, but paths
        // dimmed by several matte bounces stop instead of tracing to depth.
        // Glass has throughput 1 and always survives.
        if (bounce + 1 >= roulette_after) {
            real p = std::fmax(throughput.x(), std::fmax(throughput.y(), throughput.z()));
            if (p < 1) {
                if (random_real() >= p) return color(0, 0, 0);
                throughput = throughput / p;
            }
        }
    }
    return color(0, 0, 0);
}

enum class scene_id { materials = 0, field = 1, mesh = 2 };

// The materials scene: matte, glass and metal spheres one unit in front of the
// camera, on a huge matte sphere as ground. The glass one is hollow: a second
// sphere with a negative radius flips its normals inward, making a thin shell.
// The field scene adds about 400 small random spheres around them, which is where a
// linear scan through the list gets slow and the BVH pays off.
inline geometry make_scene(scene_id id = scene_id::materials) {
    geometry world;
    auto ground = world.own(std::make_shared<lambertian>(color(0.8, 0.8, 0.0)));
    auto matte  = world.own(std::make_shared<lambertian>(color(0.1, 0.2, 0.5)));
    auto glass  = world.own(std::make_shared<dielectric>(1.5));
    auto gold   = world.own(std::make_shared<metal>(color(0.8, 0.6, 0.2), 0.1));

    world.large_spheres.emplace_back(point3( 0, -100.5, -1), 100.0, ground);
    world.spheres.emplace_back(point3( 0,    0.0, -1),   0.5, matte);
    world.spheres.emplace_back(point3(-1,    0.0, -1),   0.5, glass);
    world.spheres.emplace_back(point3(-1,    0.0, -1),  -0.4, glass);
    world.spheres.emplace_back(point3( 1,    0.0, -1),   0.5, gold);
    if (id != scene_id::field) return world;

    // Fixed seed: the same field in every build and every worker.
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    // A 30 x 14 grid close to the camera, which sits only 0.5 above the ground,
    // so the far rows bunch up toward the horizon.
    for (int a = -15; a < 15; ++a) {
        for (int b = -15; b < -1; ++b) {
            point3 c(a * 0.2 + 0.12 * u(rng), -0.44, b * 0.2 + 0.12 * u(rng));
            vec3 d = c - point3(clamp(std::round(c.x()), -1.0, 1.0), -0.44, -1);
            if (vec3::dot(d, d) < 0.36) continue;  // Clear of the big three
            double pick = u(rng);
            const material* m;
            if (pick < 0.7)      m = world.own(std::make_shared<lambertian>(color(u(rng) * u(rng), u(rng) * u(rng), u(rng) * u(rng))));
            else if (pick < 0.9) m = world.own(std::make_shared<metal>(color(0.5 + 0.5 * u(rng), 0.5 + 0.5 * u(rng), 0.5 + 0.5 * u(rng)), 0.3 * u(rng)));
            else                 m = glass;
            world.spheres.emplace_back(c, 0.06, m);
        }
    }
    return world;
}

// The field is seen from above, so the small spheres spread out instead of
// bunching up at the horizon behind the big three.
inline camera make_camera(scene_id id) {
    if (id == scene_id::field) return camera(point3(0, 1.0, 1.2), point3(0, -0.3, -1.2), 55.0);
    if (id == scene_id::mesh) return camera(point3(0, 0.5, 1.2), point3(0, -0.15, -1.2), 45.0);
    return camera();
}

// The mesh scene: a model read from OBJ text (the Utah teapot in the repo) in
// polished copper, between a glass and a matte sphere.
inline geometry make_mesh_scene(const std::string& obj_text) {
    geometry world;
    auto ground = world.own(std::make_shared<lambertian>(color(0.8, 0.8, 0.0)));
    auto copper = world.own(std::make_shared<metal>(color(0.95, 0.64, 0.54), 0.05));

    world.large_spheres.emplace_back(point3(0, -100.5, -1), 100.0, ground);
    world.spheres.emplace_back(point3(-1.25, -0.2, -1.4), 0.3, world.own(std::make_shared<dielectric>(1.5)));
    world.spheres.emplace_back(point3( 1.25, -0.2, -1.4), 0.3, world.own(std::make_shared<lambertian>(color(0.1, 0.2, 0.5))));
    world.triangles = load_obj(obj_text, point3(0, -0.5, -1.2), 0.75, copper);
    return world;
}

// The scene as something to trace: the plain list, or a BVH over it. obj_text
// is only read by the mesh scene.
inline geometry build_world(scene_id id, bool use_bvh, const std::string& obj_text = "") {
    geometry world = id == scene_id::mesh ? make_mesh_scene(obj_text) : make_scene(id);
    world.build(use_bvh);
    return world;
}

// Seeds this thread's RNG for row j's samples from first_sample on. Every
// (seed, row, first sample) gets its own stream, so a render with a given seed
// comes out the same however its rows are shared between threads, and each
// progressive pass over a row draws new samples rather than repeating one.
inline void seed_row(uint64_t seed, int j, int first_sample) {
    thread_rng.seed(seed ^ ((uint64_t(uint32_t(j)) << 32) | uint32_t(first_sample)));
}

// Trace one scanline, where j counts up from the bottom of the image, and write
// width linear RGB floats to out: the average of samples_per_pixel samples,
// numbered from first_sample. The browser demo accumulates these over
// progressive passes.
inline void render_row_linear(const geometry& world, const camera& cam, int j, int width, int height,
                              int samples_per_pixel, int max_bounces, float* out, uint64_t seed,
                              int first_sample = 0) {
    seed_row(seed, j, first_sample);
    const real scale = real(1) / samples_per_pixel;
    for (int i = 0; i < width; ++i) {
        color pixel_color(0, 0, 0);

        // Anti-Aliasing Loop: Shoot multiple rays with slight random offsets
        for (int s = 0; s < samples_per_pixel; ++s) {
            real u = (i + random_real()) / (width - 1);
            real v = (j + random_real()) / (height - 1);
            ray r = cam.get_ray(u, v);
            pixel_color = pixel_color + ray_color(r, world, max_bounces);
        }
        out[3 * i]     = static_cast<float>(pixel_color.x() * scale);
        out[3 * i + 1] = static_cast<float>(pixel_color.y() * scale);
        out[3 * i + 2] = static_cast<float>(pixel_color.z() * scale);
    }
}

// The same scanline as width RGB bytes: gamma 2.0, then quantized.
inline void render_row(const geometry& world, const camera& cam, int j, int width, int height,
                       int samples_per_pixel, int max_bounces, uint8_t* out, uint64_t seed) {
    thread_local std::vector<float> linear;
    linear.resize(3 * static_cast<size_t>(width));
    render_row_linear(world, cam, j, width, height, samples_per_pixel, max_bounces, linear.data(), seed);

    // Gamma correction. Taking the square root is gamma 2.0, not 2.2: an
    // approximation of sRGB that is close enough by eye and one instruction
    // instead of a pow().
    for (int c = 0; c < 3 * width; ++c)
        out[c] = static_cast<uint8_t>(256 * clamp(std::sqrt(linear[c]), 0, real(0.999)));
}
