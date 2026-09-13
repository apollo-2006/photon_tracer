// Browser bindings. Each Web Worker loads its own instance of this module and
// traces rows through the same render_row() the native renderer uses.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <vector>

#include "renderer.hpp"

namespace {
const hittable_list world = make_scene();
const camera cam;
std::vector<uint8_t> row;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE uint8_t* trace_row(int j, int width, int height, int spp, int bounces) {
    row.resize(3 * static_cast<size_t>(width));
    render_row(world, cam, j, width, height, spp, bounces, row.data());
    return row.data();
}

// Rays traced since the last call.
EMSCRIPTEN_KEEPALIVE double take_rays() {
    const double n = static_cast<double>(rays_traced);
    rays_traced = 0;
    return n;
}

}  // extern "C"
