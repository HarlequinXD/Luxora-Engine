/* Lightmapper: baked lighting for static geometry, in the spirit of Bakery for Unity.
 *
 * Each static mesh with lightmap UVs (src/gfx/lightmap_uv.c) gets a square tile in a lightmap atlas, sized
 * by its surface area and the scene's texels per unit. Every texel of the tile is placed on the mesh
 * (position and normal) and lit by path tracing through the static geometry:
 *   - light from Baked lights arriving straight at the texel, with soft shadows (an area/angle per light);
 *   - sky light, and light bounced off other surfaces up to `bounces` times. At every bounce the surface is lit
 *     by Mixed and Baked lights (shadow rays) and by its own glow. Realtime lights take no part in the bake.
 * Mixed lights therefore light surfaces directly in real time and contribute their bounced light here.
 * Texels that look at the back of geometry (inside a wall, under a box) are thrown away and filled in from
 * their neighbours, which stops light and shadow leaking along the floor under walls.
 * The result is irradiance (light arriving per square metre) plus the main direction it comes from, so normal
 * maps still respond to baked light. Atlases are cleaned up (denoise, fill padding) and saved as half-float
 * files next to the project's assets.
 *
 * The tracing runs on the GPU in compute (shaders/lm_bake.comp, any Vulkan GPU, no ray-tracing hardware) and,
 * when there is no GPU, on all CPU cores with the same algorithm. Bakes are progressive: one sample per texel
 * per pass, so the editor stays responsive and can show progress. */
#include "../lx_internal.h"

#define LM_FILE_MAGIC 0x4d4c584cu   /* "LXLM" */

static float lum3(LxVec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

/* ------------------------------------------------------------------ random numbers (same as the shader) */
static uint32_t lm_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
static float lm_rnd(uint32_t *s) {
    *s = *s * 747796405u + 2891336453u;
    uint32_t w = ((*s >> ((*s >> 28u) + 4u)) ^ *s) * 277803737u;
    return (float)(((w >> 22u) ^ w) >> 8) * (1.0f / 16777216.0f);
}
static void lm_basis(LxVec3 n, LxVec3 *t, LxVec3 *b) {
    LxVec3 a = fabsf(n.x) > 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0);
    *t = lx_norm3(lx_cross3(a, n));
    *b = lx_cross3(n, *t);
}
static LxVec3 lm_cosine(LxVec3 n, uint32_t *s) {
    float u1 = lm_rnd(s), u2 = lm_rnd(s);
    float r = sqrtf(u1), phi = 2.0f * LX_PI * u2;
    LxVec3 t, b;
    lm_basis(n, &t, &b);
    return lx_add3(lx_add3(lx_scale3(t, r * cosf(phi)), lx_scale3(b, r * sinf(phi))), lx_scale3(n, sqrtf(lx_maxf(0.0f, 1.0f - u1))));
}

/* ------------------------------------------------------------------ settings */
LxBakeSettings lx_bake_settings_defaults(void) {
    LxBakeSettings b;
    memset(&b, 0, sizeof b);
    b.texels_per_unit = 10.0f;
    b.max_atlas_size = 1024;
    b.bounces = 3;
    b.samples = 128;
    b.directional = true;
    b.denoise = true;
    b.ao_intensity = 1.0f;
    b.ao_radius = 1.0f;
    return b;
}

