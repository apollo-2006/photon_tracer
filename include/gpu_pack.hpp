#pragma once
// A built scene flattened into typed arrays for the WebGPU tracer
// (web/tracer.wgsl), which reads them as storage buffers.
//
// Every tree (the flat primitives', the instances', and each mesh's) is
// appended to one array of 4-wide BVH nodes, with child indices and leaf
// ranges rebased to where the tree landed. The nodes copy byte for byte: the
// C++ layout of bvh::node is what WGSL reads. Leaves of the flat tree hold
// primitive numbers as in geometry::hit() (spheres, then triangles); leaves of
// a mesh tree hold triangle numbers in the shared triangle array; leaves of
// the instance tree hold instance numbers.
#include "renderer.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

struct gpu_scene {
    std::vector<bvh::node> nodes;
    std::vector<uint32_t> order;
    std::vector<float> spheres;    // 8 floats each: center, radius, material (bits), 3 unused
    std::vector<float> triangles;  // 28 floats each, see pack_triangle()
    std::vector<float> instances;  // 28 floats each: to_world rows, to_object rows, root, material, 2 unused
    std::vector<float> materials;  // 12 floats each: albedo, type; emission, fuzz; ior, 3 unused
    std::vector<uint32_t> lights;  // Sphere numbers of the lights
    std::vector<uint32_t> large;   // Sphere numbers of the large spheres, tested outside the trees
    uint32_t flat_root = ~0u, instance_root = ~0u, small_spheres = 0;
    bool sky = true, sample_lights = true;
    float camera[16];              // origin, corner, across, up, 4 floats each
};

// The WebGPU tracer's traversal stack (STACK in web/tracer.wgsl).
constexpr int gpu_stack_entries = 32;

namespace gpu_pack_detail {

// Levels of 4-wide nodes below and including n.
inline int depth(const std::vector<bvh::node>& nodes, uint32_t n) {
    int d = 0;
    for (int c = 0; c < 4; ++c)
        if (nodes[n].count[c] == 0 && nodes[n].lo[0][c] != std::numeric_limits<float>::infinity())
            d = std::max(d, depth(nodes, nodes[n].first[c]));
    return d + 1;
}

inline float bits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Appends tree's nodes and leaf order, rebased; returns its root, or ~0u for
// an empty tree. Each leaf entry goes through map_leaf.
template <class Map>
uint32_t append_tree(gpu_scene& g, const bvh& tree, Map&& map_leaf) {
    if (tree.nodes.empty()) return ~0u;
    // A traversal holds at most three entries a level, plus the root.
    if (3 * depth(tree.nodes, 0) + 1 > gpu_stack_entries)
        throw std::runtime_error("a BVH is too deep for the WebGPU tracer's stack");
    const uint32_t node_base = static_cast<uint32_t>(g.nodes.size()), order_base = static_cast<uint32_t>(g.order.size());
    for (bvh::node n : tree.nodes) {
        for (int c = 0; c < 4; ++c) {
            if (n.count[c] > 0) n.first[c] += order_base;
            else if (n.lo[0][c] != std::numeric_limits<float>::infinity()) n.first[c] += node_base;  // Not an empty slot
        }
        g.nodes.push_back(n);
    }
    for (uint32_t p : tree.order) g.order.push_back(map_leaf(p));
    return node_base;
}

}  // namespace gpu_pack_detail

// The scene as one buffer for the page: a 48-word header, then the sections,
// each 16-byte aligned. Header: magic "PTGP", version 1, then (byte offset,
// byte length) for nodes, order, spheres, triangles, instances, materials,
// lights and large; then the flat root, the instance root, the number of
// small spheres, flags (1: sky, 2: sample lights), and the camera, 16 floats.
inline std::vector<uint32_t> serialize(const gpu_scene& g);

