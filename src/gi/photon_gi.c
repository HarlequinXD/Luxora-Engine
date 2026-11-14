/* Virtual photon global illumination.
 *
 * Every frame a budget of photons is emitted from the scene's lights (directional, point, spot)
 * and from the sky. Photons are traced through the static-geometry BVH; at each surface they
 * deposit their flux into an irradiance volume stored as L1 spherical harmonics, then bounce
 * diffusely with Russian roulette weighted by surface albedo. Light photons only deposit after
 * their first bounce (direct light is shaded analytically with shadows); sky photons deposit on
 * every hit, which gives sky occlusion for free. Frames are blended temporally so the solution
 * converges progressively and adapts when lights move. */
#include "../lx_internal.h"

#define SH_FLOATS 12
#define SH_C0 0.282095f
#define SH_C1 0.488603f

typedef struct Deposit { LxVec3 pos, dir, flux; } Deposit;

typedef struct Emitter {
    int kind;            /* 0 directional, 1 point, 2 spot, 3 sky */
    LxVec3 pos, dir, color;
    float power;
    float cos_cone;
} Emitter;

static struct {
    LxBvh bvh;
    uint32_t built_version;
    LxScene *built_scene;
    float *accum;        /* temporally accumulated SH */
    float *filtered;     /* accum after a light spatial denoise (what the renderer samples) */
    float *scratch;
    uint64_t light_hash, prev_light_hash; /* changes when any light or the sky changes */
    float *frame;        /* this frame's estimate */
    int res[3], cells;
    LxVec3 origin, cell_size;
    uint32_t version;
    float frame_count;
    double settled_photons;   /* photons traced since the lighting last changed */
    bool gpu;                 /* the renderer traces (lx_gi_gpu_frame); grid arrays live on the GPU */
    float *vis;               /* per cell: distance to the first surface along +x -x +y -y +z -z, in cells (max VIS_MAX) */
    uint32_t vis_version;
    uint32_t scene_version;   /* bumps when the BVH is rebuilt */
    Emitter emitters[64];
    int emitter_count;
    float total_power;
    Deposit **thread_deps;
    int *thread_dep_count, *thread_dep_cap;
    int threads;
    int photons_last;
    float ms_last;
    int max_bounces;
    uint64_t seed;
    LxAabb scene_bounds;
    LxGiVolume volume;
    int grid_setting;
} G;

void lx_gi_init(void) { memset(&G, 0, sizeof G); }

/* ---- walls between cells: what stops light leaking through them ----
 * Each cell looks a short way along the six axes for the first surface. A cell then only lends its light
 * to points it can see (shading, deposits) and only blurs with neighbours it can see, so a wall thinner than a
 * cell no longer passes light from one room into the next, and cells buried inside solid geometry drop out. */
#define VIS_MAX 1.5f
static void vis_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(user);
    LX_UNUSED(thread);
    static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    float cell = G.cell_size.x;
    for (int i = begin; i < end; i++) {
        int x = i % G.res[0], y = (i / G.res[0]) % G.res[1], z = i / (G.res[0] * G.res[1]);
        LxVec3 o = lx_v3(G.origin.x + (x + 0.5f) * cell, G.origin.y + (y + 0.5f) * cell, G.origin.z + (z + 0.5f) * cell);
        for (int k = 0; k < 6; k++) {
            LxHit hit;
            LxRay r = {o, lx_v3(dirs[k][0], dirs[k][1], dirs[k][2])};
            G.vis[i * 6 + k] = lx_bvh_intersect(&G.bvh, r, VIS_MAX * cell, &hit) ? hit.t / cell : VIS_MAX;
        }
    }
}
static bool g_walls = true;
void lx_gi_set_walls(bool on) { g_walls = on; G.built_scene = NULL; }   /* tests: compare with the leaky lookup */
static void compute_vis(void) {
    lx_free(G.vis);
    G.vis = NULL;
    G.vis_version++;
    if (!g_walls) return;
    G.vis = lx_alloc((size_t)G.cells * 6 * sizeof(float));
    lx_parallel_for(G.cells, 256, vis_job, NULL);
}
/* can cell c (grid coords) see point g (continuous grid coords, cell centres at +0.5)? per-axis test against its walls */
static bool vis_sees(int cx, int cy, int cz, LxVec3 g) {
    const float *v = G.vis + ((size_t)(cz * G.res[1] + cy) * G.res[0] + cx) * 6;
    float d[3] = {g.x - (cx + 0.5f), g.y - (cy + 0.5f), g.z - (cz + 0.5f)};
    for (int a = 0; a < 3; a++) {
        float wall = d[a] >= 0 ? v[a * 2] : v[a * 2 + 1];
        if (wall < fabsf(d[a]) - 0.02f) return false;
    }
    return true;
}
/* the trilinear corner weights of a point, with the corners that can't see it removed (renormalized). Returns
 * false when no corner sees it (the caller falls back to plain trilinear). */
