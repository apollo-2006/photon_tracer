#include "vec3.hpp"
#include "ray.hpp"
#include "hittable.hpp"
#include "sphere.hpp"
#include "camera.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <memory>
#include <mutex>

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

color ray_color(const ray& r, const hittable& world, int depth) {
    if (depth <= 0) return color(0,0,0);

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

int main() {
    // 1. Image Settings
    const double aspect_ratio = 16.0 / 9.0;
    const int image_width = 1920;
    const int image_height = static_cast<int>(image_width / aspect_ratio);
    const int samples_per_pixel = 50; // Anti-aliasing quality
    const int max_bounces = 10;

    // 2. Camera Abstraction
    camera cam;

    // 3. World Composition
    hittable_list world;
    world.add(std::make_shared<sphere>(point3(0, 0, -1), 0.5));
    world.add(std::make_shared<sphere>(point3(0, -100.5, -1), 100.0));

    // 4. Threading Setup
    std::vector<color> image_buffer(image_width * image_height);
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::vector<std::thread> threads;
    int rows_per_thread = image_height / num_threads;

    std::mutex progress_mutex;
    int rows_completed = 0;

    std::cerr << "Initiating Engine using " << num_threads << " hardware threads...\n";

    // 5. The Render Worker Lambda
    auto render_chunk = [&](int start_row, int end_row) {
        for (int j = start_row; j < end_row; ++j) {
            for (int i = 0; i < image_width; ++i) {
                color pixel_color(0, 0, 0);

                // Anti-Aliasing Loop: Shoot multiple rays with slight random offsets
                for (int s = 0; s < samples_per_pixel; ++s) {
                    double u = (i + random_double()) / (image_width - 1);
                    double v = (j + random_double()) / (image_height - 1);
                    ray r = cam.get_ray(u, v);
                    pixel_color = pixel_color + ray_color(r, world, max_bounces);
                }

                // Divide the color by the number of samples
                double scale = 1.0 / samples_per_pixel;

                // Gamma correction. Taking the square root is gamma 2.0, not 2.2 --
                // an approximation of sRGB that is close enough by eye and one
                // instruction instead of a pow().
                double r_gamma = std::sqrt(pixel_color.x() * scale);
                double g_gamma = std::sqrt(pixel_color.y() * scale);
                double b_gamma = std::sqrt(pixel_color.z() * scale);

                int buffer_index = (image_height - 1 - j) * image_width + i;
                image_buffer[buffer_index] = color(r_gamma, g_gamma, b_gamma);
            }

            std::lock_guard<std::mutex> lock(progress_mutex);
            rows_completed++;
            std::cerr << "\rScanlines completed: " << rows_completed << "/" << image_height << ' ' << std::flush;
        }
    };

    // 6. Dispatch Threads
    for (int t = 0; t < num_threads; ++t) {
        int start_row = t * rows_per_thread;
        int end_row = (t == num_threads - 1) ? image_height : (t + 1) * rows_per_thread;
        threads.emplace_back(render_chunk, start_row, end_row);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // 7. Output to File
    std::cerr << "\nWriting to render.ppm...\n";
    std::ofstream out("render.ppm");
    if (!out) {
        std::cerr << "Failed to open render.ppm for writing.\n";
        return 1;
    }
    out << "P3\n" << image_width << ' ' << image_height << "\n255\n";

    for (const auto& pixel : image_buffer) {
        out << static_cast<int>(256 * clamp(pixel.x(), 0.0, 0.999)) << ' '
            << static_cast<int>(256 * clamp(pixel.y(), 0.0, 0.999)) << ' '
            << static_cast<int>(256 * clamp(pixel.z(), 0.0, 0.999)) << '\n';
    }

    std::cerr << "Render Complete.\n";
    return 0;
}