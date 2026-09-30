#pragma once
// Writing a rendered image: binary PPM (P6), or, for a path ending in .pfm,
// the linear floats as a Portable Float Map, which the render tests read
// because averaging after gamma and clamping biases noisy pixels. P6 is the
// header then the bytes as they are; the ASCII form (P3) was four times the
// size and, once rendering got fast, a fifth of the whole run.
#include "renderer.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// linear: 3 floats a pixel, top row first. False if the file cannot be written.
inline bool write_image(const std::string& path, int width, int height, const float* linear) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const size_t n = 3 * static_cast<size_t>(width) * height;
    const bool pfm = path.size() >= 4 && path.compare(path.size() - 4, 4, ".pfm") == 0;
    if (pfm) {
        // -1: little-endian floats. PFM stores the bottom row first.
        out << "PF\n" << width << ' ' << height << "\n-1.0\n";
        for (int y = height - 1; y >= 0; --y)
            out.write(reinterpret_cast<const char*>(linear + 3 * static_cast<size_t>(y) * width),
                      static_cast<std::streamsize>(3 * sizeof(float) * width));
    } else {
        std::vector<uint8_t> bytes(n);
        for (size_t c = 0; c < n; ++c) bytes[c] = to_display(linear[c]);
        out << "P6\n" << width << ' ' << height << "\n255\n";
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(out);
}
