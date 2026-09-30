#pragma once
// Affine transforms: a 3x3 matrix and a translation, for placing instances of
// a mesh. Points get both, directions only the matrix, and normals the
// inverse transpose of the matrix, which keeps them perpendicular to the
// surface under non-uniform scaling.
#include "aabb.hpp"
#include "vec3.hpp"

#include <cmath>

struct affine {
    real m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    vec3 t;

    vec3 vector(const vec3& v) const {
        return vec3(m[0][0] * v.x() + m[0][1] * v.y() + m[0][2] * v.z(),
                    m[1][0] * v.x() + m[1][1] * v.y() + m[1][2] * v.z(),
                    m[2][0] * v.x() + m[2][1] * v.y() + m[2][2] * v.z());
    }
    point3 point(const point3& p) const { return vector(p) + t; }
    // The transpose of the matrix applied to v: with the inverse's matrix,
    // this is how the forward transform moves a normal.
    vec3 transposed(const vec3& v) const {
        return vec3(m[0][0] * v.x() + m[1][0] * v.y() + m[2][0] * v.z(),
                    m[0][1] * v.x() + m[1][1] * v.y() + m[2][1] * v.z(),
                    m[0][2] * v.x() + m[1][2] * v.y() + m[2][2] * v.z());
    }

    // Scale by s, then turn yaw_deg about the vertical axis, then move by offset.
    static affine place(real s, double yaw_deg, const vec3& offset) {
        const double a = yaw_deg * M_PI / 180;
        const real c = static_cast<real>(std::cos(a)), si = static_cast<real>(std::sin(a));
        affine x;
        x.m[0][0] = c * s;   x.m[0][1] = 0; x.m[0][2] = si * s;
        x.m[1][0] = 0;       x.m[1][1] = s; x.m[1][2] = 0;
        x.m[2][0] = -si * s; x.m[2][1] = 0; x.m[2][2] = c * s;
        x.t = offset;
        return x;
    }

    // The inverse, computed in double from the cofactors.
    affine inverse() const {
        double a[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) a[i][j] = m[i][j];
        const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
        const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
        const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
        const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
        const double inv[3][3] = {
            {c00 / det, (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det, (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det},
            {c01 / det, (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det, (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det},
            {c02 / det, (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det, (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det}};
        affine r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = static_cast<real>(inv[i][j]);
        r.t = -r.vector(t);
        return r;
    }

    // The box around the transformed corners of b.
    aabb box(const aabb& b) const {
        aabb out;
        for (int k = 0; k < 8; ++k) {
            const point3 c(k & 1 ? b.max.x() : b.min.x(), k & 2 ? b.max.y() : b.min.y(), k & 4 ? b.max.z() : b.min.z());
            const point3 p = point(c);
            out.grow(aabb(p, p));
        }
        return out;
    }
};