/* ------------------------------------------------------------------ scene -> bake */
static float smooth01(float e0, float e1, float x) {
    float t = lx_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* a point on the light (soft shadows): the way to it, how far, and the light's falloff there (no cosine) */
static void light_sample(const LxLmLight *L, LxVec3 x, uint32_t *rng, LxVec3 *dir_out, float *dist_out, float *att_out) {
    LxVec3 dir;
    float dist, att;
    if (L->type == LX_LIGHT_DIRECTIONAL) {
        /* a cone the size of the light's angle, around the direction it comes from */
        LxVec3 c = lx_scale3(L->dir, -1.0f), t, bb;
        lm_basis(c, &t, &bb);
        float ct = 1.0f - lm_rnd(rng) * (1.0f - L->cos_size), st = sqrtf(lx_maxf(0.0f, 1.0f - ct * ct)), ph = 2.0f * LX_PI * lm_rnd(rng);
        dir = lx_norm3(lx_add3(lx_scale3(c, ct), lx_add3(lx_scale3(t, st * cosf(ph)), lx_scale3(bb, st * sinf(ph)))));
        dist = 1e30f;
        att = 1.0f;
    } else {
        /* a point inside the light's sphere */
        LxVec3 p = L->pos;
        if (L->radius > 0) {
            LxVec3 r;
            do r = lx_v3(lm_rnd(rng) * 2 - 1, lm_rnd(rng) * 2 - 1, lm_rnd(rng) * 2 - 1);
            while (lx_dot3(r, r) > 1.0f);
            p = lx_add3(p, lx_scale3(r, L->radius));
        }
        LxVec3 to = lx_sub3(p, x);
        float d2 = lx_maxf(lx_dot3(to, to), 1e-4f);
        dist = sqrtf(d2);
        dir = lx_scale3(to, 1.0f / dist);
        float w = lx_clampf(1.0f - (d2 / (L->range * L->range)) * (d2 / (L->range * L->range)), 0.0f, 1.0f);
        att = w * w / d2;
        if (L->type == LX_LIGHT_SPOT) {
            float cd = lx_dot3(lx_scale3(dir, -1.0f), L->dir);
            att *= smooth01(L->cos_outer, L->cos_outer + (1.0f - L->cos_outer) * 0.25f, cd);
        }
    }
    *dir_out = dir;
    *dist_out = dist;
    *att_out = att;
}
static bool light_blocked(const LxLightmapBake *b, LxVec3 x, LxVec3 dir, float dist) {
    LxRay ray = {x, dir};
    return lx_bvh_occluded(&b->bvh, ray, dist < 1e29f ? dist - b->eps : 1e30f);
}

/* the light from one light arriving at x (normal n, geometric normal ng), shadows included */
static LxVec3 light_at(const LxLightmapBake *b, const LxLmLight *L, LxVec3 x, LxVec3 n, LxVec3 ng, uint32_t *rng) {
    LxVec3 dir;
    float dist, att;
    light_sample(L, x, rng, &dir, &dist, &att);
    float cs = lx_dot3(n, dir);
    if (att <= 0 || cs <= 0 || lx_dot3(ng, dir) <= 0) return lx_v3(0, 0, 0);
    if (light_blocked(b, x, dir, dist)) return lx_v3(0, 0, 0);
    return lx_scale3(L->color, att * cs);
}

static LxVec3 sky_at(const LxLightmapBake *b, LxVec3 d) {
    float t = lx_clampf(d.y, -1.0f, 1.0f);
    LxVec3 c = t >= 0 ? lx_lerp3(b->sky_horizon, b->sky_zenith, powf(lx_maxf(t, 0.0f), 0.45f))
                      : lx_lerp3(b->sky_horizon, b->ground, lx_clampf(-t * 4.0f, 0.0f, 1.0f));
    return lx_scale3(c, b->ambient);
}

/* one path-traced sample of the irradiance at a texel; adds to acc[0..7] */
static void trace_texel(const LxLightmapBake *b, int texel, uint32_t sample, float *acc) {
    const float *tx = &b->texels[(size_t)texel * 12];
    LxVec3 n = lx_v3(tx[4], tx[5], tx[6]), ng = lx_v3(tx[8], tx[9], tx[10]);
    LxVec3 x = lx_add3(lx_v3(tx[0], tx[1], tx[2]), lx_scale3(ng, b->eps));
    uint32_t rng = lm_hash((uint32_t)texel * 9781u + lm_hash(sample + 0x9e3779b9u));
    LxVec3 E = lx_v3(0, 0, 0), D = lx_v3(0, 0, 0);
    /* Baked lights straight onto the texel */
    for (int i = 0; i < b->light_count; i++) {
        const LxLmLight *L = &b->lights[i];
        if (L->mode != LX_LIGHT_BAKED) continue;
        LxVec3 to_l = L->type == LX_LIGHT_DIRECTIONAL ? lx_scale3(L->dir, -1.0f) : lx_norm3(lx_sub3(L->pos, x));
        LxVec3 e = light_at(b, L, x, n, ng, &rng);
        E = lx_add3(E, e);
        D = lx_add3(D, lx_scale3(to_l, lum3(e)));
    }
    /* sky and bounced light along a path */
    LxVec3 d = lm_cosine(n, &rng);
    float dn = lx_dot3(d, ng);
    if (dn <= 0) d = lx_norm3(lx_sub3(d, lx_scale3(ng, 2.0f * dn - 1e-3f)));   /* keep it above the surface */
    LxVec3 first_dir = d, T = lx_v3(LX_PI, LX_PI, LX_PI), o = x;
    LxVec3 path = lx_v3(0, 0, 0);
    float back = 0;
    for (int k = 0;; k++) {
        LxHit h;
        LxRay ray = {o, d};
        if (!lx_bvh_intersect(&b->bvh, ray, 1e30f, &h)) { path = lx_add3(path, lx_mul3(T, sky_at(b, d))); break; }
        LxVec3 hn = b->bvh.normals[h.tri];
        if (lx_dot3(hn, d) > 0) { if (k == 0) back = 1; break; }   /* the back of a surface: inside geometry */
        const LxTriMaterial *m = &b->bvh.materials[b->bvh.mat_index[h.tri]];
        path = lx_add3(path, lx_mul3(T, lx_scale3(m->emissive, 1.0f / LX_PI)));   /* glow: radiance = emitted / pi */
        if (k >= b->bounces) break;
        LxVec3 p = lx_add3(lx_add3(o, lx_scale3(d, h.t)), lx_scale3(hn, b->eps));
        T = lx_mul3(T, lx_scale3(m->albedo, 1.0f / LX_PI));
        LxVec3 eh = lx_v3(0, 0, 0);
        for (int i = 0; i < b->light_count; i++)
            if (b->lights[i].mode != LX_LIGHT_REALTIME) eh = lx_add3(eh, light_at(b, &b->lights[i], p, hn, hn, &rng));
        path = lx_add3(path, lx_mul3(T, eh));
        T = lx_scale3(T, LX_PI);
        if (lum3(T) < 1e-5f) break;
        o = p;
        d = lm_cosine(hn, &rng);
    }
    E = lx_add3(E, path);
    D = lx_add3(D, lx_scale3(first_dir, lum3(path)));
    /* ambient occlusion: does a short ray get out (corners, contact under objects)? */
    float open = 1;
    if (b->set.ao_intensity > 0 && b->set.ao_radius > 0) {
        LxVec3 ad = lm_cosine(n, &rng);
        float an = lx_dot3(ad, ng);
        if (an <= 0) ad = lx_norm3(lx_sub3(ad, lx_scale3(ng, 2.0f * an - 1e-3f)));
        LxRay ar = {x, ad};
        open = lx_bvh_occluded(&b->bvh, ar, b->set.ao_radius) ? 0.0f : 1.0f;
    }
    acc[0] += E.x; acc[1] += E.y; acc[2] += E.z; acc[3] += 1.0f;
    acc[4] += D.x; acc[5] += D.y; acc[6] += D.z; acc[7] += back;
    acc[8] += open;
    acc[9] += E.x - path.x; acc[10] += E.y - path.y; acc[11] += E.z - path.z;   /* Baked lights straight on (AO leaves it alone) */
}

/* uniform direction on the sphere */
static LxVec3 lm_sphere(uint32_t *s) {
    float z = 1.0f - 2.0f * lm_rnd(s), r = sqrtf(lx_maxf(0.0f, 1.0f - z * z)), ph = 2.0f * LX_PI * lm_rnd(s);
    return lx_v3(r * cosf(ph), r * sinf(ph), z);
}
/* adds light from direction d into L1 SH (L0, y, z, x per channel), as the photon GI stores it */
static void sh_add(float *sh, LxVec3 d, LxVec3 c) {
    float y[4] = {0.282095f, 0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x};
    for (int k = 0; k < 4; k++) { sh[k] += c.x * y[k]; sh[4 + k] += c.y * y[k]; sh[8 + k] += c.z * y[k]; }
}

/* one sample of the light at a volume voxel (a point in the air, every direction): adds to acc[0..19]
 * (SH red, green, blue; samples; back-face hits; -; -; shadowmask channels 0..3) */
static void trace_voxel(const LxLightmapBake *b, int vox, uint32_t sample, float *acc) {
    const float *vp = &b->vox_pos[(size_t)vox * 4];
    LxVec3 x = lx_v3(vp[0], vp[1], vp[2]);
    uint32_t rng = lm_hash((uint32_t)vox * 7919u + 0x51ed27u + lm_hash(sample + 0x9e3779b9u));
    float sh[12] = {0};
    /* Baked lights: arriving from their direction (a character standing here faces them however it turns) */
    for (int i = 0; i < b->light_count; i++) {
        const LxLmLight *L = &b->lights[i];
        LxVec3 dir;
        float dist, att;
        light_sample(L, x, &rng, &dir, &dist, &att);
        bool lit = att > 0 && !light_blocked(b, x, dir, dist);
        if (L->mask_channel >= 0) acc[16 + L->mask_channel] += lit || att <= 0 ? 1.0f : 0.0f;   /* Mixed: is it in shadow here? */
        if (L->mode == LX_LIGHT_BAKED && lit) sh_add(sh, dir, lx_scale3(L->color, att));
    }
    /* sky and bounced light from a random direction */
    LxVec3 d = lm_sphere(&rng), first = d, T = lx_v3(1, 1, 1), o = x, rad = lx_v3(0, 0, 0);
    float back = 0;
    for (int k = 0;; k++) {
        LxHit h;
        LxRay ray = {o, d};
        if (!lx_bvh_intersect(&b->bvh, ray, 1e30f, &h)) { rad = lx_add3(rad, lx_mul3(T, sky_at(b, d))); break; }
        LxVec3 hn = b->bvh.normals[h.tri];
        if (lx_dot3(hn, d) > 0) { if (k == 0) back = 1; break; }
        const LxTriMaterial *m = &b->bvh.materials[b->bvh.mat_index[h.tri]];
        rad = lx_add3(rad, lx_mul3(T, lx_scale3(m->emissive, 1.0f / LX_PI)));
        if (k >= b->bounces) break;
        LxVec3 p = lx_add3(lx_add3(o, lx_scale3(d, h.t)), lx_scale3(hn, b->eps));
        T = lx_mul3(T, lx_scale3(m->albedo, 1.0f / LX_PI));
        LxVec3 eh = lx_v3(0, 0, 0);
        for (int i = 0; i < b->light_count; i++)
            if (b->lights[i].mode != LX_LIGHT_REALTIME) eh = lx_add3(eh, light_at(b, &b->lights[i], p, hn, hn, &rng));
        rad = lx_add3(rad, lx_mul3(T, eh));
        T = lx_scale3(T, LX_PI);
        if (lum3(T) < 1e-5f) break;
        o = p;
        d = lm_cosine(hn, &rng);
    }
    sh_add(sh, first, lx_scale3(rad, 4.0f * LX_PI));   /* Monte Carlo over the sphere: / pdf (1 / 4 pi) */
    for (int k = 0; k < 12; k++) acc[k] += sh[k];
    acc[12] += 1.0f;
    acc[13] += back;
}

typedef struct CpuPass { LxLightmapBake *b; int first; uint32_t sample; } CpuPass;
static void cpu_pass_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    CpuPass *p = user;
    for (int i = begin; i < end; i++) {
        int t = p->first + i;
        if (t < p->b->texel_count) {
            if (p->b->texels[(size_t)t * 12 + 3] > 0) trace_texel(p->b, t, p->sample, &p->b->accum[(size_t)t * LX_LM_ACC]);
        } else {
            int v = t - p->b->texel_count;
            trace_voxel(p->b, v, p->sample, &p->b->vox_acc[(size_t)v * 20]);
        }
    }
}
void lx_lightmap_trace_cpu(LxLightmapBake *b, int first, int count, uint32_t sample) {
    CpuPass p = {b, first, sample};
    lx_parallel_for(count, 256, cpu_pass_job, &p);
}

