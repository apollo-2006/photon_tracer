// Browser bindings. Each Web Worker loads its own instance of this module and
// traces rows through the same render_row_linear() the native renderer uses.
#include <emscripten/emscripten.h>

#include <memory>
#include <vector>

#include "renderer.hpp"

namespace {
std::shared_ptr<hittable> world = build_world(scene_id::materials, false);
camera cam;
std::vector<float> row;
}

extern "C" {

// Switch scene, and between the BVH and the plain list.
EMSCRIPTEN_KEEPALIVE void set_scene(int scene, int use_bvh) {
    const scene_id id = static_cast<scene_id>(scene);
    world = build_world(id, use_bvh != 0);
    cam = make_camera(id);
}

// Linear RGB floats, the average of spp samples; the page accumulates passes
// and applies gamma itself.
EMSCRIPTEN_KEEPALIVE float* trace_row(int j, int width, int height, int spp, int bounces) {
    row.resize(3 * static_cast<size_t>(width));
    render_row_linear(*world, cam, j, width, height, spp, bounces, row.data());
    return row.data();
}

// Rays traced since the last call.
EMSCRIPTEN_KEEPALIVE double take_rays() {
    const double n = static_cast<double>(rays_traced);
    rays_traced = 0;
    return n;
}

}  // extern "C"
