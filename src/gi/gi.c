/* Live GI: real-time global illumination from distance fields and a lit surface cache.
 *
 * Nothing is baked and nothing needs ray-tracing hardware. Around the camera sit three nested clipmap levels
 * (fine near the eye, coarse far away). Each level holds:
 *   - a signed distance field of the scene's static geometry (terrain measured from its heights), two
 *     voxels per probe spacing, truncated a few voxels from the surfaces. Rays march through it in big safe steps
 *     ("sphere tracing"), so a ray costs a few dozen samples whatever the triangle count;
 *   - a surface cache: the voxels the surfaces pass through, each with the surface's colour, glow and the light
 *     leaving it - direct light (sun, lamps, spots, with soft shadows traced through the distance field), plus the
 *     bounce light arriving there (read back from the probes: every frame adds another bounce, so light keeps
 *     bouncing for free);
 *   - a grid of irradiance probes. Each traces a handful of rays per update into the distance field; a ray that
 *     hits a surface brings back the cache's light there, one that leaves the coarsest level brings back the sky.
 *     The result is stored as L1 spherical harmonics and blended over updates.
 * The shader reads the probes exactly as before (nested grids, walls between cells so light doesn't leak).
 *
 * Fast by design: the work is a fixed budget of rays per frame spread over the probes that still need light, on
 * every core. Probes near a change (a lamp switched on, the geometry edited) are updated
 * first; once the lighting has settled nothing is traced at all, so a still scene costs nothing. Moving the
 * camera only fills in the thin slab of a level that came into view. */
#include "../lx_internal.h"

#define SH_FLOATS 12
#define SH_C0 0.282095f
#define SH_C1 0.488603f
#define NLEV 3                /* levels 0, 1 are the shader's camera grids; 2 is its "scene" grid */
#define TRUNC 3.0f            /* the distance field is exact up to this many voxels from a surface */
#define TARGET 8              /* updates a probe near surfaces gets after a change */
#define TARGET_AIR 3          /* ...and one out in the open (its light is smooth, and only characters pass through it) */
#define BRICK 4               /* voxels per side of an empty-space brick */
#define VIS_MAX 1.5f
#define TERRAIN_TRI 0x7fffffff  /* a voxel whose nearest surface is a terrain (measured from its height data) */

typedef struct Light {
    int kind;                 /* LxLightType */
    LxVec3 pos, dir, color;
    float range, cos_cone;
} Light;

typedef struct Level {
    bool on;
    int pres[3], sres[3];     /* probes; distance-field voxels (2 per probe spacing) */
    int np, nv;
    float pcell, scell;
    int oc[3];                /* the grid's corner, in probe cells (world = oc x pcell) */
    LxVec3 origin;
    float *sdf;               /* per voxel: signed distance (world units), TRUNC voxels at most */
    int32_t *tri;             /* per voxel: the closest triangle + 1, TERRAIN_TRI, or 0 (none) */
    float *rad;               /* per voxel: 3, light leaving the surface (surface voxels) */
    uint8_t *surf;            /* per voxel: 1 = a surface voxel */
    int *slist;               /* the surface voxels */
    int nsurf, csurf;
    int *snew;                /* surface voxels not lit yet: lit before any probe looks at them */
    int nnew, cnew;
    float *sh;                /* per probe: 12 (L1 SH, red / green / blue) */
    float *var;               /* per probe: how much its brightness jumps between updates (noise), to tell noise from change */
    float *vis;               /* per probe: 6, distance to the first surface along +x -x +y -y +z -z (probe cells) */
    uint8_t *hist, *need;     /* per probe: updates in its history; updates it still wants */
    bool unlit;               /* some probes have never been traced */
    bool feedback;            /* the bounce light may still be building up */
    uint32_t *stamp;          /* per probe: the frame it was last picked in */
    uint8_t *fresh;           /* per probe: 1 = just came into the level, to be seeded from the level around it */
    int *order;               /* the probes nearest the camera (the level's middle) first */
    uint8_t *cls;             /* per probe: 0 buried in geometry, 1 near a surface, 2 out in the open */
    int bres[3];              /* empty-space bricks (BRICK^3 voxels each) */
    uint8_t *bdist;           /* per brick: how many bricks away the nearest one with a surface is (0 = has one) */
    int rr, srr;              /* where the round-robin cursors are */
    uint32_t version, vis_version, sdf_version;
    bool dirty_sdf;           /* boxes to rebuild are queued */
    int boxes[64][7];         /* voxel boxes (min xyz, max xyz exclusive) to rebuild; [6] 1 = the geometry there changed */
    int nboxes;
} Level;

static struct {
    LxBvh bvh;
    LxScene *built_scene;
    uint32_t built_version;
    uint64_t geo_hash;        /* the static geometry's content: a copy of the scene (pressing Play) keeps the light */
    unsigned stale;           /* levels still to build again after the geometry changed (one per frame) */
    LxTerrainQuery tq;        /* the scene's terrains, read straight from their height data */
    uint64_t terrain_sig;
    float sky_sh[12];         /* the sky alone, as probes would see it in the open (new probes start from it) */
    Level lv[NLEV];
    int probe_res;            /* the setting the levels were made for */
    float base_cell;
    Light lights[64];
    int nlights;
    LxEnvironment env;
    uint64_t light_hash, soft_hash, prev_light_hash, prev_soft_hash;
    LxVec3 dn_dir_last;
    float dn_env_last[7];
    int bounces, rays_per_probe;
    float alpha_min;
    uint32_t frame;
    int rays_last;
    float ms_last;
    bool walls;
    LxGiVolume volume;
} G = {.walls = true};

void lx_gi_init(void) {
    memset(&G, 0, sizeof G);
    G.walls = true;
}
static void level_free(Level *L) {
    lx_free(L->sdf); lx_free(L->tri); lx_free(L->rad); lx_free(L->surf); lx_free(L->slist); lx_free(L->snew);
    lx_free(L->sh); lx_free(L->var); lx_free(L->vis); lx_free(L->hist); lx_free(L->need); lx_free(L->cls); lx_free(L->bdist); lx_free(L->stamp); lx_free(L->fresh);
    memset(L, 0, sizeof *L);
}
void lx_gi_shutdown(void) {
    lx_bvh_free(&G.bvh);
    for (int l = 0; l < NLEV; l++) level_free(&G.lv[l]);
    memset(&G, 0, sizeof G);
    G.walls = true;
}
void lx_gi_set_walls(bool on) {
    G.walls = on;
    for (int l = 0; l < NLEV; l++) G.lv[l].on = false;   /* rebuilt with or without them */
}
void lx_gi_reset(void) {
    for (int l = 0; l < NLEV; l++) {
        Level *L = &G.lv[l];
        if (!L->on) continue;
        memset(L->hist, 0, (size_t)L->np);
        for (int i = 0; i < L->np; i++) L->need[i] = L->cls[i] == 0 ? 0 : L->cls[i] == 1 ? TARGET : TARGET_AIR;
    }
}
const LxGiVolume *lx_gi_volume(void) { return &G.volume; }
int lx_gi_last_ray_count(void) { return G.rays_last; }
float lx_gi_last_ms(void) { return G.ms_last; }
int lx_gi_moving_triangles(void) { return 0; }   /* moving objects are lit by the GI but don't block or bounce it */

