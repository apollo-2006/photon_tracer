// The path tracer in WebGPU: one compute invocation per pixel, tracing the
// scene the C++ code built and packed (include/gpu_pack.hpp) through its
// 4-wide BVHs. It follows include/renderer.hpp: the same materials, sky,
// next event estimation and Russian roulette, so its images can be checked
// against the CPU renderer's references.
//
// Differences: random numbers come from a PCG hash rather than Sobol points,
// and with no 64-bit floats in WGSL, the large ground spheres are intersected
// with a cancellation-free form of the quadratic, and rays leaving them start
// 1e-4 off their surface, where the CPU renderer uses double.

struct Frame {
    origin: vec4f,
    corner: vec4f,
    across: vec4f,
    up: vec4f,
    size: vec4u,    // width, height, max bounces, flags (1 sky, 2 sample lights, 4 denoiser guides)
    roots: vec4u,   // flat root, instance root, small spheres, seed
    counts: vec4u,  // lights, large spheres, first sample, samples this dispatch
}

struct Node {
    lo: array<vec4f, 3>,  // lo[axis][child]
    hi: array<vec4f, 3>,
    first: vec4u,         // Child node, or start of a leaf in order
    count: vec4u,         // Primitives in a leaf child; 0 for an interior child
}

struct Sphere { cr: vec4f, material: u32, pad0: u32, pad1: u32, pad2: u32 }
struct Triangle { v: array<vec4f, 7> }   // See pack_triangle() in gpu_pack.hpp
struct Instance { to_world: array<vec4f, 3>, to_object: array<vec4f, 3>, root: u32, material: u32, pad0: u32, pad1: u32 }
struct Material { albedo_type: vec4f, emission_fuzz: vec4f, ior: vec4f }

@group(0) @binding(0) var<uniform> frame: Frame;
@group(0) @binding(1) var<storage, read> nodes: array<Node>;
@group(0) @binding(2) var<storage, read> order: array<u32>;
@group(0) @binding(3) var<storage, read> spheres: array<Sphere>;
@group(0) @binding(4) var<storage, read> triangles: array<Triangle>;
@group(0) @binding(5) var<storage, read> instances: array<Instance>;
@group(0) @binding(6) var<storage, read> materials: array<Material>;
@group(0) @binding(7) var<storage, read> lists: array<u32>;  // Lights, then large spheres
@group(0) @binding(8) var<storage, read_write> accumulated: array<vec4f>;  // rgb sum, samples
@group(0) @binding(9) var<storage, read_write> guides: array<vec4f>;  // Per pixel: (albedo sum, luminance^2 sum), (normal sum, 0)
@group(0) @binding(10) var<storage, read_write> stats: array<atomic<u32>, 2>;  // Rays, low and high words

const T_MIN = 0.001;
const T_MAX = 1000.0;
const INF = 1e30;
const PI = 3.14159265358979;
const NONE = 0xffffffffu;
// Entries in a traversal stack. Each node visited pushes at most three more
// than it pops, so 32 holds trees up to 10 levels deep; pack_for_gpu() refuses
// deeper ones (gpu_stack_entries in gpu_pack.hpp). 64 cost 40-60% more time,
// in spilled private memory.
const STACK = 32u;
// A stack entry is a node or leaf start in the low 28 bits and a leaf's
// primitive count (at most 8) above them: one word, not two, since these
// stacks live in scarce private memory, and with instances two are live at once.
const COUNT_SHIFT = 28u;
const FIRST_MASK = 0x0fffffffu;
const ROULETTE_AFTER = 3u;
const OFFSET = 1e-4;

