#pragma once

#include "aabb.hpp"
#include "hittable.hpp"
#include "vec3.hpp"

class material;

// Intersection is split from shading: intersect() only finds the distance, and
// fill() builds the hit record once the nearest hit of all is known, so the
// point and normal are not worked out for hits that something closer hides.
class sphere {
public:
    point3 center;
    real radius;
    const material* mat;

    sphere(point3 cen, real r, const material* m) : center(cen), radius(r), mat(m) {}

    // On a hit inside (t_min, t_max), moves t_max to it and returns true.
    // Precise computes in P instead of real: in float, |oc|^2 - r^2 for the
    // radius-100 ground subtracts two numbers near 10,000 and loses the few
    // thousandths that separate a bounce from its own surface, which darkened
    // the ground by re-hitting it.
    template <class P = real>
    bool intersect(const ray& r, real t_min, real& t_max) const {
        const P ox = P(r.origin().x()) - center.x(), oy = P(r.origin().y()) - center.y(),
                oz = P(r.origin().z()) - center.z();
        const P dx = r.direction().x(), dy = r.direction().y(), dz = r.direction().z();
        P a = dx * dx + dy * dy + dz * dz;
        P half_b = ox * dx + oy * dy + oz * dz;
        P c = ox * ox + oy * oy + oz * oz - P(radius) * radius;

        P discriminant = half_b * half_b - a * c;
        if (discriminant < 0) return false; // Ray missed

        P sqrtd = std::sqrt(discriminant);

        // Find the nearest root that lies in the acceptable range.
        P root = (-half_b - sqrtd) / a;
        if (root < t_min || root > t_max) {
            root = (-half_b + sqrtd) / a;
            if (root < t_min || root > t_max)
                return false;
        }
        t_max = static_cast<real>(root);
        return true;
    }

    void fill(const ray& r, real t, hit_record& rec) const {
        rec.t = t;
        rec.p = r.at(t);

        // Calculate normal and determine if we hit the front or back face
        vec3 outward_normal = (rec.p - center) * (1 / radius);
        rec.set_face_normal(r, outward_normal);
        rec.mat = mat;
    }

    aabb bounding_box() const {
        // abs: a negative radius (a hollow shell's inner wall) is still that big
        vec3 r(std::fabs(radius), std::fabs(radius), std::fabs(radius));
        return aabb(center - r, center + r);
    }
};