static bool vis_weights(LxVec3 g, int base[3], float w[8]) {
    LxVec3 t = lx_v3(g.x - 0.5f, g.y - 0.5f, g.z - 0.5f);
    base[0] = (int)floorf(t.x); base[1] = (int)floorf(t.y); base[2] = (int)floorf(t.z);
    float f[3] = {t.x - base[0], t.y - base[1], t.z - base[2]};
    float sum = 0, plain[8];
    for (int k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
        int x = base[0] + dx, y = base[1] + dy, z = base[2] + dz;
        float wt = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
        bool in = x >= 0 && y >= 0 && z >= 0 && x < G.res[0] && y < G.res[1] && z < G.res[2];
        plain[k] = in ? wt : 0;
        w[k] = (in && (!G.vis || vis_sees(x, y, z, g))) ? wt : 0;
        sum += w[k];
    }
    if (sum < 1e-6f) { memcpy(w, plain, sizeof plain); return false; }
    for (int k = 0; k < 8; k++) w[k] /= sum;
    return true;
}
/* blur neighbours: no wall between the two centres */
static bool vis_open(int i, int axis, int dir) {
    if (!G.vis) return true;
    return G.vis[(size_t)i * 6 + axis * 2 + (dir > 0 ? 0 : 1)] >= 0.98f;
}

void lx_gi_shutdown(void) {
    lx_bvh_free(&G.bvh);
    lx_free(G.vis);
    lx_free(G.accum);
    lx_free(G.frame);
    lx_free(G.filtered);
    lx_free(G.scratch);
    for (int i = 0; i < G.threads; i++) lx_free(G.thread_deps[i]);
    lx_free(G.thread_deps);
    lx_free(G.thread_dep_count);
    lx_free(G.thread_dep_cap);
    memset(&G, 0, sizeof G);
}

void lx_gi_reset(void) {
    if (G.accum) memset(G.accum, 0, sizeof(float) * (size_t)G.cells * SH_FLOATS);
    G.frame_count = 0;
    G.settled_photons = 0;
    G.version++;
}

const LxGiVolume *lx_gi_volume(void) { return &G.volume; }
int lx_gi_last_photon_count(void) { return G.photons_last; }
float lx_gi_last_ms(void) { return G.ms_last; }
const float *lx_gi_cell(int x, int y, int z) {
    if (!G.accum || x < 0 || y < 0 || z < 0 || x >= G.res[0] || y >= G.res[1] || z >= G.res[2]) return NULL;
    return G.accum + ((size_t)((z * G.res[1] + y) * G.res[0] + x)) * SH_FLOATS;
}

static void setup_grid(int longest_res, bool cpu) {
    LxAabb b = G.bvh.bounds;
    LxVec3 ext = lx_sub3(b.max, b.min);
    float pad = lx_maxf(0.5f, lx_maxf(ext.x, lx_maxf(ext.y, ext.z)) * 0.05f);
    b.min = lx_sub3(b.min, lx_v3s(pad));
    b.max = lx_add3(b.max, lx_v3s(pad));
    ext = lx_sub3(b.max, b.min);
    float longest = lx_maxf(ext.x, lx_maxf(ext.y, ext.z));
    if (longest_res < 4) longest_res = 4;
    if (longest_res > 96) longest_res = 96;
    float cell = longest / (float)longest_res;
    for (int a = 0; a < 3; a++) {
        int r = (int)ceilf(lx_comp3(ext, a) / cell);
        G.res[a] = r < 2 ? 2 : r;
    }
    G.cells = G.res[0] * G.res[1] * G.res[2];
    G.origin = b.min;
    G.cell_size = lx_v3s(cell);
    lx_free(G.accum);
    lx_free(G.frame);
    lx_free(G.filtered);
    lx_free(G.scratch);
    G.accum = G.frame = G.filtered = G.scratch = NULL;
    if (cpu) {   /* the GPU keeps its own copies */
        G.accum = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
        G.frame = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
        G.filtered = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
        G.scratch = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
    }
    G.frame_count = 0;
    G.settled_photons = 0;
    G.scene_bounds = b;
}

