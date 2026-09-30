// photon_tracer_gpu: the crowd scene, rasterized with mesh shaders and path
// traced with hardware ray queries, on Vulkan.
//
// For each sample, task shaders cull the meshlets of every instance against
// the view (and, on closed meshes, by normal cone), mesh shaders rasterize
// what is left into a visibility buffer that records which instance and
// triangle each pixel sees, and a compute pass starts every path there: it
// intersects that one triangle again for the exact hit, then traces the
// bounces with ray queries against a bottom-level acceleration structure for
// the mesh and a top-level one over its instances, the same two-level split
// as the CPU renderer's instancing. The scene, materials and camera come from
// include/renderer.hpp, so the images can be checked against the CPU
// renderer's, and are, by tests/render_test.py.
//
// Headless: the result is written as PPM or PFM, like photon_tracer.
#include "image_write.hpp"
#include "meshlets.hpp"
#include "renderer.hpp"
#include "vk.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

const uint32_t cull_task_spv[] = {
#include "cull.task.inc"
};
const uint32_t draw_mesh_spv[] = {
#include "draw.mesh.inc"
};
const uint32_t visibility_frag_spv[] = {
#include "visibility.frag.inc"
};
const uint32_t trace_comp_spv[] = {
#include "trace.comp.inc"
};

constexpr uint32_t max_textures = 16;

// Laid out as gpu/shaders/common.glsl declares them (scalar layout).
struct gpu_instance {
    float to_world[3][4];
    float to_object[3][4];
    uint32_t material;
    float scale;
    uint32_t pad0, pad1;
};
struct gpu_material {
    float albedo[3];
    float type;
    float fuzz, ior, texture, pad;
    float emission[3];
    float pad2;
};
struct gpu_sphere {
    float center[3];
    float radius;
    uint32_t material, pad0, pad1, pad2;
};
struct gpu_frame {
    float view_proj[16];  // Column major, as GLSL reads a mat4
    float origin[4], corner[4], across[4], up[4];
    float planes[5][4];
    uint32_t meshlet_count, instance_count, sphere_count, flags;
    uint32_t width, height, max_bounces, seed;
};
struct gpu_sample {
    float jitter[2], jitter_ndc[2];
    uint32_t index;
};
struct gpu_stats {
    uint32_t visible_meshlets, pad;
    uint64_t rays;
};

void rows_of(const affine& a, float out[3][4]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out[r][c] = a.m[r][c];
        out[r][3] = a.t.e[r];
    }
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream b;
    b << f.rdbuf();
    return b.str();
}

// World to clip space for the camera, at pixel centers. The CPU renderer's
// sample at (u, v) looks through corner + u * across + v * up, with
// u = (i + du) / (width - 1) and v = (j + dv) / (height - 1) for pixel column i
// and row j counted from the bottom. Writing a world point's direction from
// the camera in the basis (across, up, corner - origin) as (x, y, z) gives
// u = x / z and v = y / z, and the pixel that sample lands in follows; so the
// rasterizer covers exactly the pixel the tracer's ray for it goes through.
// Depth is near / z, reversed: 1 at the near plane, falling toward 0.
std::array<double, 16> view_projection(const camera& cam, int w, int h, double near_z) {
    const vec3 cols[3] = {cam.across(), cam.up(), cam.corner() - cam.position()};
    // The inverse of the basis matrix, whose columns are cols.
    double b[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) b[r][c] = cols[c].e[r];
    const double det = b[0][0] * (b[1][1] * b[2][2] - b[1][2] * b[2][1]) - b[0][1] * (b[1][0] * b[2][2] - b[1][2] * b[2][0]) +
                       b[0][2] * (b[1][0] * b[2][1] - b[1][1] * b[2][0]);
    double inv[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
            inv[r][c] = (b[r1][c1] * b[r2][c2] - b[r1][c2] * b[r2][c1]) / det;
        }
    // (x, y, z) = inv * (P - origin), as rows over (P, 1).
    double to_basis[3][4];
    const point3 o = cam.position();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) to_basis[r][c] = inv[r][c];
        to_basis[r][3] = -(inv[r][0] * o.x() + inv[r][1] * o.y() + inv[r][2] * o.z());
    }
    // Clip from (x, y, z, 1): at the pixel centers (du = dv = 1/2),
    //   x_ndc = 2 u (w - 1) / w - 1   and   y_ndc = 1 - 2 v (h - 1) / h,
    // Vulkan's y running down; times z, with z as w.
    const double cx[4] = {2.0 * (w - 1) / w, 0, -1, 0};
    const double cy[4] = {0, -2.0 * (h - 1) / h, 1, 0};
    const double cz[4] = {0, 0, 0, near_z};
    const double cw[4] = {0, 0, 1, 0};
    const double* clip_rows[4] = {cx, cy, cz, cw};
    std::array<double, 16> m{};  // Row major here
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += clip_rows[r][k] * to_basis[k][c];
            if (c == 3) v += clip_rows[r][3];
            m[4 * r + c] = v;
        }
    return m;
}

