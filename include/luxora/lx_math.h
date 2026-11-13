/* Luxora Engine - math library (header-only, C11)
 * Right-handed, Y-up, column-major 4x4 matrices (OpenGL convention). */
#ifndef LX_MATH_H
#define LX_MATH_H

#include <math.h>
#include <stdint.h>

#ifdef _MSC_VER
#define LX_INLINE static __forceinline
#else
#define LX_INLINE static inline __attribute__((always_inline))
#endif

#define LX_PI 3.14159265358979323846f
#define LX_DEG2RAD (LX_PI / 180.0f)
#define LX_RAD2DEG (180.0f / LX_PI)

typedef struct LxVec2 { float x, y; } LxVec2;
typedef struct LxVec3 { float x, y, z; } LxVec3;
typedef struct LxVec4 { float x, y, z, w; } LxVec4;
typedef struct LxQuat { float x, y, z, w; } LxQuat;
typedef struct LxMat4 { float m[16]; } LxMat4; /* column-major: m[col*4+row] */
typedef struct LxAabb { LxVec3 min, max; } LxAabb;
typedef struct LxRay { LxVec3 origin, dir; } LxRay;
typedef struct LxPlane { LxVec3 n; float d; } LxPlane; /* dot(n,p)+d = 0 */
typedef struct LxFrustum { LxPlane p[6]; } LxFrustum;

LX_INLINE float lx_minf(float a, float b) { return a < b ? a : b; }
LX_INLINE float lx_maxf(float a, float b) { return a > b ? a : b; }
LX_INLINE float lx_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
LX_INLINE float lx_lerpf(float a, float b, float t) { return a + (b - a) * t; }
LX_INLINE float lx_saturate(float v) { return lx_clampf(v, 0.0f, 1.0f); }

LX_INLINE LxVec2 lx_v2(float x, float y) { LxVec2 r = {x, y}; return r; }
LX_INLINE LxVec3 lx_v3(float x, float y, float z) { LxVec3 r = {x, y, z}; return r; }
LX_INLINE LxVec4 lx_v4(float x, float y, float z, float w) { LxVec4 r = {x, y, z, w}; return r; }
LX_INLINE LxVec3 lx_v3s(float s) { return lx_v3(s, s, s); }

