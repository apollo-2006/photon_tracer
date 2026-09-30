#pragma once
// Everything a ray can hit, in flat arrays by type, with or without a BVH over
// it. Primitives are plain structs rather than objects behind a virtual hit(),
// so the leaves of the tree test them without an indirect call each.
#include "bvh.hpp"
#include "material.hpp"
#include "sphere.hpp"
#include "triangle.hpp"

#include <cstdint>
#include <memory>
#include <vector>

class geometry {
public:
    std::vector<sphere> spheres;
    std::vector<triangle> triangles;
    // Tested against every ray, outside the tree. The ground is a sphere of
    // radius 100 whose box contains the whole scene: in the tree, every ray
    // would enter its box and it would widen every box above it, so it would
    // cost a test per ray either way and slow down the rest of the tree too.
    // Being few, they are also intersected in double; see sphere::intersect.
    std::vector<sphere> large_spheres;

    // Keeps a material alive as long as the geometry, and returns the pointer
    // primitives hold.
    const material* own(std::shared_ptr<material> m) {
        materials.push_back(std::move(m));
        return materials.back().get();
    }

    // With use_bvh false, every ray tests every primitive, for comparison.
    void build(bool use_bvh) {
        bvh_on = use_bvh;
        tree = bvh();
        if (!use_bvh) return;
        // Primitive i is spheres[i] for i below spheres.size(), a triangle after that.
        std::vector<aabb> boxes;
        boxes.reserve(spheres.size() + triangles.size());
        for (const auto& s : spheres) boxes.push_back(s.bounding_box());
        for (const auto& t : triangles) boxes.push_back(t.bounding_box());
        tree.build(boxes);
    }

    bool hit(const ray& r, real t_min, real t_max, hit_record& rec) const {
        // Remember only which primitive is nearest; its record is filled once, at the end.
        enum { none, large, sph, tri } kind = none;
        uint32_t index = 0;
        real u = 0, v = 0;

        for (uint32_t i = 0; i < large_spheres.size(); ++i)
            if (large_spheres[i].intersect<double>(r, t_min, t_max)) { kind = large; index = i; }

        const uint32_t n_spheres = static_cast<uint32_t>(spheres.size());
        auto test = [&](uint32_t p, real& t_far) {
            if (p < n_spheres) {
                if (spheres[p].intersect(r, t_min, t_far)) { kind = sph; index = p; }
            } else if (triangles[p - n_spheres].intersect(r, t_min, t_far, u, v)) {
                kind = tri; index = p - n_spheres;
            }
        };
        if (bvh_on) {
            tree.traverse(r, t_min, t_max, test);
        } else {
            const uint32_t n = n_spheres + static_cast<uint32_t>(triangles.size());
            for (uint32_t p = 0; p < n; ++p) test(p, t_max);
        }

        switch (kind) {
            case large: large_spheres[index].fill(r, t_max, rec); return true;
            case sph: spheres[index].fill(r, t_max, rec); return true;
            case tri: triangles[index].fill(r, t_max, u, v, rec); return true;
            default: return false;
        }
    }

private:
    std::vector<std::shared_ptr<material>> materials;
    bvh tree;
    bool bvh_on = false;
};
