#pragma once
#include "ray.hpp"
#include "aabb.hpp"

// Forward declaration
class material;

struct hit_record {
    point3 p;         // Where the ray hit
    vec3 normal;      // The surface normal vector
    double t;         // The distance along the ray
    bool front_face;  // Did we hit the outside or the inside?
    const material* mat = nullptr;  // What the surface is made of

    // Determines if the ray hit the outside of the object or from the inside
    inline void set_face_normal(const ray& r, const vec3& outward_normal) {
        front_face = vec3::dot(r.direction(), outward_normal) < 0;
        normal = front_face ? outward_normal : outward_normal * -1.0;
    }
};

class hittable {
public:
    virtual ~hittable() = default;

    // The pure virtual function every 3D object must implement
    virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const = 0;

    // A box enclosing the whole object, for the BVH
    virtual aabb bounding_box() const = 0;
};