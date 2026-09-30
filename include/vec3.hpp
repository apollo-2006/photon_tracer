#pragma once
#include <cmath>
#include <cstdint>
#include <random>

// The precision of all geometry and color.
using real = float;

// xoshiro256+ (Blackman and Vigna), one state per thread. It was mt19937 with
// uniform_real_distribution, which cost about 9% of a render.
struct xoshiro256p {
    uint64_t s[4] = {0, 0, 0, 0};  // All zero is not a valid state; it means not seeded yet

    // splitmix64 spreads one seed across the four words, as the authors recommend.
    void seed(uint64_t seed) {
        for (auto& w : s) {
            uint64_t z = (seed += 0x9e3779b97f4a7c15ull);
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            w = z ^ (z >> 31);
        }
    }

    uint64_t next() {
        const uint64_t result = s[0] + s[3], t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = (s[3] << 45) | (s[3] >> 19);
        return result;
    }
};

// Zero-initialized rather than seeded in its declaration, so reaching it needs
// no per-call check that the thread's copy was constructed, and random_real()
// stays small enough to inline. Each thread seeds its copy on first use.
inline thread_local xoshiro256p thread_rng;

#if defined(__GNUC__)
__attribute__((noinline, cold))
#endif
inline void seed_thread_rng() {
    std::random_device rd;
    thread_rng.seed((uint64_t(rd()) << 32) ^ rd());
}

// Thread-safe random number in [0, 1). The top bits make the mantissa;
// xoshiro256+'s low bits are its weakest.
inline real random_real() {
    if ((thread_rng.s[0] | thread_rng.s[1] | thread_rng.s[2] | thread_rng.s[3]) == 0) seed_thread_rng();
    if constexpr (sizeof(real) == 4) return static_cast<real>(thread_rng.next() >> 40) * 0x1.0p-24f;
    else return static_cast<real>(thread_rng.next() >> 11) * 0x1.0p-53;
}

inline real random_real(real min, real max) {
    return min + (max - min) * random_real();
}

class vec3 {
public:
    real e[3];

    vec3() : e{0,0,0} {}
    vec3(real e0, real e1, real e2) : e{e0, e1, e2} {}

    real x() const { return e[0]; }
    real y() const { return e[1]; }
    real z() const { return e[2]; }

    vec3 operator-() const { return vec3(-e[0], -e[1], -e[2]); }

    vec3 operator+(const vec3& v) const {
        return vec3(e[0] + v.e[0], e[1] + v.e[1], e[2] + v.e[2]);
    }

    vec3 operator-(const vec3& v) const {
        return vec3(e[0] - v.e[0], e[1] - v.e[1], e[2] - v.e[2]);
    }

    vec3 operator*(const vec3& v) const {
        return vec3(e[0] * v.e[0], e[1] * v.e[1], e[2] * v.e[2]);
    }

    vec3 operator*(real t) const {
        return vec3(e[0] * t, e[1] * t, e[2] * t);
    }

    vec3 operator/(real t) const {
        return *this * (1 / t);
    }

    inline static real dot(const vec3& u, const vec3& v) {
        return u.e[0] * v.e[0] + u.e[1] * v.e[1] + u.e[2] * v.e[2];
    }

    inline vec3 normalize() const {
        real length = std::sqrt(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
        return vec3(e[0]/length, e[1]/length, e[2]/length);
    }

    // Generate random vectors for anti-aliasing and lighting
    inline static vec3 random() {
        return vec3(random_real(), random_real(), random_real());
    }

    inline static vec3 random(real min, real max) {
        return vec3(random_real(min, max), random_real(min, max), random_real(min, max));
    }
};

inline vec3 operator*(real t, const vec3& v) {
    return vec3(t * v.e[0], t * v.e[1], t * v.e[2]);
}

inline vec3 cross(const vec3& a, const vec3& b) {
    return vec3(a.y() * b.z() - a.z() * b.y(),
                a.z() * b.x() - a.x() * b.z(),
                a.x() * b.y() - a.y() * b.x());
}

// Utility to generate a random point inside a 3D sphere for diffuse lighting
inline vec3 random_in_unit_sphere() {
    while (true) {
        auto p = vec3::random(-1, 1);
        if (vec3::dot(p, p) >= 1) continue;
        return p;
    }
}

// Clamps a value between a min and max
inline real clamp(real x, real min, real max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

using point3 = vec3;
using color = vec3;