static float luminance(LxVec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static uint64_t hash_floats(uint64_t h, const float *f, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t b;
        memcpy(&b, &f[i], 4);
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

/* ---------------------------------------------------------------- closest triangle (for the distance field) */
static LxVec3 closest_on_tri(LxVec3 p, LxVec3 a, LxVec3 ab, LxVec3 ac) {
    LxVec3 ap = lx_sub3(p, a);
    float d1 = lx_dot3(ab, ap), d2 = lx_dot3(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    LxVec3 bp = lx_sub3(ap, ab);
    float d3 = lx_dot3(ab, bp), d4 = lx_dot3(ac, bp);
    if (d3 >= 0 && d4 <= d3) return lx_add3(a, ab);
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return lx_madd3(a, ab, d1 / (d1 - d3));
    LxVec3 cp = lx_sub3(ap, ac);
    float d5 = lx_dot3(ab, cp), d6 = lx_dot3(ac, cp);
    if (d6 >= 0 && d5 <= d6) return lx_add3(a, ac);
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return lx_madd3(a, ac, d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return lx_add3(lx_add3(a, ab), lx_scale3(lx_sub3(ac, ab), w));
    }
    float denom = 1.0f / (va + vb + vc);
    return lx_add3(a, lx_add3(lx_scale3(ab, vb * denom), lx_scale3(ac, vc * denom)));
}
static float box_dist2(const LxAabb *b, LxVec3 p) {
    float dx = lx_maxf(lx_maxf(b->min.x - p.x, 0), p.x - b->max.x);
    float dy = lx_maxf(lx_maxf(b->min.y - p.y, 0), p.y - b->max.y);
    float dz = lx_maxf(lx_maxf(b->min.z - p.z, 0), p.z - b->max.z);
    return dx * dx + dy * dy + dz * dz;
}
/* the closest triangle within sqrt(*best2): updates *best2, *tri, *sign (the side of it p is on) */
static void bvh_closest(const LxBvh *b, LxVec3 p, float *best2, int *tri, float *sign) {
    if (!b->tri_count || !b->node_count) return;
    int stack[64], sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const LxBvhNode *n = &b->nodes[stack[--sp]];
        if (box_dist2(&n->bounds, p) >= *best2) continue;
        if (n->count) {
            for (int k = 0; k < n->count; k++) {
                int t = b->tri_index[n->left_or_first + k];
                LxVec3 q = closest_on_tri(p, b->v0[t], b->e1[t], b->e2[t]);
                LxVec3 d = lx_sub3(p, q);
                float d2 = lx_dot3(d, d);
                if (d2 < *best2) {
                    *best2 = d2;
                    *tri = t;
                    *sign = lx_dot3(d, b->normals[t]) < 0 ? -1.0f : 1.0f;
                }
            }
        } else if (sp < 62) {
            int l = n->left_or_first, r = l + 1;
            float dl = box_dist2(&b->nodes[l].bounds, p), dr = box_dist2(&b->nodes[r].bounds, p);
            if (dl < dr) { stack[sp++] = r; stack[sp++] = l; }   /* nearer child first */
            else { stack[sp++] = l; stack[sp++] = r; }
        }
    }
}

/* ---------------------------------------------------------------- levels */
static inline int vidx(const Level *L, int x, int y, int z) { return (z * L->sres[1] + y) * L->sres[0] + x; }
static inline int pidx(const Level *L, int x, int y, int z) { return (z * L->pres[1] + y) * L->pres[0] + x; }

static const Level *g_sort_level;
static float order_key(int i) {
    const Level *L = g_sort_level;
    float x = (float)(i % L->pres[0]) + 0.5f - L->pres[0] * 0.5f, y = (float)((i / L->pres[0]) % L->pres[1]) + 0.5f - L->pres[1] * 0.5f,
          z = (float)(i / (L->pres[0] * L->pres[1])) + 0.5f - L->pres[2] * 0.5f;
    return x * x + y * y + z * z;
}
static int order_cmp(const void *a, const void *b) {
    float ka = order_key(*(const int *)a), kb = order_key(*(const int *)b);
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}
static void level_alloc(Level *L) {
    L->np = L->pres[0] * L->pres[1] * L->pres[2];
    L->nv = L->sres[0] * L->sres[1] * L->sres[2];
    L->sdf = lx_alloc(sizeof(float) * (size_t)L->nv);
    L->tri = lx_calloc((size_t)L->nv, sizeof(int32_t));
    L->rad = lx_calloc((size_t)L->nv * 3, sizeof(float));
    L->surf = lx_calloc((size_t)L->nv, 1);
    L->sh = lx_calloc((size_t)L->np * SH_FLOATS, sizeof(float));
    L->var = lx_calloc((size_t)L->np, sizeof(float));
    L->vis = lx_alloc(sizeof(float) * (size_t)L->np * 6);
    L->hist = lx_calloc((size_t)L->np, 1);
    L->need = lx_alloc((size_t)L->np);
    L->cls = lx_alloc((size_t)L->np);
    L->stamp = lx_calloc((size_t)L->np, sizeof(uint32_t));
    L->fresh = lx_alloc((size_t)L->np);
    memset(L->fresh, 1, (size_t)L->np);
    /* the nearest-first order depends only on the level's size: kept for each size */
    static struct { int pres[3]; int *order; } cache[6];
    L->order = NULL;
    for (int k = 0; k < 6 && !L->order; k++)
        if (cache[k].order && !memcmp(cache[k].pres, L->pres, sizeof L->pres)) L->order = cache[k].order;
    if (!L->order) {
        int k = -1;
        for (int c = 0; c < 6 && k < 0; c++) if (!cache[c].order) k = c;
        for (int c = 0; c < 6 && k < 0; c++) {   /* full: one no level uses now */
            bool used = false;
            for (int l = 0; l < NLEV; l++) used = used || G.lv[l].order == cache[c].order;
            if (!used) k = c;
        }
        if (k < 0) k = 0;   /* can't happen: three levels, six slots */
        lx_free(cache[k].order);
        cache[k].order = lx_alloc(sizeof(int) * (size_t)L->np);
        memcpy(cache[k].pres, L->pres, sizeof L->pres);
        for (int i = 0; i < L->np; i++) cache[k].order[i] = i;
        g_sort_level = L;
        qsort(cache[k].order, (size_t)L->np, sizeof(int), order_cmp);
        L->order = cache[k].order;
    }
    L->unlit = true;
    memset(L->cls, 2, (size_t)L->np);
    for (int a = 0; a < 3; a++) L->bres[a] = (L->sres[a] + BRICK - 1) / BRICK;
    L->bdist = lx_calloc((size_t)(L->bres[0] * L->bres[1] * L->bres[2]), 1);
    float far = TRUNC * L->scell;
    for (int i = 0; i < L->nv; i++) L->sdf[i] = far;
    for (int i = 0; i < L->np * 6; i++) L->vis[i] = VIS_MAX;
    memset(L->need, TARGET, (size_t)L->np);
}
static void queue_box(Level *L, int x0, int y0, int z0, int x1, int y1, int z1, int changed) {
    x0 = lx_maxi(x0, 0); y0 = lx_maxi(y0, 0); z0 = lx_maxi(z0, 0);
    x1 = lx_mini(x1, L->sres[0]); y1 = lx_mini(y1, L->sres[1]); z1 = lx_mini(z1, L->sres[2]);
    if (x0 >= x1 || y0 >= y1 || z0 >= z1) return;
    if (L->nboxes >= 64) {   /* too many: one box around them all */
        int *b = L->boxes[0];
        for (int i = 1; i < L->nboxes; i++) {
            for (int a = 0; a < 3; a++) { b[a] = lx_mini(b[a], L->boxes[i][a]); b[3 + a] = lx_maxi(b[3 + a], L->boxes[i][3 + a]); }
            b[6] = lx_maxi(b[6], L->boxes[i][6]);
        }
        L->nboxes = 1;
    }
    int *b = L->boxes[L->nboxes++];
    b[0] = x0; b[1] = y0; b[2] = z0; b[3] = x1; b[4] = y1; b[5] = z1; b[6] = changed;
    L->dirty_sdf = true;
}

/* Place a level around the eye, in steps of two probes. A small move keeps everything that's still inside and
 * only the slab that came into view is built; a jump starts over. */
static float g_look_y;   /* the view's up/down direction: levels move down when looking down, up when looking up */
void lx_gi_set_view_dir(LxVec3 fwd) {
    float l = lx_len3(fwd);
    g_look_y = l > 1e-4f && isfinite(l) ? lx_clampf(fwd.y / l, -1.0f, 1.0f) : 0.0f;
}
static void level_place(Level *L, int lev, LxVec3 eye, int P, float pcell) {
    int pres[3] = {P, P, P};
    if (lev == NLEV - 1) { pres[0] = P * 3 / 2; pres[2] = P * 3 / 2; }   /* the far level is wider than tall */
    /* a camera high above looking down sees the ground, not the air around itself: each level slides along the
     * view's vertical direction by up to 60 % of its half height, so the ground gets the finer probes (turning
     * left and right never moves the levels) */
    eye.y += g_look_y * 0.6f * 0.5f * (float)pres[1] * pcell;
    int oc[3];
    for (int a = 0; a < 3; a++) {
        int c = (int)floorf(lx_comp3(eye, a) / pcell) - pres[a] / 2;
        oc[a] = (int)floorf((float)c / 2.0f) * 2;
    }
    bool fresh = !L->on || L->pcell != pcell || L->pres[0] != pres[0] || L->pres[1] != pres[1];
    if (!fresh && oc[0] == L->oc[0] && oc[1] == L->oc[1] && oc[2] == L->oc[2]) return;
    int d[3] = {oc[0] - L->oc[0], oc[1] - L->oc[1], oc[2] - L->oc[2]};
    if (!fresh && (abs(d[0]) >= pres[0] || abs(d[1]) >= pres[1] || abs(d[2]) >= pres[2])) fresh = true;
    if (fresh) {
        level_free(L);
        L->on = true;
        L->pcell = pcell;
        L->scell = pcell * 0.5f;
        for (int a = 0; a < 3; a++) { L->pres[a] = pres[a]; L->sres[a] = pres[a] * 2; L->oc[a] = oc[a]; }
        L->origin = lx_scale3(lx_v3((float)oc[0], (float)oc[1], (float)oc[2]), pcell);
        level_alloc(L);
        L->nboxes = 0;
        queue_box(L, 0, 0, 0, L->sres[0], L->sres[1], L->sres[2], 0);
        L->version++;
        L->vis_version++;
        return;
    }
    /* shift what's still inside into place */
    Level n = *L;
    n.sdf = NULL;
    level_alloc(&n);
    int vs[3] = {d[0] * 2, d[1] * 2, d[2] * 2};
    for (int z = 0; z < n.sres[2]; z++)
        for (int y = 0; y < n.sres[1]; y++)
            for (int x = 0; x < n.sres[0]; x++) {
                int ox = x + vs[0], oy = y + vs[1], oz = z + vs[2];
                if (ox < 0 || oy < 0 || oz < 0 || ox >= L->sres[0] || oy >= L->sres[1] || oz >= L->sres[2]) continue;
                int i = vidx(&n, x, y, z), o = vidx(L, ox, oy, oz);
                n.sdf[i] = L->sdf[o];
                n.tri[i] = L->tri[o];
                n.surf[i] = L->surf[o];
                memcpy(&n.rad[i * 3], &L->rad[o * 3], 12);
            }
    for (int z = 0; z < n.pres[2]; z++)
        for (int y = 0; y < n.pres[1]; y++)
            for (int x = 0; x < n.pres[0]; x++) {
                int ox = x + d[0], oy = y + d[1], oz = z + d[2];
                if (ox < 0 || oy < 0 || oz < 0 || ox >= L->pres[0] || oy >= L->pres[1] || oz >= L->pres[2]) continue;
                int i = pidx(&n, x, y, z), o = pidx(L, ox, oy, oz);
                memcpy(&n.sh[i * SH_FLOATS], &L->sh[o * SH_FLOATS], sizeof(float) * SH_FLOATS);
                n.var[i] = L->var[o];
                memcpy(&n.vis[i * 6], &L->vis[o * 6], sizeof(float) * 6);
                n.hist[i] = L->hist[o];
                n.need[i] = L->need[o];
                n.cls[i] = L->cls[o];
                n.fresh[i] = 0;
                n.stamp[i] = L->stamp[o];
            }
    lx_free(L->sdf); lx_free(L->tri); lx_free(L->rad); lx_free(L->surf); lx_free(L->slist);
    lx_free(L->sh); lx_free(L->var); lx_free(L->vis); lx_free(L->hist); lx_free(L->need); lx_free(L->cls); lx_free(L->bdist); lx_free(L->stamp); lx_free(L->fresh);
    n.slist = NULL;
    n.nnew = 0;
    n.nsurf = n.csurf = 0;
    for (int a = 0; a < 3; a++) n.oc[a] = oc[a];
    n.origin = lx_scale3(lx_v3((float)oc[0], (float)oc[1], (float)oc[2]), pcell);
    *L = n;
    L->nboxes = 0;
    /* the slabs that came into view (plus the voxels next to them, whose neighbours changed) */
    for (int a = 0; a < 3; a++) {
        if (!vs[a]) continue;
        int lo[3] = {0, 0, 0}, hi[3] = {L->sres[0], L->sres[1], L->sres[2]};
        if (vs[a] > 0) lo[a] = L->sres[a] - vs[a] - 2;
        else hi[a] = -vs[a] + 2;
        queue_box(L, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], 0);
    }
    L->dirty_sdf = true;
    L->feedback = true;
    L->version++;
    L->vis_version++;
    L->sdf_version++;
}

/* ---------------------------------------------------------------- distance field */
typedef struct SdfJob { Level *L; const int *box; } SdfJob;
static void sdf_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    SdfJob *j = user;
    Level *L = j->L;
    const int *b = j->box;
    int nx = b[3] - b[0], ny = b[4] - b[1];
    float far = TRUNC * L->scell;
    for (int k = begin; k < end; k++) {
        int x = b[0] + k % nx, y = b[1] + (k / nx) % ny, z = b[2] + k / (nx * ny);
        LxVec3 p = lx_add3(L->origin, lx_v3((x + 0.5f) * L->scell, (y + 0.5f) * L->scell, (z + 0.5f) * L->scell));
        float best2 = far * far, sign = 1;
        int tri = -1;
        bvh_closest(&G.bvh, p, &best2, &tri, &sign);
        int i = vidx(L, x, y, z);
        float sd = best2 < far * far ? sign * sqrtf(best2) : far;
        int32_t id = tri >= 0 ? tri + 1 : 0;
        /* terrain: exact, from its heights (its meshes are only an approximation further away) */
        float h;
        LxVec3 tn;
        if (G.tq.n && lx_terrain_query(&G.tq, p.x, p.z, &h, &tn, NULL)) {
            float st = (p.y - h) * lx_maxf(tn.y, 0.05f);
            if (st < sd) { sd = lx_maxf(st, -far); id = st < far ? TERRAIN_TRI : 0; }
        }
        sd = lx_minf(sd, far);
        L->sdf[i] = sd;
        L->tri[i] = id;
    }
}
static void rebuild_surfaces(Level *L) {
    L->nsurf = 0;
    float band = L->scell * 0.8f;
    for (int i = 0; i < L->nv; i++) {
        bool s = L->tri[i] != 0 && fabsf(L->sdf[i]) <= band;
        if (!s) { L->surf[i] = 0; continue; }
        if (!L->surf[i]) {
            L->rad[i * 3] = L->rad[i * 3 + 1] = L->rad[i * 3 + 2] = 0;
            LX_VEC_PUSH(L->snew, L->nnew, L->cnew, i);
        }
        L->surf[i] = 1;
        LX_VEC_PUSH(L->slist, L->nsurf, L->csurf, i);
    }
    if (L->srr >= L->nsurf) L->srr = 0;
}