inline gpu_scene pack_for_gpu(const geometry& world, const camera& cam) {
    using namespace gpu_pack_detail;
    if (!world.uses_bvh()) throw std::runtime_error("the GPU tracer needs the BVH");
    gpu_scene g;

    std::map<const material*, uint32_t> index;
    auto material_of = [&](const material* m) -> uint32_t {
        auto found = index.find(m);
        if (found != index.end()) return found->second;
        float type = 0, fuzz = 0, ior = 1.5f;
        color albedo(0, 0, 0);
        if (auto* l = dynamic_cast<const lambertian*>(m)) {
            albedo = l->albedo;  // Textures are not carried over; the demo has none
        } else if (auto* me = dynamic_cast<const metal*>(m)) {
            type = 1;
            albedo = me->albedo;
            fuzz = me->fuzz;
        } else if (auto* d = dynamic_cast<const dielectric*>(m)) {
            type = 2;
            ior = d->ior;
        } else {
            type = 3;
        }
        const float row[12] = {albedo.x(), albedo.y(), albedo.z(), type, m->emission.x(), m->emission.y(),
                               m->emission.z(), fuzz, ior, 0, 0, 0};
        g.materials.insert(g.materials.end(), row, row + 12);
        const uint32_t k = static_cast<uint32_t>(index.size());
        index.emplace(m, k);
        return k;
    };
    auto pack_sphere = [&](const sphere& s) {
        const float row[8] = {s.center.x(), s.center.y(), s.center.z(), s.radius, bits(material_of(s.mat)), 0, 0, 0};
        g.spheres.insert(g.spheres.end(), row, row + 8);
    };
    // 7 vec4s: (p0, material), (e1, uv0.x), (e2, uv0.y), (n0, uv1.x),
    // (n1, uv1.y), (n2, uv2.x), (face normal, uv2.y).
    auto pack_triangle = [&](const triangle& t) {
        const float row[28] = {t.p0.x(), t.p0.y(), t.p0.z(), bits(material_of(t.mat)),
                               t.e1.x(), t.e1.y(), t.e1.z(), t.uv[0],
                               t.e2.x(), t.e2.y(), t.e2.z(), t.uv[1],
                               t.n0.x(), t.n0.y(), t.n0.z(), t.uv[2],
                               t.n1.x(), t.n1.y(), t.n1.z(), t.uv[3],
                               t.n2.x(), t.n2.y(), t.n2.z(), t.uv[4],
                               t.face_normal.x(), t.face_normal.y(), t.face_normal.z(), t.uv[5]};
        g.triangles.insert(g.triangles.end(), row, row + 28);
    };

    // Spheres: the small ones first, numbered as the flat tree numbers them,
    // then the large ones.
    for (const sphere& s : world.spheres) pack_sphere(s);
    g.small_spheres = static_cast<uint32_t>(world.spheres.size());
    for (const sphere& s : world.large_spheres) {
        g.large.push_back(static_cast<uint32_t>(g.spheres.size() / 8));
        pack_sphere(s);
    }
    g.lights = world.lights;

    // Triangles: the flat ones, then each mesh's.
    for (const triangle& t : world.triangles) pack_triangle(t);
    g.flat_root = append_tree(g, world.flat_tree(), [](uint32_t p) { return p; });

    std::vector<uint32_t> mesh_root;
    for (const auto& m : world.meshes) {
        const uint32_t tri_base = static_cast<uint32_t>(g.triangles.size() / 28);
        for (const triangle& t : m.triangles) pack_triangle(t);
        mesh_root.push_back(append_tree(g, m.tree, [tri_base](uint32_t k) { return tri_base + k; }));
    }
    for (const auto& in : world.instances) {
        float row[28] = {};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                row[4 * r + c] = in.to_world.m[r][c];
                row[12 + 4 * r + c] = in.to_object.m[r][c];
            }
            row[4 * r + 3] = in.to_world.t.e[r];
            row[12 + 4 * r + 3] = in.to_object.t.e[r];
        }
        row[24] = bits(mesh_root[in.mesh]);
        row[25] = bits(in.mat ? material_of(in.mat) : ~0u);
        g.instances.insert(g.instances.end(), row, row + 28);
    }
    g.instance_root = append_tree(g, world.instances_tree(), [](uint32_t k) { return k; });

    g.sky = world.sky;
    g.sample_lights = world.sample_lights;
    const vec3 cam_rows[4] = {cam.position(), cam.corner(), cam.across(), cam.up()};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) g.camera[4 * r + c] = cam_rows[r].e[c];
        g.camera[4 * r + 3] = 0;
    }
    return g;
}

inline std::vector<uint32_t> serialize(const gpu_scene& g) {
    std::vector<uint32_t> out(48, 0);
    out[0] = 0x50475450;  // "PTGP"
    out[1] = 1;
    auto add = [&](int slot, const void* data, size_t bytes) {
        out[2 + 2 * slot] = static_cast<uint32_t>(4 * out.size());
        out[3 + 2 * slot] = static_cast<uint32_t>(bytes);
        const size_t words = (bytes + 15) / 16 * 4;  // Padded to 16 bytes
        const size_t at = out.size();
        out.resize(at + words, 0);
        if (bytes) std::memcpy(&out[at], data, bytes);
    };
    add(0, g.nodes.data(), g.nodes.size() * sizeof(bvh::node));
    add(1, g.order.data(), g.order.size() * 4);
    add(2, g.spheres.data(), g.spheres.size() * 4);
    add(3, g.triangles.data(), g.triangles.size() * 4);
    add(4, g.instances.data(), g.instances.size() * 4);
    add(5, g.materials.data(), g.materials.size() * 4);
    add(6, g.lights.data(), g.lights.size() * 4);
    add(7, g.large.data(), g.large.size() * 4);
    out[18] = g.flat_root;
    out[19] = g.instance_root;
    out[20] = g.small_spheres;
    out[21] = (g.sky ? 1u : 0u) | (g.sample_lights ? 2u : 0u);
    std::memcpy(&out[22], g.camera, sizeof g.camera);
    return out;
}