// PCG hash (O'Neill), one stream per pixel and sample.
var<private> rng: u32;
fn next_u32() -> u32 {
    rng = rng * 747796405u + 2891336453u;
    let w = ((rng >> ((rng >> 28u) + 4u)) ^ rng) * 277803737u;
    return (w >> 22u) ^ w;
}
fn random01() -> f32 { return f32(next_u32() >> 8u) * (1.0 / 16777216.0); }
fn hash(x0: u32) -> u32 {
    var x = x0;
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

fn random_in_unit_sphere() -> vec3f {
    loop {
        let p = 2.0 * vec3f(random01(), random01(), random01()) - 1.0;
        if (dot(p, p) < 1.0) { return p; }
    }
}

// The nearest hit found so far.
struct Hit {
    t: f32,
    kind: u32,      // 0 none, 1 sphere, 2 triangle
    index: u32,     // Sphere or triangle number
    instance: u32,  // NONE for a triangle not in an instance
    u: f32,
    v: f32,
}

// 1/d with zeros replaced by tiny numbers: an infinity would make NaNs in the
// slab test where a ray runs along a box face.
fn safe_inverse(d: vec3f) -> vec3f {
    let tiny = 1e-20;
    let s = select(vec3f(-1.0), vec3f(1.0), d >= vec3f(0.0));
    return 1.0 / select(d, s * tiny, abs(d) < vec3f(tiny));
}

// Where the ray enters each of a node's four boxes, or INF for a miss.
fn enter_children(n: Node, o: vec3f, inv: vec3f, tmin: f32, tmax: f32) -> vec4f {
    let x0 = (n.lo[0] - o.x) * inv.x; let x1 = (n.hi[0] - o.x) * inv.x;
    let y0 = (n.lo[1] - o.y) * inv.y; let y1 = (n.hi[1] - o.y) * inv.y;
    let z0 = (n.lo[2] - o.z) * inv.z; let z1 = (n.hi[2] - o.z) * inv.z;
    let near = max(max(min(x0, x1), min(y0, y1)), max(min(z0, z1), vec4f(tmin)));
    let far = min(min(max(x0, x1), max(y0, y1)), min(max(z0, z1), vec4f(tmax)));
    return select(vec4f(INF), near, near <= far);
}

fn intersect_sphere(cr: vec4f, o: vec3f, d: vec3f, tmin: f32, tmax: f32) -> f32 {
    // half_b^2 - a c without forming |oc|^2 - r^2, which cancels badly for
    // the ground's large radius: from the point of closest approach.
    let oc = o - cr.xyz;
    let a = dot(d, d);
    let half_b = dot(oc, d);
    let closest = oc - (half_b / a) * d;
    let disc = a * (cr.w * cr.w - dot(closest, closest));
    if (disc < 0.0) { return INF; }
    let sq = sqrt(disc);
    // The root farther from zero without cancellation, the other from the product.
    let q = -half_b - select(-sq, sq, half_b >= 0.0);
    let c = dot(oc, oc) - cr.w * cr.w;
    var t0 = q / a;
    var t1 = c / q;
    if (t0 > t1) { let s = t0; t0 = t1; t1 = s; }
    if (t0 >= tmin && t0 <= tmax) { return t0; }
    if (t1 >= tmin && t1 <= tmax) { return t1; }
    return INF;
}

// Moller-Trumbore; returns (t, u, v) with t = INF for a miss.
fn intersect_triangle(tri: Triangle, o: vec3f, d: vec3f, tmin: f32, tmax: f32) -> vec3f {
    let p0 = tri.v[0].xyz; let e1 = tri.v[1].xyz; let e2 = tri.v[2].xyz;
    let pv = cross(d, e2);
    let det = dot(e1, pv);
    if (abs(det) < 1e-12) { return vec3f(INF, 0.0, 0.0); }
    let inv_det = 1.0 / det;
    let tv = o - p0;
    let u = dot(tv, pv) * inv_det;
    if (u < 0.0 || u > 1.0) { return vec3f(INF, 0.0, 0.0); }
    let qv = cross(tv, e1);
    let v = dot(d, qv) * inv_det;
    if (v < 0.0 || u + v > 1.0) { return vec3f(INF, 0.0, 0.0); }
    let t = dot(e2, qv) * inv_det;
    if (t < tmin || t > tmax) { return vec3f(INF, 0.0, 0.0); }
    return vec3f(t, u, v);
}

// Walks one of the trees. kind 0: the flat tree, whose leaves are spheres
// then triangles; kind 1: a mesh's tree, whose leaves are triangles, in
// instance space; kind 2: the instance tree. With any_hit, it stops at the
// first hit (a shadow ray).
fn walk(root: u32, kind: u32, o: vec3f, d: vec3f, tmin: f32, hit: ptr<function, Hit>, instance: u32, any_hit: bool) -> bool {
    if (root == NONE) { return false; }
    let inv = safe_inverse(d);
    var stack_entry: array<u32, STACK>;
    var stack_t: array<f32, STACK>;
    var top = 1u;
    stack_entry[0] = root; stack_t[0] = tmin;
    var found = false;
    while (top > 0u) {
        top -= 1u;
        if (stack_t[top] >= (*hit).t) { continue; }
        let first = stack_entry[top] & FIRST_MASK;
        let count = stack_entry[top] >> COUNT_SHIFT;
        if (count > 0u) {
            for (var i = first; i < first + count; i += 1u) {
                let p = order[i];
                if (kind == 2u) {
                    if (walk_instance(p, o, d, tmin, hit, any_hit)) {
                        found = true;
                        if (any_hit) { return true; }
                    }
                    continue;
                }
                if (kind == 0u && p < frame.roots.z) {
                    let t = intersect_sphere(spheres[p].cr, o, d, tmin, (*hit).t);
                    if (t < INF) {
                        (*hit).t = t; (*hit).kind = 1u; (*hit).index = p; (*hit).instance = NONE;
                        found = true;
                        if (any_hit) { return true; }
                    }
                } else {
                    let k = select(p, p - frame.roots.z, kind == 0u);
                    let r = intersect_triangle(triangles[k], o, d, tmin, (*hit).t);
                    if (r.x < INF) {
                        (*hit).t = r.x; (*hit).kind = 2u; (*hit).index = k; (*hit).u = r.y; (*hit).v = r.z;
                        (*hit).instance = instance;
                        found = true;
                        if (any_hit) { return true; }
                    }
                }
            }
            continue;
        }
        let n = nodes[first];
        let enter = enter_children(n, o, inv, tmin, (*hit).t);
        // Push the hit children farthest first, so the nearest comes off next.
        var idx = vec4u(0u, 1u, 2u, 3u);
        var tt = enter;
        // Sort the four by entry distance, descending (a small network).
        if (tt.x < tt.y) { tt = tt.yxzw; idx = idx.yxzw; }
        if (tt.z < tt.w) { tt = tt.xywz; idx = idx.xywz; }
        if (tt.x < tt.z) { tt = tt.zyxw; idx = idx.zyxw; }
        if (tt.y < tt.w) { tt = tt.xwzy; idx = idx.xwzy; }
        if (tt.y < tt.z) { tt = tt.xzyw; idx = idx.xzyw; }
        for (var c = 0u; c < 4u; c += 1u) {
            if (tt[c] < INF && top < STACK) {
                stack_entry[top] = n.first[idx[c]] | (n.count[idx[c]] << COUNT_SHIFT);
                stack_t[top] = tt[c];
                top += 1u;
            }
        }
    }
    return found;
}

// Instance k: the ray moved into the mesh's space (where t is the same) and
// down the mesh's tree.
fn walk_instance(k: u32, o: vec3f, d: vec3f, tmin: f32, hit: ptr<function, Hit>, any_hit: bool) -> bool {
    let inst = instances[k];
    let lo = vec3f(dot(inst.to_object[0].xyz, o) + inst.to_object[0].w, dot(inst.to_object[1].xyz, o) + inst.to_object[1].w,
                   dot(inst.to_object[2].xyz, o) + inst.to_object[2].w);
    let ld = vec3f(dot(inst.to_object[0].xyz, d), dot(inst.to_object[1].xyz, d), dot(inst.to_object[2].xyz, d));
    return walk_mesh(inst.root, lo, ld, tmin, hit, k, any_hit);
}

// A mesh's tree, as walk() with kind 1. WGSL allows no recursion, so the walk
// over instances calls this copy rather than walk() itself.
fn walk_mesh(root: u32, o: vec3f, d: vec3f, tmin: f32, hit: ptr<function, Hit>, instance: u32, any_hit: bool) -> bool {
    if (root == NONE) { return false; }
    let inv = safe_inverse(d);
    var stack_entry: array<u32, STACK>;
    var stack_t: array<f32, STACK>;
    var top = 1u;
    stack_entry[0] = root; stack_t[0] = tmin;
    var found = false;
    while (top > 0u) {
        top -= 1u;
        if (stack_t[top] >= (*hit).t) { continue; }
        let first = stack_entry[top] & FIRST_MASK;
        let count = stack_entry[top] >> COUNT_SHIFT;
        if (count > 0u) {
            for (var i = first; i < first + count; i += 1u) {
                let k = order[i];
                let r = intersect_triangle(triangles[k], o, d, tmin, (*hit).t);
                if (r.x < INF) {
                    (*hit).t = r.x; (*hit).kind = 2u; (*hit).index = k; (*hit).u = r.y; (*hit).v = r.z;
                    (*hit).instance = instance;
                    found = true;
                    if (any_hit) { return true; }
                }
            }
            continue;
        }
        let n = nodes[first];
        let enter = enter_children(n, o, inv, tmin, (*hit).t);
        var idx = vec4u(0u, 1u, 2u, 3u);
        var tt = enter;
        if (tt.x < tt.y) { tt = tt.yxzw; idx = idx.yxzw; }
        if (tt.z < tt.w) { tt = tt.xywz; idx = idx.xywz; }
        if (tt.x < tt.z) { tt = tt.zyxw; idx = idx.zyxw; }
        if (tt.y < tt.w) { tt = tt.xwzy; idx = idx.xwzy; }
        if (tt.y < tt.z) { tt = tt.xzyw; idx = idx.xzyw; }
        for (var c = 0u; c < 4u; c += 1u) {
            if (tt[c] < INF && top < STACK) {
                stack_entry[top] = n.first[idx[c]] | (n.count[idx[c]] << COUNT_SHIFT);
                stack_t[top] = tt[c];
                top += 1u;
            }
        }
    }
    return found;
}

// Everything along a ray: the large spheres, the flat tree, the instances.
fn trace(o: vec3f, d: vec3f, tmax: f32, any_hit: bool) -> Hit {
    var hit = Hit(tmax, 0u, 0u, NONE, 0.0, 0.0);
    for (var k = 0u; k < frame.counts.y; k += 1u) {
        let s = lists[frame.counts.x + k];
        let t = intersect_sphere(spheres[s].cr, o, d, T_MIN, hit.t);
        if (t < INF) {
            hit.t = t; hit.kind = 1u; hit.index = s; hit.instance = NONE;
            if (any_hit) { return hit; }
        }
    }
    if (walk(frame.roots.x, 0u, o, d, T_MIN, &hit, NONE, any_hit) && any_hit) { return hit; }
    walk(frame.roots.y, 2u, o, d, T_MIN, &hit, NONE, any_hit);
    return hit;
}

struct Surface {
    p: vec3f,
    normal: vec3f,  // Against the ray
    front: bool,
    material: u32,
    light: bool,    // A sphere next event estimation samples
    large: bool,    // One of the large spheres: rays leaving it start off its surface
}

fn surface(hit: Hit, o: vec3f, d: vec3f) -> Surface {
    var s: Surface;
    s.p = o + hit.t * d;
    s.light = false;
    s.large = hit.kind == 1u && hit.index >= frame.roots.z;
    if (hit.kind == 1u) {
        let sp = spheres[hit.index];
        let outward = (s.p - sp.cr.xyz) / sp.cr.w;
        s.front = dot(d, outward) < 0.0;
        s.normal = select(-outward, outward, s.front);
        s.material = sp.material;
        s.light = (frame.size.w & 2u) != 0u && hit.index < frame.roots.z && any(materials[sp.material].emission_fuzz.xyz > vec3f(0.0));
        return s;
    }
    let tri = triangles[hit.index];
    let b0 = 1.0 - hit.u - hit.v;
    var n = normalize(tri.v[3].xyz * b0 + tri.v[4].xyz * hit.u + tri.v[5].xyz * hit.v);
    var face_dir = d;
    s.material = bitcast<u32>(tri.v[0].w);
    if (hit.instance != NONE) {
        let inst = instances[hit.instance];
        face_dir = vec3f(dot(inst.to_object[0].xyz, d), dot(inst.to_object[1].xyz, d), dot(inst.to_object[2].xyz, d));
        if (inst.material != NONE) { s.material = inst.material; }
    }
    // Which side was hit comes from the true face, as in triangle::fill().
    s.front = dot(face_dir, tri.v[6].xyz) < 0.0;
    if (!s.front) { n = -n; }
    if (hit.instance != NONE) {
        // Normals move by the inverse transpose: the transposed to_object.
        let inst = instances[hit.instance];
        n = normalize(inst.to_object[0].xyz * n.x + inst.to_object[1].xyz * n.y + inst.to_object[2].xyz * n.z);
    }
    s.normal = n;
    return s;
}

fn sky(d: vec3f) -> vec3f {
    if ((frame.size.w & 1u) == 0u) { return vec3f(0.0); }
    let t = 0.5 * (normalize(d).y + 1.0);
    return vec3f(1.0) * (1.0 - t) + vec3f(0.5, 0.7, 1.0) * t;
}

fn schlick(cosine: f32, ratio: f32) -> f32 {
    var r0 = (1.0 - ratio) / (1.0 + ratio);
    r0 *= r0;
    let x = 1.0 - cosine;
    let x2 = x * x;
    return r0 + (1.0 - r0) * x2 * x2 * x;
}

struct Scatter { ok: bool, attenuation: vec3f, dir: vec3f }

fn scatter(m: Material, s: Surface, d: vec3f) -> Scatter {
    var out: Scatter;
    out.ok = true;
    let type_ = u32(m.albedo_type.w);
    if (type_ == 0u) {  // Matte: cosine-weighted about the normal
        let a = random01(); let b = random01();
        let r = sqrt(a); let phi = 2.0 * PI * b;
        let n = s.normal;
        let sgn = select(-1.0, 1.0, n.z >= 0.0);
        let p = -1.0 / (sgn + n.z); let q = n.x * n.y * p;
        let t1 = vec3f(1.0 + sgn * n.x * n.x * p, sgn * q, -sgn * n.x);
        let t2 = vec3f(q, sgn + n.y * n.y * p, -n.y);
        out.dir = t1 * (r * cos(phi)) + t2 * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - a));
        out.attenuation = m.albedo_type.xyz;
    } else if (type_ == 1u) {  // Metal
        out.dir = reflect(normalize(d), s.normal) + random_in_unit_sphere() * m.emission_fuzz.w;
        out.attenuation = m.albedo_type.xyz;
        out.ok = dot(out.dir, s.normal) > 0.0;
    } else if (type_ == 2u) {  // Glass
        out.attenuation = vec3f(1.0);
        let ior = m.ior.x;
        let ratio = select(ior, 1.0 / ior, s.front);
        let u = normalize(d);
        let cos_t = min(dot(-u, s.normal), 1.0);
        let sin_t = sqrt(1.0 - cos_t * cos_t);
        if (ratio * sin_t > 1.0 || schlick(cos_t, ratio) > random01()) {
            out.dir = reflect(u, s.normal);
        } else {
            let perp = (u + s.normal * cos_t) * ratio;
            out.dir = perp - s.normal * sqrt(abs(1.0 - dot(perp, perp)));
        }
    } else {
        out.ok = false;  // A light scatters nothing
    }
    return out;
}

