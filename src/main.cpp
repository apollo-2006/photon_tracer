#include "../include/vec3.hpp"
#include "../include/ray.hpp"
#include "../include/hittable.hpp"
#include "../include/sphere.hpp"
#include "../include/camera.hpp"

#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <memory>
#include <mutex>

// A utility class to hold multiple 3D objects in the scene
class hittable_list : public hittable {
public:
    std::vector<std::shared_ptr<hittable>> objects;

    hittable_list() {}
    void add(std::shared_ptr<hittable> object) { objects.push_back(object); }

    // Iterates through all objects to find which one the ray hits first
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

// Recursive function: Bounces the ray off objects until it hits the sky or runs out of energy
color ray_color(const ray& r, const hittable& world, int depth) {
    if (depth <= 0) return color(0,0,0); // Photon absorbed

    hit_record rec;
    // Check if the ray hits anything in the world (0.001 fixes shadow acne/floating point errors)
    if (world.hit(r, 0.001, 1000.0, rec)) {
        vec3 target = rec.p + rec.normal + vec3(0.5, 0.5, 0.5); // Simplified random scatter (matte)
        return ray_color(ray(rec.p, target - rec.p), world, depth - 1) * 0.5;
    }

    // Sky gradient background
    vec3 unit_direction = r.direction().normalize();
    double t = 0.5 * (unit_direction.y() + 1.0);
    return color(1.0, 1.0, 1.0) * (1.0 - t) + color(0.5, 0.7, 1.0) * t;
}

int main() {
    // 1. Image Settings
    const double aspect_ratio = 16.0 / 9.0;
    const int image_width = 1920;
    const int image_height = static_cast<int>(image_width / aspect_ratio);
    const int max_bounces = 10;

    // 2. Camera Abstraction
    camera cam;

    // 3. World Composition
    hittable_list world;
    world.add(std::make_shared<sphere>(point3(0, 0, -1), 0.5));           // Main sphere
    world.add(std::make_shared<sphere>(point3(0, -100.5, -1), 100.0));    // Giant sphere acting as the ground

    // 4. Threading Setup
    std::vector<color> image_buffer(image_width * image_height);
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; // Fallback

    std::vector<std::thread> threads;
    int rows_per_thread = image_height / num_threads;

    std::mutex progress_mutex;
    int rows_completed = 0;

    std::cerr << "Initiating Engine using " << num_threads << " hardware threads...\n";

    // 5. The Render Worker Lambda
    auto render_chunk = [&](int start_row, int end_row) {
        for (int j = start_row; j < end_row; ++j) {
            for (int i = 0; i < image_width; ++i) {
                double u = double(i) / (image_width - 1);
                double v = double(j) / (image_height - 1);

                ray r = cam.get_ray(u, v);
                color pixel_color = ray_color(r, world, max_bounces);

                // Buffer indices flow top-to-bottom, but rendering math is bottom-up.
                int buffer_index = (image_height - 1 - j) * image_width + i;
                image_buffer[buffer_index] = pixel_color;
            }

            // Thread-safe progress tracking
            std::lock_guard<std::mutex> lock(progress_mutex);
            rows_completed++;
            std::cerr << "\rScanlines completed: " << rows_completed << "/" << image_height << ' ' << std::flush;
        }
    };

    // 6. Dispatch Threads
    for (int t = 0; t < num_threads; ++t) {
        int start_row = t * rows_per_thread;
        // Ensure the last thread picks up any remaining rows due to division truncation
        int end_row = (t == num_threads - 1) ? image_height : (t + 1) * rows_per_thread;
        threads.emplace_back(render_chunk, start_row, end_row);
    }

    // 7. Await Thread Completion
    for (auto& thread : threads) {
        thread.join();
    }

    // 8. Output to File
    std::cerr << "\nWriting to render.ppm...\n";
    std::ofstream out("render.ppm");
    out << "P3\n" << image_width << ' ' << image_height << "\n255\n";

    for (const auto& pixel : image_buffer) {
        out << static_cast<int>(255.999 * pixel.x()) << ' '
            << static_cast<int>(255.999 * pixel.y()) << ' '
            << static_cast<int>(255.999 * pixel.z()) << '\n';
    }

    std::cerr << "Render Complete.\n";
    return 0;
}