/* ------------------------------------------------------------------ preparing a bake */
typedef struct Tile { int entity, size, atlas, x, y; } Tile;
static int tile_cmp(const void *pa, const void *pb) {
    const Tile *a = pa, *b = pb;
    if (a->size != b->size) return b->size - a->size;
    return a->entity - b->entity;
}

/* closest point of a 2D triangle to p, as barycentrics; returns the distance */
static float closest_bary(float px, float py, const float *t, float *w) {
    float ax = t[0], ay = t[1], bx = t[2], by = t[3], cx = t[4], cy = t[5];
    float den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (fabsf(den) < 1e-12f) { w[0] = 1; w[1] = w[2] = 0; return 1e30f; }
    float l0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
    float l1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
    float l2 = 1 - l0 - l1;
    if (l0 >= 0 && l1 >= 0 && l2 >= 0) { w[0] = l0; w[1] = l1; w[2] = l2; return 0; }
    /* outside: nearest point on the three edges */
    float best = 1e30f;
    const float *P[3] = {&t[0], &t[2], &t[4]};
    for (int e = 0; e < 3; e++) {
        const float *a = P[e], *c = P[(e + 1) % 3];
        float ex = c[0] - a[0], ey = c[1] - a[1], l2e = ex * ex + ey * ey;
        float s = l2e > 0 ? lx_clampf(((px - a[0]) * ex + (py - a[1]) * ey) / l2e, 0, 1) : 0;
        float qx = a[0] + ex * s - px, qy = a[1] + ey * s - py, dd = sqrtf(qx * qx + qy * qy);
        if (dd < best) {
            best = dd;
            w[0] = w[1] = w[2] = 0;
            w[e] = 1 - s;
            w[(e + 1) % 3] = s;
        }
    }
    return best;
}

static bool lightmapped(LxScene *s, int i) {
    LxEntityData *d = &s->entities[i];
    if (!d->alive || !d->active || !(d->comps & LX_COMP_MESH) || !d->mesh.static_geometry) return false;
    if (!lx_entity_can_be_static(s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), NULL)) return false;
    for (LxEntity p = d->parent; p; p = lx_entity_data(s, p)->parent)
        if (!lx_entity_data(s, p)->active) return false;
    const LxMesh *m = lx_asset_mesh(d->mesh.mesh);
    return m && m->has_uv2 && !m->skin;
}

