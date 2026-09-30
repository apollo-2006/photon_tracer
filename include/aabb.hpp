#pragma once
// Axis-aligned bounding box, the building block of the BVH.
#include "ray.hpp"

#include <algorithm>
#include <limits>

struct aabb {
    point3 min, max;

    // Empty: grows to exactly the first thing added to it.
    aabb() : min(point3(inf(), inf(), inf())), max(point3(-inf(), -inf(), -inf())) {}
    aabb(const point3& a, const point3& b) : min(a), max(b) {}

    void grow(const aabb& b) {
        for (int a = 0; a < 3; ++a) {
            min.e[a] = std::min(min.e[a], b.min.e[a]);
            max.e[a] = std::max(max.e[a], b.max.e[a]);
        }
    }

    point3 center() const { return (min + max) * real(0.5); }

    // Half the surface area, which is all the SAH needs: the chance that a ray
    // through the parent also passes through this box is the ratio of the two.
    real half_area() const {
        if (min.x() > max.x()) return 0;  // Empty
        vec3 d = max - min;
        return d.x() * d.y() + d.y() * d.z() + d.z() * d.x();
    }

    static real inf() { return std::numeric_limits<real>::infinity(); }
};