LX_INLINE LxVec3 lx_add3(LxVec3 a, LxVec3 b) { return lx_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
LX_INLINE LxVec3 lx_sub3(LxVec3 a, LxVec3 b) { return lx_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
LX_INLINE LxVec3 lx_mul3(LxVec3 a, LxVec3 b) { return lx_v3(a.x * b.x, a.y * b.y, a.z * b.z); }
LX_INLINE LxVec3 lx_scale3(LxVec3 a, float s) { return lx_v3(a.x * s, a.y * s, a.z * s); }
LX_INLINE LxVec3 lx_madd3(LxVec3 a, LxVec3 b, float s) { return lx_v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
LX_INLINE float lx_dot3(LxVec3 a, LxVec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
LX_INLINE LxVec3 lx_cross3(LxVec3 a, LxVec3 b) {
    return lx_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
LX_INLINE float lx_len3(LxVec3 a) { return sqrtf(lx_dot3(a, a)); }
LX_INLINE float lx_len3sq(LxVec3 a) { return lx_dot3(a, a); }
LX_INLINE LxVec3 lx_norm3(LxVec3 a) {
    float l = lx_len3(a);
    return l > 1e-12f ? lx_scale3(a, 1.0f / l) : lx_v3(0, 0, 0);
}
LX_INLINE LxVec3 lx_neg3(LxVec3 a) { return lx_v3(-a.x, -a.y, -a.z); }
LX_INLINE LxVec3 lx_min3(LxVec3 a, LxVec3 b) { return lx_v3(lx_minf(a.x, b.x), lx_minf(a.y, b.y), lx_minf(a.z, b.z)); }
LX_INLINE LxVec3 lx_max3(LxVec3 a, LxVec3 b) { return lx_v3(lx_maxf(a.x, b.x), lx_maxf(a.y, b.y), lx_maxf(a.z, b.z)); }
LX_INLINE LxVec3 lx_lerp3(LxVec3 a, LxVec3 b, float t) { return lx_v3(lx_lerpf(a.x, b.x, t), lx_lerpf(a.y, b.y, t), lx_lerpf(a.z, b.z, t)); }
LX_INLINE float lx_comp3(LxVec3 a, int i) { return i == 0 ? a.x : (i == 1 ? a.y : a.z); }
LX_INLINE LxVec3 lx_reflect3(LxVec3 d, LxVec3 n) { return lx_sub3(d, lx_scale3(n, 2.0f * lx_dot3(d, n))); }

/* ---------------- quaternions ---------------- */
LX_INLINE LxQuat lx_quat_identity(void) { LxQuat q = {0, 0, 0, 1}; return q; }
LX_INLINE LxQuat lx_quat_axis_angle(LxVec3 axis, float rad) {
    LxVec3 a = lx_norm3(axis);
    float s = sinf(rad * 0.5f);
    LxQuat q = {a.x * s, a.y * s, a.z * s, cosf(rad * 0.5f)};
    return q;
}
LX_INLINE LxQuat lx_quat_mul(LxQuat a, LxQuat b) {
    LxQuat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
LX_INLINE LxQuat lx_quat_norm(LxQuat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-12f) return lx_quat_identity();
    q.x /= l; q.y /= l; q.z /= l; q.w /= l;
    return q;
}
LX_INLINE LxVec3 lx_quat_rotate(LxQuat q, LxVec3 v) {
    LxVec3 u = lx_v3(q.x, q.y, q.z);
    LxVec3 t = lx_scale3(lx_cross3(u, v), 2.0f);
    return lx_add3(lx_add3(v, lx_scale3(t, q.w)), lx_cross3(u, t));
}
/* Euler in degrees, applied Y (yaw) then X (pitch) then Z (roll). */
LX_INLINE LxQuat lx_quat_euler_deg(LxVec3 e) {
    LxQuat qy = lx_quat_axis_angle(lx_v3(0, 1, 0), e.y * LX_DEG2RAD);
    LxQuat qx = lx_quat_axis_angle(lx_v3(1, 0, 0), e.x * LX_DEG2RAD);
    LxQuat qz = lx_quat_axis_angle(lx_v3(0, 0, 1), e.z * LX_DEG2RAD);
    return lx_quat_norm(lx_quat_mul(lx_quat_mul(qy, qx), qz));
}

LX_INLINE LxQuat lx_quat_conj(LxQuat q) { LxQuat r = {-q.x, -q.y, -q.z, q.w}; return r; }
/* Inverse of lx_quat_euler_deg (Y, then X, then Z). */
LX_INLINE LxVec3 lx_quat_to_euler_deg(LxQuat q) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    float r00 = 1 - 2 * (yy + zz), r02 = 2 * (xz + wy), r10 = 2 * (xy + wz), r11 = 1 - 2 * (xx + zz);
    float r12 = 2 * (yz - wx), r20 = 2 * (xz - wy), r22 = 1 - 2 * (xx + yy);
    float sx = -r12;
    sx = sx > 1 ? 1 : (sx < -1 ? -1 : sx);
    LxVec3 e;
    e.x = asinf(sx);
    if (fabsf(sx) < 0.9999f) { e.y = atan2f(r02, r22); e.z = atan2f(r10, r11); }
    else { e.y = atan2f(-r20, r00); e.z = 0; }
    return lx_scale3(e, LX_RAD2DEG);
}
/* Rotation whose local X, Y, Z axes are the given orthonormal vectors. */
LX_INLINE LxQuat lx_quat_from_basis(LxVec3 x, LxVec3 y, LxVec3 z) {
    float tr = x.x + y.y + z.z;
    LxQuat q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q.w = 0.25f * s; q.x = (y.z - z.y) / s; q.y = (z.x - x.z) / s; q.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        float s = sqrtf(1.0f + x.x - y.y - z.z) * 2;
        q.w = (y.z - z.y) / s; q.x = 0.25f * s; q.y = (y.x + x.y) / s; q.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        float s = sqrtf(1.0f + y.y - x.x - z.z) * 2;
        q.w = (z.x - x.z) / s; q.x = (y.x + x.y) / s; q.y = 0.25f * s; q.z = (z.y + y.z) / s;
    } else {
        float s = sqrtf(1.0f + z.z - x.x - y.y) * 2;
        q.w = (x.y - y.x) / s; q.x = (z.x + x.z) / s; q.y = (z.y + y.z) / s; q.z = 0.25f * s;
    }
    return lx_quat_norm(q);
}
/* Rotation that makes local -Z (forward) point along `forward`. */
LX_INLINE LxQuat lx_quat_look(LxVec3 forward, LxVec3 up) {
    LxVec3 z = lx_scale3(lx_norm3(forward), -1.0f);
    LxVec3 x = lx_cross3(up, z);
    if (lx_dot3(x, x) < 1e-8f) x = lx_cross3(lx_v3(0, 0, 1), z);
    x = lx_norm3(x);
    LxVec3 y = lx_cross3(z, x);
    return lx_quat_from_basis(x, y, z);
}

/* ---------------- matrices ---------------- */
LX_INLINE LxMat4 lx_mat4_identity(void) {
    LxMat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return r;
}
LX_INLINE LxMat4 lx_mat4_mul(LxMat4 a, LxMat4 b) {
    LxMat4 r;
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            r.m[c * 4 + rw] = a.m[0 * 4 + rw] * b.m[c * 4 + 0] + a.m[1 * 4 + rw] * b.m[c * 4 + 1] +
                              a.m[2 * 4 + rw] * b.m[c * 4 + 2] + a.m[3 * 4 + rw] * b.m[c * 4 + 3];
    return r;
}
LX_INLINE LxVec3 lx_mat4_point(LxMat4 m, LxVec3 p) {
    return lx_v3(m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
                 m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
                 m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]);
}
LX_INLINE LxVec3 lx_mat4_dir(LxMat4 m, LxVec3 p) {
    return lx_v3(m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z,
                 m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z,
                 m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z);
}
LX_INLINE LxVec4 lx_mat4_v4(LxMat4 m, LxVec4 p) {
    LxVec4 r;
    r.x = m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12] * p.w;
    r.y = m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13] * p.w;
    r.z = m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14] * p.w;
    r.w = m.m[3] * p.x + m.m[7] * p.y + m.m[11] * p.z + m.m[15] * p.w;
    return r;
}
LX_INLINE LxMat4 lx_mat4_trs(LxVec3 t, LxQuat q, LxVec3 s) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    LxMat4 r;
    r.m[0] = (1 - 2 * (yy + zz)) * s.x; r.m[1] = 2 * (xy + wz) * s.x;       r.m[2] = 2 * (xz - wy) * s.x;        r.m[3] = 0;
    r.m[4] = 2 * (xy - wz) * s.y;       r.m[5] = (1 - 2 * (xx + zz)) * s.y; r.m[6] = 2 * (yz + wx) * s.y;        r.m[7] = 0;
    r.m[8] = 2 * (xz + wy) * s.z;       r.m[9] = 2 * (yz - wx) * s.z;       r.m[10] = (1 - 2 * (xx + yy)) * s.z; r.m[11] = 0;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; r.m[15] = 1;
    return r;
}
LX_INLINE LxMat4 lx_mat4_perspective(float fovy_rad, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    LxMat4 r = {{0}};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zf * zn) / (zn - zf);
    return r;
}
LX_INLINE LxMat4 lx_mat4_ortho(float l, float r_, float b, float t, float zn, float zf) {
    LxMat4 r = lx_mat4_identity();
    r.m[0] = 2.0f / (r_ - l);
    r.m[5] = 2.0f / (t - b);
    r.m[10] = -2.0f / (zf - zn);
    r.m[12] = -(r_ + l) / (r_ - l);
    r.m[13] = -(t + b) / (t - b);
    r.m[14] = -(zf + zn) / (zf - zn);
    return r;
}
LX_INLINE LxMat4 lx_mat4_look_at(LxVec3 eye, LxVec3 at, LxVec3 up) {
    LxVec3 f = lx_norm3(lx_sub3(at, eye));
    LxVec3 s = lx_norm3(lx_cross3(f, up));
    LxVec3 u = lx_cross3(s, f);
    LxMat4 r = lx_mat4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -lx_dot3(s, eye);
    r.m[13] = -lx_dot3(u, eye);
    r.m[14] = lx_dot3(f, eye);
    return r;
}
LxMat4 lx_mat4_inverse(LxMat4 m);   /* defined in core/math.c */
LX_INLINE LxMat4 lx_mat4_transpose(LxMat4 a) {
    LxMat4 r;
    for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) r.m[c * 4 + k] = a.m[k * 4 + c];
    return r;
}