/* empty space: a brick is empty when none of its voxels is near a surface; each brick knows how far (in bricks,
 * the largest of the three axes) the nearest non-empty one is, so rays cross open space in long strides */
static void build_bricks(Level *L) {
    int bx = L->bres[0], by = L->bres[1], bz = L->bres[2], nb = bx * by * bz;
    float far = TRUNC * L->scell * 0.999f;
    for (int i = 0; i < nb; i++) L->bdist[i] = 255;
    for (int z = 0; z < L->sres[2]; z++)
        for (int y = 0; y < L->sres[1]; y++)
            for (int x = 0; x < L->sres[0]; x++)
                if (L->sdf[vidx(L, x, y, z)] < far) L->bdist[((z / BRICK) * by + y / BRICK) * bx + x / BRICK] = 0;
    /* two passes of a 26-neighbour chamfer: Chebyshev distance */
    for (int pass = 0; pass < 2; pass++) {
        int s0 = pass ? nb - 1 : 0, st = pass ? -1 : 1;
        for (int k = 0, i = s0; k < nb; k++, i += st) {
            int x = i % bx, y = (i / bx) % by, z = i / (bx * by);
            int best = L->bdist[i];
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int X = x + dx, Y = y + dy, Z = z + dz;
                        if (X < 0 || Y < 0 || Z < 0 || X >= bx || Y >= by || Z >= bz) continue;
                        int d = L->bdist[(Z * by + Y) * bx + X] + 1;
                        if (d < best) best = d;
                    }
            L->bdist[i] = (uint8_t)lx_mini(best, 255);
        }
    }
}
/* a safe step from p: at least the distance field's own (truncated) value, more in empty bricks */
static inline float empty_step(const Level *L, LxVec3 p) {
    int x = (int)((p.x - L->origin.x) / (L->scell * BRICK)), y = (int)((p.y - L->origin.y) / (L->scell * BRICK)), z = (int)((p.z - L->origin.z) / (L->scell * BRICK));
    if (x < 0 || y < 0 || z < 0 || x >= L->bres[0] || y >= L->bres[1] || z >= L->bres[2]) return 0;
    int d = L->bdist[(z * L->bres[1] + y) * L->bres[0] + x];
    return d > 1 ? (float)(d - 1) * L->scell * BRICK : 0;
}
static inline float sdf_at(const Level *L, LxVec3 p) {
    float gx = (p.x - L->origin.x) / L->scell - 0.5f, gy = (p.y - L->origin.y) / L->scell - 0.5f, gz = (p.z - L->origin.z) / L->scell - 0.5f;
    int x = (int)floorf(gx), y = (int)floorf(gy), z = (int)floorf(gz);
    float fx = gx - x, fy = gy - y, fz = gz - z;
    if (x < 0 || y < 0 || z < 0 || x >= L->sres[0] - 1 || y >= L->sres[1] - 1 || z >= L->sres[2] - 1) return TRUNC * L->scell;
    const float *s = L->sdf + vidx(L, x, y, z);
    int sy = L->sres[0], sz = L->sres[0] * L->sres[1];
    float c00 = s[0] + (s[1] - s[0]) * fx, c10 = s[sy] + (s[sy + 1] - s[sy]) * fx;
    float c01 = s[sz] + (s[sz + 1] - s[sz]) * fx, c11 = s[sz + sy] + (s[sz + sy + 1] - s[sz + sy]) * fx;
    float c0 = c00 + (c10 - c00) * fy, c1 = c01 + (c11 - c01) * fy;
    return c0 + (c1 - c0) * fz;
}
static inline bool level_has(const Level *L, LxVec3 p) {
    float m = L->scell * 1.5f;
    return L->on && p.x > L->origin.x + m && p.y > L->origin.y + m && p.z > L->origin.z + m &&
           p.x < L->origin.x + L->sres[0] * L->scell - m && p.y < L->origin.y + L->sres[1] * L->scell - m &&
           p.z < L->origin.z + L->sres[2] * L->scell - m;
}

