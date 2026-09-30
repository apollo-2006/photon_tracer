#pragma once
// Reading texture images without an image library: PNG (non-interlaced,
// 8 or 16 bits, gray, RGB, palette, with or without alpha) and PPM (P3, P6).
// PNG's pixels are zlib-compressed, so this includes a small inflate, written
// from RFC 1951 in the style of zlib's puff.c: slow next to zlib, but a
// 1024 x 1024 texture decodes in well under a second, once, at load time.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct image8 {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;  // 3 bytes a pixel, top row first
};

namespace image_detail {

// Inflate (RFC 1951) --------------------------------------------------------

struct bit_reader {
    const uint8_t* data;
    size_t size, pos = 0;
    uint32_t bits = 0;
    int count = 0;

    int bit() {
        if (count == 0) {
            if (pos >= size) throw std::runtime_error("inflate: input ended early");
            bits = data[pos++];
            count = 8;
        }
        const int b = bits & 1;
        bits >>= 1;
        --count;
        return b;
    }
    // n bits, least significant first, as deflate stores everything but Huffman codes.
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint32_t>(bit()) << i;
        return v;
    }
    void align() { count = 0; }
};

// A canonical Huffman code: how many codes of each length, and the symbols
// in code order. Decoding walks the lengths one bit at a time.
struct huffman {
    uint16_t count[16] = {};
    std::vector<uint16_t> symbol;

    void build(const uint8_t* lengths, int n) {
        for (auto& c : count) c = 0;
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        uint16_t offset[16] = {};
        for (int len = 1; len < 15; ++len) offset[len + 1] = offset[len] + count[len];
        symbol.assign(n, 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offset[lengths[i]]++] = static_cast<uint16_t>(i);
    }

    int decode(bit_reader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= in.bit();
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: bad Huffman code");
    }
};

inline void inflate_block(bit_reader& in, std::vector<uint8_t>& out, const huffman& lit, const huffman& dist) {
    static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                          35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                          3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                           6145, 8193, 12289, 16385, 24577};
    static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (true) {
        int sym = lit.decode(in);
        if (sym < 256) { out.push_back(static_cast<uint8_t>(sym)); continue; }
        if (sym == 256) return;
        sym -= 257;
        if (sym >= 29) throw std::runtime_error("inflate: bad length symbol");
        const size_t len = len_base[sym] + in.get(len_extra[sym]);
        const int d = dist.decode(in);
        if (d >= 30) throw std::runtime_error("inflate: bad distance symbol");
        const size_t back = dist_base[d] + in.get(dist_extra[d]);
        if (back > out.size()) throw std::runtime_error("inflate: distance before the start");
        // Byte by byte: a copy may overlap what it is writing.
        for (size_t k = 0; k < len; ++k) out.push_back(out[out.size() - back]);
    }
}

// Decompresses a zlib stream (RFC 1950: a two-byte header, deflate data, and
// a checksum this does not verify).
inline std::vector<uint8_t> inflate_zlib(const uint8_t* data, size_t size) {
    if (size < 2 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0)
        throw std::runtime_error("zlib: bad header");
    bit_reader in{data + 2, size - 2};
    std::vector<uint8_t> out;
    int last;
    do {
        last = in.bit();
        const int type = static_cast<int>(in.get(2));
        if (type == 0) {  // Stored
            in.align();
            if (in.pos + 4 > in.size) throw std::runtime_error("inflate: stored block cut short");
            const size_t len = in.data[in.pos] | (in.data[in.pos + 1] << 8);
            in.pos += 4;
            if (in.pos + len > in.size) throw std::runtime_error("inflate: stored block cut short");
            out.insert(out.end(), in.data + in.pos, in.data + in.pos + len);
            in.pos += len;
        } else if (type == 1) {  // Fixed Huffman codes
            uint8_t lengths[288];
            for (int i = 0; i < 144; ++i) lengths[i] = 8;
            for (int i = 144; i < 256; ++i) lengths[i] = 9;
            for (int i = 256; i < 280; ++i) lengths[i] = 7;
            for (int i = 280; i < 288; ++i) lengths[i] = 8;
            huffman lit, dist;
            lit.build(lengths, 288);
            for (int i = 0; i < 30; ++i) lengths[i] = 5;
            dist.build(lengths, 30);
            inflate_block(in, out, lit, dist);
        } else if (type == 2) {  // Dynamic Huffman codes, sent first, themselves Huffman coded
            const int nlen = static_cast<int>(in.get(5)) + 257, ndist = static_cast<int>(in.get(5)) + 1;
            const int ncode = static_cast<int>(in.get(4)) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(in.get(3));
            huffman code_lengths;
            code_lengths.build(lengths, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int sym = code_lengths.decode(in);
                if (sym < 16) { lengths[i++] = static_cast<uint8_t>(sym); continue; }
                uint8_t value = 0;
                int repeat;
                if (sym == 16) {
                    if (i == 0) throw std::runtime_error("inflate: repeat with nothing before it");
                    value = lengths[i - 1];
                    repeat = 3 + static_cast<int>(in.get(2));
                } else if (sym == 17) {
                    repeat = 3 + static_cast<int>(in.get(3));
                } else {
                    repeat = 11 + static_cast<int>(in.get(7));
                }
                if (i + repeat > nlen + ndist) throw std::runtime_error("inflate: too many code lengths");
                while (repeat--) lengths[i++] = value;
            }
            huffman lit, dist;
            lit.build(lengths, nlen);
            dist.build(lengths + nlen, ndist);
            inflate_block(in, out, lit, dist);
        } else {
            throw std::runtime_error("inflate: bad block type");
        }
    } while (!last);
    return out;
}

