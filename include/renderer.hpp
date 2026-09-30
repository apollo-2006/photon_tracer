#pragma once
// The per-pixel work, shared by the native renderer (src/main.cpp) and the
// browser build (web/tracer_web.cpp). Neither owns a copy of the tracing code.
#include "vec3.hpp"
#include "ray.hpp"
#include "geometry.hpp"
#include "obj.hpp"
#include "camera.hpp"
#include "denoise.hpp"

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

inline color background(const geometry& world, const ray& r) {
    if (!world.sky) return color(0, 0, 0);
    vec3 unit_direction = r.direction().normalize();
    real t = real(0.5) * (unit_direction.y() + 1);
    return color(1, 1, 1) * (1 - t) + color(0.5, 0.7, 1.0) * t;
}

// Next event estimation: light reaching a matte surface at rec straight from
// one of the scene's light spheres, divided by the albedo (the caller
// multiplies it back in). A bounce only finds a small light by luck; aiming a
// shadow ray at one finds it every time the path is not blocked.
//
// One light is picked at random, and a direction is drawn uniformly from the
// cone the light sphere fills as seen from rec, whose solid angle is
// 2 pi (1 - cos_max). The estimate is the Lambertian BRDF (albedo / pi) times
// the light's emission times the cosine at the surface, over the probability
// of that direction.
inline color direct_light(const geometry& world, const hit_record& rec) {
    const uint32_t n = static_cast<uint32_t>(world.lights.size());
    const uint32_t pick = std::min(static_cast<uint32_t>(random_real() * n), n - 1);
    const sphere& light = world.spheres[world.lights[pick]];

    const vec3 to_center = light.center - rec.p;
    const real dist2 = vec3::dot(to_center, to_center), r2 = light.radius * light.radius;
    if (dist2 <= r2) return color(0, 0, 0);  // Inside the light
    const real sin2_max = r2 / dist2;
    const real cos_max = std::sqrt(1 - sin2_max);
    // 1 - cos_max, without the cancellation of subtracting two numbers near 1
    // for a small or distant light.
    const real one_minus_cos_max = sin2_max / (1 + cos_max);

    // A direction in the cone, around w, the direction to the center.
    const vec3 w = to_center / std::sqrt(dist2);
    const vec3 a = std::fabs(w.x()) > real(0.9) ? vec3(0, 1, 0) : vec3(1, 0, 0);
    const vec3 u = cross(w, a).normalize(), v = cross(w, u);
    real s1, s2;
    sample_2d(1, s1, s2);
    const real cos_t = 1 - s1 * one_minus_cos_max;
    const real sin_t = std::sqrt(std::fmax(real(0), 1 - cos_t * cos_t));
    const real phi = 2 * real(M_PI) * s2;
    const vec3 dir = u * (std::cos(phi) * sin_t) + v * (std::sin(phi) * sin_t) + w * cos_t;

    const real cos_surface = vec3::dot(dir, rec.normal);
    if (cos_surface <= 0) return color(0, 0, 0);  // The light is behind the surface here

    const ray shadow(rec.p, dir);
    real t_light = aabb::inf();
    if (!light.intersect(shadow, real(0.001), t_light)) return color(0, 0, 0);  // Grazed the rim
    ++rays_traced;
    if (world.occluded(shadow, real(0.001), t_light * real(0.9999))) return color(0, 0, 0);

    hit_record at_light;
    light.fill(shadow, t_light, at_light);
    if (!at_light.front_face) return color(0, 0, 0);
    const real pdf = 1 / (2 * real(M_PI) * one_minus_cos_max);
    return light.mat->emission * (cos_surface / real(M_PI) / pdf * n);
}

// Follows one path for up to depth rays. throughput is how much of the light
// arriving along the current ray still reaches the camera: the product of the
// attenuations so far.
// What a path's first ray hit, for the denoiser (denoise.hpp): the surface's
// albedo (white for glass, lights and sky, which have none) and its normal
// (zero for sky).
struct first_hit {
    color albedo;
    vec3 normal;
};

// With_first says, at compile time, whether first is written: the plain path
// is the hottest code there is, and even a branch per bounce on a null
// pointer cost about 5%.
template <bool with_first = false>
inline color ray_color(ray r, const geometry& world, int depth, first_hit* first = nullptr) {
    color throughput(1, 1, 1), radiance(0, 0, 0);
    // Whether the last bounce was off a matte surface that sampled the lights
    // directly: a light this ray then hits was already counted there.
    bool sampled = false;
    for (int bounce = 0; bounce < depth; ++bounce) {
        ++rays_traced;
        set_bounce(bounce);

        hit_record rec;
        if (!world.hit(r, real(0.001), 1000, rec)) {
            if constexpr (with_first) if (bounce == 0) *first = {color(1, 1, 1), vec3(0, 0, 0)};
            return radiance + throughput * background(world, r);
        }

        if (rec.front_face && !(sampled && rec.sampled_light)) radiance = radiance + throughput * rec.mat->emission;

        ray scattered;
        color attenuation;
        const bool scatters = rec.mat->scatter(r, rec, attenuation, scattered);
        if constexpr (with_first)
            if (bounce == 0) *first = {scatters && !rec.mat->transmissive ? attenuation : color(1, 1, 1), rec.normal};
        if (!scatters) return radiance;
        // Not on the last bounce allowed: a light sample there would stand in
        // for one more ray than the path is allowed, which counted about 1%
        // more light than plain path tracing in the room scene.
        sampled = rec.mat->diffuse && world.sample_lights && !world.lights.empty() && bounce + 1 < depth;
        if (sampled) radiance = radiance + throughput * attenuation * direct_light(world, rec);
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
                if (random_real() >= p) return radiance;
                throughput = throughput / p;
            }
        }
    }
    return radiance;
}