static float luminance(LxVec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

static uint64_t hash_floats(uint64_t h, const float *f, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t b;
        memcpy(&b, &f[i], 4);
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

static void gather_emitters(LxScene *s) {
    G.emitter_count = 0;
    G.total_power = 0;
    LxAabb b = G.scene_bounds;
    LxVec3 ext = lx_sub3(b.max, b.min);
    for (int i = 1; i < s->count && G.emitter_count < 63; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_LIGHT) || !d->light.contribute_gi) continue;
        Emitter e;
        memset(&e, 0, sizeof e);
        e.kind = (int)d->light.type;
        e.color = lx_scale3(d->light.color, d->light.intensity);
        e.pos = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
        e.dir = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, -1)));
        if (e.kind == LX_LIGHT_DIRECTIONAL) {
            /* power = irradiance x area of the scene seen from the light */
            float proj = fabsf(e.dir.y) * ext.x * ext.z + fabsf(e.dir.x) * ext.y * ext.z + fabsf(e.dir.z) * ext.x * ext.y;
            e.power = luminance(e.color) * proj;
        } else if (e.kind == LX_LIGHT_POINT) {
            e.power = luminance(e.color) * 4.0f * LX_PI;
        } else {
            e.cos_cone = cosf(d->light.spot_angle_deg * 0.5f * LX_DEG2RAD);
            e.power = luminance(e.color) * 2.0f * LX_PI * (1.0f - e.cos_cone);
        }
        if (e.power <= 0) continue;
        G.emitters[G.emitter_count++] = e;
        G.total_power += e.power;
    }
    /* lighting signature: any change restarts the fast-adapt phase of accumulation */
    uint64_t h = 1469598103934665603ull ^ (uint64_t)G.emitter_count;
    for (int i = 0; i < G.emitter_count; i++) {
        const Emitter *e = &G.emitters[i];
        float f[11] = {(float)e->kind, e->pos.x, e->pos.y, e->pos.z, e->dir.x, e->dir.y, e->dir.z, e->color.x, e->color.y, e->color.z, e->cos_cone};
        h = hash_floats(h, f, 11);
    }
    float env[7] = {s->env.sky_zenith.x, s->env.sky_zenith.y, s->env.sky_zenith.z, s->env.sky_horizon.x, s->env.sky_horizon.y,
                    s->env.sky_horizon.z, s->env.ambient_intensity};
    G.light_hash = hash_floats(h, env, 7);

    /* sky: a horizontal emitting plane above the scene radiating downward, L = sky radiance */
    LxVec3 sky = lx_scale3(lx_lerp3(s->env.sky_horizon, s->env.sky_zenith, 0.5f), s->env.ambient_intensity);
    if (luminance(sky) > 0) {
        Emitter e;
        memset(&e, 0, sizeof e);
        e.kind = 3;
        e.color = sky;
        e.power = luminance(sky) * LX_PI * ext.x * ext.z;
        G.emitters[G.emitter_count++] = e;
        G.total_power += e.power;
    }
}

static void emit(const Emitter *e, LxRng *rng, LxVec3 *o, LxVec3 *d) {
    LxAabb b = G.scene_bounds;
    LxVec3 ext = lx_sub3(b.max, b.min);
    switch (e->kind) {
    case LX_LIGHT_DIRECTIONAL: {
        /* pick a random point in the bounds and step back against the light direction */
        LxVec3 p = lx_v3(b.min.x + lx_rng_f(rng) * ext.x, b.min.y + lx_rng_f(rng) * ext.y, b.min.z + lx_rng_f(rng) * ext.z);
        float back = lx_len3(ext) * 1.5f;
        *o = lx_madd3(p, e->dir, -back);
        *d = e->dir;
        break;
    }
    case LX_LIGHT_POINT:
        *o = e->pos;
        *d = lx_rng_sphere(rng);
        break;
    case LX_LIGHT_SPOT: {
        float z = 1.0f - lx_rng_f(rng) * (1.0f - e->cos_cone);
        float a = lx_rng_f(rng) * 2.0f * LX_PI, r = sqrtf(lx_maxf(0, 1 - z * z));
        LxVec3 t = fabsf(e->dir.y) < 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0);
        LxVec3 bx = lx_norm3(lx_cross3(t, e->dir)), by = lx_cross3(e->dir, bx);
        *o = e->pos;
        *d = lx_add3(lx_add3(lx_scale3(bx, r * cosf(a)), lx_scale3(by, r * sinf(a))), lx_scale3(e->dir, z));
        break;
    }
    default: /* sky */
        *o = lx_v3(b.min.x + lx_rng_f(rng) * ext.x, b.max.y, b.min.z + lx_rng_f(rng) * ext.z);
        *d = lx_rng_cosine(rng, lx_v3(0, -1, 0));
        break;
    }
}

typedef struct TraceJob { int photons_per_chunk; float flux_scale; } TraceJob;

static void push_dep(int thread, LxVec3 pos, LxVec3 dir, LxVec3 flux) {
    Deposit dp = {pos, dir, flux};
    LX_VEC_PUSH(G.thread_deps[thread], G.thread_dep_count[thread], G.thread_dep_cap[thread], dp);
}