LxLightmapBake *lx_lightmap_prepare(LxScene *s) {
    lx_scene_update_transforms(s);
    LxLightmapBake *b = lx_calloc(1, sizeof *b);
    b->set = s->bake;
    if (b->set.texels_per_unit <= 0) b->set.texels_per_unit = 10;
    int amax = b->set.max_atlas_size;
    if (amax < 64) amax = 64;
    if (amax > 4096) amax = 4096;
    b->bounces = b->set.bounces < 0 ? 0 : b->set.bounces > 8 ? 8 : b->set.bounces;

    /* tiles: one square per lightmapped object, sized by its surface */
    Tile *tiles = NULL;
    int nt = 0, tcap = 0;
    for (int i = 1; i < s->count; i++) {
        if (!lightmapped(s, i)) continue;
        LxEntityData *d = &s->entities[i];
        const LxMesh *m = lx_mesh_lightmap_variant(lx_asset_mesh(d->mesh.mesh), lx_mat4_scale_of(&d->world));
        const LxMeshLod *l = &m->lods[0];
        double wa = 0, ua = 0;
        for (int t = 0; t + 2 < l->index_count; t += 3) {
            const LxVertex *va = &l->vertices[l->indices[t]], *vb = &l->vertices[l->indices[t + 1]], *vc = &l->vertices[l->indices[t + 2]];
            LxVec3 a = lx_mat4_point(d->world, lx_v3(va->px, va->py, va->pz)), bb = lx_mat4_point(d->world, lx_v3(vb->px, vb->py, vb->pz)),
                   c = lx_mat4_point(d->world, lx_v3(vc->px, vc->py, vc->pz));
            wa += 0.5 * lx_len3(lx_cross3(lx_sub3(bb, a), lx_sub3(c, a)));
            ua += 0.5 * fabs((double)(vb->u2 - va->u2) * (vc->v2 - va->v2) - (double)(vc->u2 - va->u2) * (vb->v2 - va->v2));
        }
        if (wa <= 0 || ua <= 0) continue;
        float side = (float)sqrt(wa / ua);   /* metres across the 0..1 square */
        int size = (int)ceilf(side * b->set.texels_per_unit);
        int min_size = m->lm_pack_res > 0 ? m->lm_pack_res / 2 : 8;
        if (min_size > 128) min_size = 128;   /* keeps the unwrap's padding at least a texel */
        if (min_size < 8) min_size = 8;
        if (size < min_size) size = min_size;
        if (size > amax) size = amax;
        Tile tl = {i, size, 0, 0, 0};
        LX_VEC_PUSH(tiles, nt, tcap, tl);
    }
    /* Baked Lighting Volumes: a voxel every 1 / density metres through each box */
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_BAKE_VOLUME)) continue;
        LxVec3 sc = lx_mat4_scale_of(&d->world), size = lx_v3(fabsf(d->bvol.size.x) * sc.x, fabsf(d->bvol.size.y) * sc.y, fabsf(d->bvol.size.z) * sc.z);
        float dens = lx_clampf(d->bvol.density, 0.05f, 16.0f);
        LxLmVolume v;
        memset(&v, 0, sizeof v);
        v.entity = i;
        v.n[0] = (int)lx_clampf(ceilf(size.x * dens), 2, LX_VOLUME_MAX_RES);
        v.n[1] = (int)lx_clampf(ceilf(size.y * dens), 2, LX_VOLUME_MAX_RES);
        v.n[2] = (int)lx_clampf(ceilf(size.z * dens), 2, LX_VOLUME_MAX_RES);
        v.first = b->voxel_count;
        LX_VEC_PUSH(b->vols, b->vol_count, b->vol_cap, v);
        b->voxel_count += v.n[0] * v.n[1] * v.n[2];
    }
    if (!nt && !b->vol_count) { lx_free(tiles); lx_free(b); return NULL; }
    if (nt) qsort(tiles, (size_t)nt, sizeof(Tile), tile_cmp);
    /* shelves in atlases */
    int atlas = 0, x = 0, y = 0, shelf = 0, extent = 0;
    for (int k = 0; k < nt; k++) {
        Tile *t = &tiles[k];
        if (x + t->size > amax) { y += shelf; x = 0; shelf = 0; }
        if (y + t->size > amax) { atlas++; x = y = shelf = 0; }
        t->atlas = atlas; t->x = x; t->y = y;
        x += t->size;
        if (t->size > shelf) shelf = t->size;
        if (x > extent) extent = x;
        if (y + t->size > extent) extent = y + t->size;
    }
    b->atlas_count = nt ? atlas + 1 : 0;
    int size = 64;
    if (b->atlas_count > 1) size = amax;
    else while (size < extent) size <<= 1;
    if (!nt) size = 0;
    b->atlas_size = size;
    b->tiles = lx_alloc(sizeof(LxLmTile) * (size_t)(nt ? nt : 1));
    b->tile_count = nt;
    for (int k = 0; k < nt; k++) {
        b->tiles[k].entity = tiles[k].entity;
        b->tiles[k].atlas = tiles[k].atlas;
        b->tiles[k].x = tiles[k].x; b->tiles[k].y = tiles[k].y; b->tiles[k].size = tiles[k].size;
    }

    /* texels: place each one on its mesh (closest point of the nearest triangle, so edge texels are covered too) */
    size_t texels = (size_t)b->atlas_count * (size_t)size * (size_t)size;
    b->texel_count = (int)texels;
    b->texels = lx_calloc(texels * 12, sizeof(float));
    b->accum = lx_calloc(texels * LX_LM_ACC, sizeof(float));
    b->owner = lx_alloc(sizeof(int) * texels);
    float *bestd = lx_alloc(sizeof(float) * texels);
    for (size_t i = 0; i < texels; i++) { b->owner[i] = -1; bestd[i] = 1e30f; }
    float scene_ext = 0;
    for (int k = 0; k < nt; k++) {
        const LxLmTile *tl = &b->tiles[k];
        LxEntityData *d = &s->entities[tl->entity];
        const LxMesh *m = lx_mesh_lightmap_variant(lx_asset_mesh(d->mesh.mesh), lx_mat4_scale_of(&d->world));
        const LxMeshLod *l = &m->lods[0];
        LxMat4 nm = lx_mat4_transpose(lx_mat4_inverse(d->world));
        float texel_m = 0;
        for (int t = 0; t + 2 < l->index_count; t += 3) {
            const LxVertex *v[3] = {&l->vertices[l->indices[t]], &l->vertices[l->indices[t + 1]], &l->vertices[l->indices[t + 2]]};
            LxVec3 p[3], nn[3];
            float uv[6];
            for (int j = 0; j < 3; j++) {
                p[j] = lx_mat4_point(d->world, lx_v3(v[j]->px, v[j]->py, v[j]->pz));
                nn[j] = lx_norm3(lx_mat4_dir(nm, lx_v3(v[j]->nx, v[j]->ny, v[j]->nz)));
                uv[j * 2] = (float)tl->x + v[j]->u2 * (float)tl->size;
                uv[j * 2 + 1] = (float)tl->y + v[j]->v2 * (float)tl->size;
                scene_ext = lx_maxf(scene_ext, lx_maxf(fabsf(p[j].x), lx_maxf(fabsf(p[j].y), fabsf(p[j].z))));
            }
            LxVec3 gn = lx_cross3(lx_sub3(p[1], p[0]), lx_sub3(p[2], p[0]));
            float wa = lx_len3(gn);
            if (wa < 1e-12f) continue;
            gn = lx_scale3(gn, 1.0f / wa);
            float ua = fabsf((uv[2] - uv[0]) * (uv[5] - uv[1]) - (uv[4] - uv[0]) * (uv[3] - uv[1]));
            if (ua > 1e-9f) texel_m = lx_maxf(texel_m, sqrtf(wa / ua));
            int x0 = (int)floorf(lx_minf(uv[0], lx_minf(uv[2], uv[4])) - 1), x1 = (int)ceilf(lx_maxf(uv[0], lx_maxf(uv[2], uv[4])) + 1);
            int y0 = (int)floorf(lx_minf(uv[1], lx_minf(uv[3], uv[5])) - 1), y1 = (int)ceilf(lx_maxf(uv[1], lx_maxf(uv[3], uv[5])) + 1);
            if (x0 < tl->x) x0 = tl->x;
            if (y0 < tl->y) y0 = tl->y;
            if (x1 > tl->x + tl->size - 1) x1 = tl->x + tl->size - 1;
            if (y1 > tl->y + tl->size - 1) y1 = tl->y + tl->size - 1;
            for (int yy = y0; yy <= y1; yy++)
                for (int xx = x0; xx <= x1; xx++) {
                    float w[3];
                    float dd = closest_bary((float)xx + 0.5f, (float)yy + 0.5f, uv, w);
                    if (dd > 0.7072f) continue;   /* the triangle doesn't touch this texel */
                    size_t ti = ((size_t)tl->atlas * (size_t)size + (size_t)yy) * (size_t)size + (size_t)xx;
                    if (dd >= bestd[ti]) continue;
                    bestd[ti] = dd;
                    b->owner[ti] = k;
                    float *o = &b->texels[ti * 12];
                    LxVec3 pos = lx_add3(lx_add3(lx_scale3(p[0], w[0]), lx_scale3(p[1], w[1])), lx_scale3(p[2], w[2]));
                    LxVec3 sn = lx_add3(lx_add3(lx_scale3(nn[0], w[0]), lx_scale3(nn[1], w[1])), lx_scale3(nn[2], w[2]));
                    sn = lx_len3(sn) > 1e-6f ? lx_norm3(sn) : gn;
                    if (lx_dot3(sn, gn) < 0.05f) sn = gn;   /* bent too far from the face */
                    o[0] = pos.x; o[1] = pos.y; o[2] = pos.z; o[3] = 1;
                    o[4] = sn.x; o[5] = sn.y; o[6] = sn.z;
                    o[8] = gn.x; o[9] = gn.y; o[10] = gn.z;
                }
        }
        /* each texel's size in metres (for the denoiser) */
        for (int yy = tl->y; yy < tl->y + tl->size; yy++)
            for (int xx = tl->x; xx < tl->x + tl->size; xx++) {
                size_t ti = ((size_t)tl->atlas * (size_t)size + (size_t)yy) * (size_t)size + (size_t)xx;
                if (b->owner[ti] == k) b->texels[ti * 12 + 7] = texel_m;
            }
    }
    lx_free(bestd);
    lx_free(tiles);
    /* voxel centres, in the volume's box (its rotation and scale apply) */
    b->vox_pos = lx_calloc((size_t)(b->voxel_count ? b->voxel_count : 1) * 4, sizeof(float));
    b->vox_acc = lx_calloc((size_t)(b->voxel_count ? b->voxel_count : 1) * 20, sizeof(float));
    for (int k = 0; k < b->vol_count; k++) {
        const LxLmVolume *v = &b->vols[k];
        const LxEntityData *d = &s->entities[v->entity];
        for (int z = 0; z < v->n[2]; z++)
            for (int y = 0; y < v->n[1]; y++)
                for (int x2 = 0; x2 < v->n[0]; x2++) {
                    LxVec3 local = lx_v3(((float)x2 + 0.5f) / (float)v->n[0] - 0.5f, ((float)y + 0.5f) / (float)v->n[1] - 0.5f,
                                         ((float)z + 0.5f) / (float)v->n[2] - 0.5f);
                    local = lx_v3(local.x * d->bvol.size.x, local.y * d->bvol.size.y, local.z * d->bvol.size.z);
                    LxVec3 w = lx_mat4_point(d->world, local);
                    float *o = &b->vox_pos[(size_t)(v->first + (z * v->n[1] + y) * v->n[0] + x2) * 4];
                    o[0] = w.x; o[1] = w.y; o[2] = w.z; o[3] = 1;
                    scene_ext = lx_maxf(scene_ext, lx_maxf(fabsf(w.x), lx_maxf(fabsf(w.y), fabsf(w.z))));
                }
    }
    b->eps = lx_maxf(1e-3f, scene_ext * 2e-5f);

    /* what the light hits: the level's static geometry */
    lx_bvh_build_from_scene(&b->bvh, s);
    /* lights */
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_LIGHT) || d->light.mode == LX_LIGHT_REALTIME) continue;
        bool hidden = false;
        for (LxEntity p = d->parent; p && !hidden; p = lx_entity_data(s, p)->parent) hidden = !lx_entity_data(s, p)->active;
        if (hidden || d->light.intensity <= 0) continue;
        LxLmLight L;
        memset(&L, 0, sizeof L);
        L.type = d->light.type;
        L.mode = d->light.mode;
        L.color = lx_scale3(d->light.color, d->light.intensity);
        L.pos = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
        L.dir = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, -1)));
        L.range = d->light.range > 0.01f ? d->light.range : 0.01f;
        L.cos_outer = cosf(d->light.spot_angle_deg * 0.5f * LX_DEG2RAD);
        float soft = lx_maxf(0.0f, d->light.shadow_radius);
        if (L.type == LX_LIGHT_DIRECTIONAL) L.cos_size = cosf(lx_clampf(soft, 0.0f, 30.0f) * 0.5f * LX_DEG2RAD);   /* degrees across */
        else L.radius = soft;
        L.entity = i;
        /* Mixed lights get a shadowmask channel (4 at most): volumes remember where static objects shade them */
        L.mask_channel = -1;
        if (L.mode == LX_LIGHT_MIXED && b->vol_count) {
            int used = 0;
            for (int k = 0; k < b->light_count; k++) used += b->lights[k].mask_channel >= 0;
            if (used < 4) L.mask_channel = used;
        }
        LX_VEC_PUSH(b->lights, b->light_count, b->light_cap, L);
    }
    b->sky_zenith = s->env.sky_zenith;
    b->sky_horizon = s->env.sky_horizon;
    b->ground = s->env.ground_color;
    b->ambient = s->env.ambient_intensity;
    return b;
}