/* ---------------------------------------------------------------- the sky */
static LxVec3 sky_radiance(LxVec3 d) {
    const LxEnvironment *e = &G.env;
    LxVec3 c = d.y >= 0 ? lx_lerp3(e->sky_horizon, e->sky_zenith, sqrtf(d.y)) : lx_lerp3(e->sky_horizon, e->ground_color, sqrtf(-d.y));
    return lx_scale3(c, e->ambient_intensity);
}

/* ---------------------------------------------------------------- tracing */
/* March from o along d (unit) through the levels, finest first. Returns true on a hit (*lev, *hit set). */
static bool march(LxVec3 o, LxVec3 d, float tmax, int lev, int *hit_lev, LxVec3 *hit, float *steps) {
    float t = 0, prev = 1e30f;
    for (int it = 0; it < 160 && t < tmax; it++) {
        LxVec3 p = lx_madd3(o, d, t);
        while (lev < NLEV && !level_has(&G.lv[lev], p)) { lev++; prev = 1e30f; }
        if (lev >= NLEV) return false;
        const Level *L = &G.lv[lev];
        float s = sdf_at(L, p);
        if (steps) *steps += 1;
        if (s < L->scell * 0.35f && s < prev && it > 0) {
            *hit_lev = lev;
            *hit = p;
            return true;
        }
        prev = s;
        t += lx_maxf(lx_maxf(s, empty_step(L, p)), lx_maxf(L->scell * 0.5f, t * 0.015f));
    }
    return false;
}
/* how much of a light gets through from p towards d over dist (soft: 0..1) */
static float shadow(LxVec3 p, LxVec3 d, float dist, int lev) {
    float t = 0, vis = 1.0f;
    for (int it = 0; it < 96 && t < dist; it++) {
        LxVec3 q = lx_madd3(p, d, t);
        while (lev < NLEV && !level_has(&G.lv[lev], q)) lev++;
        if (lev >= NLEV) return vis;
        const Level *L = &G.lv[lev];
        float s = sdf_at(L, q);
        if (s < L->scell * 0.25f) return 0.0f;
        if (t > L->scell) vis = lx_minf(vis, 6.0f * (s - L->scell * 0.25f) / t);
        t += lx_maxf(lx_maxf(s, empty_step(L, q)), L->scell * 0.5f);
    }
    return lx_clampf(vis, 0, 1);
}

/* the light leaving the surface at p (a hit): the nearest surface voxel of that level */
static LxVec3 surface_light(const Level *L, LxVec3 p) {
    int x = (int)floorf((p.x - L->origin.x) / L->scell), y = (int)floorf((p.y - L->origin.y) / L->scell), z = (int)floorf((p.z - L->origin.z) / L->scell);
    float best = 1e30f;
    int bi = -1;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int X = x + dx, Y = y + dy, Z = z + dz;
                if (X < 0 || Y < 0 || Z < 0 || X >= L->sres[0] || Y >= L->sres[1] || Z >= L->sres[2]) continue;
                int i = vidx(L, X, Y, Z);
                if (!L->surf[i]) continue;
                float w = fabsf(L->sdf[i]) + 0.5f * L->scell * (float)(dx * dx + dy * dy + dz * dz);
                if (w < best) { best = w; bi = i; }
            }
    return bi >= 0 ? lx_v3(L->rad[bi * 3], L->rad[bi * 3 + 1], L->rad[bi * 3 + 2]) : lx_v3(0, 0, 0);
}

/* ---------------------------------------------------------------- probes */
float lx_gi_eval_l1(const float *c, LxVec3 n);
/* irradiance from a level's probes at p (no walls: surfaces in the cache are lit from just in front of them) */
static LxVec3 probe_irradiance(const Level *L, LxVec3 p, LxVec3 n) {
    float gx = (p.x - L->origin.x) / L->pcell - 0.5f, gy = (p.y - L->origin.y) / L->pcell - 0.5f, gz = (p.z - L->origin.z) / L->pcell - 0.5f;
    int x = (int)floorf(gx), y = (int)floorf(gy), z = (int)floorf(gz);
    float f[3] = {gx - x, gy - y, gz - z}, c[12] = {0}, sum = 0;
    for (int k = 0; k < 8; k++) {
        int X = x + (k & 1), Y = y + ((k >> 1) & 1), Z = z + (k >> 2);
        if (X < 0 || Y < 0 || Z < 0 || X >= L->pres[0] || Y >= L->pres[1] || Z >= L->pres[2]) continue;
        int i = pidx(L, X, Y, Z);
        if (!L->cls[i]) continue;   /* buried */
        float w = ((k & 1) ? f[0] : 1 - f[0]) * (((k >> 1) & 1) ? f[1] : 1 - f[1]) * ((k >> 2) ? f[2] : 1 - f[2]);
        if (w <= 0) continue;
        sum += w;
        for (int q = 0; q < 12; q++) c[q] += L->sh[i * 12 + q] * w;
    }
    if (sum < 1e-4f) return lx_v3(0, 0, 0);
    for (int q = 0; q < 12; q++) c[q] /= sum;
    return lx_v3(lx_gi_eval_l1(c, n), lx_gi_eval_l1(c + 4, n), lx_gi_eval_l1(c + 8, n));
}

/* light the surface cache: direct light with traced soft shadows, plus the bounce light the probes see */
typedef struct LightJob { Level *L; int lev; const int *idx; } LightJob;
static void light_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    LightJob *j = user;
    Level *L = j->L;
    for (int k = begin; k < end; k++) {
        int i = j->idx[k];
        int32_t t = L->tri[i];
        if (!t) continue;
        int x = i % L->sres[0], y = (i / L->sres[0]) % L->sres[1], z = i / (L->sres[0] * L->sres[1]);
        LxVec3 c = lx_add3(L->origin, lx_v3((x + 0.5f) * L->scell, (y + 0.5f) * L->scell, (z + 0.5f) * L->scell));
        LxVec3 n, albedo, glow = lx_v3(0, 0, 0);
        if (t == TERRAIN_TRI) {
            float h;
            if (!lx_terrain_query(&G.tq, c.x, c.z, &h, &n, &albedo)) continue;
        } else {
            const LxBvh *b = &G.bvh;
            int ti = t - 1;
            if (ti >= b->tri_count) continue;
            n = b->normals[ti];
            const LxTriMaterial *mt = &b->materials[b->mat_index[ti]];
            albedo = b->tri_albedo ? lx_bvh_tri_albedo(b, ti) : mt->albedo;   /* its own part of the texture */
            glow = mt->emissive;
        }
        if (L->sdf[i] < 0) n = lx_neg3(n);   /* seen from behind: light its back */
        LxVec3 p = lx_madd3(c, n, -fabsf(L->sdf[i]));   /* onto the surface */
        LxVec3 from = lx_madd3(p, n, L->scell * 1.2f);
        LxVec3 e = lx_v3(0, 0, 0);
        for (int li = 0; li < G.nlights; li++) {
            const Light *lt = &G.lights[li];
            LxVec3 l;
            float att, dist;
            if (lt->kind == LX_LIGHT_DIRECTIONAL) {
                l = lx_neg3(lt->dir);
                att = 1.0f;
                dist = 1e4f;
            } else {
                LxVec3 to = lx_sub3(lt->pos, p);
                float d2 = lx_maxf(lx_dot3(to, to), 1e-4f);
                if (d2 > lt->range * lt->range) continue;
                dist = sqrtf(d2);
                l = lx_scale3(to, 1.0f / dist);
                float w = lx_clampf(1.0f - (d2 / (lt->range * lt->range)) * (d2 / (lt->range * lt->range)), 0, 1);
                att = w * w / d2;
                if (lt->kind == LX_LIGHT_SPOT) {
                    float cd = lx_dot3(lx_neg3(l), lt->dir), lo = lt->cos_cone, hi = lo + (1.0f - lo) * 0.25f;
                    float s = lx_clampf((cd - lo) / lx_maxf(hi - lo, 1e-4f), 0, 1);
                    att *= s * s * (3 - 2 * s);
                }
                dist -= L->scell;
            }
            float ndl = lx_dot3(n, l);
            if (ndl <= 0 || att <= 0) continue;
            float sh = shadow(from, l, dist, j->lev);
            if (sh <= 0) continue;
            e = lx_add3(e, lx_scale3(lt->color, ndl * att * sh));
        }
        if (G.bounces > 1) e = lx_add3(e, probe_irradiance(L, lx_madd3(p, n, L->pcell * 0.5f), n));
        LxVec3 r = lx_add3(lx_scale3(lx_mul3(albedo, e), 1.0f / LX_PI), glow);
        L->rad[i * 3] = r.x; L->rad[i * 3 + 1] = r.y; L->rad[i * 3 + 2] = r.z;
    }
}

