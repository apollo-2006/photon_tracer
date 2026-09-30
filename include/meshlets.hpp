#pragma once
// Splitting a mesh into meshlets for mesh shaders.
//
// A mesh shader workgroup emits a small, self-contained piece of a mesh: here
// up to 64 vertices and 124 triangles, whose triangles index the meshlet's own
// vertex list with bytes. Each meshlet also gets a bounding sphere and a cone
// that contains every one of its face normals, so a task shader can drop a
// meshlet that is outside the view, or whose every triangle faces away from
// the camera, before any of it is rasterized.
//
// Triangles are grouped in the order of the leaves of a BVH over them, which
// already puts triangles that are near each other together, and a meshlet is
// closed when the next triangle would take it past either limit.
#include "bvh.hpp"
#include "triangle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

constexpr uint32_t meshlet_max_vertices = 64;
constexpr uint32_t meshlet_max_triangles = 124;

// Laid out as the shaders read it (std430): 48 bytes.
struct meshlet {
    float center[3];
    float radius;
    float cone_axis[3];
    // Sine of the widest angle between cone_axis and a face normal: every
    // triangle faces away from a viewer at unit direction d from the center
    // when dot(d, axis) >= cone_cutoff + radius / distance. 1 disables it.
    float cone_cutoff;
    uint32_t vertex_offset;    // Into meshlet_mesh::meshlet_vertices
    uint32_t triangle_offset;  // Into meshlet_mesh::meshlet_triangles, in triangles
    uint32_t vertex_count;
    uint32_t triangle_count;
};
static_assert(sizeof(meshlet) == 48, "meshlet must match the shaders' layout");

struct meshlet_mesh {
    // The mesh indexed: vertices deduplicated by position, normal and texture
    // coordinates, and three indices per triangle, in the original order, so
    // triangle k here is triangle k of the input (and primitive k of the
    // ray tracing geometry built from it).
    std::vector<float> positions, normals, uvs;  // 3, 3 and 2 floats a vertex
    std::vector<uint32_t> indices;

    std::vector<meshlet> meshlets;
    std::vector<uint32_t> meshlet_vertices;   // Mesh vertex index, per meshlet vertex
    std::vector<uint8_t> meshlet_triangles;   // Three meshlet-local indices per triangle
    std::vector<uint32_t> meshlet_triangle_ids;  // The input index of each meshlet triangle

    // Whether every edge is shared by exactly two triangles. Only then can
    // normal cones cull: through a hole in an open mesh, the inside of the far
    // wall shows, and it faces away from the camera.
    bool closed = false;
};

