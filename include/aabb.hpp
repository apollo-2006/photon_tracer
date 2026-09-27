#pragma once
// Axis-aligned bounding box, the building block of the BVH.
#include "ray.hpp"

#include <algorithm>

struct aabb {
    point3 min, max;

    aabb() {}
    aabb(const point3& a, const point3& b) : min(a), max(b) {}

    // Slab test: clip [t_min, t_max] against the ray's entry and exit on each
    // axis. An empty interval on any axis means a miss.
    bool hit(const ray& r, double t_min, double t_max) const {
        for (int a = 0; a < 3; ++a) {
            double inv_d = 1.0 / r.direction().e[a];
            double t0 = (min.e[a] - r.origin().e[a]) * inv_d;
            double t1 = (max.e[a] - r.origin().e[a]) * inv_d;
            if (inv_d < 0.0) std::swap(t0, t1);
            t_min = t0 > t_min ? t0 : t_min;
            t_max = t1 < t_max ? t1 : t_max;
            if (t_max <= t_min) return false;
        }
        return true;
    }

    static aabb surrounding(const aabb& a, const aabb& b) {
        return aabb(point3(std::min(a.min.x(), b.min.x()), std::min(a.min.y(), b.min.y()), std::min(a.min.z(), b.min.z())),
                    point3(std::max(a.max.x(), b.max.x()), std::max(a.max.y(), b.max.y()), std::max(a.max.z(), b.max.z())));
    }
};