/* a probe's rays: a spherical Fibonacci set, turned at random every update so the history covers every
 * direction evenly */
typedef struct ProbeJob { Level *L; int lev; const int *idx; int rays; uint32_t seed; float rays_done; } ProbeJob;
static void probe_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    ProbeJob *j = user;
    Level *L = j->L;
    for (int k = begin; k < end; k++) {
        int i = j->idx[k];
        int x = i % L->pres[0], y = (i / L->pres[0]) % L->pres[1], z = i / (L->pres[0] * L->pres[1]);
        LxVec3 o = lx_add3(L->origin, lx_v3((x + 0.5f) * L->pcell, (y + 0.5f) * L->pcell, (z + 0.5f) * L->pcell));
        if (L->need[i]) L->need[i]--;
        if (sdf_at(L, o) < -0.05f * L->pcell) { L->hist[i] = 0; continue; }   /* buried in a wall: filled from its neighbours */
        LxRng rng = lx_rng_seed(j->seed, (uint64_t)i * 2654435761u + 1u);
        LxQuat q = lx_quat_norm((LxQuat){lx_rng_f(&rng) - 0.5f, lx_rng_f(&rng) - 0.5f, lx_rng_f(&rng) - 0.5f, lx_rng_f(&rng) - 0.5f});
        int N = L->cls[i] == 2 ? lx_maxi(j->rays / 2, 8) : j->rays;
        if (!L->hist[i]) N *= 2;   /* starting over: twice the rays, so the first estimate isn't a noisy one */
        LxVec3 rad_ray[512], dir_ray[512];
        if (N > 512) N = 512;
        float mean = 0;
        for (int r = 0; r < N; r++) {
            float zz = 1.0f - (2.0f * r + 1.0f) / (float)N, rr = sqrtf(lx_maxf(0, 1 - zz * zz)), a = (float)r * 2.39996323f;
            LxVec3 d = lx_quat_rotate(q, lx_v3(rr * cosf(a), zz, rr * sinf(a)));
            int hl;
            LxVec3 hp;
            dir_ray[r] = d;
            rad_ray[r] = march(o, d, 1e4f, j->lev, &hl, &hp, NULL) ? surface_light(&G.lv[hl], hp) : sky_radiance(d);
            mean += luminance(rad_ray[r]);
        }
        mean /= (float)N;
        /* fireflies: one ray finding a small, very bright spot (a lamp's hot surface, a sunlit gap) would flash the
         * probe for an update; no ray may bring more than 12 times this update's average */
        float cap = lx_maxf(mean * 12.0f, 1e-4f);
        float c[12] = {0};
        for (int r = 0; r < N; r++) {
            LxVec3 rad = rad_ray[r], d = dir_ray[r];
            float lum = luminance(rad);
            if (lum > cap) rad = lx_scale3(rad, cap / lum);
            float sh[4] = {SH_C0, SH_C1 * d.y, SH_C1 * d.z, SH_C1 * d.x};
            for (int b = 0; b < 4; b++) { c[b] += rad.x * sh[b]; c[4 + b] += rad.y * sh[b]; c[8 + b] += rad.z * sh[b]; }
        }
        float w = 4.0f * LX_PI / (float)N;
        float *s = &L->sh[i * SH_FLOATS];
        /* Stable and quick at once: how far this update's brightness is from the history, against how much it
         * usually jumps (the noise of a handful of rays). Within the noise: blend in gently, so a settled probe stays
         * still. Well beyond it (a light switched on, a door opened): blend in fast, so the change shows at once. */
        float est = luminance(lx_v3(c[0] * w, c[4] * w, c[8] * w)), was = luminance(lx_v3(s[0], s[4], s[8]));
        float a;
        if (!L->hist[i]) {
            a = 1.0f;
            L->var[i] = est * est * 0.04f;
        } else {
            float diff = est - was, noise = sqrtf(L->var[i]) + 0.03f * lx_maxf(was, 0) + 1e-5f;
            float z = fabsf(diff) / noise;
            float base = lx_maxf(1.0f / (float)(L->hist[i] + 1), G.alpha_min);
            if (z < 2.0f) a = lx_maxf(1.0f / (float)(L->hist[i] + 1), G.alpha_min * 0.35f);
            else a = lx_minf(base * (1.0f + (z - 2.0f) * 0.5f), 0.65f);
            L->var[i] += (diff * diff - L->var[i]) * 0.15f;
        }
        for (int b = 0; b < 12; b++) s[b] += (c[b] * w - s[b]) * a;
        if (L->hist[i] < 250) L->hist[i]++;
    }
}

/* each probe's walls: how far along the six axes the first surface is (probe cells, at most VIS_MAX) */
static void vis_probe(Level *L, int i) {
    float *v = &L->vis[i * 6];
    int x = i % L->pres[0], y = (i / L->pres[0]) % L->pres[1], z = i / (L->pres[0] * L->pres[1]);
    LxVec3 o = lx_add3(L->origin, lx_v3((x + 0.5f) * L->pcell, (y + 0.5f) * L->pcell, (z + 0.5f) * L->pcell));
    float c = sdf_at(L, o);
    L->cls[i] = c < -0.05f * L->pcell ? 0 : c < TRUNC * L->scell * 0.99f ? 1 : 2;
    if (!G.walls) { for (int k = 0; k < 6; k++) v[k] = VIS_MAX; return; }
    if (c < 0) { for (int k = 0; k < 6; k++) v[k] = 0; return; }
    static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int k = 0; k < 6; k++) {
        LxVec3 d = lx_v3(dirs[k][0], dirs[k][1], dirs[k][2]);
        float t = 0, wall = VIS_MAX, step = L->scell * 0.25f;
        while (t < VIS_MAX * L->pcell) {
            t += step;
            if (sdf_at(L, lx_madd3(o, d, t)) < 0) { wall = t / L->pcell; break; }
        }
        v[k] = lx_minf(wall, VIS_MAX);
    }
}
typedef struct VisJob { Level *L; const int *box; int changed; } VisJob;
static void vis_job(void *user, int begin, int end, int thread) {
    LX_UNUSED(thread);
    VisJob *j = user;
    const int *b = j->box;
    int nx = b[3] - b[0], ny = b[4] - b[1];
    for (int k = begin; k < end; k++) {
        int x = b[0] + k % nx, y = b[1] + (k / nx) % ny, z = b[2] + k / (nx * ny);
        Level *L = j->L;
        int i = pidx(L, x, y, z);
        vis_probe(L, i);
        if (L->cls[i] == 0) { L->need[i] = 0; L->hist[i] = 0; continue; }
        if (!j->changed) continue;   /* only came into view: seeded, and traced nearest first */
        if (j->changed == 1) {   /* a small change: a few more updates, keeping the history */
            if (L->need[i] < 3) L->need[i] = 3;
            continue;
        }
        /* the geometry around it changed a lot: gather again, forgetting what was there */
        int want = L->cls[i] == 1 ? TARGET / 2 : TARGET_AIR;
        if (L->need[i] < want) L->need[i] = want;
        L->hist[i] = 0;   /* its next update replaces what it had (shown until then, so nothing goes dark) */
        L->unlit = true;
    }
}

