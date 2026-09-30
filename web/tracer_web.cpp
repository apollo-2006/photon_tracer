// Browser bindings. Each Web Worker loads its own instance of this module and
// traces rows through the same render_row_linear() the native renderer uses.
#include <emscripten/emscripten.h>

#include <string>
#include <vector>

#include "renderer.hpp"

namespace {
geometry world = build_world(scene_id::materials, false);
camera cam;
std::vector<float> row, row_aux_data;
// The denoiser's inputs and output, sized by denoise_buffers().
std::vector<float> dn_color, dn_albedo, dn_normal, dn_variance, dn_out;
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
// With want_aux, row_aux() then holds the denoiser's guide data for the row.
EMSCRIPTEN_KEEPALIVE float* trace_row(int j, int width, int height, int spp, int bounces, int first_sample,
                                      int want_aux) {
    row.resize(3 * static_cast<size_t>(width));
    row_aux_data.resize(want_aux ? aux_floats * static_cast<size_t>(width) : 0);
    render_row_linear(world, cam, j, width, height, spp, bounces, row.data(), seed, first_sample,
                      want_aux ? row_aux_data.data() : nullptr);
    return row.data();
}

// aux_floats per pixel: albedo (3), normal (3), mean squared luminance (1).
EMSCRIPTEN_KEEPALIVE float* row_aux() { return row_aux_data.data(); }

// Sizes the denoiser's buffers for a width x height frame. The page then
// writes into denoise_buffer(0..3): color, albedo, normal (3 floats a pixel)
// and variance (1), and calls denoise_run().
EMSCRIPTEN_KEEPALIVE void denoise_buffers(int width, int height) {
    const size_t n = static_cast<size_t>(width) * height;
    dn_color.resize(3 * n);
    dn_albedo.resize(3 * n);
    dn_normal.resize(3 * n);
    dn_variance.resize(n);
    dn_out.resize(3 * n);
}

EMSCRIPTEN_KEEPALIVE float* denoise_buffer(int which) {
    switch (which) {
        case 0: return dn_color.data();
        case 1: return dn_albedo.data();
        case 2: return dn_normal.data();
        default: return dn_variance.data();
    }
}

// The denoised frame, linear RGB.
EMSCRIPTEN_KEEPALIVE float* denoise_run(int width, int height) {
    denoise({width, height, dn_color.data(), dn_albedo.data(), dn_normal.data(), dn_variance.data()}, dn_out.data());
    return dn_out.data();
}

// Rays traced since the last call.
EMSCRIPTEN_KEEPALIVE double take_rays() {
    const double n = static_cast<double>(rays_traced);
    rays_traced = 0;
    return n;
}

}  // extern "C"
