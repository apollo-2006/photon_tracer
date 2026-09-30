// Browser bindings. Each Web Worker loads its own instance of this module and
// traces rows through the same render_row_linear() the native renderer uses.
#include <emscripten/emscripten.h>

#include <string>
#include <vector>

#include "renderer.hpp"

namespace {
geometry world = build_world(scene_id::materials, false);
camera cam;
std::vector<float> row;
std::string obj_text;  // The mesh scene's model, written in by the page
// One per render, from the page, so every worker draws from the same streams.
uint64_t seed = 0;
}

extern "C" {

// A buffer of len bytes for the page to copy an OBJ file into before it
// switches to the mesh scene.
EMSCRIPTEN_KEEPALIVE char* obj_buffer(int len) {
    obj_text.assign(static_cast<size_t>(len), '\0');
    return obj_text.data();
}

EMSCRIPTEN_KEEPALIVE void set_seed(double s) { seed = static_cast<uint64_t>(s); }

// Switch scene, and between the BVH and the plain list.
EMSCRIPTEN_KEEPALIVE void set_scene(int scene, int use_bvh) {
    const scene_id id = static_cast<scene_id>(scene);
    world = build_world(id, use_bvh != 0, obj_text);
    cam = make_camera(id);
}

// Linear RGB floats, the average of spp samples; the page accumulates passes
// and applies gamma itself.
// first_sample is how many samples earlier passes took of this row, so each
// pass draws new ones.
EMSCRIPTEN_KEEPALIVE float* trace_row(int j, int width, int height, int spp, int bounces, int first_sample) {
    row.resize(3 * static_cast<size_t>(width));
    render_row_linear(world, cam, j, width, height, spp, bounces, row.data(), seed, first_sample);
    return row.data();
}

// Rays traced since the last call.
EMSCRIPTEN_KEEPALIVE double take_rays() {
    const double n = static_cast<double>(rays_traced);
    rays_traced = 0;
    return n;
}

}  // extern "C"