/* build the queued boxes of a level's field, then the walls and surface list that depend on it */
static void level_build(Level *L) {
    if (!L->dirty_sdf) return;
    for (int bi = 0; bi < L->nboxes; bi++) {
        const int *b = L->boxes[bi];
        int n = (b[3] - b[0]) * (b[4] - b[1]) * (b[5] - b[2]);
        SdfJob j = {L, b};
        lx_parallel_for(n, 512, sdf_job, &j);
        /* the probes over this box (and one around it) find their walls again */
        int pb[6] = {lx_maxi(b[0] / 2 - 2, 0), lx_maxi(b[1] / 2 - 2, 0), lx_maxi(b[2] / 2 - 2, 0),
                     lx_mini((b[3] + 1) / 2 + 2, L->pres[0]), lx_mini((b[4] + 1) / 2 + 2, L->pres[1]), lx_mini((b[5] + 1) / 2 + 2, L->pres[2])};
        int pn = (pb[3] - pb[0]) * (pb[4] - pb[1]) * (pb[5] - pb[2]);
        if (pn > 0) {
            VisJob vj = {L, pb, b[6]};
            lx_parallel_for(pn, 256, vis_job, &vj);
        }
    }
    L->nboxes = 0;
    L->dirty_sdf = false;
    rebuild_surfaces(L);
    build_bricks(L);
    L->sdf_version++;
    L->vis_version++;
}

/* probes buried in walls take the light of their open neighbours (the shader still blends them at times) */
static void fill_buried(Level *L) {
    for (int z = 0; z < L->pres[2]; z++)
        for (int y = 0; y < L->pres[1]; y++)
            for (int x = 0; x < L->pres[0]; x++) {
                int i = pidx(L, x, y, z);
                if (L->hist[i]) continue;
                float c[12] = {0};
                int n = 0;
                static const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                for (int k = 0; k < 6; k++) {
                    int X = x + nb[k][0], Y = y + nb[k][1], Z = z + nb[k][2];
                    if (X < 0 || Y < 0 || Z < 0 || X >= L->pres[0] || Y >= L->pres[1] || Z >= L->pres[2]) continue;
                    int o = pidx(L, X, Y, Z);
                    if (!L->hist[o]) continue;
                    for (int q = 0; q < 12; q++) c[q] += L->sh[o * 12 + q];
                    n++;
                }
                if (n) for (int q = 0; q < 12; q++) L->sh[i * 12 + q] = c[q] / (float)n;
            }
}

/* ---------------------------------------------------------------- the scene: lights
 * Things that move (characters, Rigidbodies) are lit by the GI but don't block or bounce it: with their own probes
 * half a metre apart they only made dark halos on the ground around them, flickering as they moved. Their shadows
 * come from the shadow maps. */
static void gather_lights(LxScene *s) {
    G.nlights = 0;
    for (int i = 1; i < s->count && G.nlights < 64; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_LIGHT) || !d->light.contribute_gi) continue;
        Light l;
        memset(&l, 0, sizeof l);
        l.kind = (int)d->light.type;
        l.color = lx_scale3(d->light.color, d->light.intensity);
        if (luminance(l.color) <= 0) continue;
        l.pos = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
        l.dir = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, 1)));
        l.range = lx_maxf(d->light.range, 0.01f);
        l.cos_cone = cosf(d->light.spot_angle_deg * 0.5f * LX_DEG2RAD);
        G.lights[G.nlights++] = l;
    }
    lx_env_effective(s, &G.env);
    /* what changed: a light moved, appeared or went (start over), or only drifted (the Day-night sun creeping,
     * a flickering lamp, the sky's colour at dusk: keep the history, keep updating) */
    uint64_t h = 1469598103934665603ull ^ (uint64_t)G.nlights, tint = 1469598103934665603ull;
    bool dn = s->env.sky_mode == LX_SKY_DAY_NIGHT, sun_done = false;
    for (int i = 0; i < G.nlights; i++) {
        const Light *l = &G.lights[i];
        float f[9] = {(float)l->kind, l->pos.x, l->pos.y, l->pos.z, l->dir.x, l->dir.y, l->dir.z, l->cos_cone, l->range};
        if (dn && l->kind == LX_LIGHT_DIRECTIONAL && !sun_done) {
            sun_done = true;
            bool jumped = lx_dot3(l->dir, G.dn_dir_last) < 0.9995f;
            G.dn_dir_last = l->dir;
            if (!jumped) { tint = hash_floats(tint, f, 9); f[1] = f[2] = f[3] = f[4] = f[5] = f[6] = 0; }
        }
        h = hash_floats(h, f, 9);
        float c[3] = {l->color.x, l->color.y, l->color.z};
        float lum = luminance(l->color);
        float big[1] = {floorf(log2f(lx_maxf(lum, 1e-6f)) * 2.0f)};   /* switched on / off, or a big change: start over */
        h = hash_floats(h, big, 1);
        tint = hash_floats(tint, c, 3);
    }
    const LxEnvironment *e = &G.env;
    float env[10] = {e->sky_zenith.x, e->sky_zenith.y, e->sky_zenith.z, e->sky_horizon.x, e->sky_horizon.y, e->sky_horizon.z,
                     e->ambient_intensity, e->ground_color.x, e->ground_color.y, e->ground_color.z};
    if (dn) {
        bool big = false;
        for (int k = 0; k < 7; k++)
            if (fabsf(env[k] - G.dn_env_last[k]) > 0.15f * fabsf(G.dn_env_last[k]) + 0.002f) big = true;
        memcpy(G.dn_env_last, env, sizeof(float) * 7);
        if (!big) { tint = hash_floats(tint, env, 10); memset(env, 0, sizeof env); }
    }
    G.light_hash = hash_floats(h, env, 10);
    G.soft_hash = tint;
}

/* ---------------------------------------------------------------- new probes start from what's around them */
static void sky_sh_update(void) {
    float c[12] = {0};
    const int N = 64;
    for (int r = 0; r < N; r++) {
        float zz = 1.0f - (2.0f * r + 1.0f) / (float)N, rr = sqrtf(lx_maxf(0, 1 - zz * zz)), a = (float)r * 2.39996323f;
        LxVec3 d = lx_v3(rr * cosf(a), zz, rr * sinf(a));
        LxVec3 L = sky_radiance(d);
        float sh[4] = {SH_C0, SH_C1 * d.y, SH_C1 * d.z, SH_C1 * d.x};
        for (int b = 0; b < 4; b++) { c[b] += L.x * sh[b]; c[4 + b] += L.y * sh[b]; c[8 + b] += L.z * sh[b]; }
    }
    for (int b = 0; b < 12; b++) G.sky_sh[b] = c[b] * 4.0f * LX_PI / (float)N;
}
/* the coarser level's light at p (trilinear over its open probes); false: it isn't there */
static bool coarse_sh(const Level *C, LxVec3 p, float out[12]) {
    if (!C->on) return false;
    float gx = (p.x - C->origin.x) / C->pcell - 0.5f, gy = (p.y - C->origin.y) / C->pcell - 0.5f, gz = (p.z - C->origin.z) / C->pcell - 0.5f;
    int x = (int)floorf(gx), y = (int)floorf(gy), z = (int)floorf(gz);
    if (x < 0 || y < 0 || z < 0 || x >= C->pres[0] - 1 || y >= C->pres[1] - 1 || z >= C->pres[2] - 1) return false;
    float f[3] = {gx - x, gy - y, gz - z}, sum = 0;
    memset(out, 0, sizeof(float) * 12);
    for (int k = 0; k < 8; k++) {
        int i = pidx(C, x + (k & 1), y + ((k >> 1) & 1), z + (k >> 2));
        if (!C->cls[i] || C->fresh[i]) continue;
        float w = ((k & 1) ? f[0] : 1 - f[0]) * (((k >> 1) & 1) ? f[1] : 1 - f[1]) * ((k >> 2) ? f[2] : 1 - f[2]);
        sum += w;
        for (int q = 0; q < 12; q++) out[q] += C->sh[i * 12 + q] * w;
    }
    if (sum < 1e-4f) return false;
    for (int q = 0; q < 12; q++) out[q] /= sum;
    return true;
}
/* probes that just came into a level (the camera moved, or the level is new) start from the coarser level's light
 * there - or the open sky's - instead of black, so nothing goes dark while they are traced (nearest first) */
