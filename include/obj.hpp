#pragma once
// A small Wavefront OBJ reader: vertex positions and faces, nothing else.
// Texture coordinates, normals, groups and materials in the file are skipped;
// smooth normals are computed here instead, by summing the area-weighted face
// normals around each vertex.
#include "triangle.hpp"

#include <array>
#include <sstream>
#include <string>
#include <vector>

// Triangles from OBJ text, scaled to height and set down with the bottom of
// their bounding box centered on base. Polygons are split into fans.
inline std::vector<triangle> load_obj(const std::string& text, point3 base, double height, const material* mat) {
    std::vector<point3> verts;
    std::vector<std::array<int, 3>> faces;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            double x, y, z;
            ls >> x >> y >> z;
            verts.emplace_back(x, y, z);
        } else if (tag == "f") {
            std::vector<int> idx;
            std::string tok;
            while (ls >> tok) {
                // "7", "7/1" or "7/1/3": only the position index matters.
                int i = std::stoi(tok.substr(0, tok.find('/')));
                idx.push_back(i > 0 ? i - 1 : static_cast<int>(verts.size()) + i);  // Negative: relative
            }
            for (size_t k = 2; k < idx.size(); ++k) faces.push_back({idx[0], idx[k - 1], idx[k]});
        }
    }
    if (verts.empty() || faces.empty()) return {};

    point3 lo = verts[0], hi = verts[0];
    for (const auto& v : verts) {
        lo = point3(std::min(lo.x(), v.x()), std::min(lo.y(), v.y()), std::min(lo.z(), v.z()));
        hi = point3(std::max(hi.x(), v.x()), std::max(hi.y(), v.y()), std::max(hi.z(), v.z()));
    }
    const double scale = height / (hi.y() - lo.y());
    const point3 bottom((lo.x() + hi.x()) * 0.5, lo.y(), (lo.z() + hi.z()) * 0.5);
    for (auto& v : verts) v = base + (v - bottom) * scale;

    // The cross product's length is twice the face area, so larger faces
    // weigh more in the vertex normal without an extra step.
    std::vector<vec3> normals(verts.size());
    for (const auto& f : faces) {
        vec3 n = cross(verts[f[1]] - verts[f[0]], verts[f[2]] - verts[f[0]]);
        for (int i : f) normals[i] = normals[i] + n;
    }

    std::vector<triangle> tris;
    tris.reserve(faces.size());
    for (const auto& f : faces) {
        const point3 &a = verts[f[0]], &b = verts[f[1]], &c = verts[f[2]];
        if (vec3::dot(cross(b - a, c - a), cross(b - a, c - a)) < 1e-24) continue;  // Degenerate
        tris.emplace_back(a, b, c, normals[f[0]].normalize(), normals[f[1]].normalize(), normals[f[2]].normalize(), mat);
    }
    return tris;
}