struct accel {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    vk::buffer storage;
    VkDeviceAddress address = 0;
};

// Builds an acceleration structure over geometry and waits for it.
accel build_accel(vk::context& ctx, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geom,
                  uint32_t primitives) {
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = type;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geom;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    ctx.as_build_sizes(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);

    accel a;
    a.storage = ctx.make_buffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = a.storage.handle;
    ci.size = sizes.accelerationStructureSize;
    ci.type = type;
    VK_CHECK(ctx.create_as(ctx.device, &ci, nullptr, &a.handle));

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &asp;
    vkGetPhysicalDeviceProperties2(ctx.gpu, &p2);
    const VkDeviceSize align = asp.minAccelerationStructureScratchOffsetAlignment;
    vk::buffer scratch = ctx.make_buffer(sizes.buildScratchSize + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    info.dstAccelerationStructure = a.handle;
    info.scratchData.deviceAddress = (scratch.address + align - 1) / align * align;
    VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    ctx.submit([&](VkCommandBuffer cmd) { ctx.build_as(cmd, 1, &info, &ranges); });
    ctx.destroy(scratch);

    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = a.handle;
    a.address = ctx.as_address(ctx.device, &ai);
    return a;
}

void barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags src_access, VkPipelineStageFlags dst,
             VkAccessFlags dst_access) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void image_layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout to, VkPipelineStageFlags dst,
                  VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