static void seed_fresh(int lev) {
    Level *L = &G.lv[lev];
    if (!L->on) return;
    const Level *C = lev + 1 < NLEV ? &G.lv[lev + 1] : NULL;
    for (int i = 0; i < L->np; i++) {
        if (!L->fresh[i]) continue;
        L->fresh[i] = 0;
        int x = i % L->pres[0], y = (i / L->pres[0]) % L->pres[1], z = i / (L->pres[0] * L->pres[1]);
        LxVec3 o = lx_add3(L->origin, lx_v3((x + 0.5f) * L->pcell, (y + 0.5f) * L->pcell, (z + 0.5f) * L->pcell));
        float c[12];
        if (!(C && coarse_sh(C, o, c))) memcpy(c, G.sky_sh, sizeof c);
        memcpy(&L->sh[i * 12], c, sizeof c);
        L->hist[i] = 0;
        L->unlit = true;
    }
    L->version++;
}

/* ---------------------------------------------------------------- per frame */
static void publish(void) {
    LxGiVolume *v = &G.volume;
    const Level *F = &G.lv[NLEV - 1];
    v->valid = F->on;
    v->origin = F->origin;
    v->cell_size = lx_v3s(F->pcell);
    for (int a = 0; a < 3; a++) v->res[a] = F->pres[a];
    v->sh = F->sh;
    v->vis = F->vis;
    v->version = F->version;
    v->vis_version = F->vis_version;
    for (int l = 0; l < LX_GI_CASCADES; l++) {
        const Level *L = &G.lv[l];
        v->casc[l].on = L->on;
        v->casc[l].origin = L->origin;
        v->casc[l].cell = L->pcell;
        v->casc[l].res = L->pres[0];
        v->casc[l].sh = L->sh;
        v->casc[l].vis = L->vis;
        v->casc[l].version = L->version;
        v->casc[l].vis_version = L->vis_version;
        v->casc[l].sdf = L->sdf;
        v->casc[l].sdf_version = L->sdf_version;
    }
}

void lx_gi_update(LxScene *s, const LxGraphicsSettings *g, LxVec3 eye) {
    double t0 = lx_time();
    G.rays_last = 0;
    if (!g->gi_enabled || !s) { G.volume.valid = false; G.ms_last = 0; return; }
    if (!isfinite(eye.x) || !isfinite(eye.y) || !isfinite(eye.z)) eye = lx_v3(0, 0, 0);
    lx_scene_update_transforms(s);
    /* geometry */
    bool rebuilt = false;
    if (G.built_scene != s || G.built_version != s->static_version) {
        lx_bvh_build_gi(&G.bvh, s);
        G.built_scene = s;
        G.built_version = s->static_version;
        /* the same geometry (Play makes a copy of the scene): keep every level and its light */
        uint64_t h = 1469598103934665603ull ^ (uint64_t)G.bvh.tri_count;
        h = hash_floats(h, (const float *)G.bvh.v0, G.bvh.tri_count * 3);
        h = hash_floats(h, (const float *)G.bvh.e1, G.bvh.tri_count * 3);
        h = hash_floats(h, (const float *)G.bvh.e2, G.bvh.tri_count * 3);
        for (int m = 0; m < G.bvh.material_count; m++)
            h = hash_floats(h, (const float *)&G.bvh.materials[m], 6);
        rebuilt = h != G.geo_hash;
        G.geo_hash = h;
    }
    lx_terrain_query_prepare(s, &G.tq);
    uint64_t tsig = lx_terrain_signature(s);
    if (tsig != G.terrain_sig) { G.terrain_sig = tsig; rebuilt = true; }   /* sculpted, grown, moved: build again */
    lx_env_effective(s, &G.env);
    sky_sh_update();
    /* levels: probe count per axis from the setting; the finest covers ~12 m, each next one twice (the far one 3x) */
    int P = lx_clampi(g->gi_probe_resolution, 8, 48) & ~1;
    float base = 12.0f / (float)P;
    if (P != G.probe_res) {
        for (int l = 0; l < NLEV; l++) G.lv[l].on = false;
        G.probe_res = P;
        G.stale = 0;
    } else if (rebuilt) G.stale = (1u << NLEV) - 1;
    /* new geometry: build the levels again one per frame, the finest first (all at once stalls a frame), the
     * others keep their light until their turn */
    for (int l = 0; l < NLEV; l++)
        if (G.stale & (1u << l)) { G.lv[l].on = false; G.stale &= ~(1u << l); break; }
    static const float SCALE[NLEV] = {1, 2, 4};
    for (int l = NLEV - 1; l >= 0; l--) level_place(&G.lv[l], l, eye, P, base * SCALE[l]);
    for (int l = NLEV - 1; l >= 0; l--) {   /* coarse first: finer levels seed their new probes from it */
        level_build(&G.lv[l]);
        seed_fresh(l);
    }
    if (!G.bvh.tri_count && !G.tq.n) {   /* nothing to bounce off: sky only, the shader's ambient does that */
        G.volume.valid = false;
        G.ms_last = (float)((lx_time() - t0) * 1000.0);
        return;
    }
    /* lights and what changed */
    gather_lights(s);
    {
        const uint8_t *gb = (const uint8_t *)g;
        uint64_t h = G.light_hash;
        for (size_t i = 0; i < sizeof *g; i++) h = (h ^ gb[i]) * 1099511628211ull;
        G.light_hash = h;
    }
    bool hard = G.light_hash != G.prev_light_hash, soft = G.soft_hash != G.prev_soft_hash;
    G.prev_light_hash = G.light_hash;
    G.prev_soft_hash = G.soft_hash;
    for (int l = 0; l < NLEV; l++) {
        Level *L = &G.lv[l];
        if (hard) {   /* new light: the old history would smear; start each probe again from its next update */
            for (int i = 0; i < L->np; i++) {
                if (L->hist[i] > 1) L->hist[i] = 1;
                L->need[i] = L->cls[i] == 0 ? 0 : L->cls[i] == 1 ? TARGET : TARGET_AIR;
            }
        } else if (soft) {
            for (int i = 0; i < L->np; i++) if (L->cls[i] && L->need[i] < 2) L->need[i] = 2;
        }
    }
    G.bounces = g->gi_max_bounces;
    G.alpha_min = lx_clampf(g->gi_responsiveness * 2.0f, 0.04f, 1.0f);
    G.rays_per_probe = lx_clampi(g->gi_rays_per_probe, 8, 256);
    G.frame++;
    /* the budget: rays this frame, the finest level first (it's what's on screen up close) */
    int budget = lx_maxi(g->gi_rays_per_frame, 1024);
    static const float SHARE[NLEV] = {0.45f, 0.3f, 0.25f};
    static int *pick;
    static int pick_cap;
    for (int l = 0; l < NLEV; l++) {
        Level *L = &G.lv[l];
        if (!L->on) continue;
        int rays = (int)(budget * SHARE[l]), spent = 0;
        if (pick_cap < L->np) { pick_cap = L->np; pick = lx_realloc(pick, sizeof(int) * (size_t)pick_cap); }
        int n = 0;
        /* probes that have never been traced come first (light everywhere soon, refined after) */
        for (int k = 0; k < L->np && spent < rays && L->unlit; k++) {
            int i = L->order[k];
            if (L->hist[i] || !L->need[i] || !L->cls[i]) continue;
            pick[n++] = i;
            L->stamp[i] = G.frame;
            spent += (L->cls[i] == 1 ? G.rays_per_probe : G.rays_per_probe / 2) * 2;
        }
        if (spent < rays) L->unlit = false;
        for (int scanned = 0; scanned < L->np && spent < rays; scanned++) {
            int i = L->rr;
            L->rr = (L->rr + 1) % L->np;
            if (!L->need[i] || L->stamp[i] == G.frame) continue;
            if (L->cls[i] == 0) { L->need[i] = 0; L->hist[i] = 0; continue; }
            pick[n++] = i;
            spent += L->cls[i] == 1 ? G.rays_per_probe : G.rays_per_probe / 2;
        }
        if (L->nnew) {   /* new surfaces get their light before any ray sees them (else it would remember them dark) */
            LightJob lj = {L, l, L->snew};
            lx_parallel_for(L->nnew, 128, light_job, &lj);
            L->nnew = 0;
        }
        if (!n && !L->feedback) continue;
        /* light the surfaces these rays will see: a share of the cache every frame while probes are updating, and
         * after they settle while the bounce light is still building up (each pass is another bounce: when the
         * surfaces' light still changes, the probes gather once more, until it stops changing) */
        if (L->nsurf) {
            int sn = lx_mini(L->nsurf, lx_maxi(L->nsurf / 3, 2048));
            static int *sidx;
            static float *before;
            static int scap;
            if (scap < sn) {
                scap = sn;
                sidx = lx_realloc(sidx, sizeof(int) * (size_t)scap);
                before = lx_realloc(before, sizeof(float) * (size_t)scap);
            }
            for (int k = 0; k < sn; k++) {
                sidx[k] = L->slist[L->srr];
                L->srr = (L->srr + 1) % L->nsurf;
                const float *r = &L->rad[sidx[k] * 3];
                before[k] = r[0] + r[1] + r[2];
            }
            LightJob lj = {L, l, sidx};
            lx_parallel_for(sn, 128, light_job, &lj);
            double change = 0, total = 0;
            for (int k = 0; k < sn; k++) {
                const float *r = &L->rad[sidx[k] * 3];
                float now = r[0] + r[1] + r[2];
                change += fabsf(now - before[k]);
                total += now;
            }
            bool moving = total > 0 && change > total * 0.01;
            if (n) L->feedback = G.bounces > 1;
            else if (moving && G.bounces > 1) {   /* another bounce: the open probes gather again */
                for (int i = 0; i < L->np; i++) if (L->cls[i] && L->need[i] < 2) L->need[i] = 2;
            } else L->feedback = false;
        } else L->feedback = false;
        if (!n) continue;
        ProbeJob pj = {L, l, pick, G.rays_per_probe, G.frame * 2654435761u + (uint32_t)l, 0};
        lx_parallel_for(n, 4, probe_job, &pj);
        fill_buried(L);
        L->version++;
        G.rays_last += spent;
    }
    publish();
    G.ms_last = (float)((lx_time() - t0) * 1000.0);
}