void lx_lightmap_bake_free(LxLightmapBake *b) {
    if (!b) return;
    lx_bvh_free(&b->bvh);
    lx_free(b->lights); lx_free(b->tiles); lx_free(b->texels); lx_free(b->accum); lx_free(b->owner);
    lx_free(b->color); lx_free(b->dir);
    lx_free(b->vols); lx_free(b->vox_pos); lx_free(b->vox_acc); lx_free(b->vox_out);
    lx_free(b);
}

/* ------------------------------------------------------------------ finishing: average, denoise, fill */
void lx_lightmap_resolve(LxLightmapBake *b) {
    int A = b->atlas_size;
    size_t n = (size_t)b->texel_count;
    lx_free(b->color); lx_free(b->dir);
    b->color = lx_calloc(n * 4, sizeof(float));
    b->dir = lx_calloc(n * 4, sizeof(float));
    uint8_t *valid = lx_calloc(n, 1), *inside = lx_calloc(n ? n : 1, 1);
    float ao_k = lx_clampf(b->set.ao_intensity, 0.0f, 1.0f);
    for (size_t i = 0; i < n; i++) {
        const float *a = &b->accum[i * LX_LM_ACC];
        if (b->owner[i] < 0 || a[3] <= 0) continue;
        if (a[7] / a[3] > 0.5f) { inside[i] = 1; continue; }   /* mostly sees the back of geometry: covered by something */
        valid[i] = 1;
        float inv = 1.0f / a[3];
        /* ambient occlusion: corners and contact darken */
        /* (on the sky and bounced light: the light of Baked lights shining straight on has its own exact shadows) */
        float ao = b->set.ao_radius > 0 ? 1.0f - ao_k * (1.0f - a[8] * inv) : 1.0f;
        for (int c = 0; c < 3; c++) b->color[i * 4 + c] = (a[9 + c] + (a[c] - a[9 + c]) * ao) * inv;
        b->color[i * 4 + 3] = 1;
        float l = lum3(lx_v3(a[0], a[1], a[2]));
        LxVec3 dv = l > 1e-8f ? lx_scale3(lx_v3(a[4], a[5], a[6]), 1.0f / l) : lx_v3(0, 0, 0);
        if (lx_len3(dv) > 1) dv = lx_norm3(dv);
        b->dir[i * 4] = dv.x; b->dir[i * 4 + 1] = dv.y; b->dir[i * 4 + 2] = dv.z;
    }
    /* denoise: average with neighbours on the same object that face the same way and lie in the same plane */
    if (b->set.denoise) {
        float *out = lx_alloc(sizeof(float) * n * 4);
        memcpy(out, b->color, sizeof(float) * n * 4);
        const int R = 2;
        for (int a = 0; a < b->atlas_count; a++)
            for (int y = 0; y < A; y++)
                for (int x = 0; x < A; x++) {
                    size_t i = ((size_t)a * A + y) * A + x;
                    if (!valid[i]) continue;
                    const float *ti = &b->texels[i * 12];
                    LxVec3 pi = lx_v3(ti[0], ti[1], ti[2]), ni = lx_v3(ti[4], ti[5], ti[6]);
                    float tm = lx_maxf(ti[7], 1e-4f);
                    float li = lum3(lx_v3(b->color[i * 4], b->color[i * 4 + 1], b->color[i * 4 + 2]));
                    LxVec3 sum = lx_v3(0, 0, 0);
                    float ws = 0;
                    for (int dy = -R; dy <= R; dy++)
                        for (int dx = -R; dx <= R; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= A || yy >= A) continue;
                            size_t j = ((size_t)a * A + yy) * A + xx;
                            if (!valid[j] || b->owner[j] != b->owner[i]) continue;
                            const float *tj = &b->texels[j * 12];
                            LxVec3 nj = lx_v3(tj[4], tj[5], tj[6]);
                            float nd = lx_dot3(ni, nj);
                            if (nd < 0.8f) continue;
                            float plane = fabsf(lx_dot3(lx_sub3(lx_v3(tj[0], tj[1], tj[2]), pi), ni)) / tm;   /* steps off the plane, in texels */
                            LxVec3 cj = lx_v3(b->color[j * 4], b->color[j * 4 + 1], b->color[j * 4 + 2]);
                            float lj = lum3(cj);
                            float rel = fabsf(lj - li) / (lx_maxf(li, lj) + 1e-3f);   /* keeps shadow edges */
                            float w = expf(-(float)(dx * dx + dy * dy) / 4.5f) * powf(nd, 16.0f) * expf(-plane * plane * 4.0f) * expf(-rel * rel * 6.0f);
                            sum = lx_add3(sum, lx_scale3(cj, w));
                            ws += w;
                        }
                    if (ws > 0) { out[i * 4] = sum.x / ws; out[i * 4 + 1] = sum.y / ws; out[i * 4 + 2] = sum.z / ws; }
                }
        lx_free(b->color);
        b->color = out;
    }
    /* texels covered by something (a floor under a box standing on it) are dark: no light gets there, and the
     * darkness reaching a texel's width past the box's foot is its contact shadow. Filling them from their lit
     * neighbours would put light under the box and cut its shadow off at its outline. */
    for (size_t i = 0; i < n; i++)
        if (inside[i]) {
            for (int k = 0; k < 4; k++) { b->color[i * 4 + k] = 0; b->dir[i * 4 + k] = 0; }
            valid[i] = 1;
        }
    lx_free(inside);
    /* fill: texels without light of their own (padding around the charts) take their object's neighbours', ring
     * by ring, so filtering never reads black at chart edges */
    uint8_t *nv = lx_alloc(n);
    for (int pass = 0; pass < 16; pass++) {
        memcpy(nv, valid, n);
        int filled = 0;
        for (int a = 0; a < b->atlas_count; a++)
            for (int y = 0; y < A; y++)
                for (int x = 0; x < A; x++) {
                    size_t i = ((size_t)a * A + y) * A + x;
                    if (valid[i]) continue;
                    float c[8] = {0};
                    int cnt = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if ((!dx && !dy) || xx < 0 || yy < 0 || xx >= A || yy >= A) continue;
                            size_t j = ((size_t)a * A + yy) * A + xx;
                            if (!valid[j]) continue;
                            for (int k = 0; k < 4; k++) { c[k] += b->color[j * 4 + k]; c[4 + k] += b->dir[j * 4 + k]; }
                            cnt++;
                        }
                    if (!cnt) continue;
                    for (int k = 0; k < 4; k++) { b->color[i * 4 + k] = c[k] / cnt; b->dir[i * 4 + k] = c[4 + k] / cnt; }
                    nv[i] = 1;
                    filled++;
                }
        memcpy(valid, nv, n);
        if (!filled) break;
    }
    lx_free(nv);
    lx_free(valid);
    /* direction map: the main direction (xyz, shorter when light comes from all around) and how much of it the
     * texel's own normal catches (w), so the shader can turn the irradiance towards a bumped normal */
    for (size_t i = 0; i < n; i++) {
        LxVec3 dv = lx_v3(b->dir[i * 4], b->dir[i * 4 + 1], b->dir[i * 4 + 2]);
        if (!b->set.directional) dv = lx_v3(0, 0, 0);
        const float *ti = &b->texels[i * 12];
        float w = lx_dot3(dv, lx_v3(ti[4], ti[5], ti[6])) * 0.5f + 0.5f;
        b->dir[i * 4] = dv.x * 0.5f + 0.5f; b->dir[i * 4 + 1] = dv.y * 0.5f + 0.5f; b->dir[i * 4 + 2] = dv.z * 0.5f + 0.5f;
        b->dir[i * 4 + 3] = lx_clampf(w, 0.0f, 1.0f);
        b->color[i * 4 + 3] = 1;
    }
    /* volumes: average; voxels inside geometry take their neighbours' light, ring by ring */
    lx_free(b->vox_out);
    b->vox_out = lx_calloc((size_t)(b->voxel_count ? b->voxel_count : 1) * 16, sizeof(float));
    uint8_t *ok = lx_calloc((size_t)(b->voxel_count ? b->voxel_count : 1), 1), *nok = lx_alloc((size_t)(b->voxel_count ? b->voxel_count : 1));
    for (int v = 0; v < b->voxel_count; v++) {
        const float *a = &b->vox_acc[(size_t)v * 20];
        float *o = &b->vox_out[(size_t)v * 16];
        for (int k = 12; k < 16; k++) o[k] = 1;   /* no shadowmask channel: lit */
        if (a[12] <= 0) continue;
        for (int k = 0; k < 4; k++) o[12 + k] = a[16 + k] / a[12];
        if (a[13] / a[12] > 0.4f) continue;   /* mostly sees the back of geometry: inside a wall */
        for (int k = 0; k < 12; k++) o[k] = a[k] / a[12];
        ok[v] = 1;
    }
    for (int vi = 0; vi < b->vol_count; vi++) {
        const LxLmVolume *vol = &b->vols[vi];
        int nx = vol->n[0], ny = vol->n[1], nz = vol->n[2];
        for (int pass = 0; pass < LX_VOLUME_MAX_RES; pass++) {
            int filled = 0;
            memcpy(nok + vol->first, ok + vol->first, (size_t)nx * ny * nz);
            for (int z = 0; z < nz; z++)
                for (int y = 0; y < ny; y++)
                    for (int x = 0; x < nx; x++) {
                        int v = vol->first + (z * ny + y) * nx + x;
                        if (ok[v]) continue;
                        float sum[16] = {0};
                        int c = 0;
                        static const int D[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                        for (int k = 0; k < 6; k++) {
                            int xx = x + D[k][0], yy = y + D[k][1], zz = z + D[k][2];
                            if (xx < 0 || yy < 0 || zz < 0 || xx >= nx || yy >= ny || zz >= nz) continue;
                            int u = vol->first + (zz * ny + yy) * nx + xx;
                            if (!ok[u]) continue;
                            for (int q = 0; q < 16; q++) sum[q] += b->vox_out[(size_t)u * 16 + q];
                            c++;
                        }
                        if (!c) continue;
                        for (int q = 0; q < 12; q++) b->vox_out[(size_t)v * 16 + q] = sum[q] / (float)c;   /* light; its own shadowmask stays */
                        nok[v] = 1;
                        filled++;
                    }
            memcpy(ok + vol->first, nok + vol->first, (size_t)nx * ny * nz);
            if (!filled) break;
        }
    }
    lx_free(ok);
    lx_free(nok);
}

