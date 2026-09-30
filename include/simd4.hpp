#pragma once
// Four floats at once, for testing a ray against a BVH node's four child boxes
// together: SSE on x86-64, SIMD128 in WebAssembly (with -msimd128), and plain
// loops anywhere else. Only what the slab test needs.

#if defined(__SSE2__) || defined(_M_X64)
#include <immintrin.h>

struct f4 { __m128 v; };
inline f4 load4(const float* p) { return {_mm_loadu_ps(p)}; }
inline f4 splat4(float x) { return {_mm_set1_ps(x)}; }
inline void store4(float* p, f4 a) { _mm_storeu_ps(p, a.v); }
inline f4 operator-(f4 a, f4 b) { return {_mm_sub_ps(a.v, b.v)}; }
inline f4 operator*(f4 a, f4 b) { return {_mm_mul_ps(a.v, b.v)}; }
inline f4 min4(f4 a, f4 b) { return {_mm_min_ps(a.v, b.v)}; }
inline f4 max4(f4 a, f4 b) { return {_mm_max_ps(a.v, b.v)}; }
// Bit i set where a[i] <= b[i].
inline int le_mask(f4 a, f4 b) { return _mm_movemask_ps(_mm_cmple_ps(a.v, b.v)); }

#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>

struct f4 { v128_t v; };
inline f4 load4(const float* p) { return {wasm_v128_load(p)}; }
inline f4 splat4(float x) { return {wasm_f32x4_splat(x)}; }
inline void store4(float* p, f4 a) { wasm_v128_store(p, a.v); }
inline f4 operator-(f4 a, f4 b) { return {wasm_f32x4_sub(a.v, b.v)}; }
inline f4 operator*(f4 a, f4 b) { return {wasm_f32x4_mul(a.v, b.v)}; }
// pmin/pmax are the SSE-style ones, a single instruction each; the plain
// min/max also order NaNs and signed zeros, which costs several.
inline f4 min4(f4 a, f4 b) { return {wasm_f32x4_pmin(a.v, b.v)}; }
inline f4 max4(f4 a, f4 b) { return {wasm_f32x4_pmax(a.v, b.v)}; }
inline int le_mask(f4 a, f4 b) { return static_cast<int>(wasm_i32x4_bitmask(wasm_f32x4_le(a.v, b.v))); }

#else
struct f4 { float v[4]; };
inline f4 load4(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline f4 splat4(float x) { return {{x, x, x, x}}; }
inline void store4(float* p, f4 a) { for (int i = 0; i < 4; ++i) p[i] = a.v[i]; }
inline f4 operator-(f4 a, f4 b) { for (int i = 0; i < 4; ++i) a.v[i] -= b.v[i]; return a; }
inline f4 operator*(f4 a, f4 b) { for (int i = 0; i < 4; ++i) a.v[i] *= b.v[i]; return a; }
inline f4 min4(f4 a, f4 b) { for (int i = 0; i < 4; ++i) a.v[i] = b.v[i] < a.v[i] ? b.v[i] : a.v[i]; return a; }
inline f4 max4(f4 a, f4 b) { for (int i = 0; i < 4; ++i) a.v[i] = a.v[i] < b.v[i] ? b.v[i] : a.v[i]; return a; }
inline int le_mask(f4 a, f4 b) {
    int m = 0;
    for (int i = 0; i < 4; ++i) m |= (a.v[i] <= b.v[i]) << i;
    return m;
}
#endif
