#pragma once
#include <cmath>

class vec3 {
public:
    double e[3];

    vec3() : e{0,0,0} {}
    vec3(double e0, double e1, double e2) : e{e0, e1, e2} {}

    double x() const { return e[0]; }
    double y() const { return e[1]; }
    double z() const { return e[2]; }

    // Unary minus (e.g., -vector)
    vec3 operator-() const {
        return vec3(-e[0], -e[1], -e[2]);
    }

    // Vector addition
    vec3 operator+(const vec3& v) const {
        return vec3(e[0] + v.e[0], e[1] + v.e[1], e[2] + v.e[2]);
    }

    // Vector subtraction
    vec3 operator-(const vec3& v) const {
        return vec3(e[0] - v.e[0], e[1] - v.e[1], e[2] - v.e[2]);
    }

    // Vector multiplication (Hadamard product)
    vec3 operator*(const vec3& v) const {
        return vec3(e[0] * v.e[0], e[1] * v.e[1], e[2] * v.e[2]);
    }

    // Scalar multiplication
    vec3 operator*(double t) const {
        return vec3(e[0] * t, e[1] * t, e[2] * t);
    }

    // Scalar division
    vec3 operator/(double t) const {
        return *this * (1.0 / t);
    }

    // Dot product for lighting calculations
    inline static double dot(const vec3& u, const vec3& v) {
        return u.e[0] * v.e[0] + u.e[1] * v.e[1] + u.e[2] * v.e[2];
    }

    // Normalization (making the vector length exactly 1)
    inline vec3 normalize() const {
        double length = std::sqrt(e[0]*e[0] + e[1]*e[1] + e[2]*e[2]);
        return vec3(e[0]/length, e[1]/length, e[2]/length);
    }
};

// Global utility allowing scalar multiplication from the left (e.g., 0.5 * vector)
inline vec3 operator*(double t, const vec3& v) {
    return vec3(t * v.e[0], t * v.e[1], t * v.e[2]);
}

// Aliases for geometric clarity
using point3 = vec3;   // 3D position
using color = vec3;    // RGB color