/* ------------------------------------------------------------------ files */
static uint16_t lm_f2h(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int e = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (((x >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00u | (m ? 0x200u : 0));
    if (e >= 31) return (uint16_t)(sign | 0x7bffu);   /* clamp to the largest half */
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000u;
        return (uint16_t)(sign | (m >> (uint32_t)(14 - e)));
    }
    return (uint16_t)(sign | ((uint32_t)e << 10) | (m >> 13));
}
static float lm_h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ffu, x;
    if (!e) {
        if (!m) x = s;
        else { e = 127 - 15 + 1; while (!(m & 0x400u)) { m <<= 1; e--; } m &= 0x3ffu; x = s | (e << 23) | (m << 13); }
    } else if (e == 31) x = s | 0x7f800000u | (m << 13);
    else x = s | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* <dir>/lm<i>.lxlm: magic, version, size, layers, then RGBA half floats layer by layer (colour, direction) */
bool lx_lightmap_write(const char *path, int size, const float *color, const float *dir) {
    size_t px = (size_t)size * (size_t)size;
    size_t bytes = 16 + px * 8 * 2;
    uint8_t *buf = lx_alloc(bytes);
    uint32_t hdr[4] = {LM_FILE_MAGIC, 1, (uint32_t)size, 2};
    memcpy(buf, hdr, 16);
    uint16_t *h = (uint16_t *)(buf + 16);
    for (size_t i = 0; i < px * 4; i++) h[i] = lm_f2h(color[i]);
    for (size_t i = 0; i < px * 4; i++) h[px * 4 + i] = lm_f2h(dir[i]);
    bool ok = lx_file_write(path, buf, bytes);
    lx_free(buf);
    return ok;
}
/* reads one atlas; out gets size*size*4 half floats per layer (2 layers). NULL if missing or broken. */
uint16_t *lx_lightmap_read(const char *path, int *size) {
    size_t n = 0;
    char *data = lx_file_read(path, &n);
    if (!data) return NULL;
    uint32_t hdr[4] = {0};
    if (n >= 16) memcpy(hdr, data, 16);
    size_t px = (size_t)hdr[2] * hdr[2];
    if (hdr[0] != LM_FILE_MAGIC || hdr[1] != 1 || hdr[3] != 2 || !hdr[2] || hdr[2] > 8192 || n < 16 + px * 16) { lx_free(data); return NULL; }
    uint16_t *out = lx_alloc(px * 16);
    memcpy(out, data + 16, px * 16);
    lx_free(data);
    *size = (int)hdr[2];
    return out;
}
float lx_lightmap_half(uint16_t h) { return lm_h2f(h); }

#define VOL_FILE_MAGIC 0x564c584cu   /* "LXLV" */
bool lx_volume_write(const char *path, const int n[3], const float *vox) {
    size_t count = (size_t)n[0] * n[1] * n[2], bytes = 20 + count * 4 * 4 * 2;
    uint8_t *buf = lx_alloc(bytes);
    uint32_t hdr[5] = {VOL_FILE_MAGIC, 1, (uint32_t)n[0], (uint32_t)n[1], (uint32_t)n[2]};
    memcpy(buf, hdr, 20);
    uint16_t *h = (uint16_t *)(buf + 20);
    for (int layer = 0; layer < 4; layer++)
        for (size_t v = 0; v < count; v++)
            for (int k = 0; k < 4; k++) h[((size_t)layer * count + v) * 4 + (size_t)k] = lm_f2h(vox[v * 16 + (size_t)layer * 4 + (size_t)k]);
    bool ok = lx_file_write(path, buf, bytes);
    lx_free(buf);
    return ok;
}
uint16_t *lx_volume_read(const char *path, int n[3]) {
    size_t len = 0;
    char *data = lx_file_read(path, &len);
    if (!data) return NULL;
    uint32_t hdr[5] = {0};
    if (len >= 20) memcpy(hdr, data, 20);
    size_t count = (size_t)hdr[2] * hdr[3] * hdr[4];
    if (hdr[0] != VOL_FILE_MAGIC || hdr[1] != 1 || !count || hdr[2] > LX_VOLUME_MAX_RES || hdr[3] > LX_VOLUME_MAX_RES || hdr[4] > LX_VOLUME_MAX_RES ||
        len < 20 + count * 32) {
        lx_free(data);
        return NULL;
    }
    uint16_t *out = lx_alloc(count * 32);
    memcpy(out, data + 20, count * 32);
    lx_free(data);
    n[0] = (int)hdr[2]; n[1] = (int)hdr[3]; n[2] = (int)hdr[4];
    return out;
}

/* ------------------------------------------------------------------ applying a bake to the scene */
static uint32_t g_epoch = 1;   /* bumped whenever lightmap files are written or a scene's are cleared */
uint32_t lx_lightmap_epoch(void) { return g_epoch; }

bool lx_lightmap_apply(LxLightmapBake *b, LxScene *s, const char *dir_ref) {
    char dir[LX_PATH_MAX];
    lx_asset_path(dir, sizeof dir, dir_ref);
    lx_mkdir_p(dir);
    int A = b->atlas_size;
    size_t px = (size_t)A * A;
    for (int a = 0; a < b->atlas_count; a++) {
        char path[LX_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/lm%d.lxlm", dir, a);
        if (!lx_lightmap_write(path, A, b->color + px * 4 * (size_t)a, b->dir + px * 4 * (size_t)a)) {
            LX_ERROR("Couldn't write the lightmap %s", path);
            return false;
        }
    }
    for (int v = 0; v < b->vol_count && v < LX_MAX_BAKE_VOLUMES; v++) {
        char path[LX_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/vol%d.lxlv", dir, v);
        if (!lx_volume_write(path, b->vols[v].n, b->vox_out + (size_t)b->vols[v].first * 16)) {
            LX_ERROR("Couldn't write the lighting volume %s", path);
            return false;
        }
    }
    for (int i = 1; i < s->count; i++) {
        s->entities[i].lm_index = -1;
        s->entities[i].vol_index = -1;
        if (s->entities[i].comps & LX_COMP_LIGHT) s->entities[i].light.mask_channel = -1;
    }
    for (int v = 0; v < b->vol_count && v < LX_MAX_BAKE_VOLUMES; v++) s->entities[b->vols[v].entity].vol_index = (int16_t)v;
    for (int k = 0; k < b->light_count; k++) s->entities[b->lights[k].entity].light.mask_channel = (int8_t)b->lights[k].mask_channel;
    for (int k = 0; k < b->tile_count; k++) {
        const LxLmTile *t = &b->tiles[k];
        LxEntityData *d = &s->entities[t->entity];
        d->lm_index = (int16_t)t->atlas;
        d->lm_st[0] = (float)t->size / (float)A;
        d->lm_st[1] = (float)t->x / (float)A;
        d->lm_st[2] = (float)t->y / (float)A;
    }
    lx_strcpy(s->lightmap_ref, sizeof s->lightmap_ref, dir_ref);
    s->lightmap_count = b->atlas_count;
    s->volume_count = b->vol_count < LX_MAX_BAKE_VOLUMES ? b->vol_count : LX_MAX_BAKE_VOLUMES;
    s->lightmap_version++;
    g_epoch++;
    return true;
}

void lx_lightmap_clear(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        s->entities[i].lm_index = -1;
        s->entities[i].vol_index = -1;
        if (s->entities[i].comps & LX_COMP_LIGHT) s->entities[i].light.mask_channel = -1;
    }
    s->lightmap_ref[0] = 0;
    s->lightmap_count = 0;
    s->volume_count = 0;
    s->lightmap_version++;
    g_epoch++;
}
int lx_scene_lightmap_count(const LxScene *s) { return s->lightmap_count; }
LxBakeSettings *lx_scene_bake_settings(LxScene *s) { return &s->bake; }

/* ------------------------------------------------------------------ the running bake (one at a time) */
static struct {
    LxLightmapBake *b;
    LxScene *scene;
    bool gpu;
    int sample, next_texel;   /* progress: passes done, and where this pass is */
    double started;
    char status[160];
} BK;

bool lx_lightmap_bake_begin(LxScene *s) {
    lx_lightmap_bake_cancel();
    LxLightmapBake *b = lx_lightmap_prepare(s);
    if (!b) {
        snprintf(BK.status, sizeof BK.status, "Nothing to bake: no Static object with a mesh and no Baked Lighting Volume");
        return false;
    }
    BK.b = b;
    BK.scene = s;
    BK.sample = 0;
    BK.next_texel = 0;
    BK.started = lx_time();
    BK.gpu = lx_renderer_lightmap_begin(b);
    snprintf(BK.status, sizeof BK.status, "Baking %d object%s into %d lightmap%s (%d x %d) and %d volume%s on the %s", b->tile_count,
             b->tile_count == 1 ? "" : "s", b->atlas_count, b->atlas_count == 1 ? "" : "s", b->atlas_size, b->atlas_size, b->vol_count,
             b->vol_count == 1 ? "" : "s", BK.gpu ? "GPU" : "CPU");
    LX_INFO("%s", BK.status);
    return true;
}

bool lx_lightmap_baking(void) { return BK.b != NULL; }
float lx_lightmap_bake_progress(void) {
    if (!BK.b) return -1;
    int samples = BK.b->set.samples < 1 ? 1 : BK.b->set.samples;
    int work = BK.b->texel_count + BK.b->voxel_count;
    return lx_clampf(((float)BK.sample + (float)BK.next_texel / (float)(work ? work : 1)) / (float)samples, 0.0f, 1.0f);
}
const char *lx_lightmap_bake_status(void) { return BK.status; }

void lx_lightmap_bake_cancel(void) {
    if (!BK.b) return;
    if (BK.gpu) lx_renderer_lightmap_end(BK.b, false);
    lx_lightmap_bake_free(BK.b);
    BK.b = NULL;
    BK.scene = NULL;
}

/* does about `budget_ms` of tracing; when the last pass is done, finishes the bake. Returns 1 while running,
 * 0 when it finished in this call (or nothing runs), -1 if it failed. */
int lx_lightmap_bake_update(LxScene *s, double budget_ms) {
    LxLightmapBake *b = BK.b;
    if (!b) return 0;
    if (s != BK.scene) { lx_lightmap_bake_cancel(); snprintf(BK.status, sizeof BK.status, "Bake cancelled: the scene changed"); return -1; }
    int samples = b->set.samples < 1 ? 1 : b->set.samples;
    double t0 = lx_time();
    while (BK.sample < samples && (lx_time() - t0) * 1000.0 < budget_ms) {
        /* a slice of texels: whole passes when they're small, else pieces */
        int chunk = BK.gpu ? 1 << 16 : 1 << 15;
        int work = b->texel_count + b->voxel_count;   /* texels, then volume voxels */
        int count = work - BK.next_texel;
        if (count > chunk) count = chunk;
        if (BK.next_texel < b->texel_count && BK.next_texel + count > b->texel_count) count = b->texel_count - BK.next_texel;   /* one kind per slice */
        if (BK.gpu) lx_renderer_lightmap_trace(b, BK.next_texel, count, (uint32_t)BK.sample);
        else lx_lightmap_trace_cpu(b, BK.next_texel, count, (uint32_t)BK.sample);
        BK.next_texel += count;
        if (BK.next_texel >= work) { BK.next_texel = 0; BK.sample++; }
    }
    if (BK.sample < samples) return 1;
    if (BK.gpu && !lx_renderer_lightmap_end(b, true)) {
        lx_lightmap_bake_cancel();
        snprintf(BK.status, sizeof BK.status, "Bake failed: couldn't read the result back from the GPU");
        return -1;
    }
    BK.gpu = false;
    lx_lightmap_resolve(b);
    char ref[LX_REF_MAX];
    snprintf(ref, sizeof ref, "Lightmaps/%s", s->name[0] ? s->name : "Scene");
    bool ok = lx_lightmap_apply(b, s, ref);
    double secs = lx_time() - BK.started;
    if (ok && b->vol_count)
        snprintf(BK.status, sizeof BK.status, "Baked %d object%s and %d volume%s in %.1f s", b->tile_count, b->tile_count == 1 ? "" : "s", b->vol_count,
                 b->vol_count == 1 ? "" : "s", secs);
    else if (ok) snprintf(BK.status, sizeof BK.status, "Baked %d object%s in %.1f s", b->tile_count, b->tile_count == 1 ? "" : "s", secs);
    else snprintf(BK.status, sizeof BK.status, "Bake failed: couldn't save the lightmaps");
    LX_INFO("%s", BK.status);
    lx_lightmap_bake_free(b);
    BK.b = NULL;
    BK.scene = NULL;
    return ok ? 0 : -1;
}

/* a whole bake at once (tests, command line) */
bool lx_lightmap_bake_now(LxScene *s) {
    if (!lx_lightmap_bake_begin(s)) return false;
    int r;
    while ((r = lx_lightmap_bake_update(s, 1e9)) == 1) {}
    return r == 0;
}