static void trace_job(void *user, int begin, int end, int thread) {
    TraceJob *job = user;
    for (int chunk = begin; chunk < end; chunk++) {
        LxRng rng = lx_rng_seed(G.seed, (uint64_t)chunk * 7919u + 1u);
        for (int p = 0; p < job->photons_per_chunk; p++) {
            /* choose emitter proportional to power */
            float pick = lx_rng_f(&rng) * G.total_power;
            const Emitter *e = &G.emitters[G.emitter_count - 1];
            for (int k = 0; k < G.emitter_count; k++) {
                if (pick < G.emitters[k].power) { e = &G.emitters[k]; break; }
                pick -= G.emitters[k].power;
            }
            LxVec3 o, d;
            emit(e, &rng, &o, &d);
            /* every photon carries the same luminous power; colour preserves the light's chroma */
            float lum = luminance(e->color);
            LxVec3 flux = lx_scale3(e->color, job->flux_scale / (lum > 1e-6f ? lum : 1.0f));
            bool is_sky = e->kind == 3;
            for (int bounce = 0; bounce <= G.max_bounces; bounce++) {
                LxHit hit;
                if (!lx_bvh_intersect(&G.bvh, (LxRay){o, d}, 1e30f, &hit)) break;
                LxVec3 pos = lx_madd3(o, d, hit.t);
                LxVec3 n = G.bvh.normals[hit.tri];
                if (lx_dot3(n, d) > 0) n = lx_neg3(n);
                if (bounce > 0 || is_sky) push_dep(thread, lx_madd3(pos, n, G.cell_size.x * 0.5f), d, flux);
                const LxTriMaterial *m = &G.bvh.materials[G.bvh.mat_index[hit.tri]];
                float survive = lx_clampf(lx_maxf(m->albedo.x, lx_maxf(m->albedo.y, m->albedo.z)), 0.0f, 0.95f);
                if (lx_rng_f(&rng) >= survive) break;
                flux = lx_scale3(lx_mul3(flux, m->albedo), 1.0f / survive);
                o = lx_madd3(pos, n, 1e-3f);
                d = lx_rng_cosine(&rng, n);
            }
        }
    }
}

typedef struct SplatJob { int slabs; } SplatJob;
static void splat_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(user);
    LX_UNUSED(thread);
    /* each job owns z-slices [begin,end) so no two threads write the same cell */
    float inv_area = 1.0f / (G.cell_size.x * G.cell_size.x);
    for (int t = 0; t < G.threads; t++) {
        const Deposit *deps = G.thread_deps[t];
        int n = G.thread_dep_count[t];
        for (int i = 0; i < n; i++) {
            LxVec3 g = lx_v3((deps[i].pos.x - G.origin.x) / G.cell_size.x, (deps[i].pos.y - G.origin.y) / G.cell_size.y,
                             (deps[i].pos.z - G.origin.z) / G.cell_size.z);
            int z0 = (int)floorf(g.z - 0.5f);
            if (z0 + 1 < begin || z0 >= end) continue;
            int base[3];
            float w[8];
            vis_weights(g, base, w);   /* only the cells on the photon's side of any wall */
            /* incoming radiance arrives from -dir */
            LxVec3 wd = lx_neg3(deps[i].dir);
            float sh[4] = {SH_C0, SH_C1 * wd.y, SH_C1 * wd.z, SH_C1 * wd.x};
            LxVec3 f = lx_scale3(deps[i].flux, inv_area);
            for (int k = 0; k < 8; k++) {
                int x = base[0] + (k & 1), y = base[1] + ((k >> 1) & 1), z = base[2] + (k >> 2);
                if (w[k] <= 0 || z < begin || z >= end || x < 0 || y < 0 || z < 0 || x >= G.res[0] || y >= G.res[1] || z >= G.res[2]) continue;
                float *c = G.frame + ((size_t)((z * G.res[1] + y) * G.res[0] + x)) * SH_FLOATS;
                for (int q = 0; q < 4; q++) {
                    c[q] += f.x * sh[q] * w[k];
                    c[4 + q] += f.y * sh[q] * w[k];
                    c[8 + q] += f.z * sh[q] * w[k];
                }
            }
        }
    }
}

