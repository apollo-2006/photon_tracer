#pragma once
// Everything a ray can hit, in flat arrays by type, with or without a BVH over
// it. Primitives are plain structs rather than objects behind a virtual hit(),
// so the leaves of the tree test them without an indirect call each.
#include "bvh.hpp"
#include "material.hpp"
#include "sphere.hpp"
#include "transform.hpp"
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

    // Indices into spheres of those made of a light, which matte surfaces
    // sample directly. Filled by build().
    std::vector<uint32_t> lights;
    // False for a closed scene lit only by its lights: rays that escape find black.
    bool sky = true;
    // Next event estimation on or off, for comparison.
    bool sample_lights = true;

    // Keeps a material alive as long as the geometry, and returns the pointer
    // primitives hold.
    const material* own(std::shared_ptr<material> m) {
        materials.push_back(std::move(m));
        return materials.back().get();
    }

    // A mesh placed any number of times by instances, each with its own
    // transform: the two-level structure hardware ray tracing APIs use
    // (bottom-level trees per mesh, a top-level tree over instances). A ray
    // that reaches an instance's box is moved into the mesh's own space and
    // continues down the mesh's tree, so a thousand copies of a mesh cost a
    // thousand boxes, not a thousand copies of its triangles.
    struct mesh {
        std::vector<triangle> triangles;
        bvh tree;
        aabb bounds;
    };
    struct instance {
        uint32_t mesh;
        affine to_world, to_object;
        const material* mat;  // Replaces the mesh's own materials, if set
    };
    std::vector<mesh> meshes;
    std::vector<instance> instances;

    // Adds a mesh made of triangles and returns its index, for instances.
    uint32_t add_mesh(std::vector<triangle> tris) {
        mesh m;
        m.triangles = std::move(tris);
        meshes.push_back(std::move(m));
        return static_cast<uint32_t>(meshes.size() - 1);
    }

    void add_instance(uint32_t mesh_index, const affine& to_world, const material* mat = nullptr) {
        instances.push_back({mesh_index, to_world, to_world.inverse(), mat});
    }

    // With use_bvh false, every ray tests every top-level primitive, for
    // comparison; meshes always get their own trees.
    void build(bool use_bvh) {
        lights.clear();
        for (uint32_t i = 0; i < spheres.size(); ++i)
            if (spheres[i].mat->emits()) lights.push_back(i);
        for (auto& m : meshes) {
            std::vector<aabb> boxes;
            boxes.reserve(m.triangles.size());
            m.bounds = aabb();
            for (const auto& t : m.triangles) {
                boxes.push_back(t.bounding_box());
                m.bounds.grow(boxes.back());
            }
            m.tree.build(boxes);
        }
        bvh_on = use_bvh;
        tree = bvh();
        instance_tree = bvh();
        if (!use_bvh) return;
        // Primitive p is spheres[p] below spheres.size(), a triangle after that.
        std::vector<aabb> boxes;
        boxes.reserve(spheres.size() + triangles.size());
        for (const auto& s : spheres) boxes.push_back(s.bounding_box());
        for (const auto& t : triangles) boxes.push_back(t.bounding_box());
        tree.build(boxes);
        // Instances get a top-level tree of their own rather than joining
        // the one above: a leaf test that also had to check for instances
        // made every scene without any 5-7% slower.
        boxes.clear();
        for (const auto& in : instances) boxes.push_back(in.to_world.box(meshes[in.mesh].bounds));
        instance_tree.build(boxes);
    }

    // Forced inline: called once per ray from the render loop, and with the
    // instance path in it, GCC stopped inlining it, which cost 5-15%.
#if defined(__GNUC__)
    __attribute__((always_inline))
