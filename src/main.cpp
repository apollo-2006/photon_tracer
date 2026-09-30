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
    // to use fewer than every hardware thread, --width N for a smaller or larger
    // 16:9 image, --out PATH to write somewhere other than render.ppm, and
    // --seed N to render the same image every time.
    // By default the BVH is used for the field and the mesh: on the five-sphere scene
    // the ground sphere's box covers nearly every ray and the tree is overhead.
    scene_id scene = scene_id::materials;
    int bvh_flag = -1;
    std::string obj_path = "models/teapot.obj";
    int spp = 50;
    int threads_flag = 0;
    bool nee = true;
    bool denoise_flag = false;
    double turn_deg = 0;
    int image_width = 1920;
    std::string out_path = "render.ppm";
    uint64_t seed = (uint64_t(std::random_device{}()) << 32) ^ std::random_device{}();
    for (int a = 1; a < argc; ++a) {
        std::string arg = argv[a];
        if (arg == "--field") scene = scene_id::field;
        else if (arg == "--mesh") scene = scene_id::mesh;
        else if (arg == "--room") scene = scene_id::room;
        else if (arg == "--no-nee") nee = false;
        else if (arg == "--denoise") denoise_flag = true;
        else if (arg == "--turn" && a + 1 < argc) turn_deg = std::stod(argv[++a]);
        else if (arg == "--obj" && a + 1 < argc) { scene = scene_id::mesh; obj_path = argv[++a]; }
        else if (arg == "--bvh") bvh_flag = 1;
        else if (arg == "--no-bvh") bvh_flag = 0;
        else if (arg == "--spp" && a + 1 < argc) spp = std::stoi(argv[++a]);
        else if (arg == "--threads" && a + 1 < argc) threads_flag = std::stoi(argv[++a]);
        else if (arg == "--width" && a + 1 < argc) image_width = std::stoi(argv[++a]);
        else if (arg == "--out" && a + 1 < argc) out_path = argv[++a];
        else if (arg == "--seed" && a + 1 < argc) seed = std::stoull(argv[++a]);
        else {
            std::cerr << "usage: " << argv[0] << " [--field | --mesh | --obj PATH [--turn DEG] | --room] [--bvh|--no-bvh] [--no-nee] [--denoise] [--spp N]"
                      << " [--threads N] [--width N] [--out PATH] [--seed N]\n";
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
    const int image_height = static_cast<int>(image_width / aspect_ratio);
    const int samples_per_pixel = spp; // Anti-aliasing quality
    const int max_bounces = 10;

    // 2. Camera Abstraction

    // 3. World Composition
    // Material and texture files named by the OBJ are found next to it.
    const std::string obj_dir = obj_path.find('/') == std::string::npos ? "" : obj_path.substr(0, obj_path.find_last_of('/') + 1);
    const file_reader read_beside_obj = [&obj_dir](const std::string& name) {
        std::ifstream f(obj_dir + name, std::ios::binary);
        if (!f) {
            std::cerr << "Could not open " << obj_dir + name << "; skipping it.\n";
            return std::string();
        }
        std::ostringstream buf;
        buf << f.rdbuf();
        return buf.str();
    };
    geometry world;
    try {
        world = build_world(scene, use_bvh, obj_text, read_beside_obj, turn_deg);
    } catch (const std::exception& e) {
        std::cerr << "Failed to load " << obj_path << ": " << e.what() << '\n';
        return 1;
    }
    world.sample_lights = nee;

    // 4. Threading Setup. Linear color, converted for display when written.
    std::vector<float> image(3 * static_cast<size_t>(image_width) * image_height);
    // Albedo, normal and squared luminance per pixel, for --denoise.
    std::vector<float> aux(denoise_flag ? aux_floats * static_cast<size_t>(image_width) * image_height : 0);
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
            // j counts up from the bottom; image is stored top row first.
            const size_t row = static_cast<size_t>(image_height - 1 - j) * image_width;
            render_row_linear(world, cam, j, image_width, image_height, samples_per_pixel, max_bounces,
                              &image[3 * row], seed, 0, denoise_flag ? &aux[aux_floats * row] : nullptr);

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

    double denoise_s = 0;
    if (denoise_flag) {
        const auto start = std::chrono::steady_clock::now();
        const size_t n = static_cast<size_t>(image_width) * image_height;
        std::vector<float> albedo(3 * n), normal(3 * n), variance(n), denoised(3 * n);
        for (size_t p = 0; p < n; ++p) {
            const float* a = &aux[aux_floats * p];
            for (int c = 0; c < 3; ++c) { albedo[3 * p + c] = a[c]; normal[3 * p + c] = a[3 + c]; }
            // Variance of the mean: that of one sample over the sample count.
            const float mean = luminance(color(image[3 * p], image[3 * p + 1], image[3 * p + 2]));
            variance[p] = (a[6] - mean * mean) / samples_per_pixel;
        }
        denoise({image_width, image_height, image.data(), albedo.data(), normal.data(), variance.data()},
                denoised.data(), num_threads);
        image.swap(denoised);
        denoise_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    // 7. Output to File. Binary PPM (P6): the header, then the bytes as they
    // are. The ASCII form (P3) was four times the size and, once rendering got
    // fast, a fifth of the whole run. A path ending in .pfm gets the linear
    // floats instead, as a Portable Float Map, for the render tests: averaging
    // after gamma and clamping would bias noisy pixels.
    std::cerr << "\nWriting to " << out_path << "...\n";
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open " << out_path << " for writing.\n";
        return 1;
    }
    const bool pfm = out_path.size() >= 4 && out_path.compare(out_path.size() - 4, 4, ".pfm") == 0;
    if (pfm) {
        // -1: little-endian floats. PFM stores the bottom row first.
        out << "PF\n" << image_width << ' ' << image_height << "\n-1.0\n";
        for (int y = image_height - 1; y >= 0; --y)
            out.write(reinterpret_cast<const char*>(&image[3 * static_cast<size_t>(y) * image_width]),
                      static_cast<std::streamsize>(3 * sizeof(float) * image_width));
    } else {
        std::vector<uint8_t> bytes(image.size());
        for (size_t c = 0; c < image.size(); ++c) bytes[c] = to_display(image[c]);
        out << "P6\n" << image_width << ' ' << image_height << "\n255\n";
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        std::cerr << "Failed to write " << out_path << ".\n";
        return 1;
    }

    std::cerr << "Render Complete.\n";
    std::cerr << image_width << "x" << image_height << " at " << samples_per_pixel
              << " spp: rendered in " << render_s << " s, "
              << std::setprecision(3) << total_rays / render_s / 1e6 << std::setprecision(6) << "M rays/s ("
              << total_rays << " rays) on " << num_threads << " threads, "
              << (scene == scene_id::field ? "field" : scene == scene_id::mesh ? "mesh"
                  : scene == scene_id::room ? "room" : "materials") << " scene, "
              << (use_bvh ? "BVH" : "no BVH") << ", seed " << seed;
    if (denoise_flag) std::cerr << ", denoised in " << denoise_s << " s";
    std::cerr << '\n';
    return 0;
}
