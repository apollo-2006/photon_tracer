// Shared by every shader of the GPU renderer: its resources, laid out as
// gpu/main.cpp writes them. One descriptor set serves the mesh shading
// pipeline and the path tracing compute pass alike.
#extension GL_EXT_scalar_block_layout : require

struct Meshlet {
    vec3 center;
    float radius;
    vec3 cone_axis;
    float cone_cutoff;  // 1: no cone culling for this meshlet
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
};

// An affine transform as three rows: (m[r][0], m[r][1], m[r][2], t[r]).
struct Instance {
    vec4 to_world[3];
    vec4 to_object[3];
    uint material;  // Replaces the mesh's materials unless ~0u
    float scale;    // Largest scale factor of to_world, for bounding spheres
    uint pad0, pad1;
};

// type: 0 matte, 1 metal, 2 glass, 3 light, as in include/material.hpp.
struct Material {
    vec3 albedo;
    float type;
    float fuzz;
    float ior;
    float texture;  // Index into textures, or -1
    float pad;
    vec3 emission;
    float pad2;
};

struct Sphere {
    vec3 center;
    float radius;
    uint material;
    uint pad0, pad1, pad2;
};

layout(set = 0, binding = 0, scalar) uniform Frame {
    mat4 view_proj;        // World to clip, for pixel centers; jitter is added per sample
    vec4 origin;           // Camera: a sample at (u, v) looks from origin toward
    vec4 corner;           //   corner + u * across + v * up, as camera::get_ray()
    vec4 across;
    vec4 up;
    vec4 planes[5];        // Frustum, world space: inside where dot(xyz, p) + w >= 0
    uint meshlet_count;
    uint instance_count;
    uint sphere_count;
    uint flags;            // 1: normal cone culling
    uint width, height, max_bounces, seed;
} frame;

layout(push_constant, scalar) uniform Sample {
    vec2 jitter;       // Offset within the pixel, [0, 1)^2, the same for every pixel
    vec2 jitter_ndc;   // The same offset in clip space, for the rasterizer
    uint index;        // Sample number
} sample_pc;

layout(set = 0, binding = 1, scalar) readonly buffer Meshlets { Meshlet meshlets[]; };
layout(set = 0, binding = 2, scalar) readonly buffer MeshletVertices { uint meshlet_vertices[]; };
layout(set = 0, binding = 3, scalar) readonly buffer MeshletTriangles { uint meshlet_triangles[]; };  // Three bytes each
layout(set = 0, binding = 4, scalar) readonly buffer MeshletTriangleIds { uint meshlet_triangle_ids[]; };
layout(set = 0, binding = 5, scalar) readonly buffer Positions { vec3 positions[]; };
layout(set = 0, binding = 6, scalar) readonly buffer Normals { vec3 normals[]; };
layout(set = 0, binding = 7, scalar) readonly buffer Uvs { vec2 uvs[]; };
layout(set = 0, binding = 8, scalar) readonly buffer Indices { uint indices[]; };
layout(set = 0, binding = 9, scalar) readonly buffer Instances { Instance instances[]; };
layout(set = 0, binding = 10, scalar) readonly buffer Materials { Material materials[]; };
layout(set = 0, binding = 11, scalar) readonly buffer TriangleMaterials { uint triangle_materials[]; };
layout(set = 0, binding = 12, scalar) readonly buffer Spheres { Sphere spheres[]; };

layout(set = 0, binding = 16, scalar) buffer Stats {
    uint visible_meshlets;  // Meshlets the task shaders kept, over every sample
    uint pad;
    uint64_t rays;
} stats;

vec3 transform_point(vec4 rows[3], vec3 p) {
    return vec3(dot(rows[0].xyz, p) + rows[0].w, dot(rows[1].xyz, p) + rows[1].w, dot(rows[2].xyz, p) + rows[2].w);
}

vec3 transform_vector(vec4 rows[3], vec3 v) {
    return vec3(dot(rows[0].xyz, v), dot(rows[1].xyz, v), dot(rows[2].xyz, v));
}

// What the task shader hands each mesh shader workgroup: the instance, and
// the meshlets that survived culling.
struct Payload {
    uint instance;
    uint meshlets[64];
};
