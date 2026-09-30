// Decodes a PNG or PPM with include/image_io.hpp and writes the pixels as P6,
// for tests/image_test.py to compare with what it encoded.
#include "image_io.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: decode_image IN OUT.ppm\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    try {
        const image8 img = decode_image(bytes.str());
        std::ofstream out(argv[2], std::ios::binary);
        out << "P6\n" << img.width << ' ' << img.height << "\n255\n";
        out.write(reinterpret_cast<const char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    } catch (const std::exception& e) {
        std::cerr << argv[1] << ": " << e.what() << '\n';
        return 1;
    }
}
