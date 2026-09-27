#pragma once
// A triangle with optional per-vertex normals, the building block of meshes.
#include "hittable.hpp"
#include "vec3.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

class material;

class triangle : public hittable {
public:
    point3 p0;
    vec3 e1, e2;        // Edges p1 - p0 and p2 - p0
    vec3 n0, n1, n2;    // Vertex normals, interpolated across the face
    vec3 face_normal;
    std::shared_ptr<material> mat;

    // Flat shaded: every vertex normal is the face normal.
    triangle(point3 a, point3 b, point3 c, std::shared_ptr<material> m)
        : triangle(a, b, c, vec3(), vec3(), vec3(), std::move(m)) {
        n0 = n1 = n2 = face_normal;
    }

    triangle(point3 a, point3 b, point3 c, vec3 na, vec3 nb, vec3 nc, std::shared_ptr<material> m)
        : p0(a), e1(b - a), e2(c - a), n0(na), n1(nb), n2(nc), mat(std::move(m)) {
        face_normal = cross(e1, e2).normalize();
    }

    // Moller-Trumbore: solve origin + t*dir = p0 + u*e1 + v*e2 directly for
    // t and the barycentric u, v, without first intersecting the plane.
    bool hit(const ray& r, double t_min, double t_max, hit_record& rec) const override {
        vec3 pvec = cross(r.direction(), e2);
        double det = vec3::dot(e1, pvec);
        if (std::fabs(det) < 1e-12) return false;  // Ray parallel to the face
        double inv_det = 1.0 / det;

        vec3 tvec = r.origin() - p0;
        double u = vec3::dot(tvec, pvec) * inv_det;
        if (u < 0.0 || u > 1.0) return false;

        vec3 qvec = cross(tvec, e1);
        double v = vec3::dot(r.direction(), qvec) * inv_det;
        if (v < 0.0 || u + v > 1.0) return false;

        double t = vec3::dot(e2, qvec) * inv_det;
        if (t < t_min || t > t_max) return false;

        rec.t = t;
        rec.p = r.at(t);
        // Which side was hit comes from the true face, so a smoothed normal
        // near a silhouette cannot flip it; shading uses the smooth normal.
        rec.front_face = vec3::dot(r.direction(), face_normal) < 0;
        vec3 n = (n0 * (1.0 - u - v) + n1 * u + n2 * v).normalize();
        rec.normal = rec.front_face ? n : -n;
        rec.mat = mat.get();
        return true;
    }

    aabb bounding_box() const override {
        point3 p1 = p0 + e1, p2 = p0 + e2;
        const double pad = 1e-4;  // A flat, axis-aligned face would have a zero-width box
        point3 lo(std::min({p0.x(), p1.x(), p2.x()}) - pad, std::min({p0.y(), p1.y(), p2.y()}) - pad,
                  std::min({p0.z(), p1.z(), p2.z()}) - pad);
        point3 hi(std::max({p0.x(), p1.x(), p2.x()}) + pad, std::max({p0.y(), p1.y(), p2.y()}) + pad,
                  std::max({p0.z(), p1.z(), p2.z()}) + pad);
        return aabb(lo, hi);
    }
};
