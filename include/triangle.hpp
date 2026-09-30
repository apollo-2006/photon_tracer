#pragma once
// A triangle with optional per-vertex normals, the building block of meshes.
#include "aabb.hpp"
#include "hittable.hpp"
#include "vec3.hpp"

#include <algorithm>
#include <cmath>

class material;

class triangle {
public:
    // What intersect() reads comes first, so it shares a cache line.
    point3 p0;
    vec3 e1, e2;        // Edges p1 - p0 and p2 - p0
    vec3 n0, n1, n2;    // Vertex normals, interpolated across the face
    vec3 face_normal;
    const material* mat;
    real uv[6] = {0, 0, 0, 0, 0, 0};  // Texture coordinates at the three corners
    // The second and third corners exactly as given: p0 + e1 can be an ulp
    // off, which would split a vertex two triangles share when the mesh is
    // indexed again (meshlets.hpp).
    point3 p1, p2;

    // Flat shaded: every vertex normal is the face normal.
    triangle(point3 a, point3 b, point3 c, const material* m)
        : triangle(a, b, c, vec3(), vec3(), vec3(), m) {
        n0 = n1 = n2 = face_normal;
    }

    triangle(point3 a, point3 b, point3 c, vec3 na, vec3 nb, vec3 nc, const material* m)
        : p0(a), e1(b - a), e2(c - a), n0(na), n1(nb), n2(nc), mat(m), p1(b), p2(c) {
        face_normal = cross(e1, e2).normalize();
    }

    // Moller-Trumbore: solve origin + t*dir = p0 + u*e1 + v*e2 directly for
    // t and the barycentric u, v, without first intersecting the plane. On a
    // hit inside (t_min, t_max), moves t_max to it and keeps u and v for fill().
    bool intersect(const ray& r, real t_min, real& t_max, real& u_out, real& v_out) const {
        vec3 pvec = cross(r.direction(), e2);
        real det = vec3::dot(e1, pvec);
        if (std::fabs(det) < real(1e-12)) return false;  // Ray parallel to the face
        real inv_det = 1 / det;

        vec3 tvec = r.origin() - p0;
        real u = vec3::dot(tvec, pvec) * inv_det;
        if (u < 0 || u > 1) return false;

        vec3 qvec = cross(tvec, e1);
        real v = vec3::dot(r.direction(), qvec) * inv_det;
        if (v < 0 || u + v > 1) return false;

        real t = vec3::dot(e2, qvec) * inv_det;
        if (t < t_min || t > t_max) return false;

        t_max = t;
        u_out = u;
        v_out = v;
        return true;
    }

    void fill(const ray& r, real t, real u, real v, hit_record& rec) const {
        rec.t = t;
        rec.p = r.at(t);
        // Which side was hit comes from the true face, so a smoothed normal
        // near a silhouette cannot flip it; shading uses the smooth normal.
        rec.front_face = vec3::dot(r.direction(), face_normal) < 0;
        vec3 n = (n0 * (1 - u - v) + n1 * u + n2 * v).normalize();
        rec.normal = rec.front_face ? n : -n;
        rec.mat = mat;
        rec.tex_u = uv[0] * (1 - u - v) + uv[2] * u + uv[4] * v;
        rec.tex_v = uv[1] * (1 - u - v) + uv[3] * u + uv[5] * v;
    }

    aabb bounding_box() const {
        const real pad = real(1e-4);  // A flat, axis-aligned face would have a zero-width box
        point3 lo(std::min({p0.x(), p1.x(), p2.x()}) - pad, std::min({p0.y(), p1.y(), p2.y()}) - pad,
                  std::min({p0.z(), p1.z(), p2.z()}) - pad);
        point3 hi(std::max({p0.x(), p1.x(), p2.x()}) + pad, std::max({p0.y(), p1.y(), p2.y()}) + pad,
                  std::max({p0.z(), p1.z(), p2.z()}) + pad);
        return aabb(lo, hi);
    }
};
