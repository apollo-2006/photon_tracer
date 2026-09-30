#pragma once
// An image mapped onto a surface by texture coordinates.
#include "image_io.hpp"
#include "vec3.hpp"

#include <cmath>
#include <vector>

class image_texture {
public:
    // Image bytes are sRGB, so they are decoded to linear once here: shading
    // multiplies light by albedo, which only makes sense in linear color.
    explicit image_texture(const image8& img) : width(img.width), height(img.height), texels(img.rgb.size()) {
        float table[256];
        for (int v = 0; v < 256; ++v) {
            const float c = v / 255.0f;
            table[v] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (size_t k = 0; k < img.rgb.size(); ++k) texels[k] = table[img.rgb[k]];
    }

    // Bilinear lookup at (u, v), repeating outside [0, 1). v runs up the image,
    // as in OBJ files, while rows are stored top first.
    color sample(real u, real v) const {
        const real x = (u - std::floor(u)) * width - real(0.5);
        const real y = (1 - (v - std::floor(v))) * height - real(0.5);
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const real fx = x - x0, fy = y - y0;
        return texel(x0, y0) * ((1 - fx) * (1 - fy)) + texel(x0 + 1, y0) * (fx * (1 - fy)) +
               texel(x0, y0 + 1) * ((1 - fx) * fy) + texel(x0 + 1, y0 + 1) * (fx * fy);
    }

private:
    int width, height;
    std::vector<float> texels;  // Linear RGB

    color texel(int x, int y) const {
        x = ((x % width) + width) % width;
        y = ((y % height) + height) % height;
        const float* t = &texels[3 * (static_cast<size_t>(y) * width + x)];
        return color(t[0], t[1], t[2]);
    }
};
