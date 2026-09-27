#pragma once
// The per-pixel work, shared by the native renderer (src/main.cpp) and the
// browser build (web/tracer_web.cpp). Neither owns a copy of the tracing code.
#include "vec3.hpp"
#include "ray.hpp"
#include "hittable.hpp"
#include "sphere.hpp"
#include "material.hpp"
#include "bvh.hpp"
#include "camera.hpp"

#include <cstdint>
#include <memory>
#include <random>
#include <vector>

class hittable_list : public hittable {
public:
    std::vector<std::shared_ptr<hittable>> objects;

    hittable_list() {}
    void add(std::shared_ptr<hittable> object) { objects.push_back(object); }

    virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const override {
        hit_record temp_rec;
        bool hit_anything = false;
        double closest_so_far = t_max;

        for (const auto& object : objects) {
            if (object->hit(r, t_min, closest_so_far, temp_rec)) {
                hit_anything = true;
                closest_so_far = temp_rec.t;
                rec = temp_rec;
            }
        }
        return hit_anything;
    }

    aabb bounding_box() const override {
        aabb box = objects[0]->bounding_box();
        for (size_t i = 1; i < objects.size(); ++i)
            box = aabb::surrounding(box, objects[i]->bounding_box());
        return box;
    }
};

// Rays traced by this thread, primary and bounced. Summed after the render for
// the rays/s figure; thread_local so counting costs no synchronization.
inline thread_local long long rays_traced = 0;

inline color ray_color(const ray& r, const hittable& world, int depth) {
    if (depth <= 0) return color(0,0,0);
    ++rays_traced;

    hit_record rec;
    if (world.hit(r, 0.001, 1000.0, rec)) {
        ray scattered;
        color attenuation;
        if (rec.mat->scatter(r, rec, attenuation, scattered))
            return attenuation * ray_color(scattered, world, depth - 1);
        return color(0,0,0);
    }

    vec3 unit_direction = r.direction().normalize();
    double t = 0.5 * (unit_direction.y() + 1.0);
    return color(1.0, 1.0, 1.0) * (1.0 - t) + color(0.5, 0.7, 1.0) * t;
}

enum class scene_id { materials = 0, field = 1 };

// The materials scene: matte, glass and metal spheres one unit in front of the
// camera, on a huge matte sphere as ground. The glass one is hollow: a second
// sphere with a negative radius flips its normals inward, making a thin shell.
// The field scene adds about 400 small random spheres around them, which is where a
// linear scan through the list gets slow and the BVH pays off.
inline hittable_list make_scene(scene_id id = scene_id::materials) {
    auto ground = std::make_shared<lambertian>(color(0.8, 0.8, 0.0));
    auto matte  = std::make_shared<lambertian>(color(0.1, 0.2, 0.5));
    auto glass  = std::make_shared<dielectric>(1.5);
    auto gold   = std::make_shared<metal>(color(0.8, 0.6, 0.2), 0.1);

    hittable_list world;
    world.add(std::make_shared<sphere>(point3( 0, -100.5, -1), 100.0, ground));
    world.add(std::make_shared<sphere>(point3( 0,    0.0, -1),   0.5, matte));
    world.add(std::make_shared<sphere>(point3(-1,    0.0, -1),   0.5, glass));
    world.add(std::make_shared<sphere>(point3(-1,    0.0, -1),  -0.4, glass));
    world.add(std::make_shared<sphere>(point3( 1,    0.0, -1),   0.5, gold));
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
            std::shared_ptr<material> m;
            if (pick < 0.7)      m = std::make_shared<lambertian>(color(u(rng) * u(rng), u(rng) * u(rng), u(rng) * u(rng)));
            else if (pick < 0.9) m = std::make_shared<metal>(color(0.5 + 0.5 * u(rng), 0.5 + 0.5 * u(rng), 0.5 + 0.5 * u(rng)), 0.3 * u(rng));
            else                 m = glass;
            world.add(std::make_shared<sphere>(c, 0.06, m));
        }
    }
    return world;
}

// The field is seen from above, so the small spheres spread out instead of
// bunching up at the horizon behind the big three.
inline camera make_camera(scene_id id) {
    if (id == scene_id::field) return camera(point3(0, 1.0, 1.2), point3(0, -0.3, -1.2), 55.0);
    return camera();
}

// The scene as something to trace: the plain list, or a BVH over it.
inline std::shared_ptr<hittable> build_world(scene_id id, bool use_bvh) {
    auto list = std::make_shared<hittable_list>(make_scene(id));
    if (!use_bvh) return list;
    return std::make_shared<bvh_node>(list->objects, 0, list->objects.size());
}

// Trace one scanline, where j counts up from the bottom of the image, and write
// width linear RGB floats to out: the average of samples_per_pixel samples.
// The browser demo accumulates these over progressive passes.
inline void render_row_linear(const hittable& world, const camera& cam, int j, int width, int height,
                              int samples_per_pixel, int max_bounces, float* out) {
    const double scale = 1.0 / samples_per_pixel;
    for (int i = 0; i < width; ++i) {
        color pixel_color(0, 0, 0);

        // Anti-Aliasing Loop: Shoot multiple rays with slight random offsets
        for (int s = 0; s < samples_per_pixel; ++s) {
            double u = (i + random_double()) / (width - 1);
            double v = (j + random_double()) / (height - 1);
            ray r = cam.get_ray(u, v);
            pixel_color = pixel_color + ray_color(r, world, max_bounces);
        }
        out[3 * i]     = static_cast<float>(pixel_color.x() * scale);
        out[3 * i + 1] = static_cast<float>(pixel_color.y() * scale);
        out[3 * i + 2] = static_cast<float>(pixel_color.z() * scale);
    }
}

// The same scanline as width RGB bytes: gamma 2.0, then quantized.
inline void render_row(const hittable& world, const camera& cam, int j, int width, int height,
                       int samples_per_pixel, int max_bounces, uint8_t* out) {
    thread_local std::vector<float> linear;
    linear.resize(3 * static_cast<size_t>(width));
    render_row_linear(world, cam, j, width, height, samples_per_pixel, max_bounces, linear.data());

    // Gamma correction. Taking the square root is gamma 2.0, not 2.2: an
    // approximation of sRGB that is close enough by eye and one instruction
    // instead of a pow().
    for (int c = 0; c < 3 * width; ++c)
        out[c] = static_cast<uint8_t>(256 * clamp(std::sqrt(static_cast<double>(linear[c])), 0.0, 0.999));
}
