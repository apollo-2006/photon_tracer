#pragma once
// Well-spread sample points for the first few dimensions of each path.
//
// Independent random numbers clump and leave gaps, so after n samples a pixel
// has error that falls like 1/sqrt(n). A low-discrepancy sequence places each
// new sample where earlier ones left room, which converges faster wherever the
// integrand is smooth: the pixel footprint, a matte bounce, a point on a
// light. This is shuffled, Owen-scrambled Sobol (Burley, "Practical
// Hash-based Owen Scrambling", JCGT 2020): the first two Sobol dimensions,
// scrambled per pixel and per dimension pair by a hash, with the sample index
// shuffled per dimension pair too, so pairs used together in one path are not
// correlated with each other. It is indexed by the pixel's global sample
// number, so a progressive pass continues the sequence rather than restarting
// it.
//
// Dimensions are laid out per bounce: pair 0 is the pixel position, and bounce
// k uses pairs 1 + 2k (the scattered direction) and 2 + 2k (the light sample).
// Past qmc_bounces, and for everything else (glass's reflect-or-refract
// choice, metal fuzz, Russian roulette), paths use the plain random stream.
#include "vec3.hpp"

#include <cstdint>

constexpr int qmc_bounces = 2;

struct path_sampler {
    uint32_t pixel_seed = 0;
    uint32_t index = 0;    // This sample's number within its pixel
    int base = 0;          // First pair of the current bounce
    bool active = false;
};

inline thread_local path_sampler qmc;

inline uint32_t reverse_bits(uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0f0f0f0fu) | ((x & 0x0f0f0f0fu) << 4);
    x = ((x >> 8) & 0x00ff00ffu) | ((x & 0x00ff00ffu) << 8);
    return (x >> 16) | (x << 16);
}

inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Laine and Karras's permutation: flips each bit depending only on the bits
// below it. On bit-reversed input that is an Owen scramble, which flips each
// bit depending only on the bits above it and keeps the sequence well spread:
// owen(x) = reverse_bits(lk_permute(reverse_bits(x), seed)).
inline uint32_t lk_permute(uint32_t x, uint32_t seed) {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

// The second Sobol dimension, bit-reversed, a byte of the index at a time.
// It is linear in the bits of the index (an XOR of one direction number per
// set bit), so four 256-entry tables replace a loop over 32 bits, which cost
// more than the rest of the sampler together.
struct sobol_1_tables {
    uint32_t t[4][256];
    constexpr sobol_1_tables() : t() {
        // Direction numbers of the second Sobol dimension: bit k of the index
        // contributes v_k, with v_0 = 1 << 31 and v_{k+1} = v_k ^ (v_k >> 1).
        uint32_t v[32] = {};
        v[0] = 1u << 31;
        for (int k = 1; k < 32; ++k) v[k] = v[k - 1] ^ (v[k - 1] >> 1);
        for (int byte = 0; byte < 4; ++byte)
            for (uint32_t b = 0; b < 256; ++b) {
                uint32_t r = 0;
                for (int k = 0; k < 8; ++k)
                    if (b & (1u << k)) r ^= v[8 * byte + k];
                t[byte][b] = reverse(r);
            }
    }
    static constexpr uint32_t reverse(uint32_t x) {
        uint32_t r = 0;
        for (int k = 0; k < 32; ++k) r |= ((x >> k) & 1u) << (31 - k);
        return r;
    }
};
inline constexpr sobol_1_tables sobol_1_reversed_table{};

inline uint32_t sobol_1_reversed(uint32_t i) {
    const auto& t = sobol_1_reversed_table.t;
    return t[0][i & 255] ^ t[1][(i >> 8) & 255] ^ t[2][(i >> 16) & 255] ^ t[3][i >> 24];
}

// Starts a sample: pixel (i, j) of a render with the given seed, sample
// number index within the pixel.
inline void begin_sample(uint64_t seed, int i, int j, uint32_t index) {
    qmc.pixel_seed = hash_u32(static_cast<uint32_t>(i) * 0x9e3779b1u ^ hash_u32(static_cast<uint32_t>(j) ^
                              hash_u32(static_cast<uint32_t>(seed) ^ static_cast<uint32_t>(seed >> 32))));
    qmc.index = index;
    qmc.base = 0;
    qmc.active = true;
}

// Two numbers in [0, 1) from dimension pair pair_offset of the current bounce.
inline void sample_2d(int pair_offset, real& a, real& b) {
    const int pair = qmc.base + pair_offset;
    if (!qmc.active || pair > 2 * qmc_bounces) {
        a = random_real();
        b = random_real();
        return;
    }
    const uint32_t seed = hash_u32(qmc.pixel_seed + static_cast<uint32_t>(pair) * 0x68bc21ebu);
    // The shuffled index, owen(index); then the first Sobol dimension of it,
    // which is reverse_bits(i), and the second, each Owen-scrambled with its
    // own seed. Written out, the reversals around lk_permute cancel in pairs.
    const uint32_t i = reverse_bits(lk_permute(reverse_bits(qmc.index), seed));
    const uint32_t x = reverse_bits(lk_permute(i, seed ^ 0xa511e9b3u));
    const uint32_t y = reverse_bits(lk_permute(sobol_1_reversed(i), seed ^ 0x63d83595u));
    // The top 24 bits, so the result stays below 1 in float.
    a = static_cast<real>(x >> 8) * real(0x1.0p-24);
    b = static_cast<real>(y >> 8) * real(0x1.0p-24);
}

// Moves the dimension pairs on to bounce k.
inline void set_bounce(int k) { qmc.base = 1 + 2 * k; }
