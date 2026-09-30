#pragma once
#include "ray.hpp"

// Forward declaration
class material;

struct hit_record {
    point3 p;         // Where the ray hit
    vec3 normal;      // The surface normal vector
    real t;           // The distance along the ray
    bool front_face;  // Did we hit the outside or the inside?
    const material* mat = nullptr;  // What the surface is made of
    bool sampled_light = false;     // One of the lights direct_light() samples

    // Determines if the ray hit the outside of the object or from the inside
    inline void set_face_normal(const ray& r, const vec3& outward_normal) {
        front_face = vec3::dot(r.direction(), outward_normal) < 0;
        normal = front_face ? outward_normal : -outward_normal;
    }
};
