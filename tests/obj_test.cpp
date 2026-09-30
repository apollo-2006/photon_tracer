// Unit tests for the OBJ and MTL reader (include/obj.hpp). Run by make test.
#include "renderer.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

bool near(real a, real b, real eps = real(1e-4)) { return std::fabs(a - b) < eps; }
bool near(const vec3& a, const vec3& b, real eps = real(1e-4)) {
    return near(a.x(), b.x(), eps) && near(a.y(), b.y(), eps) && near(a.z(), b.z(), eps);
}

}  // namespace

int main() {
    // A unit quad in the xy plane (split into two triangles), a triangle
    // given with negative indices, and one with normals from the file, each
    // with its own material from an MTL "file" read through the reader.
    const std::string obj =
        "mtllib parts/materials.mtl\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 0 1\n"
        "usemtl textured\n"
        "f 1/1 2/2 3/3 4/4\n"
        "usemtl glass\n"
        "f -4 -3 -2\n"
        "usemtl lamp\n"
        "f 1//1 2//1 4//1\n"
        "usemtl shiny\n"
        "f 2 3 4\n"
        "usemtl nosuch\n"
        "f 1 3 4\n";
    const std::map<std::string, std::string> files = {
        {"parts/materials.mtl",
         "newmtl textured\nKd 0.5 0.5 0.5\nmap_Kd -s 1 1 1 checker.ppm\n"
         "newmtl glass\nNi 1.33\nd 0.2\n"
         "newmtl lamp\nKe 4 3 2\n"
         "newmtl shiny\nillum 3\nKs 0.9 0.8 0.7\nNs 1998\n"},
        // A 2x2 texture, white except the top-left texel, which is red. The
        // path is relative to the MTL's directory.
        {"parts/checker.ppm", "P3\n2 2\n255\n255 0 0  255 255 255\n255 255 255  255 255 255\n"},
    };
    const file_reader read = [&](const std::string& name) {
        const auto f = files.find(name);
        return f == files.end() ? std::string() : f->second;
    };

    geometry world;
    const material* fallback = world.own(std::make_shared<lambertian>(color(0, 1, 0)));
    // Height 1 and base at the origin leave the unit quad where it is, but
    // centered on x: x runs from -0.5 to 0.5.
    load_obj(world, obj, point3(0, 0, 0), 1, fallback, read);

    check(world.triangles.size() == 6, "quad split into two triangles, plus four more");
    const triangle& t0 = world.triangles[0];
    check(near(t0.p0, point3(-0.5, 0, 0)) && near(t0.p0 + t0.e1, point3(0.5, 0, 0)), "positions recentered and scaled");
    check(near(t0.uv[2], 1) && near(t0.uv[3], 0) && near(t0.uv[4], 1) && near(t0.uv[5], 1), "texture coordinates per corner");

    auto* textured = dynamic_cast<const lambertian*>(t0.mat);
    check(textured && textured->texture, "map_Kd found beside the MTL, with options before the name");
    if (textured && textured->texture) {
        // The top-left texel is at (u, v) = (0.25, 0.75); red there, white
        // at the bottom right; Kd scales both.
        const color red = textured->texture->sample(0.25f, 0.75f), white = textured->texture->sample(0.75f, 0.25f);
        check(near(red, color(1, 0, 0), real(1e-3)) && near(white, color(1, 1, 1), real(1e-3)), "texture v runs up the image");
        check(near(textured->albedo, color(0.5, 0.5, 0.5)), "Kd kept alongside the texture");
    }

    auto* glass = dynamic_cast<const dielectric*>(world.triangles[2].mat);
    check(glass && near(glass->ior, real(1.33)), "d below 1 makes glass with Ni");
    // -4 -3 -2 of four vertices are the first three.
    const triangle& t2 = world.triangles[2];
    check(near(t2.p0, point3(-0.5, 0, 0)) && near(t2.p0 + t2.e1, point3(0.5, 0, 0)) &&
              near(t2.p0 + t2.e2, point3(0.5, 1, 0)),
          "negative indices count back from the end");

    check(world.triangles[3].mat->emits() && near(world.triangles[3].mat->emission, color(4, 3, 2)), "Ke makes a light");
    check(near(world.triangles[3].n0, vec3(0, 0, 1)), "normals from the file");

    auto* shiny = dynamic_cast<const metal*>(world.triangles[4].mat);
    check(shiny && near(shiny->albedo, color(0.9, 0.8, 0.7)) && shiny->fuzz < real(0.05), "illum 3 makes metal, sharp for large Ns");

    check(world.triangles[5].mat == fallback, "an unknown material falls back");

    // Without a reader, materials are skipped and everything gets the fallback.
    geometry plain;
    load_obj(plain, obj, point3(0, 0, 0), 1, fallback);
    bool all_fallback = !plain.triangles.empty();
    for (const auto& t : plain.triangles) all_fallback = all_fallback && t.mat == fallback;
    check(all_fallback, "no reader: every face gets the fallback");

    // Turning 90 degrees about y takes +x to -z.
    geometry turned;
    load_obj(turned, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", point3(0, 0, 0), 1, fallback, {}, 90);
    const triangle& tt = turned.triangles[0];
    check(near(tt.e1, vec3(0, 0, -1)), "turn rotates about the vertical axis");

    return failures ? 1 : 0;
}