/* ---------------------------------------------------------------- what the shader does, on the CPU (tests) */
static bool vis_sees(const Level *L, int cx, int cy, int cz, LxVec3 g) {
    const float *v = L->vis + (size_t)pidx(L, cx, cy, cz) * 6;
    float d[3] = {g.x - (cx + 0.5f), g.y - (cy + 0.5f), g.z - (cz + 0.5f)};
    for (int a = 0; a < 3; a++) {
        float wall = d[a] >= 0 ? v[a * 2] : v[a * 2 + 1];
        if (wall < fabsf(d[a]) - 0.02f) return false;
    }
    return true;
}
/* the GI's L1 SH (red, green, blue; L0 y z x) at a point, as a surface facing n reads it: false outside every grid */
bool lx_gi_sh_at(LxVec3 p, LxVec3 n, bool walls, float out[12]) {
    for (int l = 0; l < NLEV; l++) {
        const Level *L = &G.lv[l];
        if (!L->on) continue;
        LxVec3 q = lx_madd3(p, n, L->pcell * 0.25f);
        LxVec3 g = lx_scale3(lx_sub3(q, L->origin), 1.0f / L->pcell);
        if (g.x < 1 || g.y < 1 || g.z < 1 || g.x > L->pres[0] - 1 || g.y > L->pres[1] - 1 || g.z > L->pres[2] - 1) continue;
        LxVec3 t = lx_v3(g.x - 0.5f, g.y - 0.5f, g.z - 0.5f);
        int b[3] = {(int)floorf(t.x), (int)floorf(t.y), (int)floorf(t.z)};
        float f[3] = {t.x - b[0], t.y - b[1], t.z - b[2]}, w[8], plain[8], sum = 0;
        for (int k = 0; k < 8; k++) {
            int x = b[0] + (k & 1), y = b[1] + ((k >> 1) & 1), z = b[2] + (k >> 2);
            float wt = ((k & 1) ? f[0] : 1 - f[0]) * (((k >> 1) & 1) ? f[1] : 1 - f[1]) * ((k >> 2) ? f[2] : 1 - f[2]);
            plain[k] = wt;
            w[k] = (!walls || vis_sees(L, x, y, z, g)) ? wt : 0;
            sum += w[k];
        }
        if (sum < 1e-6f) { memcpy(w, plain, sizeof w); sum = 1; }
        float c[12] = {0};
        for (int k = 0; k < 8; k++) {
            if (w[k] <= 0) continue;
            int i = pidx(L, b[0] + (k & 1), b[1] + ((k >> 1) & 1), b[2] + (k >> 2));
            for (int j = 0; j < 12; j++) c[j] += L->sh[i * 12 + j] * w[k] / sum;
        }
        memcpy(out, c, sizeof c);
        return true;
    }
    return false;
}

LxVec3 lx_gi_irradiance(LxVec3 p, LxVec3 n, bool walls) {
    for (int l = 0; l < NLEV; l++) {
        const Level *L = &G.lv[l];
        if (!L->on) continue;
        LxVec3 q = lx_madd3(p, n, L->pcell * 0.25f);
        LxVec3 g = lx_scale3(lx_sub3(q, L->origin), 1.0f / L->pcell);
        if (g.x < 1 || g.y < 1 || g.z < 1 || g.x > L->pres[0] - 1 || g.y > L->pres[1] - 1 || g.z > L->pres[2] - 1) continue;
        LxVec3 t = lx_v3(g.x - 0.5f, g.y - 0.5f, g.z - 0.5f);
        int b[3] = {(int)floorf(t.x), (int)floorf(t.y), (int)floorf(t.z)};
        float f[3] = {t.x - b[0], t.y - b[1], t.z - b[2]}, w[8], plain[8], sum = 0;
        for (int k = 0; k < 8; k++) {
            int x = b[0] + (k & 1), y = b[1] + ((k >> 1) & 1), z = b[2] + (k >> 2);
            float wt = ((k & 1) ? f[0] : 1 - f[0]) * (((k >> 1) & 1) ? f[1] : 1 - f[1]) * ((k >> 2) ? f[2] : 1 - f[2]);
            plain[k] = wt;
            w[k] = (!walls || vis_sees(L, x, y, z, g)) ? wt : 0;
            sum += w[k];
        }
        if (sum < 1e-6f) { memcpy(w, plain, sizeof w); sum = 1; }
        float c[12] = {0};
        for (int k = 0; k < 8; k++) {
            if (w[k] <= 0) continue;
            int i = pidx(L, b[0] + (k & 1), b[1] + ((k >> 1) & 1), b[2] + (k >> 2));
            for (int j = 0; j < 12; j++) c[j] += L->sh[i * 12 + j] * w[k] / sum;
        }
        return lx_v3(lx_gi_eval_l1(c, n), lx_gi_eval_l1(c + 4, n), lx_gi_eval_l1(c + 8, n));
    }
    return lx_v3(0, 0, 0);
}

/* Irradiance from one channel's L1 SH (L0, y, z, x): Geomerics' non-negative reconstruction (as the shader's
 * gi_eval_l1). The plain linear form goes negative, and is clamped to black, facing away from strongly directional
 * light; this keeps the same average and main direction, never goes negative and is exact for light from one
 * direction. */
float lx_gi_eval_l1(const float *c, LxVec3 n) {
    float a = LX_PI * 0.282095f * c[0];
    if (a <= 0) return 0;
    LxVec3 b = lx_scale3(lx_v3(c[3], c[1], c[2]), (2.0f * LX_PI / 3.0f) * 0.488603f);
    float lb = lx_len3(b);
    if (lb < 1e-6f * a) return a;
    float rho = lx_minf(lb / (2.0f * a), 1.0f);
    float q = 0.5f * (1.0f + lx_dot3(lx_scale3(b, 1.0f / lb), n));
    float p = 1.0f + 2.0f * rho;
    float k = (1.0f - rho) / (1.0f + rho);
    return a * (k + (1.0f - k) * (p + 1.0f) * powf(lx_maxf(q, 0.0f), p));
}

/* the shader's distance-field occlusion (gi_dfao), on the CPU (tests) */
float lx_gi_dfao(LxVec3 p, LxVec3 n) {
    int level = -1;
    for (int l = 0; l < LX_GI_CASCADES && level < 0; l++) {
        const Level *L = &G.lv[l];
        if (!L->on) continue;
        LxVec3 g = lx_scale3(lx_sub3(p, L->origin), 1.0f / L->pcell);
        if (g.x > 1.5f && g.y > 1.5f && g.z > 1.5f && g.x < L->pres[0] - 1.5f && g.y < L->pres[1] - 1.5f && g.z < L->pres[2] - 1.5f) level = l;
    }
    if (level < 0) return 1.0f;
    const Level *L = &G.lv[level];
    float occ = 0, wsum = 0;
    for (int i = 1; i <= 4; i++) {
        float d = L->scell * (0.6f + 0.9f * (float)i);
        float s = lx_clampf(sdf_at(L, lx_madd3(p, n, d)), -3.0f * L->scell, 3.0f * L->scell);
        float w = 1.0f / (float)i;
        occ += w * lx_clampf((d - s - 0.4f * L->scell) / d, 0, 1);   /* a little slack: the field is only half a cell fine */
        wsum += w;
    }
    return lx_clampf(1.0f - 0.7f * occ / wsum, 0.3f, 1.0f);
}