/* separable [1,2,1] blur along one axis: softens the cell-sized blotches left by Monte Carlo noise */
typedef struct BlurJob { const float *src; float *dst; int axis; } BlurJob;
static void blur_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    BlurJob *b = user;
    int rx = G.res[0], ry = G.res[1], rz = G.res[2];
    int stride = b->axis == 0 ? 1 : b->axis == 1 ? rx : rx * ry;
    int len = b->axis == 0 ? rx : b->axis == 1 ? ry : rz;
    for (int i = begin; i < end; i++) {
        int x = i % rx, y = (i / rx) % ry, z = i / (rx * ry);
        int p = b->axis == 0 ? x : b->axis == 1 ? y : z;
        const float *c = b->src + (size_t)i * SH_FLOATS;
        /* a neighbour behind a wall counts as this cell again (as at the grid's edge) */
        const float *lo = p > 0 && vis_open(i, b->axis, -1) && vis_open(i - stride, b->axis, 1) ? c - (size_t)stride * SH_FLOATS : c;
        const float *hi = p < len - 1 && vis_open(i, b->axis, 1) && vis_open(i + stride, b->axis, -1) ? c + (size_t)stride * SH_FLOATS : c;
        float *o = b->dst + (size_t)i * SH_FLOATS;
        for (int k = 0; k < SH_FLOATS; k++) o[k] = 0.25f * lo[k] + 0.5f * c[k] + 0.25f * hi[k];
    }
}

typedef struct BlendJob { float alpha; } BlendJob;
static void blend_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    float a = ((BlendJob *)user)->alpha;
    for (int i = begin * SH_FLOATS; i < end * SH_FLOATS; i++) G.accum[i] += (G.frame[i] - G.accum[i]) * a;
}

/* Everything both tracers share: the BVH and grid, the emitters, the lighting signature and convergence.
 * Returns 0 = no GI, 1 = converged (keep showing the last solution), 2 = trace this frame. */
static int prepare(LxScene *s, const LxGraphicsSettings *g, bool gpu) {
    G.volume.valid = false;
    if (!g->gi_enabled) return 0;
    if (G.gpu != gpu) { G.gpu = gpu; G.built_scene = NULL; }   /* switching tracer: rebuild the grid for it */
    if (G.built_scene != s || G.built_version != s->static_version) {
        lx_bvh_build_from_scene(&G.bvh, s);
        G.built_scene = s;
        G.built_version = s->static_version;
        if (G.bvh.tri_count) { setup_grid(g->gi_grid_resolution, !gpu); compute_vis(); }
        G.grid_setting = g->gi_grid_resolution;
        G.version++;
        G.scene_version++;
    }
    if (!G.bvh.tri_count) return 0;
    if (g->gi_grid_resolution != G.grid_setting) {
        G.grid_setting = g->gi_grid_resolution;
        setup_grid(g->gi_grid_resolution, !gpu);
        compute_vis();
        G.version++;
    }
    lx_scene_update_transforms(s);
    gather_emitters(s);
    {   /* graphics settings changes (bounces, intensity...) count as a lighting change too */
        const uint8_t *gb = (const uint8_t *)g;
        uint64_t h = G.light_hash;
        for (size_t i = 0; i < sizeof *g; i++) h = (h ^ gb[i]) * 1099511628211ull;
        G.light_hash = h;
    }
    G.volume.origin = G.origin;
    G.volume.cell_size = G.cell_size;
    G.volume.res[0] = G.res[0];
    G.volume.res[1] = G.res[1];
    G.volume.res[2] = G.res[2];
    G.volume.vis = G.vis;
    G.volume.vis_version = G.vis_version;
    /* Converged: nothing has changed since enough photons were traced, so tracing more only burns every
     * core for no visible difference. Stop until a light, the sky or the geometry changes. */
    static const double SETTLED = 24e6;
    if (G.light_hash == G.prev_light_hash && G.frame_count >= 8 && (G.settled_photons >= SETTLED || G.frame_count >= 1024.0f) &&
        (gpu || G.filtered)) {
        G.photons_last = 0;
        G.volume.sh = G.filtered;
        G.volume.version = G.version;
        G.volume.valid = true;
        return 1;
    }
    if (!G.emitter_count) return 0;
    G.max_bounces = g->gi_max_bounces < 1 ? 1 : g->gi_max_bounces;
    G.seed = G.seed * 6364136223846793005ull + 1442695040888963407ull;
    return 2;
}

/* Progressive accumulation: while the lighting is unchanged every frame gets equal weight
 * (a true running average), so noise keeps falling as 1/sqrt(frames) up to a long history.
 * When a light, the sky or the geometry changes, the history is shortened to 1/responsiveness
 * frames so the solution follows quickly, then it lengthens again and the noise settles. */
static float next_blend(const LxGraphicsSettings *g) {
    static const float MAX_HISTORY = 1024.0f;
    if (G.light_hash != G.prev_light_hash) {
        float fast = 1.0f / lx_clampf(g->gi_responsiveness, 0.01f, 1.0f);
        if (G.frame_count > fast) G.frame_count = fast;
        G.prev_light_hash = G.light_hash;
        G.settled_photons = 0;
    }
    G.frame_count = lx_minf(G.frame_count + 1.0f, MAX_HISTORY);
    return 1.0f / G.frame_count;
}

