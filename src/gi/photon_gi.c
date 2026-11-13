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

void lx_gi_shutdown(void) {
    lx_bvh_free(&G.bvh);
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
    G.version++;
}

const LxGiVolume *lx_gi_volume(void) { return &G.volume; }
int lx_gi_last_photon_count(void) { return G.photons_last; }
float lx_gi_last_ms(void) { return G.ms_last; }
const float *lx_gi_cell(int x, int y, int z) {
    if (!G.accum || x < 0 || y < 0 || z < 0 || x >= G.res[0] || y >= G.res[1] || z >= G.res[2]) return NULL;
    return G.accum + ((size_t)((z * G.res[1] + y) * G.res[0] + x)) * SH_FLOATS;
}

static void setup_grid(int longest_res) {
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
    G.accum = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
    G.frame = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
    G.filtered = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
    G.scratch = lx_calloc((size_t)G.cells * SH_FLOATS, sizeof(float));
    G.frame_count = 0;
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
            LxVec3 g = lx_v3((deps[i].pos.x - G.origin.x) / G.cell_size.x - 0.5f, (deps[i].pos.y - G.origin.y) / G.cell_size.y - 0.5f,
                             (deps[i].pos.z - G.origin.z) / G.cell_size.z - 0.5f);
            int z0 = (int)floorf(g.z);
            if (z0 + 1 < begin || z0 >= end) continue;
            int x0 = (int)floorf(g.x), y0 = (int)floorf(g.y);
            float fx = g.x - (float)x0, fy = g.y - (float)y0, fz = g.z - (float)z0;
            /* incoming radiance arrives from -dir */
            LxVec3 w = lx_neg3(deps[i].dir);
            float sh[4] = {SH_C0, SH_C1 * w.y, SH_C1 * w.z, SH_C1 * w.x};
            LxVec3 f = lx_scale3(deps[i].flux, inv_area);
            for (int dz = 0; dz < 2; dz++) {
                int z = z0 + dz;
                if (z < begin || z >= end || z < 0 || z >= G.res[2]) continue;
                float wz = dz ? fz : 1 - fz;
                for (int dy = 0; dy < 2; dy++) {
                    int y = y0 + dy;
                    if (y < 0 || y >= G.res[1]) continue;
                    float wy = dy ? fy : 1 - fy;
                    for (int dx = 0; dx < 2; dx++) {
                        int x = x0 + dx;
                        if (x < 0 || x >= G.res[0]) continue;
                        float wgt = wz * wy * (dx ? fx : 1 - fx);
                        float *c = G.frame + ((size_t)((z * G.res[1] + y) * G.res[0] + x)) * SH_FLOATS;
                        for (int k = 0; k < 4; k++) {
                            c[k] += f.x * sh[k] * wgt;
                            c[4 + k] += f.y * sh[k] * wgt;
                            c[8 + k] += f.z * sh[k] * wgt;
                        }
                    }
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
        const float *lo = p > 0 ? c - (size_t)stride * SH_FLOATS : c;
        const float *hi = p < len - 1 ? c + (size_t)stride * SH_FLOATS : c;
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

void lx_gi_update(LxScene *s, const LxGraphicsSettings *g) {
    G.volume.valid = false;
    if (!g->gi_enabled) return;
    double t0 = lx_time();
    if (G.built_scene != s || G.built_version != s->static_version) {
        lx_bvh_build_from_scene(&G.bvh, s);
        G.built_scene = s;
        G.built_version = s->static_version;
        if (G.bvh.tri_count) setup_grid(g->gi_grid_resolution);
        G.grid_setting = g->gi_grid_resolution;
        G.version++;
    }
    if (!G.bvh.tri_count) return;
    if (g->gi_grid_resolution != G.grid_setting) {
        G.grid_setting = g->gi_grid_resolution;
        setup_grid(g->gi_grid_resolution);
        G.version++;
    }
    lx_scene_update_transforms(s);
    gather_emitters(s);
    if (!G.emitter_count) return;
    if (!G.threads) {
        G.threads = lx_jobs_thread_count();
        G.thread_deps = lx_calloc((size_t)G.threads, sizeof(Deposit *));
        G.thread_dep_count = lx_calloc((size_t)G.threads, sizeof(int));
        G.thread_dep_cap = lx_calloc((size_t)G.threads, sizeof(int));
    }
    for (int t = 0; t < G.threads; t++) G.thread_dep_count[t] = 0;
    G.max_bounces = g->gi_max_bounces < 1 ? 1 : g->gi_max_bounces;
    G.seed = G.seed * 6364136223846793005ull + 1442695040888963407ull;
    int photons = g->gi_photons_per_frame < 256 ? 256 : g->gi_photons_per_frame;
    TraceJob tj;
    tj.photons_per_chunk = 256;
    int chunks = (photons + tj.photons_per_chunk - 1) / tj.photons_per_chunk;
    photons = chunks * tj.photons_per_chunk;
    tj.flux_scale = G.total_power / (float)photons;
    lx_parallel_for(chunks, 1, trace_job, &tj);
    memset(G.frame, 0, sizeof(float) * (size_t)G.cells * SH_FLOATS);
    lx_parallel_for(G.res[2], 1, splat_job, NULL);
    /* Progressive accumulation: while the lighting is unchanged every frame gets equal weight
     * (a true running average), so noise keeps falling as 1/sqrt(frames) up to a long history.
     * When a light, the sky or the geometry changes, the history is shortened to 1/responsiveness
     * frames so the solution follows quickly, then it lengthens again and the noise settles. */
    static const float MAX_HISTORY = 1024.0f;
    if (G.light_hash != G.prev_light_hash) {
        float fast = 1.0f / lx_clampf(g->gi_responsiveness, 0.01f, 1.0f);
        if (G.frame_count > fast) G.frame_count = fast;
        G.prev_light_hash = G.light_hash;
    }
    G.frame_count = lx_minf(G.frame_count + 1.0f, MAX_HISTORY);
    BlendJob bj = {1.0f / G.frame_count};
    lx_parallel_for(G.cells, 4096, blend_job, &bj);
    BlurJob bx = {G.accum, G.scratch, 0}, by = {G.scratch, G.filtered, 1}, bz = {G.filtered, G.scratch, 2};
    lx_parallel_for(G.cells, 4096, blur_job, &bx);
    lx_parallel_for(G.cells, 4096, blur_job, &by);
    lx_parallel_for(G.cells, 4096, blur_job, &bz);
    memcpy(G.filtered, G.scratch, sizeof(float) * (size_t)G.cells * SH_FLOATS);
    G.version++;
    G.photons_last = photons;
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
