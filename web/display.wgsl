// Shows the render on the canvas: the accumulated light averaged, or the
// denoised image, then gamma 2.0 and quantized as to_display() does it.

struct View { size: vec4u }  // width, height, source (0 accumulated, 1 denoised), unused

@group(0) @binding(0) var<uniform> view: View;
@group(0) @binding(1) var<storage, read> accumulated: array<vec4f>;
@group(0) @binding(2) var<storage, read> denoised: array<vec4f>;

// One triangle that covers the screen.
@vertex
fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(p * 2.0 - 1.0, 0.0, 1.0);
}

@fragment
fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let index = u32(pos.y) * view.size.x + u32(pos.x);
    var c: vec3f;
    if (view.size.z == 1u) {
        c = denoised[index].xyz;
    } else {
        let a = accumulated[index];
        c = a.xyz / max(a.w, 1.0);
    }
    let level = floor(256.0 * clamp(sqrt(max(c, vec3f(0.0))), vec3f(0.0), vec3f(0.999)));
    return vec4f(level / 255.0, 1.0);
}