void lx_gi_update(LxScene *s, const LxGraphicsSettings *g) {
    double t0 = lx_time();
    int st = prepare(s, g, false);
    if (st != 2) { G.ms_last = (float)((lx_time() - t0) * 1000.0); return; }
    if (!G.threads) {
        G.threads = lx_jobs_thread_count();
        G.thread_deps = lx_calloc((size_t)G.threads, sizeof(Deposit *));
        G.thread_dep_count = lx_calloc((size_t)G.threads, sizeof(int));
        G.thread_dep_cap = lx_calloc((size_t)G.threads, sizeof(int));
    }
    for (int t = 0; t < G.threads; t++) G.thread_dep_count[t] = 0;
    int photons = g->gi_photons_per_frame < 256 ? 256 : g->gi_photons_per_frame;
    TraceJob tj;
    tj.photons_per_chunk = 256;
    int chunks = (photons + tj.photons_per_chunk - 1) / tj.photons_per_chunk;
    photons = chunks * tj.photons_per_chunk;
    tj.flux_scale = G.total_power / (float)photons;
    lx_parallel_for(chunks, 1, trace_job, &tj);
    memset(G.frame, 0, sizeof(float) * (size_t)G.cells * SH_FLOATS);
    lx_parallel_for(G.res[2], 1, splat_job, NULL);
    BlendJob bj = {next_blend(g)};
    lx_parallel_for(G.cells, 4096, blend_job, &bj);
    BlurJob bx = {G.accum, G.scratch, 0}, by = {G.scratch, G.filtered, 1}, bz = {G.filtered, G.scratch, 2};
    lx_parallel_for(G.cells, 4096, blur_job, &bx);
    lx_parallel_for(G.cells, 4096, blur_job, &by);
    lx_parallel_for(G.cells, 4096, blur_job, &bz);
    memcpy(G.filtered, G.scratch, sizeof(float) * (size_t)G.cells * SH_FLOATS);
    G.version++;
    G.photons_last = photons;
    G.settled_photons += photons;
    G.ms_last = (float)((lx_time() - t0) * 1000.0);
    G.volume.origin = G.origin;
    G.volume.cell_size = G.cell_size;
    G.volume.res[0] = G.res[0];
    G.volume.res[1] = G.res[1];
    G.volume.res[2] = G.res[2];
    G.volume.sh = G.filtered;
    G.volume.version = G.version;
    G.volume.valid = true;
}

/* ---------------- the GPU tracer's side: the renderer runs the photons in compute shaders ---------------- */
/* Fill one GPU dispatch: emitters (sun and sky powers follow the area photons are aimed at), budget, scales. */
static void fill_trace(LxGiTrace *t, int photons, LxAabb aim, float step_back, float cell, LxVec3 near_c, float near_r) {
    LxVec3 ext = lx_sub3(aim.max, aim.min);
    t->trace = true;
    t->photons = photons;
    t->seed = (uint32_t)(G.seed >> 32) ^ (uint32_t)G.seed ^ (uint32_t)(cell * 7919.0f);
    t->bounds_min = aim.min;
    t->bounds_ext = ext;
    t->step_back = step_back;
    float chroma = 1.0f, cum = 0;
    int n = 0;
    for (int i = 0; i < G.emitter_count; i++) {
        const Emitter *e = &G.emitters[i];
        float power = e->power;
        float lum = luminance(e->color);
        if (e->kind == LX_LIGHT_DIRECTIONAL)
            power = lum * (fabsf(e->dir.y) * ext.x * ext.z + fabsf(e->dir.x) * ext.y * ext.z + fabsf(e->dir.z) * ext.x * ext.y);
        else if (e->kind == 3)
            power = lum * LX_PI * ext.x * ext.z;
        else if (near_r > 0 && lx_len3(lx_sub3(e->pos, near_c)) > near_r * 6.0f)
            continue;   /* a lamp far beyond a camera grid hardly lights it: don't spend its photons there */
        if (power <= 0) continue;
        LxVec3 c = lx_scale3(e->color, 1.0f / (lum > 1e-6f ? lum : 1.0f));
        chroma = lx_maxf(chroma, lx_maxf(c.x, lx_maxf(c.y, c.z)));
        cum += power;
        float *E = t->emitters[n++];
        E[0] = e->pos.x; E[1] = e->pos.y; E[2] = e->pos.z; E[3] = (float)e->kind;
        E[4] = e->dir.x; E[5] = e->dir.y; E[6] = e->dir.z; E[7] = e->cos_cone;
        E[8] = c.x; E[9] = c.y; E[10] = c.z; E[11] = cum;
    }
    t->emitter_count = n;
    t->total_power = cum;
    t->flux_scale = cum / (float)photons;
    /* Deposits are summed with integer atomics (every Vulkan GPU has them). The fixed-point scale is
     * chosen so even every deposit of the frame landing in one cell can't overflow: a deposit is at
     * most flux x chroma x 1.4 (albedo / survival over the bounces) x 0.49 (largest SH basis value). */
    float inv_area = 1.0f / (cell * cell);
    double worst = (double)photons * (double)(G.max_bounces + 1) * (double)t->flux_scale * inv_area * chroma * 1.4 * 0.49;
    t->fixed_scale = (float)(2.0e9 / lx_maxf((float)worst, 1e-20f));
    t->deposit_scale = t->fixed_scale * inv_area;
    if (n == 0) t->trace = false;
}

