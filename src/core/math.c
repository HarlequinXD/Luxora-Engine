#include "../lx_internal.h"

LxMat4 lx_mat4_inverse(LxMat4 mat) {
    const float *m = mat.m;
    LxMat4 r;
    float *inv = r.m;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f) return lx_mat4_identity();
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) inv[i] *= det;
    return r;
}

LxAabb lx_aabb_transform(LxAabb b, LxMat4 m) {
    LxVec3 c = lx_aabb_center(b), e = lx_aabb_extent(b);
    LxVec3 nc = lx_mat4_point(m, c);
    LxVec3 ne = lx_v3(fabsf(m.m[0]) * e.x + fabsf(m.m[4]) * e.y + fabsf(m.m[8]) * e.z,
                      fabsf(m.m[1]) * e.x + fabsf(m.m[5]) * e.y + fabsf(m.m[9]) * e.z,
                      fabsf(m.m[2]) * e.x + fabsf(m.m[6]) * e.y + fabsf(m.m[10]) * e.z);
    LxAabb r = {lx_sub3(nc, ne), lx_add3(nc, ne)};
    return r;
}

LxFrustum lx_frustum_from_matrix(LxMat4 vp) {
    const float *m = vp.m;
    LxFrustum f;
    float rows[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) rows[r][c] = m[c * 4 + r];
    for (int i = 0; i < 3; i++) {
        for (int s = 0; s < 2; s++) {
            float sign = s == 0 ? 1.0f : -1.0f;
            LxPlane *p = &f.p[i * 2 + s];
            p->n = lx_v3(rows[3][0] + sign * rows[i][0], rows[3][1] + sign * rows[i][1], rows[3][2] + sign * rows[i][2]);
            p->d = rows[3][3] + sign * rows[i][3];
            float l = lx_len3(p->n);
            if (l > 0) { p->n = lx_scale3(p->n, 1.0f / l); p->d /= l; }
        }
    }
    return f;
}

int lx_frustum_test_aabb(const LxFrustum *f, LxAabb b) {
    LxVec3 c = lx_aabb_center(b), e = lx_aabb_extent(b);
    for (int i = 0; i < 6; i++) {
        const LxPlane *p = &f->p[i];
        float r = e.x * fabsf(p->n.x) + e.y * fabsf(p->n.y) + e.z * fabsf(p->n.z);
        if (lx_dot3(p->n, c) + p->d < -r) return 0;
    }
    return 1;
}

int lx_frustum_test_sphere(const LxFrustum *f, LxVec3 c, float r) {
    for (int i = 0; i < 6; i++)
        if (lx_dot3(f->p[i].n, c) + f->p[i].d < -r) return 0;
    return 1;
}

float lx_ray_aabb(LxRay r, LxAabb b) {
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        float o = lx_comp3(r.origin, i), d = lx_comp3(r.dir, i);
        float lo = lx_comp3(b.min, i), hi = lx_comp3(b.max, i);
        if (fabsf(d) < 1e-12f) {
            if (o < lo || o > hi) return -1.0f;
            continue;
        }
        float inv = 1.0f / d;
        float t0 = (lo - o) * inv, t1 = (hi - o) * inv;
        if (t0 > t1) { float t = t0; t0 = t1; t1 = t; }
        tmin = lx_maxf(tmin, t0);
        tmax = lx_minf(tmax, t1);
        if (tmin > tmax) return -1.0f;
    }
    return tmin;
}

float lx_ray_triangle(LxRay r, LxVec3 a, LxVec3 b, LxVec3 c, float *u, float *v) {
    LxVec3 e1 = lx_sub3(b, a), e2 = lx_sub3(c, a);
    LxVec3 p = lx_cross3(r.dir, e2);
    float det = lx_dot3(e1, p);
    if (fabsf(det) < 1e-10f) return -1.0f;
    float inv = 1.0f / det;
    LxVec3 t = lx_sub3(r.origin, a);
    float uu = lx_dot3(t, p) * inv;
    if (uu < 0 || uu > 1) return -1.0f;
    LxVec3 q = lx_cross3(t, e1);
    float vv = lx_dot3(r.dir, q) * inv;
    if (vv < 0 || uu + vv > 1) return -1.0f;
    float dist = lx_dot3(e2, q) * inv;
    if (dist <= 1e-5f) return -1.0f;
    if (u) *u = uu;
    if (v) *v = vv;
    return dist;
}