// Next event estimation, as direct_light() in renderer.hpp: light reaching a
// matte surface straight from a light sphere, over the albedo.
fn direct_light(s: Surface, rays: ptr<function, u32>) -> vec3f {
    let n_lights = frame.counts.x;
    let pick = min(u32(random01() * f32(n_lights)), n_lights - 1u);
    let light = spheres[lists[pick]];
    let to_center = light.cr.xyz - s.p;
    let dist2 = dot(to_center, to_center);
    let r2 = light.cr.w * light.cr.w;
    if (dist2 <= r2) { return vec3f(0.0); }
    let sin2_max = r2 / dist2;
    let cos_max = sqrt(1.0 - sin2_max);
    let one_minus_cos_max = sin2_max / (1.0 + cos_max);
    let w = to_center / sqrt(dist2);
    let a = select(vec3f(1.0, 0.0, 0.0), vec3f(0.0, 1.0, 0.0), abs(w.x) > 0.9);
    let u = normalize(cross(w, a));
    let v = cross(w, u);
    let cos_t = 1.0 - random01() * one_minus_cos_max;
    let sin_t = sqrt(max(0.0, 1.0 - cos_t * cos_t));
    let phi = 2.0 * PI * random01();
    let dir = u * (cos(phi) * sin_t) + v * (sin(phi) * sin_t) + w * cos_t;
    let cos_surface = dot(dir, s.normal);
    if (cos_surface <= 0.0) { return vec3f(0.0); }
    let o = select(s.p, s.p + s.normal * OFFSET, s.large);
    let t_light = intersect_sphere(light.cr, o, dir, T_MIN, INF);
    if (t_light >= INF) { return vec3f(0.0); }
    *rays += 1u;
    let blocker = trace(o, dir, t_light * 0.9999, true);
    if (blocker.kind != 0u) { return vec3f(0.0); }
    let pdf = 1.0 / (2.0 * PI * one_minus_cos_max);
    return materials[light.material].emission_fuzz.xyz * (cos_surface / PI / pdf * f32(n_lights));
}