/* camera grids: per cascade where it is and how settled it is */
static struct {
    bool on;
    int origin[3];        /* in its own cells */
    float cell;
    int res;
    uint32_t scene_version;
    uint64_t light_hash;
    float frame_count;
    double settled;
} C[LX_GI_CASCADES];

bool lx_gi_gpu_frame(LxScene *s, const LxGraphicsSettings *g, int photon_multiplier, LxVec3 eye, LxGiGpuFrame *f) {
    memset(f, 0, sizeof *f);
    double t0 = lx_time();
    int st = prepare(s, g, true);
    if (st == 0) { G.ms_last = (float)((lx_time() - t0) * 1000.0); return false; }
    f->bvh = &G.bvh;
    f->scene_version = G.scene_version;
    f->res[0] = G.res[0]; f->res[1] = G.res[1]; f->res[2] = G.res[2];
    f->origin = G.origin;
    f->cell = G.cell_size.x;
    f->max_bounces = G.max_bounces;
    int photons = g->gi_photons_per_frame < 256 ? 256 : g->gi_photons_per_frame;
    photons *= photon_multiplier < 1 ? 1 : photon_multiplier;
    photons = (photons + 63) / 64 * 64;
    LxVec3 sext = lx_sub3(G.scene_bounds.max, G.scene_bounds.min);
    float step_back = lx_len3(sext) * 1.5f;
    if (st == 2) {
        fill_trace(&f->t, photons, G.scene_bounds, step_back, G.cell_size.x, lx_v3(0, 0, 0), 0);
        f->t.blend = next_blend(g);
        G.version++;
        G.photons_last = photons;
        G.settled_photons += photons;
    }
    /* Camera grids, finest first: the same number of cells as the scene grid's quality, half the cell size of
     * the one outside it, centred on the camera in steps of 2 cells. Only where the scene grid is coarser. */
    int cres = g->gi_grid_resolution < 16 ? 16 : g->gi_grid_resolution > 64 ? 64 : g->gi_grid_resolution;
    float cell0 = 16.0f / (float)cres;
    for (int l = 0; l < LX_GI_CASCADES; l++) {
        LxGiCascadeFrame *cf = &f->casc[l];
        float cell = cell0 * (float)(1 << l);
        bool want = cell < G.cell_size.x * 0.75f && isfinite(eye.x) && isfinite(eye.y) && isfinite(eye.z);
        if (!want) { C[l].on = false; continue; }
        int o[3];
        for (int a = 0; a < 3; a++) {
            int c = (int)floorf(lx_comp3(eye, a) / cell) - cres / 2;
            o[a] = c >> 1 << 1;   /* even cells: it moves in steps of two */
        }
        bool reset = !C[l].on || C[l].cell != cell || C[l].res != cres || C[l].scene_version != G.scene_version;
        cf->on = true;
        cf->reset = reset;
        cf->res = cres;
        cf->cell = cell;
        if (!reset && (o[0] != C[l].origin[0] || o[1] != C[l].origin[1] || o[2] != C[l].origin[2])) {
            cf->shifted = true;
            for (int a = 0; a < 3; a++) cf->shift[a] = o[a] - C[l].origin[a];
        }
        cf->origin = lx_v3(o[0] * cell, o[1] * cell, o[2] * cell);
        if (reset) { C[l].frame_count = 0; C[l].settled = 0; C[l].light_hash = 0; }
        if (cf->shifted) C[l].settled = 0;   /* new cells at the edge: keep tracing */
        C[l].on = true;
        C[l].cell = cell;
        C[l].res = cres;
        C[l].scene_version = G.scene_version;
        memcpy(C[l].origin, o, sizeof o);
        /* trace unless settled: the same rule as the scene grid, per cascade */
        bool changed = C[l].light_hash != G.light_hash;
        if (changed) {
            float fast = 1.0f / lx_clampf(g->gi_responsiveness, 0.01f, 1.0f);
            if (C[l].frame_count > fast) C[l].frame_count = fast;
            C[l].light_hash = G.light_hash;
            C[l].settled = 0;
        }
        cf->show = C[l].frame_count >= 2;
        if (C[l].frame_count >= 8 && C[l].settled >= 24e6 && !changed && !cf->shifted) continue;
        if (!G.emitter_count) continue;
        /* aim the sun and sky at this grid plus a margin of half its size (light bouncing in from around it),
         * over the scene's full height (sky photons must start above every roof) */
        float size = cell * (float)cres;
        LxVec3 lo = lx_madd3(cf->origin, lx_v3(1, 1, 1), -size * 0.5f), hi = lx_madd3(cf->origin, lx_v3(1, 1, 1), size * 1.5f);
        LxAabb aim;
        aim.min = lx_v3(lx_maxf(lo.x, G.scene_bounds.min.x), G.scene_bounds.min.y, lx_maxf(lo.z, G.scene_bounds.min.z));
        aim.max = lx_v3(lx_minf(hi.x, G.scene_bounds.max.x), G.scene_bounds.max.y, lx_minf(hi.z, G.scene_bounds.max.z));
        if (aim.max.x <= aim.min.x || aim.max.z <= aim.min.z) continue;   /* the camera is outside the scene */
        LxVec3 centre = lx_madd3(cf->origin, lx_v3(1, 1, 1), size * 0.5f);
        fill_trace(&cf->t, photons, aim, step_back, cell, centre, size);
        if (!cf->t.trace) continue;
        C[l].frame_count = lx_minf(C[l].frame_count + 1.0f, 1024.0f);
        cf->t.blend = 1.0f / C[l].frame_count;
        C[l].settled += photons;
        G.photons_last += photons;
    }
    G.volume.sh = NULL;   /* the solution lives in the GPU's textures */
    G.volume.version = G.version;
    G.volume.valid = true;
    G.ms_last = (float)((lx_time() - t0) * 1000.0);
    return true;
}

