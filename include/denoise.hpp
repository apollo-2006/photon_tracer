#pragma once
// A denoiser for low sample counts: an edge-avoiding a-trous wavelet filter
// (Dammertz et al. 2010), steered by each pixel's noise level as in SVGF
// (Schied et al. 2017).
//
// The color is first divided by the albedo of what each pixel's rays first
// hit, leaving the lighting, which is what is noisy; texture and material
// edges come back when the albedo is multiplied in again. The lighting is then
// blurred five times with a 5x5 kernel whose taps spread 1, 2, 4, 8 and 16
// pixels apart, so the filter reaches 61 pixels across for 125 taps a pixel.
// Each tap is weighted down where the normal turns, where the albedo changes,
// and where its brightness differs from the center by more than the center's
// noise explains, so edges and real shading survive while noise is averaged
// away. After each pass the variance is filtered along with the color, so
// later, wider passes blur less where the image has already settled.
//
// It only knows what the first ray hit: a reflection in a mirror or through
// glass has the mirror's normal and a white albedo, so detail seen in them is
// blurred along with their noise.
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

struct denoise_input {
    int width, height;
    const float* color;     // Linear RGB, 3 per pixel
    const float* albedo;    // First-hit albedo, 3 per pixel
    const float* normal;    // First-hit normal, averaged over samples, 3 per pixel
    const float* variance;  // Variance of each pixel's mean luminance, 1 per pixel
};

namespace denoise_detail {

constexpr int passes = 5;
constexpr float kernel[3] = {3.0f / 8, 1.0f / 4, 1.0f / 16};  // B3 spline, center outward
constexpr float sigma_luminance = 4;    // In standard deviations of the center's noise
constexpr float albedo_sigma2 = 0.01f;  // Squared albedo difference that halves a tap, roughly

inline float lum(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// Runs body(y0, y1) over row ranges on up to threads threads.
template <class Body>
void for_rows(int height, int threads, Body&& body) {
    if (threads <= 1) { body(0, height); return; }
    std::vector<std::thread> pool;
    const int step = (height + threads - 1) / threads;
    for (int y = 0; y < height; y += step) pool.emplace_back(body, y, std::min(height, y + step));
    for (auto& t : pool) t.join();
}

}  // namespace denoise_detail

// Writes the denoised image, linear RGB, to out (3 floats a pixel).
inline void denoise(const denoise_input& in, float* out, int threads = 1) {
    using namespace denoise_detail;
    const int w = in.width, h = in.height;
    const size_t n = static_cast<size_t>(w) * h;

    // Demodulate: lighting = color / albedo. Albedo near zero (black
    // surfaces) would blow noise up, so it is floored.
    std::vector<float> light(3 * n), light_next(3 * n), var(n), var_next(n), normal(3 * n);
    auto safe_albedo = [&](size_t p, int c) { return std::max(in.albedo[3 * p + c], 0.01f); };
    for (size_t p = 0; p < n; ++p) {
        for (int c = 0; c < 3; ++c) light[3 * p + c] = in.color[3 * p + c] / safe_albedo(p, c);
        const float a = std::max(lum(in.albedo + 3 * p), 0.01f);
        var[p] = std::max(in.variance[p], 0.0f) / (a * a);
        // Unit normals, so the center's weight against itself is exactly 1;
        // averaged over samples at an edge, they can be much shorter. Zero
        // stays zero: the sky.
        const float* s = in.normal + 3 * p;
        const float len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        for (int c = 0; c < 3; ++c) normal[3 * p + c] = len > 1e-6f ? s[c] / len : 0;
    }

    for (int pass = 0; pass < passes; ++pass) {
        const int step = 1 << pass;
        for_rows(h, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x) {
                    const size_t p = static_cast<size_t>(y) * w + x;
                    const float* np = &normal[3 * p];
                    const float* ap = in.albedo + 3 * p;
                    const bool p_sky = np[0] == 0 && np[1] == 0 && np[2] == 0;
                    const float lp = lum(&light[3 * p]);
                    // The center's noise, blurred over 3x3 as in SVGF so one
                    // unlucky estimate does not stop the filter cold.
                    float v = 0, vw = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int qx = x + dx, qy = y + dy;
                            if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                            const float k = (dx ? 0.5f : 1) * (dy ? 0.5f : 1);
                            v += k * var[static_cast<size_t>(qy) * w + qx];
                            vw += k;
                        }
                    const float scale = 1 / (sigma_luminance * std::sqrt(v / vw) + 1e-4f);

                    float sum[3] = {0, 0, 0}, sum_w = 0, sum_var = 0;
                    for (int dy = -2; dy <= 2; ++dy) {
                        const int qy = y + dy * step;
                        if (qy < 0 || qy >= h) continue;
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int qx = x + dx * step;
                            if (qx < 0 || qx >= w) continue;
                            const size_t q = static_cast<size_t>(qy) * w + qx;
                            const float* nq = &normal[3 * q];
                            const float* aq = in.albedo + 3 * q;
                            const bool q_sky = nq[0] == 0 && nq[1] == 0 && nq[2] == 0;
                            if (p_sky != q_sky) continue;  // Never blur sky into surface
                            float weight = kernel[std::abs(dx)] * kernel[std::abs(dy)];
                            if (!p_sky) {
                                // The cosine between the normals to the 64th
                                // power: a turn of 10 degrees keeps 38%, 20 keeps 2%.
                                float nd = std::max(np[0] * nq[0] + np[1] * nq[1] + np[2] * nq[2], 0.0f);
                                for (int k = 0; k < 6; ++k) nd *= nd;
                                weight *= nd;
                            }
                            // The albedo and brightness terms share one exp().
                            const float da0 = ap[0] - aq[0], da1 = ap[1] - aq[1], da2 = ap[2] - aq[2];
                            weight *= std::exp(-(da0 * da0 + da1 * da1 + da2 * da2) * (1 / albedo_sigma2) -
                                               std::fabs(lp - lum(&light[3 * q])) * scale);
                            for (int c = 0; c < 3; ++c) sum[c] += weight * light[3 * q + c];
                            sum_w += weight;
                            sum_var += weight * weight * var[q];
                        }
                    }
                    // The center tap always has weight, so sum_w > 0.
                    for (int c = 0; c < 3; ++c) light_next[3 * p + c] = sum[c] / sum_w;
                    var_next[p] = sum_var / (sum_w * sum_w);
                }
        });
        std::swap(light, light_next);
        std::swap(var, var_next);
    }

    for (size_t p = 0; p < n; ++p)
        for (int c = 0; c < 3; ++c) out[3 * p + c] = light[3 * p + c] * safe_albedo(p, c);
}