fn luminance(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }

var<workgroup> workgroup_rays: atomic<u32>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3u, @builtin(local_invocation_index) local: u32) {
    if (local == 0u) { atomicStore(&workgroup_rays, 0u); }
    workgroupBarrier();

    let width = frame.size.x;
    let height = frame.size.y;
    let inside = gid.x < width && gid.y < height;
    var rays = 0u;
    if (inside) {
        let j = height - 1u - gid.y;  // Rows counted from the bottom, as on the CPU
        var sum = vec3f(0.0);
        var albedo_sum = vec3f(0.0);
        var normal_sum = vec3f(0.0);
        var lum2 = 0.0;
        for (var s = 0u; s < frame.counts.w; s += 1u) {
            let sample_index = frame.counts.z + s;
            rng = hash((gid.x * 0x9e3779b1u) ^ hash(gid.y ^ hash(sample_index ^ hash(frame.roots.w))));
            let u = (f32(gid.x) + random01()) / f32(width - 1u);
            let v = (f32(j) + random01()) / f32(height - 1u);
            var o = frame.origin.xyz;
            var d = frame.corner.xyz + u * frame.across.xyz + v * frame.up.xyz - o;

            var radiance = vec3f(0.0);
            var throughput = vec3f(1.0);
            var sampled = false;
            var first_albedo = vec3f(1.0);
            var first_normal = vec3f(0.0);
            for (var bounce = 0u; bounce < frame.size.z; bounce += 1u) {
                rays += 1u;
                let hit = trace(o, d, T_MAX, false);
                if (hit.kind == 0u) {
                    radiance += throughput * sky(d);
                    break;
                }
                let surf = surface(hit, o, d);
                let m = materials[surf.material];
                if (surf.front && !(sampled && surf.light)) { radiance += throughput * m.emission_fuzz.xyz; }
                let sc = scatter(m, surf, d);
                if (bounce == 0u) {
                    first_normal = surf.normal;
                    first_albedo = select(vec3f(1.0), sc.attenuation, sc.ok && u32(m.albedo_type.w) != 2u);
                }
                if (!sc.ok) { break; }
                sampled = u32(m.albedo_type.w) == 0u && (frame.size.w & 2u) != 0u && frame.counts.x > 0u &&
                          bounce + 1u < frame.size.z;
                if (sampled) { radiance += throughput * sc.attenuation * direct_light(surf, &rays); }
                throughput *= sc.attenuation;
                // Off a large sphere's surface, on the side the new ray leaves
                // from: in float, a ray from on the ground can find the ground
                // again. Nothing else moves, as nothing moves on the CPU, where
                // a nudge at grazing angles changed reflections along edges.
                o = surf.p;
                if (surf.large) { o += surf.normal * select(-OFFSET, OFFSET, dot(sc.dir, surf.normal) > 0.0); }
                d = sc.dir;
                if (bounce + 1u >= ROULETTE_AFTER) {
                    let p = max(throughput.x, max(throughput.y, throughput.z));
                    if (p < 1.0) {
                        if (random01() >= p) { break; }
                        throughput /= p;
                    }
                }
            }
            sum += radiance;
            albedo_sum += first_albedo;
            normal_sum += first_normal;
            lum2 += luminance(radiance) * luminance(radiance);
        }
        let index = gid.y * width + gid.x;
        accumulated[index] += vec4f(sum, f32(frame.counts.w));
        if ((frame.size.w & 4u) != 0u) {
            guides[2u * index] += vec4f(albedo_sum, lum2);
            guides[2u * index + 1u] += vec4f(normal_sum, 0.0);
        }
    }
    atomicAdd(&workgroup_rays, rays);
    workgroupBarrier();
    if (local == 0u) {
        // A 64-bit count from two words: carry when the low word wraps.
        let n = atomicLoad(&workgroup_rays);
        let old = atomicAdd(&stats[0], n);
        if (old + n < old) { atomicAdd(&stats[1], 1u); }
    }
}