/* A CPU solution of the same scene, for checking the GPU tracer: runs the CPU photons for `frames` frames on
 * the side (the live state is kept) and returns the unblurred accumulation (cells x 12, free with lx_free). */
float *lx_gi_cpu_reference(LxScene *s, const LxGraphicsSettings *g, int frames, int res[3]) {
    static __typeof__(G) saved;
    saved = G;
    memset(&G, 0, sizeof G);
    LxGraphicsSettings gg = *g;
    gg.gi_enabled = true;
    for (int i = 0; i < frames; i++) {
        G.prev_light_hash = G.light_hash;   /* never "converged" or "changed": plain averaging */
        G.settled_photons = 0;
        lx_gi_update(s, &gg);
    }
    float *out = NULL;
    if (G.accum) {
        out = lx_alloc((size_t)G.cells * SH_FLOATS * sizeof(float));
        memcpy(out, G.accum, (size_t)G.cells * SH_FLOATS * sizeof(float));
        res[0] = G.res[0]; res[1] = G.res[1]; res[2] = G.res[2];
    }
    lx_gi_shutdown();
    G = saved;
    return out;
}

/* What the PBR shader does with the GI volume, on the CPU (tests): irradiance at a surface point with normal n.
 * walls = false gives the old plain trilinear lookup, for comparison. */
LxVec3 lx_gi_irradiance(LxVec3 p, LxVec3 n, bool walls) {
    const float *sh = G.filtered;
    if (!sh) return lx_v3(0, 0, 0);
    LxVec3 q = lx_madd3(p, n, G.cell_size.x * 0.25f);
    LxVec3 g = lx_v3((q.x - G.origin.x) / G.cell_size.x, (q.y - G.origin.y) / G.cell_size.y, (q.z - G.origin.z) / G.cell_size.z);
    int base[3];
    float w[8];
    float *keep = G.vis;
    if (!walls) G.vis = NULL;
    vis_weights(g, base, w);
    G.vis = keep;
    float c[12] = {0};
    for (int k = 0; k < 8; k++) {
        int x = base[0] + (k & 1), y = base[1] + ((k >> 1) & 1), z = base[2] + (k >> 2);
        if (w[k] <= 0) continue;
        const float *src = sh + ((size_t)((z * G.res[1] + y) * G.res[0] + x)) * SH_FLOATS;
        for (int j = 0; j < 12; j++) c[j] += src[j] * w[k];
    }
    float e[3];
    for (int ch = 0; ch < 3; ch++) {
        const float *k = c + ch * 4;
        e[ch] = lx_maxf(0, LX_PI * 0.282095f * k[0] + (2.0f * LX_PI / 3.0f) * 0.488603f * (k[1] * n.y + k[2] * n.z + k[3] * n.x));
    }
    return lx_v3(e[0], e[1], e[2]);
}
