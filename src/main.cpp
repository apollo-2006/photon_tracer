#include "renderer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    // Flags: --field for the 400-sphere scene, --mesh for the teapot (or
    // --obj PATH for another model in its place), --bvh or --no-bvh to force the
    // BVH on or off (for comparing the two), --spp N to change the sample count, --threads N
    // to use fewer than every hardware thread.
    // By default the BVH is used for the field and the mesh: on the five-sphere scene
    // the ground sphere's box covers nearly every ray and the tree is overhead.
    scene_id scene = scene_id::materials;
    int bvh_flag = -1;
    std::string obj_path = "models/teapot.obj";
    int spp = 50;
    int threads_flag = 0;
    for (int a = 1; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--field") scene = scene_id::field;
        else if (arg == "--mesh") scene = scene_id::mesh;
        else if (arg == "--obj" && a + 1 < argc) { scene = scene_id::mesh; obj_path = argv[++a]; }
        else if (arg == "--bvh") bvh_flag = 1;
        else if (arg == "--no-bvh") bvh_flag = 0;
        else if (arg == "--spp" && a + 1 < argc) spp = std::stoi(argv[++a]);
        else if (arg == "--threads" && a + 1 < argc) threads_flag = std::stoi(argv[++a]);
        else {
            std::cerr << "usage: " << argv[0] << " [--field | --mesh | --obj PATH] [--bvh|--no-bvh] [--spp N] [--threads N]\n";
            return 2;
        }
    }
    const camera cam = make_camera(scene);
    const bool use_bvh = bvh_flag < 0 ? scene != scene_id::materials : bvh_flag == 1;

    std::string obj_text;
    if (scene == scene_id::mesh) {
        std::ifstream obj(obj_path);
        if (!obj) {
            std::cerr << "Failed to open " << obj_path << ".\n";
            return 1;
        }
        std::ostringstream buf;
        buf << obj.rdbuf();
        obj_text = buf.str();
    }

    // 1. Image Settings
    const double aspect_ratio = 16.0 / 9.0;
    const int image_width = 1920;
    const int image_height = static_cast<int>(image_width / aspect_ratio);
    const int samples_per_pixel = spp; // Anti-aliasing quality
    const int max_bounces = 10;

    // 2. Camera Abstraction

    // 3. World Composition
    const std::shared_ptr<hittable> world = build_world(scene, use_bvh, obj_text);

    // 4. Threading Setup
    std::vector<uint8_t> image(3 * image_width * image_height);
    int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (threads_flag > 0) num_threads = threads_flag;

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
            render_row(*world, cam, j, image_width, image_height, samples_per_pixel, max_bounces,
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
              << std::setprecision(3) << total_rays / render_s / 1e6 << std::setprecision(6) << "M rays/s ("
              << total_rays << " rays) on " << num_threads << " threads, "
              << (scene == scene_id::field ? "field" : scene == scene_id::mesh ? "mesh" : "materials") << " scene, "
              << (use_bvh ? "BVH" : "no BVH") << '\n';
    return 0;
}