enum class scene_id { materials = 0, field = 1, mesh = 2, room = 3 };

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

    // Fixed seed: the same field in every build and every worker. Each number
    // is drawn into its own variable, in order: C++ leaves the order of
    // function arguments unspecified, and color(u() * u(), ...) came out as a
    // different field under GCC (native) and clang (the WebAssembly demo). The
    // conversion to [0, 1) is spelled out for the same reason, since
    // std::uniform_real_distribution comes from a different standard library
    // in each build.
    std::mt19937 rng(7);
    auto u = [&rng] { return rng() * 0x1.0p-32; };
    // A 30 x 14 grid close to the camera, which sits only 0.5 above the ground,
    // so the far rows bunch up toward the horizon.
    for (int a = -15; a < 15; ++a) {
        for (int b = -15; b < -1; ++b) {
            const double jitter_x = u();
            const double jitter_z = u();
            point3 c(a * 0.2 + 0.12 * jitter_x, -0.44, b * 0.2 + 0.12 * jitter_z);
            vec3 d = c - point3(clamp(std::round(c.x()), -1.0, 1.0), -0.44, -1);
            if (vec3::dot(d, d) < 0.36) continue;  // Clear of the big three
            const double pick = u();
            const material* m;
            if (pick < 0.7) {
                double albedo[3];
                for (double& x : albedo) { x = u(); x *= u(); }
                m = world.own(std::make_shared<lambertian>(color(albedo[0], albedo[1], albedo[2])));
            } else if (pick < 0.9) {
                double albedo[3];
                for (double& x : albedo) x = 0.5 + 0.5 * u();
                const double fuzz = 0.3 * u();
                m = world.own(std::make_shared<metal>(color(albedo[0], albedo[1], albedo[2]), fuzz));
            } else {
                m = glass;
            }
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
    if (id == scene_id::room) return camera(point3(0, 0, 0.3), point3(0, -0.15, -3.0), 62.0);
    return camera();
}

// The mesh scene: a model read from OBJ text (the Utah teapot in the repo) in
// polished copper, between a glass and a matte sphere.
inline geometry make_mesh_scene(const std::string& obj_text, const file_reader& read = {}, double turn_deg = 0) {
    geometry world;
    auto ground = world.own(std::make_shared<lambertian>(color(0.8, 0.8, 0.0)));
    auto copper = world.own(std::make_shared<metal>(color(0.95, 0.64, 0.54), 0.05));

    world.large_spheres.emplace_back(point3(0, -100.5, -1), 100.0, ground);
    world.spheres.emplace_back(point3(-1.25, -0.2, -1.4), 0.3, world.own(std::make_shared<dielectric>(1.5)));
    world.spheres.emplace_back(point3( 1.25, -0.2, -1.4), 0.3, world.own(std::make_shared<lambertian>(color(0.1, 0.2, 0.5))));
    load_obj(world, obj_text, point3(0, -0.5, -1.2), 0.75, copper, read, turn_deg);
    return world;
}

// A parallelogram as two triangles: corner p, edges e1 and e2.
inline void add_quad(geometry& world, point3 p, vec3 e1, vec3 e2, const material* m) {
    world.triangles.emplace_back(p, p + e1, p + e1 + e2, m);
    world.triangles.emplace_back(p, p + e1 + e2, p + e2, m);
}

// The room scene: a closed box, red on the left and green on the right, lit
// only by a small sphere lamp under the ceiling, with glass, metal and matte
// spheres on the floor. No sky reaches in, so every bit of light comes from
// the lamp, which is the case next event estimation is for: a bounce finds a
// lamp this small only by luck.
inline geometry make_room_scene() {
    geometry world;
    world.sky = false;
    auto white = world.own(std::make_shared<lambertian>(color(0.73, 0.73, 0.73)));
    auto red   = world.own(std::make_shared<lambertian>(color(0.65, 0.05, 0.05)));
    auto green = world.own(std::make_shared<lambertian>(color(0.12, 0.45, 0.15)));
    auto lamp  = world.own(std::make_shared<diffuse_light>(color(30, 27, 22)));

    // x from -1.75 to 1.75, y from -1 to 1, z from -3.5 to 0.5; the camera
    // stands inside, near the front wall.
    const double x0 = -1.75, x1 = 1.75, y0 = -1, y1 = 1, z0 = -3.5, z1 = 0.5;
    add_quad(world, point3(x0, y0, z1), vec3(x1 - x0, 0, 0), vec3(0, 0, z0 - z1), white);  // Floor
    add_quad(world, point3(x0, y1, z1), vec3(x1 - x0, 0, 0), vec3(0, 0, z0 - z1), white);  // Ceiling
    add_quad(world, point3(x0, y0, z0), vec3(x1 - x0, 0, 0), vec3(0, y1 - y0, 0), white);  // Back
    add_quad(world, point3(x0, y0, z1), vec3(x1 - x0, 0, 0), vec3(0, y1 - y0, 0), white);  // Front
    add_quad(world, point3(x0, y0, z1), vec3(0, 0, z0 - z1), vec3(0, y1 - y0, 0), red);    // Left
    add_quad(world, point3(x1, y0, z1), vec3(0, 0, z0 - z1), vec3(0, y1 - y0, 0), green);  // Right

    world.spheres.emplace_back(point3(0, 0.72, -2.0), 0.14, lamp);
    world.spheres.emplace_back(point3(-0.8, -0.55, -2.3), 0.45, world.own(std::make_shared<dielectric>(1.5)));
    world.spheres.emplace_back(point3(0.85, -0.6, -1.8), 0.4, world.own(std::make_shared<metal>(color(0.9, 0.9, 0.9), 0.02)));
    world.spheres.emplace_back(point3(0.15, -0.75, -2.9), 0.25, world.own(std::make_shared<lambertian>(color(0.1, 0.2, 0.5))));
    return world;
}

// The scene as something to trace: the plain list, or a BVH over it. obj_text
// is only read by the mesh scene.
// read, if given, finds the files the OBJ refers to (see obj.hpp).
inline geometry build_world(scene_id id, bool use_bvh, const std::string& obj_text = "", const file_reader& read = {},
                            double turn_deg = 0) {
    geometry world = id == scene_id::mesh ? make_mesh_scene(obj_text, read, turn_deg)
                   : id == scene_id::room ? make_room_scene()
                                          : make_scene(id);
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
//
// If aux is not null, it gets aux_floats floats per pixel for the denoiser,
// also averaged over the samples: the first hit's albedo (3) and normal (3),
// and the mean squared luminance of the samples (1), which with the mean gives
// the pixel's variance.
constexpr int aux_floats = 7;

inline real luminance(const color& c) { return real(0.2126) * c.x() + real(0.7152) * c.y() + real(0.0722) * c.z(); }

template <bool with_aux>
inline void render_row_impl(const geometry& world, const camera& cam, int j, int width, int height,
                            int samples_per_pixel, int max_bounces, float* out, uint64_t seed, int first_sample,
                            float* aux) {
    seed_row(seed, j, first_sample);
    const real scale = real(1) / samples_per_pixel;
    for (int i = 0; i < width; ++i) {
        color pixel_color(0, 0, 0);
        color albedo(0, 0, 0);
        vec3 normal(0, 0, 0);
        real lum2 = 0;

        // Anti-Aliasing Loop: Shoot multiple rays with slight random offsets
        for (int s = 0; s < samples_per_pixel; ++s) {
            begin_sample(seed, i, j, static_cast<uint32_t>(first_sample + s));
            real du, dv;
            sample_2d(0, du, dv);
            real u = (i + du) / (width - 1);
            real v = (j + dv) / (height - 1);
            ray r = cam.get_ray(u, v);
            if constexpr (!with_aux) {
                pixel_color = pixel_color + ray_color(r, world, max_bounces);
            } else {
                first_hit first;
                const color c = ray_color<true>(r, world, max_bounces, &first);
                pixel_color = pixel_color + c;
                albedo = albedo + first.albedo;
                normal = normal + first.normal;
                lum2 += luminance(c) * luminance(c);
            }
        }
        out[3 * i]     = static_cast<float>(pixel_color.x() * scale);
        out[3 * i + 1] = static_cast<float>(pixel_color.y() * scale);
        out[3 * i + 2] = static_cast<float>(pixel_color.z() * scale);
        if constexpr (with_aux) {
            float* a = aux + aux_floats * i;
            for (int c = 0; c < 3; ++c) {
                a[c] = static_cast<float>(albedo.e[c] * scale);
                a[3 + c] = static_cast<float>(normal.e[c] * scale);
            }
            a[6] = static_cast<float>(lum2 * scale);
        }
    }
}

inline void render_row_linear(const geometry& world, const camera& cam, int j, int width, int height,
                              int samples_per_pixel, int max_bounces, float* out, uint64_t seed,
                              int first_sample = 0, float* aux = nullptr) {
    if (aux) render_row_impl<true>(world, cam, j, width, height, samples_per_pixel, max_bounces, out, seed, first_sample, aux);
    else render_row_impl<false>(world, cam, j, width, height, samples_per_pixel, max_bounces, out, seed, first_sample, aux);
}

// A linear color channel as a display byte: gamma 2.0, then quantized.
// Taking the square root is gamma 2.0, not 2.2: an approximation of sRGB that
// is close enough by eye and one instruction instead of a pow().
inline uint8_t to_display(float linear) {
    return static_cast<uint8_t>(256 * clamp(std::sqrt(linear), 0, real(0.999)));
}