#endif
    inline bool hit(const ray& r, real t_min, real t_max, hit_record& rec) const {
        // Remember only which primitive is nearest; its record is filled once, at the end.
        enum { none, large, sph, tri, inst } kind = none;
        uint32_t index = 0, sub = 0;
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
        auto test_instance = [&](uint32_t k, real& t_far) {
            const instance_hit h = hit_instance(k, r, t_min, t_far, false);
            if (h.found) { kind = inst; index = k; t_far = h.t; u = h.u; v = h.v; sub = h.triangle; }
        };
        if (bvh_on) {
            tree.traverse(r, t_min, t_max, test);
            if (!instances.empty()) instance_tree.traverse(r, t_min, t_max, test_instance);
        } else {
            const uint32_t n = n_spheres + static_cast<uint32_t>(triangles.size());
            for (uint32_t p = 0; p < n; ++p) test(p, t_max);
            for (uint32_t k = 0; k < instances.size(); ++k) test_instance(k, t_max);
        }

        rec.sampled_light = false;
        switch (kind) {
            case large: large_spheres[index].fill(r, t_max, rec); return true;
            case sph:
                spheres[index].fill(r, t_max, rec);
                rec.sampled_light = sample_lights && rec.mat->emits();
                return true;
            case tri: triangles[index].fill(r, t_max, u, v, rec); return true;
            case inst: fill_instance(index, sub, r, t_max, u, v, rec); return true;
            default: return false;
        }
    }

    // Whether anything lies on the ray within (t_min, t_max): a shadow ray.
    // Stops at the first hit rather than looking for the nearest.
    bool occluded(const ray& r, real t_min, real t_max) const {
        for (const auto& s : large_spheres) {
            real t = t_max;
            if (s.intersect<double>(r, t_min, t)) return true;
        }
        bool blocked = false;
        const uint32_t n_spheres = static_cast<uint32_t>(spheres.size());
        real u, v;
        // Pulling t_far in to t_min makes a traversal skip everything left.
        auto test = [&](uint32_t p, real& t_far) {
            if (blocked) return;
            real t = t_far;
            blocked = p < n_spheres ? spheres[p].intersect(r, t_min, t)
                                    : triangles[p - n_spheres].intersect(r, t_min, t, u, v);
            if (blocked) t_far = t_min;
        };
        auto test_instance = [&](uint32_t k, real& t_far) {
            if (blocked) return;
            blocked = hit_instance(k, r, t_min, t_far, true).found;
            if (blocked) t_far = t_min;
        };
        real t_far = t_max;
        if (bvh_on) {
            tree.traverse(r, t_min, t_far, test);
            if (!blocked && !instances.empty()) {
                t_far = t_max;
                instance_tree.traverse(r, t_min, t_far, test_instance);
            }
        } else {
            const uint32_t n = n_spheres + static_cast<uint32_t>(triangles.size());
            for (uint32_t p = 0; p < n && !blocked; ++p) test(p, t_far);
            for (uint32_t k = 0; k < instances.size() && !blocked; ++k) test_instance(k, t_far);
        }
        return blocked;
    }

    // The top-level trees, for packing the scene for the GPU (gpu_pack.hpp).
    const bvh& flat_tree() const { return tree; }
    const bvh& instances_tree() const { return instance_tree; }
    bool uses_bvh() const { return bvh_on; }

private:
    struct instance_hit {
        bool found = false;
        real t, u = 0, v = 0;
        uint32_t triangle = 0;
    };

    // Instance k along r: the nearest hit closer than t_far, or with any_hit,
    // any hit at all. Out of line and returning by value, so the code for
    // instances does not crowd the plain sphere and triangle path of hit():
    // inline, or writing through references to its locals, it cost that path
    // 5-15%.
#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    instance_hit hit_instance(uint32_t k, const ray& r, real t_min, real t_far, bool any_hit) const {
        // The same t along the ray in either space: the transform is affine,
        // and the direction is transformed without normalizing.
        const instance& in = instances[k];
        const mesh& m = meshes[in.mesh];
        const ray local(in.to_object.point(r.origin()), in.to_object.vector(r.direction()));
        instance_hit h;
        h.t = t_far;
        m.tree.traverse(local, t_min, h.t, [&](uint32_t tri, real& t_mesh) {
            if (h.found && any_hit) return;
            if (m.triangles[tri].intersect(local, t_min, t_mesh, h.u, h.v)) {
                h.found = true;
                h.triangle = tri;
                if (any_hit) t_mesh = t_min;  // Nothing else needs looking at
            }
        });
        return h;
    }

#if defined(__GNUC__)
    __attribute__((noinline))
#endif
    void fill_instance(uint32_t k, uint32_t sub, const ray& r, real t, real u, real v, hit_record& rec) const {
        const instance& in = instances[k];
        const ray local(in.to_object.point(r.origin()), in.to_object.vector(r.direction()));
        meshes[in.mesh].triangles[sub].fill(local, t, u, v, rec);
        // Back to world space: the point from the world ray, the normal by
        // the inverse transpose. front_face carries over, since the
        // transforms here do not mirror.
        rec.p = r.at(t);
        rec.normal = in.to_object.transposed(rec.normal).normalize();
        if (in.mat) rec.mat = in.mat;
    }

    std::vector<std::shared_ptr<material>> materials;
    bvh tree, instance_tree;
    bool bvh_on = false;
};
