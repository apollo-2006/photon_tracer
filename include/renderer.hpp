#pragma once
// The per-pixel work, shared by the native renderer (src/main.cpp) and the
// browser build (web/tracer_web.cpp). Neither owns a copy of the tracing code.
#include "vec3.hpp"
#include "ray.hpp"
#include "hittable.hpp"
#include "sphere.hpp"
#include "camera.hpp"

#include <cstdint>
#include <memory>
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
};

// Rays traced by this thread, primary and bounced. Summed after the render for
// the rays/s figure; thread_local so counting costs no synchronization.
inline thread_local long long rays_traced = 0;

inline color ray_color(const ray& r, const hittable& world, int depth) {
    if (depth <= 0) return color(0,0,0);
    ++rays_traced;

    hit_record rec;
    if (world.hit(r, 0.001, 1000.0, rec)) {
        // True Lambertian Diffuse Bounce
        point3 target = rec.p + rec.normal + random_in_unit_sphere();
        return ray_color(ray(rec.p, target - rec.p), world, depth - 1) * 0.5;
    }

    vec3 unit_direction = r.direction().normalize();
    double t = 0.5 * (unit_direction.y() + 1.0);
    return color(1.0, 1.0, 1.0) * (1.0 - t) + color(0.5, 0.7, 1.0) * t;
}

// The scene: a small sphere one unit in front of the camera on a huge one as ground.
inline hittable_list make_scene() {
    hittable_list world;
    world.add(std::make_shared<sphere>(point3(0, 0, -1), 0.5));
    world.add(std::make_shared<sphere>(point3(0, -100.5, -1), 100.0));
    return world;
}

// Trace one scanline, where j counts up from the bottom of the image, and write
// width RGB bytes to out: averaged samples, gamma 2.0, then quantized.
inline void render_row(const hittable& world, const camera& cam, int j, int width, int height,
                       int samples_per_pixel, int max_bounces, uint8_t* out) {
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

        // Gamma correction. Taking the square root is gamma 2.0, not 2.2: an
        // approximation of sRGB that is close enough by eye and one instruction
        // instead of a pow().
        out[3 * i]     = static_cast<uint8_t>(256 * clamp(std::sqrt(pixel_color.x() * scale), 0.0, 0.999));
        out[3 * i + 1] = static_cast<uint8_t>(256 * clamp(std::sqrt(pixel_color.y() * scale), 0.0, 0.999));
        out[3 * i + 2] = static_cast<uint8_t>(256 * clamp(std::sqrt(pixel_color.z() * scale), 0.0, 0.999));
    }
}