// PNG ------------------------------------------------------------------------

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

inline image8 decode_png(const std::string& file) {
    const auto* d = reinterpret_cast<const uint8_t*>(file.data());
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (file.size() < 8 || !std::equal(signature, signature + 8, d)) throw std::runtime_error("png: not a PNG");
    image8 img;
    int depth = 0, type = 0;
    std::vector<uint8_t> idat, palette;
    for (size_t pos = 8; pos + 12 <= file.size();) {
        const uint32_t len = be32(d + pos);
        const std::string tag(file, pos + 4, 4);
        const uint8_t* body = d + pos + 8;
        if (pos + 12 + len > file.size()) throw std::runtime_error("png: chunk runs past the end");
        if (tag == "IHDR") {
            img.width = static_cast<int>(be32(body));
            img.height = static_cast<int>(be32(body + 4));
            depth = body[8];
            type = body[9];
            if (body[12] != 0) throw std::runtime_error("png: interlaced images are not supported");
            if (depth != 8 && depth != 16) throw std::runtime_error("png: only 8 and 16 bit images are supported");
        } else if (tag == "PLTE") {
            palette.assign(body, body + len);
        } else if (tag == "IDAT") {
            idat.insert(idat.end(), body, body + len);
        } else if (tag == "IEND") {
            break;
        }
        pos += 12 + len;
    }
    static const int channels_of[7] = {1, 0, 3, 1, 2, 0, 4};
    if (type > 6 || channels_of[type] == 0) throw std::runtime_error("png: unknown color type");
    if (type == 3 && depth != 8) throw std::runtime_error("png: only 8 bit palettes are supported");
    const int channels = channels_of[type];
    const size_t bpp = static_cast<size_t>(channels) * depth / 8;  // Bytes a pixel
    const size_t stride = bpp * img.width;
    std::vector<uint8_t> raw = inflate_zlib(idat.data(), idat.size());
    if (raw.size() < (stride + 1) * img.height) throw std::runtime_error("png: image data cut short");

    // Undo each row's filter, which predicted each byte from its neighbors.
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    img.rgb.resize(3 * static_cast<size_t>(img.width) * img.height);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = &raw[y * (stride + 1)];
        const int filter = row[0];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= bpp ? cur[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
            int pred = 0;
            switch (filter) {
                case 0: pred = 0; break;
                case 1: pred = a; break;
                case 2: pred = b; break;
                case 3: pred = (a + b) / 2; break;
                case 4: pred = paeth(a, b, c); break;
                default: throw std::runtime_error("png: bad filter");
            }
            cur[x] = static_cast<uint8_t>(row[1 + x] + pred);
        }
        for (int x = 0; x < img.width; ++x) {
            // The high byte of each sample, for 16 bit images.
            auto sample = [&](int ch) { return cur[(static_cast<size_t>(x) * channels + ch) * depth / 8]; };
            uint8_t* out = &img.rgb[3 * (static_cast<size_t>(y) * img.width + x)];
            if (type == 3) {
                const size_t k = 3 * static_cast<size_t>(cur[x]);
                if (k + 2 >= palette.size()) throw std::runtime_error("png: palette index out of range");
                out[0] = palette[k]; out[1] = palette[k + 1]; out[2] = palette[k + 2];
            } else if (channels >= 3) {  // RGB, RGBA; alpha is dropped
                out[0] = sample(0); out[1] = sample(1); out[2] = sample(2);
            } else {  // Gray, gray + alpha
                out[0] = out[1] = out[2] = sample(0);
            }
        }
        std::swap(prev, cur);
    }
    return img;
}

// PPM ------------------------------------------------------------------------

inline image8 decode_ppm(const std::string& file) {
    std::istringstream in(file);
    std::string magic;
    in >> magic;
    auto next = [&]() {  // The next number in the header, skipping # comments
        while (in >> std::ws && in.peek() == '#') in.ignore(1 << 20, '\n');
        long v;
        if (!(in >> v)) throw std::runtime_error("ppm: bad header");
        return v;
    };
    if (magic != "P3" && magic != "P6") throw std::runtime_error("ppm: only P3 and P6 are supported");
    image8 img;
    img.width = static_cast<int>(next());
    img.height = static_cast<int>(next());
    const long max = next();
    if (max <= 0 || max > 255) throw std::runtime_error("ppm: only 8 bit images are supported");
    const size_t n = 3 * static_cast<size_t>(img.width) * img.height;
    img.rgb.resize(n);
    if (magic == "P6") {
        in.get();  // The single whitespace byte after the header
        in.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(n));
        if (static_cast<size_t>(in.gcount()) != n) throw std::runtime_error("ppm: pixel data cut short");
    } else {
        for (auto& v : img.rgb) v = static_cast<uint8_t>(next());
    }
    if (max != 255)
        for (auto& v : img.rgb) v = static_cast<uint8_t>(v * 255 / max);
    return img;
}

}  // namespace image_detail

// Decodes a PNG or PPM file's bytes, by their first bytes rather than a name.
inline image8 decode_image(const std::string& file) {
    if (file.size() >= 2 && file[0] == 'P' && (file[1] == '3' || file[1] == '6')) return image_detail::decode_ppm(file);
    return image_detail::decode_png(file);
}
