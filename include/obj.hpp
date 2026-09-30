#pragma once
// A small Wavefront OBJ reader: positions, texture coordinates, normals and
// faces, and materials from the .mtl files the OBJ names.
//
// Where every corner of a face gives a normal, those are used; otherwise
// smooth normals are computed by summing the area-weighted face normals
// around each vertex. Polygons are split into fans.
//
// Materials map onto this renderer's four kinds, following the MTL
// illumination models loosely: an emissive color (Ke) makes a light; a
// dissolve below 1 (d, or Tr) or a glass illumination model (4, 6, 7, 9) makes
// glass with index of refraction Ni; a reflection model (3, 5) makes metal,
// tinted by Ks and blurred by a fuzz derived from the shininess Ns; anything
// else is matte, colored by Kd times the map_Kd texture if there is one.
// Textures may be PNG or PPM. Groups, smoothing groups and the rest of MTL
// are skipped.
#include "geometry.hpp"
#include "image_io.hpp"
#include "texture.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Returns a file's contents by a path relative to the OBJ's directory, or ""
// if there is no such file. An empty function reads nothing, so materials are
// skipped (the browser demo passes none).
using file_reader = std::function<std::string(const std::string&)>;

namespace obj_detail {

struct mtl {
    color kd = color(0.8, 0.8, 0.8), ks = color(0, 0, 0), ke = color(0, 0, 0);
    double ns = 0, ni = 1.5, d = 1;
    int illum = 2;
    std::string map_kd;
};

inline std::string dir_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

inline void read_mtl(const std::string& text, const std::string& dir, std::map<std::string, mtl>& out) {
    std::istringstream in(text);
    std::string line;
    mtl* cur = nullptr;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "newmtl") {
            std::string name;
            ls >> name;
            cur = &out[name];
            *cur = mtl();
        } else if (!cur) {
            continue;
        } else if (tag == "Kd" || tag == "Ks" || tag == "Ke") {
            double r, g, b;
            if (!(ls >> r)) continue;
            if (!(ls >> g >> b)) g = b = r;  // One value means gray
            (tag == "Kd" ? cur->kd : tag == "Ks" ? cur->ks : cur->ke) = color(r, g, b);
        } else if (tag == "Ns") {
            ls >> cur->ns;
        } else if (tag == "Ni") {
            ls >> cur->ni;
        } else if (tag == "d") {
            ls >> cur->d;
        } else if (tag == "Tr") {
            double tr;
            if (ls >> tr) cur->d = 1 - tr;
        } else if (tag == "illum") {
            ls >> cur->illum;
        } else if (tag == "map_Kd") {
            // Options such as -s 1 1 1 come before the file name.
            std::string tok, last;
            while (ls >> tok) last = tok;
            if (!last.empty()) cur->map_kd = dir + last;
        }
    }
}

inline const material* make_material(geometry& world, const mtl& m, const file_reader& read) {
    auto nonzero = [](const color& c) { return c.x() > 0 || c.y() > 0 || c.z() > 0; };
    if (nonzero(m.ke)) return world.own(std::make_shared<diffuse_light>(m.ke));
    if (m.d < 1 || m.illum == 4 || m.illum == 6 || m.illum == 7 || m.illum == 9)
        return world.own(std::make_shared<dielectric>(m.ni > 1 ? m.ni : 1.5));
    if (m.illum == 3 || m.illum == 5) {
        // Phong shininess to a blur: sharp highlights (large Ns) are near
        // mirrors. sqrt(2 / (Ns + 2)) is the usual roughness equivalent.
        const double fuzz = std::min(1.0, std::sqrt(2 / (m.ns + 2)));
        return world.own(std::make_shared<metal>(nonzero(m.ks) ? m.ks : m.kd, fuzz));
    }
    std::shared_ptr<const image_texture> tex;
    if (!m.map_kd.empty()) {
        const std::string bytes = read(m.map_kd);
        if (!bytes.empty()) tex = std::make_shared<image_texture>(decode_image(bytes));
    }
    return world.own(std::make_shared<lambertian>(m.kd, tex));
}

}  // namespace obj_detail

