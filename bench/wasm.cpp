// WebAssembly throughput on one thread, as one of the demo's workers sees it:
// a 480x270 frame of each scene, best of three, with a fixed seed. Built and
// run under Node by bench/wasm.sh.
#include "renderer.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

int main() {
    std::ifstream f("models/teapot.obj");
    std::ostringstream obj;
    obj << f.rdbuf();
    const int w = 480, h = 270;
    struct run { scene_id id; bool bvh; int spp; const char* name; };
    const run runs[] = {{scene_id::materials, false, 10, "materials"},
                        {scene_id::materials, true, 10, "materials, BVH"},
                        {scene_id::field, true, 10, "field"},
                        {scene_id::mesh, true, 4, "teapot, 4 spp"},
                        {scene_id::room, true, 4, "room, 4 spp"},
                        {scene_id::crowd, true, 4, "crowd, 4 spp"}};
    std::vector<float> row(3 * w);
    std::printf("| scene | best of 3 | rays/s |\n|---|---|---|\n");
    for (const run& r : runs) {
        const geometry world = build_world(r.id, r.bvh, obj.str());
        const camera cam = make_camera(r.id);
        double best = 1e9;
        long long rays = 0;
        for (int rep = 0; rep < 3; ++rep) {
            rays_traced = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (int j = 0; j < h; ++j) render_row_linear(world, cam, j, w, h, r.spp, 10, row.data(), 1);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (s < best) { best = s; rays = rays_traced; }
        }
        std::printf("| %s | %.3f s | %.1fM |\n", r.name, best, rays / best / 1e6);
    }
}
