#pragma once

#include "hittable.hpp"
#include "vec3.hpp"

class sphere : public hittable {
public:
    point3 center;
    double radius;

    sphere() {}
    sphere(point3 cen, double r) : center(cen), radius(r) {}

    // The 'override' keyword ensures we are properly adhering to the hittable interface
    virtual bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const override {
        vec3 oc = r.origin() - center;
        double a = vec3::dot(r.direction(), r.direction());
        double half_b = vec3::dot(oc, r.direction());
        double c = vec3::dot(oc, oc) - radius * radius;

        double discriminant = half_b * half_b - a * c;
        if (discriminant < 0) return false; // Ray missed

        double sqrtd = std::sqrt(discriminant);

        // Find the nearest root that lies in the acceptable range.
        double root = (-half_b - sqrtd) / a;
        if (root < t_min || root > t_max) {
            root = (-half_b + sqrtd) / a;
            if (root < t_min || root > t_max)
                return false;
        }

        // Record the hit data
        rec.t = root;
        rec.p = r.at(rec.t);

        // Calculate normal and determine if we hit the front or back face
        vec3 outward_normal = (rec.p - center) * (1.0 / radius);
        rec.set_face_normal(r, outward_normal);

        return true;
    }
};