// Adds the triangles of OBJ text to world, turned turn_deg about the vertical
// axis, scaled to height and set down with the bottom of their bounding box
// centered on base. Faces with no material, or a material the reader cannot
// find, get fallback.
inline void load_obj(geometry& world, const std::string& text, point3 base, double height, const material* fallback,
                     const file_reader& read = {}, double turn_deg = 0) {
    using namespace obj_detail;
    std::vector<point3> verts;
    std::vector<std::array<real, 2>> uvs;
    std::vector<vec3> file_normals;
    struct corner { int v, vt, vn; };  // Indices, -1 where the file gave none
    struct face { corner c[3]; const material* mat; };
    std::vector<face> faces;
    std::map<std::string, mtl> library;
    std::map<std::string, const material*> made;
    const material* current = fallback;

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
        } else if (tag == "vt") {
            double u = 0, v = 0;
            ls >> u >> v;
            uvs.push_back({static_cast<real>(u), static_cast<real>(v)});
        } else if (tag == "vn") {
            double x, y, z;
            ls >> x >> y >> z;
            file_normals.emplace_back(x, y, z);
        } else if (tag == "mtllib" && read) {
            std::string name;
            while (ls >> name) read_mtl(read(name), dir_of(name), library);
        } else if (tag == "usemtl") {
            std::string name;
            ls >> name;
            const auto found = library.find(name);
            if (found == library.end()) {
                current = fallback;
            } else {
                auto& m = made[name];
                if (!m) m = make_material(world, found->second, read);
                current = m;
            }
        } else if (tag == "f") {
            // Each corner is "v", "v/vt", "v//vn" or "v/vt/vn"; negative
            // indices count back from the end of their list so far.
            auto index = [](const std::string& s, size_t count) {
                if (s.empty()) return -1;
                const int i = std::stoi(s);
                return i > 0 ? i - 1 : static_cast<int>(count) + i;
            };
            std::vector<corner> poly;
            std::string tok;
            while (ls >> tok) {
                const size_t s1 = tok.find('/'), s2 = s1 == std::string::npos ? s1 : tok.find('/', s1 + 1);
                corner c;
                c.v = index(tok.substr(0, s1), verts.size());
                c.vt = s1 == std::string::npos ? -1 : index(tok.substr(s1 + 1, s2 - s1 - 1), uvs.size());
                c.vn = s2 == std::string::npos ? -1 : index(tok.substr(s2 + 1), file_normals.size());
                poly.push_back(c);
            }
            for (size_t k = 2; k < poly.size(); ++k) faces.push_back({{poly[0], poly[k - 1], poly[k]}, current});
        }
    }
    if (verts.empty() || faces.empty()) return;

    const double turn = turn_deg * M_PI / 180, c_turn = std::cos(turn), s_turn = std::sin(turn);
    auto rotate = [&](const vec3& p) {
        return vec3(c_turn * p.x() + s_turn * p.z(), p.y(), -s_turn * p.x() + c_turn * p.z());
    };
    for (auto& v : verts) v = rotate(v);
    for (auto& n : file_normals) n = rotate(n);

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
    std::vector<vec3> smooth(verts.size());
    for (const auto& f : faces) {
        vec3 n = cross(verts[f.c[1].v] - verts[f.c[0].v], verts[f.c[2].v] - verts[f.c[0].v]);
        for (const corner& c : f.c) smooth[c.v] = smooth[c.v] + n;
    }

    world.triangles.reserve(world.triangles.size() + faces.size());
    for (const auto& f : faces) {
        const point3 &a = verts[f.c[0].v], &b = verts[f.c[1].v], &c = verts[f.c[2].v];
        if (vec3::dot(cross(b - a, c - a), cross(b - a, c - a)) < 1e-24) continue;  // Degenerate
        const bool from_file = f.c[0].vn >= 0 && f.c[1].vn >= 0 && f.c[2].vn >= 0;
        vec3 n[3];
        for (int k = 0; k < 3; ++k) n[k] = (from_file ? file_normals[f.c[k].vn] : smooth[f.c[k].v]).normalize();
        triangle t(a, b, c, n[0], n[1], n[2], f.mat);
        for (int k = 0; k < 3; ++k)
            if (f.c[k].vt >= 0) { t.uv[2 * k] = uvs[f.c[k].vt][0]; t.uv[2 * k + 1] = uvs[f.c[k].vt][1]; }
        world.triangles.push_back(t);
    }
}