namespace meshlet_detail {

struct vertex_key {
    float v[8];
    bool operator==(const vertex_key& o) const { return std::memcmp(v, o.v, sizeof v) == 0; }
};
struct vertex_hash {
    size_t operator()(const vertex_key& k) const {
        uint64_t h = 1469598103934665603ull;
        const auto* b = reinterpret_cast<const unsigned char*>(k.v);
        for (size_t i = 0; i < sizeof k.v; ++i) h = (h ^ b[i]) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

}  // namespace meshlet_detail

inline meshlet_mesh build_meshlets(const std::vector<triangle>& tris) {
    using namespace meshlet_detail;
    meshlet_mesh out;
    const size_t n = tris.size();
    if (n == 0) return out;

    // Index the mesh. Positions alone are also indexed, for the closed test,
    // since a seam in normals or texture coordinates splits a vertex without
    // opening the surface.
    std::unordered_map<vertex_key, uint32_t, vertex_hash> vertex_index;
    std::map<std::array<float, 3>, uint32_t> position_index;
    std::vector<uint32_t> position_of;  // Per triangle corner
    out.indices.reserve(3 * n);
    for (const triangle& t : tris) {
        const point3 corner[3] = {t.p0, t.p1, t.p2};
        const vec3 normal[3] = {t.n0, t.n1, t.n2};
        for (int c = 0; c < 3; ++c) {
            vertex_key key{{corner[c].x(), corner[c].y(), corner[c].z(), normal[c].x(), normal[c].y(),
                            normal[c].z(), t.uv[2 * c], t.uv[2 * c + 1]}};
            auto [it, added] = vertex_index.emplace(key, static_cast<uint32_t>(out.positions.size() / 3));
            if (added) {
                out.positions.insert(out.positions.end(), key.v, key.v + 3);
                out.normals.insert(out.normals.end(), key.v + 3, key.v + 6);
                out.uvs.insert(out.uvs.end(), key.v + 6, key.v + 8);
            }
            out.indices.push_back(it->second);
            const std::array<float, 3> p = {corner[c].x(), corner[c].y(), corner[c].z()};
            position_of.push_back(position_index.emplace(p, static_cast<uint32_t>(position_index.size())).first->second);
        }
    }

    std::map<std::pair<uint32_t, uint32_t>, int> edge_uses;
    for (size_t k = 0; k < n; ++k)
        for (int c = 0; c < 3; ++c) {
            uint32_t a = position_of[3 * k + c], b = position_of[3 * k + (c + 1) % 3];
            ++edge_uses[{std::min(a, b), std::max(a, b)}];
        }
    out.closed = std::all_of(edge_uses.begin(), edge_uses.end(), [](const auto& e) { return e.second == 2; });

    // Spatial order: the BVH's leaves.
    std::vector<aabb> boxes;
    boxes.reserve(n);
    for (const triangle& t : tris) boxes.push_back(t.bounding_box());
    bvh tree;
    tree.build(boxes);

    std::vector<uint32_t> local(out.positions.size() / 3, UINT32_MAX);  // Mesh vertex -> meshlet vertex
    meshlet cur{};
    std::vector<uint32_t> cur_tris;
    auto flush = [&]() {
        if (cur.triangle_count == 0) return;
        // Bounds: a sphere around the box of the vertices, and the cone of face normals.
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (uint32_t i = 0; i < cur.vertex_count; ++i) {
            const float* p = &out.positions[3 * out.meshlet_vertices[cur.vertex_offset + i]];
            for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
        }
        float r2 = 0;
        for (int a = 0; a < 3; ++a) cur.center[a] = 0.5f * (lo[a] + hi[a]);
        for (uint32_t i = 0; i < cur.vertex_count; ++i) {
            const float* p = &out.positions[3 * out.meshlet_vertices[cur.vertex_offset + i]];
            float d2 = 0;
            for (int a = 0; a < 3; ++a) d2 += (p[a] - cur.center[a]) * (p[a] - cur.center[a]);
            r2 = std::max(r2, d2);
        }
        cur.radius = std::sqrt(r2);

        vec3 axis(0, 0, 0);
        std::vector<vec3> face_normals;
        for (uint32_t k : cur_tris) {
            const vec3 fn = cross(tris[k].e1, tris[k].e2);
            if (vec3::dot(fn, fn) > 1e-30f) face_normals.push_back(fn.normalize());
        }
        for (const vec3& fn : face_normals) axis = axis + fn;
        cur.cone_cutoff = 1;
        cur.cone_axis[0] = 0; cur.cone_axis[1] = 0; cur.cone_axis[2] = 1;
        if (vec3::dot(axis, axis) > 1e-12f) {
            axis = axis.normalize();
            float min_dot = 1;
            for (const vec3& fn : face_normals) min_dot = std::min(min_dot, static_cast<float>(vec3::dot(fn, axis)));
            for (int a = 0; a < 3; ++a) cur.cone_axis[a] = axis.e[a];
            // Normals spread past about 84 degrees from the axis leave too
            // little to cull, and the test would only cost time.
            if (min_dot > 0.1f) cur.cone_cutoff = std::sqrt(1 - min_dot * min_dot);
        }
        out.meshlets.push_back(cur);
        for (uint32_t i = 0; i < cur.vertex_count; ++i) local[out.meshlet_vertices[cur.vertex_offset + i]] = UINT32_MAX;
        cur = meshlet{};
        cur.vertex_offset = static_cast<uint32_t>(out.meshlet_vertices.size());
        cur.triangle_offset = static_cast<uint32_t>(out.meshlet_triangle_ids.size());
        cur_tris.clear();
    };

    for (uint32_t k : tree.order) {
        const uint32_t* idx = &out.indices[3 * k];
        uint32_t fresh = 0;
        for (int c = 0; c < 3; ++c)
            if (local[idx[c]] == UINT32_MAX && (c < 1 || idx[c] != idx[0]) && (c < 2 || idx[c] != idx[1])) ++fresh;
        if (cur.vertex_count + fresh > meshlet_max_vertices || cur.triangle_count + 1 > meshlet_max_triangles) flush();
        for (int c = 0; c < 3; ++c) {
            if (local[idx[c]] == UINT32_MAX) {
                local[idx[c]] = cur.vertex_count++;
                out.meshlet_vertices.push_back(idx[c]);
            }
            out.meshlet_triangles.push_back(static_cast<uint8_t>(local[idx[c]]));
        }
        out.meshlet_triangle_ids.push_back(k);
        cur_tris.push_back(k);
        ++cur.triangle_count;
    }
    flush();
    return out;
}
