// Unit tests for the meshlet builder (include/meshlets.hpp), on the models in
// models/. Run by make test.
#include "meshlets.hpp"
#include "renderer.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream b;
    b << f.rdbuf();
    return b.str();
}

void test_model(const std::string& name, const std::string& path, bool expect_closed) {
    geometry world;
    const material* m = world.own(std::make_shared<lambertian>(color(1, 1, 1)));
    load_obj(world, read(path), point3(0, 0, 0), 1, m);
    const std::vector<triangle>& tris = world.triangles;
    const meshlet_mesh mm = build_meshlets(tris);

    // Every triangle in exactly one meshlet, with its corners in order.
    std::vector<int> seen(tris.size(), 0);
    bool limits = true, corners = true, bounded = true, coned = true;
    for (const meshlet& ml : mm.meshlets) {
        limits &= ml.vertex_count <= meshlet_max_vertices && ml.triangle_count <= meshlet_max_triangles &&
                  ml.triangle_count > 0;
        for (uint32_t t = 0; t < ml.triangle_count; ++t) {
            const uint32_t k = mm.meshlet_triangle_ids[ml.triangle_offset + t];
            ++seen[k];
            for (int c = 0; c < 3; ++c) {
                const uint8_t local = mm.meshlet_triangles[3 * (ml.triangle_offset + t) + c];
                corners &= local < ml.vertex_count &&
                           mm.meshlet_vertices[ml.vertex_offset + local] == mm.indices[3 * k + c];
            }
            // The cone holds every face normal.
            if (ml.cone_cutoff < 1) {
                const vec3 fn = cross(tris[k].e1, tris[k].e2);
                if (vec3::dot(fn, fn) > 1e-30f) {
                    const vec3 axis(ml.cone_axis[0], ml.cone_axis[1], ml.cone_axis[2]);
                    const float min_dot = std::sqrt(1 - ml.cone_cutoff * ml.cone_cutoff);
                    coned &= vec3::dot(fn.normalize(), axis) >= min_dot - 1e-4f;
                }
            }
        }
        // The sphere holds every vertex.
        for (uint32_t i = 0; i < ml.vertex_count; ++i) {
            const float* p = &mm.positions[3 * mm.meshlet_vertices[ml.vertex_offset + i]];
            float d2 = 0;
            for (int a = 0; a < 3; ++a) d2 += (p[a] - ml.center[a]) * (p[a] - ml.center[a]);
            bounded &= std::sqrt(d2) <= ml.radius * 1.0001f + 1e-6f;
        }
    }
    bool once = true;
    for (int s : seen) once &= s == 1;

    // The indexed mesh reproduces the triangles.
    bool same = true;
    for (size_t k = 0; k < tris.size() && same; ++k) {
        const float* p = &mm.positions[3 * mm.indices[3 * k]];
        same = p[0] == tris[k].p0.x() && p[1] == tris[k].p0.y() && p[2] == tris[k].p0.z();
    }

    size_t cones = 0;
    for (const meshlet& ml : mm.meshlets) cones += ml.cone_cutoff < 1;
    std::printf("     %s: %zu triangles, %zu vertices, %zu meshlets (%.1f triangles each), %zu with a usable cone, %s\n",
                name.c_str(), tris.size(), mm.positions.size() / 3, mm.meshlets.size(),
                double(tris.size()) / mm.meshlets.size(), cones, mm.closed ? "closed" : "open");
    check(once, name + ": every triangle in exactly one meshlet");
    check(limits, name + ": at most 64 vertices and 124 triangles a meshlet");
    check(corners, name + ": meshlet triangles index the right mesh vertices, in order");
    check(bounded, name + ": bounding spheres hold their vertices");
    check(coned, name + ": normal cones hold their face normals");
    check(same, name + ": indexed positions match the triangles");
    check(mm.closed == expect_closed, name + (expect_closed ? ": closed" : ": open"));
}

}  // namespace

int main() {
    test_model("teapot", "models/teapot.obj", false);
    test_model("bunny", "models/stanford-bunny.obj", false);
    test_model("spot", "models/spot/spot.obj", true);
    return failures ? 1 : 0;
}