/* ---------------- bounds / frustum ---------------- */
LX_INLINE LxAabb lx_aabb_empty(void) {
    LxAabb b = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    return b;
}
LX_INLINE void lx_aabb_grow(LxAabb *b, LxVec3 p) { b->min = lx_min3(b->min, p); b->max = lx_max3(b->max, p); }
LX_INLINE LxVec3 lx_aabb_center(LxAabb b) { return lx_scale3(lx_add3(b.min, b.max), 0.5f); }
LX_INLINE LxVec3 lx_aabb_extent(LxAabb b) { return lx_scale3(lx_sub3(b.max, b.min), 0.5f); }
LxAabb lx_aabb_transform(LxAabb b, LxMat4 m);
LxFrustum lx_frustum_from_matrix(LxMat4 viewproj);
int lx_frustum_test_aabb(const LxFrustum *f, LxAabb b);       /* 0 = outside */
int lx_frustum_test_sphere(const LxFrustum *f, LxVec3 c, float r);
/* Returns hit distance or -1 */
float lx_ray_aabb(LxRay r, LxAabb b);
float lx_ray_triangle(LxRay r, LxVec3 a, LxVec3 b, LxVec3 c, float *u, float *v);

/* Fast PCG32 random */
typedef struct LxRng { uint64_t state, inc; } LxRng;
LX_INLINE uint32_t lx_rng_u32(LxRng *r) {
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + (r->inc | 1u);
    uint32_t xs = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xs >> rot) | (xs << ((-(int32_t)rot) & 31));
}
LX_INLINE float lx_rng_f(LxRng *r) { return (lx_rng_u32(r) >> 8) * (1.0f / 16777216.0f); }
LX_INLINE LxRng lx_rng_seed(uint64_t seed, uint64_t seq) {
    LxRng r = {0, (seq << 1u) | 1u};
    lx_rng_u32(&r); r.state += seed; lx_rng_u32(&r);
    return r;
}
LX_INLINE LxVec3 lx_rng_sphere(LxRng *r) {
    float z = 1.0f - 2.0f * lx_rng_f(r);
    float a = 2.0f * LX_PI * lx_rng_f(r);
    float s = sqrtf(lx_maxf(0.0f, 1.0f - z * z));
    return lx_v3(s * cosf(a), s * sinf(a), z);
}
/* cosine-weighted hemisphere around n */
LX_INLINE LxVec3 lx_rng_cosine(LxRng *r, LxVec3 n) {
    float u1 = lx_rng_f(r), u2 = lx_rng_f(r);
    float rr = sqrtf(u1), phi = 2.0f * LX_PI * u2;
    LxVec3 t = fabsf(n.x) > 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0);
    LxVec3 b1 = lx_norm3(lx_cross3(t, n));
    LxVec3 b2 = lx_cross3(n, b1);
    float x = rr * cosf(phi), y = rr * sinf(phi), z = sqrtf(lx_maxf(0.0f, 1.0f - u1));
    return lx_add3(lx_add3(lx_scale3(b1, x), lx_scale3(b2, y)), lx_scale3(n, z));
}

#endif