int main(int argc, char** argv) {
    int crowd_side = 20, spp = 50, width = 1920;
    uint64_t seed = (uint64_t(std::random_device{}()) << 32) ^ std::random_device{}();
    std::string obj_path = "models/teapot.obj", out_path = "render.ppm";
    double turn_deg = 0;
    bool validate = false, cone_culling = true;
    for (int a = 1; a < argc; ++a) {
        const std::string arg = argv[a];
        if (arg == "--crowd") {
            if (a + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[a + 1][0]))) crowd_side = std::stoi(argv[++a]);
        } else if (arg == "--obj" && a + 1 < argc) obj_path = argv[++a];
        else if (arg == "--turn" && a + 1 < argc) turn_deg = std::stod(argv[++a]);
        else if (arg == "--spp" && a + 1 < argc) spp = std::stoi(argv[++a]);
        else if (arg == "--width" && a + 1 < argc) width = std::stoi(argv[++a]);
        else if (arg == "--seed" && a + 1 < argc) seed = std::stoull(argv[++a]);
        else if (arg == "--out" && a + 1 < argc) out_path = argv[++a];
        else if (arg == "--validate") validate = true;
        else if (arg == "--no-cone-culling") cone_culling = false;
        else {
            std::cerr << "usage: " << argv[0] << " [--crowd [N]] [--obj PATH [--turn DEG]] [--spp N] [--width N] [--seed N]"
                      << " [--out PATH] [--no-cone-culling] [--validate]\n"
                      << "Renders the crowd scene on the GPU: mesh shaders for what the camera sees first,\n"
                      << "ray queries for everything after.\n";
            return 2;
        }
    }
    const int height = static_cast<int>(width / (16.0 / 9.0));
    const int max_bounces = 10;

    try {
        // The scene, exactly as the CPU renderer builds it.
        const std::string obj_text = read_file(obj_path);
        if (obj_text.empty()) throw std::runtime_error("could not read " + obj_path);
        const std::string obj_dir = obj_path.find('/') == std::string::npos ? "" : obj_path.substr(0, obj_path.find_last_of('/') + 1);
        const file_reader read = [&obj_dir](const std::string& name) { return read_file(obj_dir + name); };
        const auto t_scene = std::chrono::steady_clock::now();
        const geometry world = build_world(scene_id::crowd, true, obj_text, read, turn_deg, crowd_side, false);
        const camera cam = make_camera(scene_id::crowd);
        if (world.meshes.size() != 1) throw std::runtime_error("expected the crowd to be one instanced mesh");
        const auto& mesh_tris = world.meshes[0].triangles;
        const meshlet_mesh mm = build_meshlets(mesh_tris);

        // Materials and textures, numbered in the order first seen.
        std::map<const material*, uint32_t> material_index;
        std::map<const image_texture*, uint32_t> texture_index;
        std::vector<gpu_material> materials;
        std::vector<const image_texture*> textures;
        auto index_of = [&](const material* m) -> uint32_t {
            auto found = material_index.find(m);
            if (found != material_index.end()) return found->second;
            gpu_material g{};
            g.texture = -1;
            g.ior = 1.5f;
            if (auto* l = dynamic_cast<const lambertian*>(m)) {
                g.type = 0;
                for (int c = 0; c < 3; ++c) g.albedo[c] = l->albedo.e[c];
                if (l->texture) {
                    auto t = texture_index.find(l->texture.get());
                    if (t == texture_index.end()) {
                        if (textures.size() == max_textures) throw std::runtime_error("more than 16 textures");
                        t = texture_index.emplace(l->texture.get(), static_cast<uint32_t>(textures.size())).first;
                        textures.push_back(l->texture.get());
                    }
                    g.texture = static_cast<float>(t->second);
                }
            } else if (auto* me = dynamic_cast<const metal*>(m)) {
                g.type = 1;
                for (int c = 0; c < 3; ++c) g.albedo[c] = me->albedo.e[c];
                g.fuzz = me->fuzz;
            } else if (auto* d = dynamic_cast<const dielectric*>(m)) {
                g.type = 2;
                g.ior = d->ior;
            } else {
                g.type = 3;
            }
            for (int c = 0; c < 3; ++c) g.emission[c] = m->emission.e[c];
            const uint32_t k = static_cast<uint32_t>(materials.size());
            materials.push_back(g);
            material_index.emplace(m, k);
            return k;
        };
        std::vector<uint32_t> triangle_materials;
        for (const triangle& t : mesh_tris) triangle_materials.push_back(index_of(t.mat));
        std::vector<gpu_instance> instances;
        for (const auto& in : world.instances) {
            gpu_instance g{};
            rows_of(in.to_world, g.to_world);
            rows_of(in.to_object, g.to_object);
            g.material = in.mat ? index_of(in.mat) : ~0u;
            // The longest column: how much the instance scales a radius.
            float s = 0;
            for (int c = 0; c < 3; ++c)
                s = std::max(s, std::sqrt(in.to_world.m[0][c] * in.to_world.m[0][c] + in.to_world.m[1][c] * in.to_world.m[1][c] +
                                          in.to_world.m[2][c] * in.to_world.m[2][c]));
            g.scale = s;
            instances.push_back(g);
        }
        std::vector<gpu_sphere> spheres;
        for (const auto& s : world.large_spheres)
            spheres.push_back({{s.center.x(), s.center.y(), s.center.z()}, s.radius, index_of(s.mat), 0, 0, 0});
        if (!world.spheres.empty()) throw std::runtime_error("the GPU renderer draws no small spheres");
        std::vector<uint32_t> packed_triangles(mm.meshlet_triangles.size() / 3);
        for (size_t k = 0; k < packed_triangles.size(); ++k)
            packed_triangles[k] = mm.meshlet_triangles[3 * k] | (mm.meshlet_triangles[3 * k + 1] << 8) |
                                  (mm.meshlet_triangles[3 * k + 2] << 16);
        const double scene_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_scene).count();

        vk::context ctx(validate);
        std::cerr << "GPU: " << ctx.device_name << "\n";
        std::cerr << "Scene built in " << scene_s << " s: " << instances.size() << " instances of " << mesh_tris.size()
                  << " triangles, " << mm.meshlets.size() << " meshlets each, mesh " << (mm.closed ? "closed" : "open")
                  << (mm.closed && cone_culling ? " (cone culling on)" : " (no cone culling)") << "\n";

        // Buffers.
        const VkBufferUsageFlags ssbo = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        const VkBufferUsageFlags as_input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        vk::buffer b_meshlets = ctx.upload(mm.meshlets, ssbo);
        vk::buffer b_meshlet_vertices = ctx.upload(mm.meshlet_vertices, ssbo);
        vk::buffer b_meshlet_triangles = ctx.upload(packed_triangles, ssbo);
        vk::buffer b_meshlet_ids = ctx.upload(mm.meshlet_triangle_ids, ssbo);
        vk::buffer b_positions = ctx.upload(mm.positions, ssbo | as_input);
        vk::buffer b_normals = ctx.upload(mm.normals, ssbo);
        vk::buffer b_uvs = ctx.upload(mm.uvs, ssbo);
        vk::buffer b_indices = ctx.upload(mm.indices, ssbo | as_input);
        vk::buffer b_instances = ctx.upload(instances, ssbo);
        vk::buffer b_materials = ctx.upload(materials, ssbo);
        vk::buffer b_triangle_materials = ctx.upload(triangle_materials, ssbo);
        vk::buffer b_spheres = ctx.upload(spheres, ssbo);
        vk::buffer b_accum = ctx.make_buffer(sizeof(float) * 4 * width * height,
                                             ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
        vk::buffer b_readback = ctx.make_buffer(b_accum.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        vk::buffer b_stats = ctx.make_buffer(sizeof(gpu_stats), ssbo | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        std::memset(b_stats.mapped, 0, sizeof(gpu_stats));

        // Acceleration structures: one bottom level for the mesh, as the
        // indexed triangles, and a top level over the instances.
        const auto t_as = std::chrono::steady_clock::now();
        VkAccelerationStructureGeometryKHR tri_geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tri_geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        tri_geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tris = tri_geom.geometry.triangles;
        tris.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tris.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tris.vertexData.deviceAddress = b_positions.address;
        tris.vertexStride = 3 * sizeof(float);
        tris.maxVertex = static_cast<uint32_t>(mm.positions.size() / 3 - 1);
        tris.indexType = VK_INDEX_TYPE_UINT32;
        tris.indexData.deviceAddress = b_indices.address;
        accel blas = build_accel(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, tri_geom,
                                 static_cast<uint32_t>(mm.indices.size() / 3));

        std::vector<VkAccelerationStructureInstanceKHR> as_instances(instances.size());
        for (size_t k = 0; k < instances.size(); ++k) {
            auto& ai = as_instances[k];
            std::memcpy(ai.transform.matrix, instances[k].to_world, sizeof ai.transform.matrix);
            ai.instanceCustomIndex = static_cast<uint32_t>(k);
            ai.mask = 0xFF;
            ai.instanceShaderBindingTableRecordOffset = 0;
            // Two-sided, as the CPU renderer's triangles are.
            ai.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            ai.accelerationStructureReference = blas.address;
        }
        vk::buffer b_as_instances = ctx.upload(as_instances, as_input);
        VkAccelerationStructureGeometryKHR inst_geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        inst_geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        inst_geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        inst_geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        inst_geom.geometry.instances.data.deviceAddress = b_as_instances.address;
        accel tlas = build_accel(ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, inst_geom,
                                 static_cast<uint32_t>(as_instances.size()));
        const double as_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_as).count();

        // Images: the visibility buffer (instance + 1, triangle) and depth.
        vk::image vis = ctx.make_image(width, height, VK_FORMAT_R32G32_UINT,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        vk::image depth = ctx.make_image(width, height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         VK_IMAGE_ASPECT_DEPTH_BIT);

        // Textures, as the sRGB bytes they are, which the tracer decodes and
        // filters itself (see sample_texture() in trace.comp), and a white
        // one to fill the unused slots.
        std::vector<vk::image> tex_images;
        VkSampler sampler;
        {
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            VK_CHECK(vkCreateSampler(ctx.device, &sci, nullptr, &sampler));
        }
        auto add_texture = [&](int w, int h, const std::vector<uint8_t>& rgba) {
            vk::image im = ctx.make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            vk::buffer staging = ctx.make_buffer(rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
            std::memcpy(staging.mapped, rgba.data(), rgba.size());
            ctx.submit([&](VkCommandBuffer cmd) {
                image_layout(cmd, im.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
                vkCmdCopyBufferToImage(cmd, staging.handle, im.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = im.handle;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &b);
            });
            ctx.destroy(staging);
            tex_images.push_back(im);
        };
        for (const image_texture* t : textures) {
            std::vector<uint8_t> rgba(4 * static_cast<size_t>(t->source.width) * t->source.height, 255);
            for (size_t p = 0; p < rgba.size() / 4; ++p)
                for (int c = 0; c < 3; ++c) rgba[4 * p + c] = t->source.rgb[3 * p + c];
            add_texture(t->source.width, t->source.height, rgba);
        }
        add_texture(1, 1, {255, 255, 255, 255});

        // Frame constants.
        gpu_frame frame{};
        const auto vp = view_projection(cam, width, height, 1e-3);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) frame.view_proj[4 * c + r] = static_cast<float>(vp[4 * r + c]);
        auto put = [](float* dst, const vec3& v) { dst[0] = v.x(); dst[1] = v.y(); dst[2] = v.z(); dst[3] = 0; };
        put(frame.origin, cam.position());
        put(frame.corner, cam.corner());
        put(frame.across, cam.across());
        put(frame.up, cam.up());
        // Frustum planes from the clip rows (Gribb and Hartmann), widened by
        // two pixels on each side for the jitter: w + x >= 0 and so on,
        // and z >= near for the near plane, normalized to world distances.
        const double mx = 1 + 4.0 / width, my = 1 + 4.0 / height;
        const double plane_rows[5][4] = {
            {vp[12] * mx + vp[0], vp[13] * mx + vp[1], vp[14] * mx + vp[2], vp[15] * mx + vp[3]},
            {vp[12] * mx - vp[0], vp[13] * mx - vp[1], vp[14] * mx - vp[2], vp[15] * mx - vp[3]},
            {vp[12] * my + vp[4], vp[13] * my + vp[5], vp[14] * my + vp[6], vp[15] * my + vp[7]},
            {vp[12] * my - vp[4], vp[13] * my - vp[5], vp[14] * my - vp[6], vp[15] * my - vp[7]},
            {vp[12] - vp[8], vp[13] - vp[9], vp[14] - vp[10], vp[15] - vp[11]},
        };
        for (int k = 0; k < 5; ++k) {
            const double len = std::sqrt(plane_rows[k][0] * plane_rows[k][0] + plane_rows[k][1] * plane_rows[k][1] +
                                         plane_rows[k][2] * plane_rows[k][2]);
            for (int c = 0; c < 4; ++c) frame.planes[k][c] = static_cast<float>(plane_rows[k][c] / len);
        }
        frame.meshlet_count = static_cast<uint32_t>(mm.meshlets.size());
        frame.instance_count = static_cast<uint32_t>(instances.size());
        frame.sphere_count = static_cast<uint32_t>(spheres.size());
        frame.flags = mm.closed && cone_culling ? 1u : 0u;
        frame.width = width;
        frame.height = height;
        frame.max_bounces = max_bounces;
        frame.seed = static_cast<uint32_t>(seed ^ (seed >> 32));
        vk::buffer b_frame = ctx.upload(&frame, sizeof frame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        // One descriptor set for both pipelines.
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        auto bind = [&](uint32_t b, VkDescriptorType type, uint32_t count = 1) {
            bindings.push_back({b, type, count, VK_SHADER_STAGE_ALL, nullptr});
        };
        bind(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        for (uint32_t b = 1; b <= 12; ++b) bind(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bind(13, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
        bind(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        bind(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bind(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bind(17, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures);
        VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dlci.bindingCount = static_cast<uint32_t>(bindings.size());
        dlci.pBindings = bindings.data();
        VkDescriptorSetLayout set_layout;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &dlci, nullptr, &set_layout));

        const VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14},
            {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, max_textures},
        };
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1;
        dpci.poolSizeCount = static_cast<uint32_t>(std::size(pool_sizes));
        dpci.pPoolSizes = pool_sizes;
        VkDescriptorPool dpool;
        VK_CHECK(vkCreateDescriptorPool(ctx.device, &dpci, nullptr, &dpool));
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = dpool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &set_layout;
        VkDescriptorSet set;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device, &dsai, &set));

        std::vector<VkDescriptorBufferInfo> buffer_infos;
        buffer_infos.reserve(16);
        std::vector<VkWriteDescriptorSet> writes;
        auto write_buffer = [&](uint32_t b, VkDescriptorType type, const vk::buffer& buf) {
            buffer_infos.push_back({buf.handle, 0, VK_WHOLE_SIZE});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pBufferInfo = &buffer_infos.back();
            writes.push_back(w);
        };
        write_buffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b_frame);
        const vk::buffer* storage[] = {&b_meshlets, &b_meshlet_vertices, &b_meshlet_triangles, &b_meshlet_ids,
                                       &b_positions, &b_normals, &b_uvs, &b_indices, &b_instances, &b_materials,
                                       &b_triangle_materials, &b_spheres};
        for (uint32_t k = 0; k < 12; ++k) write_buffer(k + 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, *storage[k]);
        write_buffer(15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b_accum);
        write_buffer(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, b_stats);
        VkWriteDescriptorSetAccelerationStructureKHR as_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &tlas.handle;
        {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.pNext = &as_write;
            w.dstSet = set;
            w.dstBinding = 13;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes.push_back(w);
        }
        VkDescriptorImageInfo vis_info{VK_NULL_HANDLE, vis.view, VK_IMAGE_LAYOUT_GENERAL};
        {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = 14;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = &vis_info;
            writes.push_back(w);
        }
        std::vector<VkDescriptorImageInfo> tex_infos(max_textures);
        for (uint32_t k = 0; k < max_textures; ++k)
            tex_infos[k] = {sampler, tex_images[std::min<size_t>(k, tex_images.size() - 1)].view,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = 17;
            w.descriptorCount = max_textures;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = tex_infos.data();
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(ctx.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // Pipelines.
        VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, sizeof(gpu_sample)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        VkPipelineLayout layout;
        VK_CHECK(vkCreatePipelineLayout(ctx.device, &plci, nullptr, &layout));

        VkShaderModule task = ctx.shader(cull_task_spv, sizeof cull_task_spv);
        VkShaderModule meshm = ctx.shader(draw_mesh_spv, sizeof draw_mesh_spv);
        VkShaderModule frag = ctx.shader(visibility_frag_spv, sizeof visibility_frag_spv);
        VkShaderModule comp = ctx.shader(trace_comp_spv, sizeof trace_comp_spv);

        VkPipeline raster;
        {
            VkPipelineShaderStageCreateInfo stages[3] = {};
            const VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT,
                                                    VK_SHADER_STAGE_FRAGMENT_BIT};
            const VkShaderModule mods[3] = {task, meshm, frag};
            for (int k = 0; k < 3; ++k) {
                stages[k].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[k].stage = kinds[k];
                stages[k].module = mods[k];
                stages[k].pName = "main";
            }
            VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
            VkRect2D scissor{{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
            VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vps.viewportCount = 1;
            vps.pViewports = &viewport;
            vps.scissorCount = 1;
            vps.pScissors = &scissor;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;  // Two-sided, as in the tracer
            rs.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_GREATER;  // Reversed depth
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1;
            cb.pAttachments = &blend;
            const VkFormat color_format = VK_FORMAT_R32G32_UINT;
            VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rci.colorAttachmentCount = 1;
            rci.pColorAttachmentFormats = &color_format;
            rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
            VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gpci.pNext = &rci;
            gpci.stageCount = 3;
            gpci.pStages = stages;
            gpci.pViewportState = &vps;
            gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms;
            gpci.pDepthStencilState = &ds;
            gpci.pColorBlendState = &cb;
            gpci.layout = layout;
            VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &gpci, nullptr, &raster));
        }
        VkPipeline trace;
        {
            VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = comp;
            cpci.stage.pName = "main";
            cpci.layout = layout;
            VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &trace));
        }

        // Timestamps around each raster and trace pass.
        VkQueryPool queries;
        {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 4 * static_cast<uint32_t>(spp);
            VK_CHECK(vkCreateQueryPool(ctx.device, &qci, nullptr, &queries));
        }

        // Per-sample offsets within the pixel: the 2D Sobol points, randomly
        // shifted per render, so each sample covers the pixel where earlier
        // ones did not.
        std::mt19937 jitter_rng(static_cast<uint32_t>(seed));
        const double shift_x = jitter_rng() * 0x1.0p-32, shift_y = jitter_rng() * 0x1.0p-32;

        ctx.submit([&](VkCommandBuffer cmd) {
            vkCmdFillBuffer(cmd, b_accum.handle, 0, VK_WHOLE_SIZE, 0);
            vkCmdResetQueryPool(cmd, queries, 0, 4 * spp);
            image_layout(cmd, vis.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            image_layout(cmd, depth.handle, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        });

        const auto t_render = std::chrono::steady_clock::now();
        // Batches of samples per submission, so no single one runs long
        // enough for the driver to think the GPU hung.
        const int batch = 16;
        for (int first = 0; first < spp; first += batch) {
            ctx.submit([&](VkCommandBuffer cmd) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                for (int s = first; s < std::min(spp, first + batch); ++s) {
                    const uint32_t us = static_cast<uint32_t>(s);
                    gpu_sample pc{};
                    const double jx = reverse_bits(us) * 0x1.0p-32 + shift_x;
                    const double jy = reverse_bits(sobol_1_reversed(us)) * 0x1.0p-32 + shift_y;
                    pc.jitter[0] = static_cast<float>(jx - std::floor(jx));
                    pc.jitter[1] = static_cast<float>(jy - std::floor(jy));
                    // In clip space: x moves right by (1/2 - du) pixels and y,
                    // which runs down, by (dv - 1/2); see view_projection().
                    pc.jitter_ndc[0] = static_cast<float>(2 * (0.5 - pc.jitter[0]) / width);
                    pc.jitter_ndc[1] = static_cast<float>(2 * (pc.jitter[1] - 0.5) / height);
                    pc.index = us;
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof pc, &pc);

                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 4 * us);
                    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    color.imageView = vis.view;
                    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    color.clearValue.color.uint32[0] = 0;
                    VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    depth_att.imageView = depth.view;
                    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    depth_att.clearValue.depthStencil.depth = 0;  // Reversed: 0 is farthest
                    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    ri.renderArea = {{0, 0}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}};
                    ri.layerCount = 1;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &color;
                    ri.pDepthAttachment = &depth_att;
                    vkCmdBeginRendering(cmd, &ri);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, raster);
                    // One task workgroup per 64 meshlets per instance.
                    ctx.draw_mesh_tasks(cmd, (frame.meshlet_count + 63) / 64, frame.instance_count, 1);
                    vkCmdEndRendering(cmd);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, queries, 4 * us + 1);

                    barrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 4 * us + 2);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace);
                    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 4 * us + 3);
                    // The next raster overwrites the visibility buffer this
                    // pass read, and the next trace adds to what it wrote.
                    barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                }
            });
        }
        const double render_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_render).count();

        ctx.submit([&](VkCommandBuffer cmd) {
            VkBufferCopy copy{0, 0, b_accum.size};
            vkCmdCopyBuffer(cmd, b_accum.handle, b_readback.handle, 1, &copy);
        });
        std::vector<uint64_t> stamps(4 * spp);
        VK_CHECK(vkGetQueryPoolResults(ctx.device, queries, 0, 4 * spp, stamps.size() * sizeof(uint64_t), stamps.data(),
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double raster_ms = 0, trace_ms = 0;
        const double tick_ms = ctx.properties.limits.timestampPeriod * 1e-6;
        for (int s = 0; s < spp; ++s) {
            raster_ms += (stamps[4 * s + 1] - stamps[4 * s]) * tick_ms;
            trace_ms += (stamps[4 * s + 3] - stamps[4 * s + 2]) * tick_ms;
        }

        // Average, and write out.
        const float* acc = static_cast<const float*>(b_readback.mapped);
        std::vector<float> image(3 * static_cast<size_t>(width) * height);
        for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p)
            for (int c = 0; c < 3; ++c) image[3 * p + c] = acc[4 * p + c] / acc[4 * p + 3];
        if (!write_image(out_path, width, height, image.data())) throw std::runtime_error("could not write " + out_path);

        const auto* st = static_cast<const gpu_stats*>(b_stats.mapped);
        const double drawn = double(st->visible_meshlets) / spp, total = double(frame.meshlet_count) * frame.instance_count;
        std::cerr << "Acceleration structures built in " << as_s << " s\n";
        std::cerr << width << "x" << height << " at " << spp << " spp: rendered in " << render_s << " s; on the GPU, "
                  << raster_ms << " ms rasterizing, " << trace_ms << " ms tracing, " << st->rays / render_s / 1e6
                  << "M rays/s (" << st->rays << " rays); meshlets drawn " << drawn << " of " << total << " ("
                  << 100 * (1 - drawn / total) << "% culled), crowd scene, seed " << seed << "\n";

        vkDeviceWaitIdle(ctx.device);
        vkDestroyQueryPool(ctx.device, queries, nullptr);
        vkDestroyPipeline(ctx.device, raster, nullptr);
        vkDestroyPipeline(ctx.device, trace, nullptr);
        for (VkShaderModule m : {task, meshm, frag, comp}) vkDestroyShaderModule(ctx.device, m, nullptr);
        vkDestroyPipelineLayout(ctx.device, layout, nullptr);
        vkDestroyDescriptorPool(ctx.device, dpool, nullptr);
        vkDestroyDescriptorSetLayout(ctx.device, set_layout, nullptr);
        vkDestroySampler(ctx.device, sampler, nullptr);
        for (auto& im : tex_images) ctx.destroy(im);
        ctx.destroy(vis);
        ctx.destroy(depth);
        ctx.destroy_as(ctx.device, tlas.handle, nullptr);
        ctx.destroy_as(ctx.device, blas.handle, nullptr);
        for (vk::buffer* b : {&tlas.storage, &blas.storage, &b_as_instances, &b_meshlets, &b_meshlet_vertices, &b_meshlet_triangles,
                              &b_meshlet_ids, &b_positions, &b_normals, &b_uvs, &b_indices, &b_instances, &b_materials,
                              &b_triangle_materials, &b_spheres, &b_accum, &b_readback, &b_stats, &b_frame})
            ctx.destroy(*b);
        if (validate && vk::validation_errors) {
            std::cerr << vk::validation_errors << " validation errors\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "photon_tracer_gpu: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
