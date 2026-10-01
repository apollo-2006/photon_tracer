// include/denoise.hpp on the GPU: an edge-avoiding a-trous wavelet filter,
// steered by each pixel's noise, over the lighting with the albedo divided
// out. Three entry points, run in order: prepare, then atrous five times with
// the stride doubling from 1, then finish.

struct Params { size: vec4u }  // width, height, stride, unused

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> accumulated: array<vec4f>;      // rgb sum, samples
@group(0) @binding(2) var<storage, read> guides: array<vec4f>;           // (albedo sum, lum^2 sum), (normal sum, 0)
@group(0) @binding(3) var<storage, read_write> surface: array<vec4f>;    // Per pixel: albedo, unit normal (0 for sky)
@group(0) @binding(4) var<storage, read_write> src: array<vec4f>;        // Lighting, variance
@group(0) @binding(5) var<storage, read_write> dst: array<vec4f>;
@group(0) @binding(6) var<storage, read_write> denoised: array<vec4f>;

const SIGMA_LUMINANCE = 4.0;
const ALBEDO_SIGMA2 = 0.01;

fn lum(c: vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }
fn floored(a: vec3f) -> vec3f { return max(a, vec3f(0.01)); }

@compute @workgroup_size(8, 8)
fn prepare(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.size.x || gid.y >= params.size.y) { return; }
    let p = gid.y * params.size.x + gid.x;
    let acc = accumulated[p];
    let n = max(acc.w, 1.0);
    let color = acc.xyz / n;
    let g0 = guides[2u * p];
    let albedo = g0.xyz / n;
    let normal_sum = guides[2u * p + 1u].xyz;
    let len = length(normal_sum);
    surface[2u * p] = vec4f(albedo, 0.0);
    surface[2u * p + 1u] = vec4f(select(vec3f(0.0), normal_sum / len, len > 1e-6), 0.0);
    // Variance of the mean: that of one sample over the sample count.
    let l = lum(color);
    let variance = max(g0.w / n - l * l, 0.0) / n;
    let a = max(lum(albedo), 0.01);
    src[p] = vec4f(color / floored(albedo), variance / (a * a));
}

@compute @workgroup_size(8, 8)
fn atrous(@builtin(global_invocation_id) gid: vec3u) {
    let w = i32(params.size.x);
    let h = i32(params.size.y);
    let x = i32(gid.x);
    let y = i32(gid.y);
    if (x >= w || y >= h) { return; }
    let stride = i32(params.size.z);
    let weights = array<f32, 3>(3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
    let p = u32(y * w + x);
    let np = surface[2u * p + 1u].xyz;
    let ap = surface[2u * p].xyz;
    let p_sky = all(np == vec3f(0.0));
    let lp = lum(src[p].xyz);

    // The center's noise, blurred over 3x3 as in SVGF.
    var v = 0.0;
    var vw = 0.0;
    for (var dy = -1; dy <= 1; dy += 1) {
        for (var dx = -1; dx <= 1; dx += 1) {
            let qx = x + dx; let qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= w || qy >= h) { continue; }
            let k = select(1.0, 0.5, dx != 0) * select(1.0, 0.5, dy != 0);
            v += k * src[u32(qy * w + qx)].w;
            vw += k;
        }
    }
    let scale = 1.0 / (SIGMA_LUMINANCE * sqrt(v / vw) + 1e-4);

    var sum = vec3f(0.0);
    var sum_w = 0.0;
    var sum_var = 0.0;
    for (var dy = -2; dy <= 2; dy += 1) {
        let qy = y + dy * stride;
        if (qy < 0 || qy >= h) { continue; }
        for (var dx = -2; dx <= 2; dx += 1) {
            let qx = x + dx * stride;
            if (qx < 0 || qx >= w) { continue; }
            let q = u32(qy * w + qx);
            let nq = surface[2u * q + 1u].xyz;
            let q_sky = all(nq == vec3f(0.0));
            if (p_sky != q_sky) { continue; }  // Never blur sky into surface
            var weight = weights[abs(dx)] * weights[abs(dy)];
            if (!p_sky) {
                var nd = max(dot(np, nq), 0.0);
                for (var k = 0; k < 6; k += 1) { nd *= nd; }  // cos^64
                weight *= nd;
            }
            let da = ap - surface[2u * q].xyz;
            let sq = src[q];
            weight *= exp(-dot(da, da) * (1.0 / ALBEDO_SIGMA2) - abs(lp - lum(sq.xyz)) * scale);
            sum += weight * sq.xyz;
            sum_w += weight;
            sum_var += weight * weight * sq.w;
        }
    }
    dst[p] = vec4f(sum / sum_w, sum_var / (sum_w * sum_w));
}

@compute @workgroup_size(8, 8)
fn finish(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.size.x || gid.y >= params.size.y) { return; }
    let p = gid.y * params.size.x + gid.x;
    denoised[p] = vec4f(src[p].xyz * floored(surface[2u * p].xyz), 1.0);
}
