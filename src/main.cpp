#include "renderer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

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
    const hittable_list world = make_scene();

    // 4. Threading Setup
    std::vector<uint8_t> image(3 * image_width * image_height);
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;

    std::vector<std::thread> threads;

    // Rows are claimed one at a time from a shared counter rather than split
    // into fixed bands. The top of the frame is sky, one miss per sample, and
    // the bottom is ground with bounces, so fixed bands left the sky threads
    // idle while the ground threads were still working.
    std::atomic<int> next_row{0};
    std::atomic<long long> total_rays{0};

    std::mutex progress_mutex;
    int rows_completed = 0;

    std::cerr << "Initiating Engine using " << num_threads << " hardware threads...\n";

    // 5. The Render Worker
    auto render_worker = [&]() {
        for (int j = next_row.fetch_add(1); j < image_height; j = next_row.fetch_add(1)) {
            // j counts up from the bottom; the file is written top row first.
            render_row(world, cam, j, image_width, image_height, samples_per_pixel, max_bounces,
                       &image[3 * (image_height - 1 - j) * image_width]);

            std::lock_guard<std::mutex> lock(progress_mutex);
            rows_completed++;
            std::cerr << "\rScanlines completed: " << rows_completed << "/" << image_height << ' ' << std::flush;
        }
        total_rays += rays_traced;
    };

    // 6. Dispatch Threads
    const auto render_start = std::chrono::steady_clock::now();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(render_worker);
    }

    for (auto& thread : threads) {
        thread.join();
    }
    const double render_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - render_start).count();

    // 7. Output to File
    std::cerr << "\nWriting to render.ppm...\n";
    std::ofstream out("render.ppm");
    if (!out) {
        std::cerr << "Failed to open render.ppm for writing.\n";
        return 1;
    }
    out << "P3\n" << image_width << ' ' << image_height << "\n255\n";

    for (size_t p = 0; p < image.size(); p += 3) {
        out << int(image[p]) << ' ' << int(image[p + 1]) << ' ' << int(image[p + 2]) << '\n';
    }

    std::cerr << "Render Complete.\n";
    std::cerr << image_width << "x" << image_height << " at " << samples_per_pixel
              << " spp: rendered in " << render_s << " s, "
              << static_cast<long long>(total_rays / render_s / 1e6) << "M rays/s ("
              << total_rays << " rays) on " << num_threads << " threads\n";
    return 0;
}
