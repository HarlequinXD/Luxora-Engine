/* Reflection cards: small snapshots of every object from its six sides, so traced reflections can show what an
 * object really looks like (its colours and texture), on screen or not.
 *
 * For each mesh + material pair the object's box is looked at straight on from +X, -X, +Y, -Y, +Z and -Z. Each
 * view is a 32 x 32 card holding the surface colour (linear albedo, written as sRGB) and how far into the box
 * the surface starts (depth, 0 at that face of the box, 254 at the far face, 255 = nothing there). The cards are
 * drawn here on the CPU - a tiny rasteriser, the object's coarsest LOD and a 64 x 64 copy of its colour map - a
 * few objects a frame, and packed into one 2048 x 2048 atlas (six tiles per object) that the renderer uploads.
 *
 * Reflection rays find an object through the renderer's grid of nearby objects, step through its box, and are
 * inside it where all three pairs of opposite cards agree (the visual hull: in front of the far surface and
 * behind the near one, along every axis). The hit takes the colour of the card facing it and is lit there with
 * the sun, its shadow and the Live GI. lx_cards_trace_box is the same test on the CPU, for the tests. */
#include "../lx_internal.h"

#define R_ LX_CARD_RES
#define TILES_X (LX_CARD_ATLAS / LX_CARD_RES)
#define MAX_SETS ((TILES_X * TILES_X) / 6)
#define SMALL_TEX 64

typedef struct CardSet {
    const LxMesh *mesh;
    const LxMaterial *mat;
    uint64_t hash;       /* what the cards were drawn from: when it changes they're drawn again */
    int tile;            /* first of six tiles, -1 = free */
    bool ready, queued;
    uint32_t used;       /* the frame it was last asked for */
} CardSet;

typedef struct SmallTex { char ref[LX_REF_MAX]; uint8_t px[SMALL_TEX * SMALL_TEX * 4]; bool ok; } SmallTex;

static struct {
    CardSet sets[MAX_SETS];
    int count;
    uint8_t *albedo;     /* the atlas on the CPU: RGBA8 sRGB, and depth R8 */
    uint8_t *depth;
    int *dirty;          /* tiles to upload */
    int ndirty, cdirty;
    SmallTex *small;     /* colour maps shrunk to 64 x 64 */
    int nsmall, csmall;
    uint32_t frame;
} C;

static void ensure_atlas(void) {
    if (C.albedo) return;
    C.albedo = lx_calloc((size_t)LX_CARD_ATLAS * LX_CARD_ATLAS, 4);
    C.depth = lx_alloc((size_t)LX_CARD_ATLAS * LX_CARD_ATLAS);
    memset(C.depth, 255, (size_t)LX_CARD_ATLAS * LX_CARD_ATLAS);
    for (int i = 0; i < MAX_SETS; i++) C.sets[i].tile = -1;
}

void lx_cards_reset(void) {
    lx_free(C.albedo);
    lx_free(C.depth);
    lx_free(C.dirty);
    lx_free(C.small);
    memset(&C, 0, sizeof C);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static uint64_t mat_hash(const LxMesh *m, const LxMaterial *mat) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &mat->albedo, sizeof mat->albedo);
    h = fnv(h, mat->albedo_map, strlen(mat->albedo_map));
    h = fnv(h, &mat->uv_scale, sizeof mat->uv_scale);
    h = fnv(h, &mat->blend_mode, sizeof mat->blend_mode);
    h = fnv(h, &mat->alpha_cutoff, sizeof mat->alpha_cutoff);
    h = fnv(h, &m->bounds, sizeof m->bounds);
    int vc = m->lods[0].vertex_count;
    h = fnv(h, &vc, sizeof vc);
    return h;
}

/* which objects get cards: solid or cut-out, not terrain (it's a height field the GI already knows) */
bool lx_cards_wanted(const LxMesh *m, const LxMaterial *mat) {
    if (!m || !mat || m->lods[0].index_count < 3 || m->dynamic) return false;
    if (lx_blend_see_through(mat->blend_mode) || lx_terrain_gpu_layers(mat)) return false;
    return true;
}

/* the box the cards look at: the mesh's bounds, never flatter than 2 % of its size (a plane still has a thickness) */
void lx_cards_box(const LxMesh *m, LxVec3 *mn, LxVec3 *ext) {
    LxVec3 lo = m->bounds.min, hi = m->bounds.max;
    LxVec3 e = lx_sub3(hi, lo);
    float big = lx_maxf(lx_maxf(e.x, e.y), e.z), least = lx_maxf(big * 0.02f, 0.01f);
    for (int a = 0; a < 3; a++) {
        float *ea = a == 0 ? &e.x : a == 1 ? &e.y : &e.z, *la = a == 0 ? &lo.x : a == 1 ? &lo.y : &lo.z;
        if (*ea < least) { *la -= (least - *ea) * 0.5f; *ea = least; }
    }
    *mn = lo;
    *ext = e;
}

/* face f (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z): a point q in the box (0..1) as the card's u, v and depth */
static void face_uvd(int f, LxVec3 q, float *u, float *v, float *d) {
    int a = f >> 1;
    float qa = a == 0 ? q.x : a == 1 ? q.y : q.z;
    if (a == 0) { *u = q.z; *v = q.y; }
    else if (a == 1) { *u = q.x; *v = q.z; }
    else { *u = q.x; *v = q.y; }
    *d = (f & 1) ? qa : 1.0f - qa;
}

static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static uint8_t lin_to_srgb8(float c) {
    c = lx_clampf(c, 0.0f, 1.0f);
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(s * 255.0f + 0.5f);
}

/* a colour map shrunk to 64 x 64 (box filter): a card is too small to want more, and it stops the sparkle */
static const SmallTex *small_tex(const char *ref) {
    if (!ref || !ref[0]) return NULL;
    for (int i = 0; i < C.nsmall; i++) if (!strcmp(C.small[i].ref, ref)) return C.small[i].ok ? &C.small[i] : NULL;
    if (C.nsmall == C.csmall) { C.csmall = C.csmall ? C.csmall * 2 : 16; C.small = lx_realloc(C.small, sizeof(SmallTex) * (size_t)C.csmall); }
    SmallTex *s = &C.small[C.nsmall++];
    memset(s, 0, sizeof *s);
    lx_strcpy(s->ref, sizeof s->ref, ref);
    LxTexture *t = lx_asset_texture(ref);
    if (!t || !t->pixels || t->width <= 0 || t->height <= 0) return NULL;
    for (int y = 0; y < SMALL_TEX; y++)
        for (int x = 0; x < SMALL_TEX; x++) {
            int x0 = x * t->width / SMALL_TEX, x1 = lx_maxf((float)((x + 1) * t->width / SMALL_TEX), (float)(x0 + 1));
            int y0 = y * t->height / SMALL_TEX, y1 = lx_maxf((float)((y + 1) * t->height / SMALL_TEX), (float)(y0 + 1));
            float acc[4] = {0};
            int n = 0, step = (x1 - x0) * (y1 - y0) > 256 ? 2 : 1;
            for (int yy = y0; yy < y1; yy += step)
                for (int xx = x0; xx < x1; xx += step) {
                    const uint8_t *p = t->pixels + ((size_t)yy * (size_t)t->width + (size_t)xx) * 4;
                    acc[0] += srgb_to_lin(p[0] / 255.0f); acc[1] += srgb_to_lin(p[1] / 255.0f); acc[2] += srgb_to_lin(p[2] / 255.0f);
                    acc[3] += p[3] / 255.0f;
                    n++;
                }
            uint8_t *o = s->px + ((size_t)y * SMALL_TEX + (size_t)x) * 4;
            for (int c = 0; c < 3; c++) o[c] = lin_to_srgb8(acc[c] / (float)n);
            o[3] = (uint8_t)(acc[3] / (float)n * 255.0f + 0.5f);
        }
    s->ok = true;
    return s;
}
/* linear colour and alpha of a shrunk map at uv (wrapped, bilinear) */
static void sample_small(const SmallTex *s, float u, float v, float out[4]) {
    u = u - floorf(u);
    v = v - floorf(v);
    float x = u * SMALL_TEX - 0.5f, y = v * SMALL_TEX - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    for (int c = 0; c < 4; c++) out[c] = 0;
    for (int k = 0; k < 4; k++) {
        int xx = ((x0 + (k & 1)) % SMALL_TEX + SMALL_TEX) % SMALL_TEX, yy = ((y0 + (k >> 1)) % SMALL_TEX + SMALL_TEX) % SMALL_TEX;
        float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
        const uint8_t *p = s->px + ((size_t)yy * SMALL_TEX + (size_t)xx) * 4;
        out[0] += srgb_to_lin(p[0] / 255.0f) * w;
        out[1] += srgb_to_lin(p[1] / 255.0f) * w;
        out[2] += srgb_to_lin(p[2] / 255.0f) * w;
        out[3] += p[3] / 255.0f * w;
    }
}

/* Draws the six cards of a mesh + material: albedo (RGBA8, sRGB, alpha = covered) and depth (R8, 255 = empty),
 * each R x R, one after the other. */
void lx_cards_capture(const LxMesh *m, const LxMaterial *mat, uint8_t *albedo, uint8_t *depth) {
    const int N = R_ * R_;
    memset(albedo, 0, (size_t)N * 6 * 4);
    memset(depth, 255, (size_t)N * 6);
    float *zb = lx_alloc(sizeof(float) * (size_t)N * 6);
    for (int i = 0; i < N * 6; i++) zb[i] = 2.0f;
    LxVec3 mn, ext;
    lx_cards_box(m, &mn, &ext);
    /* the coarsest LOD that still has a fair share of the triangles: enough for a 32 x 32 card */
    const LxMeshLod *L = &m->lods[0];
    for (int l = m->lod_count - 1; l > 0; l--)
        if (m->lods[l].index_count >= 3 && m->lods[l].index_count * 4 >= m->lods[0].index_count) { L = &m->lods[l]; break; }
    const SmallTex *tex = small_tex(mat->albedo_map);
    float us = mat->uv_scale > 0 ? mat->uv_scale : 1.0f;
    bool cut = mat->blend_mode == LX_BLEND_CUTOUT;
    for (int t = 0; t + 2 < L->index_count; t += 3) {
        const LxVertex *v[3] = {&L->vertices[L->indices[t]], &L->vertices[L->indices[t + 1]], &L->vertices[L->indices[t + 2]]};
        LxVec3 q[3];
        for (int k = 0; k < 3; k++)
            q[k] = lx_v3((v[k]->px - mn.x) / ext.x, (v[k]->py - mn.y) / ext.y, (v[k]->pz - mn.z) / ext.z);
        for (int f = 0; f < 6; f++) {
            float pu[3], pv[3], pd[3];
            for (int k = 0; k < 3; k++) face_uvd(f, q[k], &pu[k], &pv[k], &pd[k]);
            float x[3], y[3];
            for (int k = 0; k < 3; k++) { x[k] = pu[k] * R_; y[k] = pv[k] * R_; }
            float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
            if (fabsf(area) < 1.0f) {
                /* edge on (or thinner than a texel): its edges are drawn as lines, so a wall or a plane seen side on
                 * still shows as a line on this card rather than nothing */
                for (int e = 0; e < 3; e++) {
                    int e2 = (e + 1) % 3;
                    float len = lx_maxf(fabsf(x[e2] - x[e]), fabsf(y[e2] - y[e]));
                    int n = (int)ceilf(len * 2.0f) + 1;
                    for (int k = 0; k <= n; k++) {
                        float w = (float)k / (float)n;
                        int ix = (int)(x[e] + (x[e2] - x[e]) * w), iy = (int)(y[e] + (y[e2] - y[e]) * w);
                        if (ix < 0 || iy < 0 || ix >= R_ || iy >= R_) continue;
                        float dd = pd[e] + (pd[e2] - pd[e]) * w;
                        int at = f * N + iy * R_ + ix;
                        if (dd >= zb[at]) continue;
                        float col[4] = {1, 1, 1, 1};
                        if (tex) sample_small(tex, (v[e]->u + (v[e2]->u - v[e]->u) * w) * us, (v[e]->v + (v[e2]->v - v[e]->v) * w) * us, col);
                        if (cut && col[3] * mat->albedo.w < mat->alpha_cutoff) continue;
                        zb[at] = dd;
                        uint8_t *o = albedo + (size_t)at * 4;
                        o[0] = lin_to_srgb8(col[0] * mat->albedo.x);
                        o[1] = lin_to_srgb8(col[1] * mat->albedo.y);
                        o[2] = lin_to_srgb8(col[2] * mat->albedo.z);
                        o[3] = 255;
                        depth[at] = (uint8_t)(lx_clampf(dd, 0.0f, 1.0f) * 254.0f + 0.5f);
                    }
                }
                if (fabsf(area) < 1e-9f) continue;
            }
            int ix0 = (int)lx_maxf(0, floorf(lx_minf(x[0], lx_minf(x[1], x[2])))), ix1 = (int)lx_minf(R_ - 1, ceilf(lx_maxf(x[0], lx_maxf(x[1], x[2]))));
            int iy0 = (int)lx_maxf(0, floorf(lx_minf(y[0], lx_minf(y[1], y[2])))), iy1 = (int)lx_minf(R_ - 1, ceilf(lx_maxf(y[0], lx_maxf(y[1], y[2]))));
            for (int iy = iy0; iy <= iy1; iy++)
                for (int ix = ix0; ix <= ix1; ix++) {
                    float px = (float)ix + 0.5f, py = (float)iy + 0.5f;
                    float w0 = ((x[1] - px) * (y[2] - py) - (x[2] - px) * (y[1] - py)) / area;
                    float w1 = ((x[2] - px) * (y[0] - py) - (x[0] - px) * (y[2] - py)) / area;
                    float w2 = 1.0f - w0 - w1;
                    const float E = -1e-4f;
                    if (w0 < E || w1 < E || w2 < E) continue;
                    float d = w0 * pd[0] + w1 * pd[1] + w2 * pd[2];
                    int at = f * N + iy * R_ + ix;
                    if (d >= zb[at]) continue;
                    float col[4] = {1, 1, 1, 1};
                    if (tex) {
                        float tu = (w0 * v[0]->u + w1 * v[1]->u + w2 * v[2]->u) * us, tv = (w0 * v[0]->v + w1 * v[1]->v + w2 * v[2]->v) * us;
                        sample_small(tex, tu, tv, col);
                    }
                    if (cut && col[3] * mat->albedo.w < mat->alpha_cutoff) continue;   /* the cut-out part isn't there */
                    zb[at] = d;
                    uint8_t *o = albedo + (size_t)at * 4;
                    o[0] = lin_to_srgb8(col[0] * mat->albedo.x);
                    o[1] = lin_to_srgb8(col[1] * mat->albedo.y);
                    o[2] = lin_to_srgb8(col[2] * mat->albedo.z);
                    o[3] = 255;
                    depth[at] = (uint8_t)(lx_clampf(d, 0.0f, 1.0f) * 254.0f + 0.5f);
                }
        }
    }
    /* colour bleeds one texel into the empty space around each shape, so filtering at an edge isn't black */
    for (int f = 0; f < 6; f++)
        for (int iy = 0; iy < R_; iy++)
            for (int ix = 0; ix < R_; ix++) {
                int at = f * N + iy * R_ + ix;
                if (albedo[at * 4 + 3]) continue;
                int acc[3] = {0}, n = 0;
                for (int k = 0; k < 4; k++) {
                    int nx = ix + (k == 0) - (k == 1), ny = iy + (k == 2) - (k == 3);
                    if (nx < 0 || ny < 0 || nx >= R_ || ny >= R_) continue;
                    const uint8_t *p = albedo + (size_t)(f * N + ny * R_ + nx) * 4;
                    if (p[3] != 255) continue;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                    n++;
                }
                if (n) for (int c = 0; c < 3; c++) albedo[at * 4 + c] = (uint8_t)(acc[c] / n);
            }
    lx_free(zb);
}

/* where tile t lives in the atlas */
static void tile_xy(int t, int *x, int *y) { *x = (t % TILES_X) * R_; *y = (t / TILES_X) * R_; }

static void mark_dirty(int tile) {
    if (C.ndirty == C.cdirty) { C.cdirty = C.cdirty ? C.cdirty * 2 : 256; C.dirty = lx_realloc(C.dirty, sizeof(int) * (size_t)C.cdirty); }
    C.dirty[C.ndirty++] = tile;
}

static void store(CardSet *cs) {
    static uint8_t a[R_ * R_ * 6 * 4], d[R_ * R_ * 6];
    lx_cards_capture(cs->mesh, cs->mat, a, d);
    for (int f = 0; f < 6; f++) {
        int tx, ty, tile = cs->tile + f;
        tile_xy(tile, &tx, &ty);
        for (int y = 0; y < R_; y++) {
            memcpy(C.albedo + ((size_t)(ty + y) * LX_CARD_ATLAS + (size_t)tx) * 4, a + ((size_t)f * R_ * R_ + (size_t)y * R_) * 4, R_ * 4);
            memcpy(C.depth + (size_t)(ty + y) * LX_CARD_ATLAS + (size_t)tx, d + (size_t)f * R_ * R_ + (size_t)y * R_, R_);
        }
        mark_dirty(tile);
    }
    cs->ready = true;
    cs->queued = false;
}

void lx_cards_begin_frame(uint32_t frame) { C.frame = frame; }

/* the first of an object's six tiles, or -1 while its cards aren't drawn yet (they're queued) */
int lx_cards_lookup(const LxMesh *m, const LxMaterial *mat) {
    if (!lx_cards_wanted(m, mat)) return -1;
    ensure_atlas();
    uint64_t h = mat_hash(m, mat);
    CardSet *free_set = NULL, *oldest = NULL;
    for (int i = 0; i < C.count; i++) {
        CardSet *s = &C.sets[i];
        if (s->mesh == m && s->mat == mat) {
            s->used = C.frame;
            if (s->hash != h) { s->hash = h; s->ready = false; s->queued = true; }   /* the material changed: draw again */
            return s->ready ? s->tile : -1;
        }
        if (!s->mesh && !free_set) free_set = s;
        if (s->mesh && C.frame - s->used > 120 && (!oldest || s->used < oldest->used)) oldest = s;
    }
    CardSet *s = free_set;
    if (!s && C.count < MAX_SETS) { s = &C.sets[C.count]; s->tile = C.count * 6; C.count++; }
    if (!s) s = oldest;   /* full: the one unseen the longest gives its tiles up */
    if (!s) return -1;
    int tile = s->tile;
    memset(s, 0, sizeof *s);
    s->tile = tile;
    s->mesh = m;
    s->mat = mat;
    s->hash = h;
    s->used = C.frame;
    s->queued = true;
    return -1;
}

/* draws up to `budget` queued objects' cards (a few a frame, so a new level doesn't stall) */
int lx_cards_capture_pending(int budget) {
    int done = 0;
    for (int i = 0; i < C.count && done < budget; i++)
        if (C.sets[i].mesh && C.sets[i].queued) { store(&C.sets[i]); done++; }
    return done;
}

/* a mesh is going away: its cards go too */
void lx_cards_forget_mesh(const LxMesh *m) {
    for (int i = 0; i < C.count; i++)
        if (C.sets[i].mesh == m) { int t = C.sets[i].tile; memset(&C.sets[i], 0, sizeof C.sets[i]); C.sets[i].tile = t; }
}

int lx_cards_take_dirty(int *tiles, int cap) {
    int n = C.ndirty < cap ? C.ndirty : cap;
    memcpy(tiles, C.dirty, sizeof(int) * (size_t)n);
    memmove(C.dirty, C.dirty + n, sizeof(int) * (size_t)(C.ndirty - n));
    C.ndirty -= n;
    return n;
}
void lx_cards_tile_pixels(int tile, uint8_t *albedo, uint8_t *depth) {
    int tx, ty;
    tile_xy(tile, &tx, &ty);
    for (int y = 0; y < R_; y++) {
        memcpy(albedo + (size_t)y * R_ * 4, C.albedo + ((size_t)(ty + y) * LX_CARD_ATLAS + (size_t)tx) * 4, R_ * 4);
        memcpy(depth + (size_t)y * R_, C.depth + (size_t)(ty + y) * LX_CARD_ATLAS + (size_t)tx, R_);
    }
}

/* ---- the trace, as the shader does it (tests) ----
 * o, d in box space (0..1 is the box); the six cards one after the other as lx_cards_capture writes them. Returns
 * the distance along d to the surface (d's length = 1 unit of t), or -1. */
static float card_depth(const uint8_t *depth, int f, float u, float v) {
    int x = (int)lx_clampf(u * R_, 0, R_ - 1), y = (int)lx_clampf(v * R_, 0, R_ - 1);
    uint8_t d = depth[f * R_ * R_ + y * R_ + x];
    return d == 255 ? 2.0f : d / 254.0f;
}
static bool hull_inside(const uint8_t *depth, LxVec3 q, int *face) {
    float best = 1e9f;
    for (int a = 0; a < 3; a++) {
        float u, v, dd, qa = a == 0 ? q.x : a == 1 ? q.y : q.z;
        face_uvd(a * 2, q, &u, &v, &dd);
        float front = card_depth(depth, a * 2, u, v), back = card_depth(depth, a * 2 + 1, u, v);
        const float SLACK = 1.0f / R_;   /* a texel's grace: thin things (a plane, a pole) have something to hit */
        float hi = 1.0f - front + SLACK, lo = back - SLACK;
        if (qa > hi || qa < lo) return false;
        if (hi - qa < best) { best = hi - qa; *face = a * 2; }
        if (qa - lo < best) { best = qa - lo; *face = a * 2 + 1; }
    }
    return true;
}
float lx_cards_trace_box(const uint8_t *albedo, const uint8_t *depth, LxVec3 o, LxVec3 d, int steps, uint8_t rgb_out[3], int *face_out) {
    float t0 = 0, t1 = 1e9f;
    for (int a = 0; a < 3; a++) {
        float oa = a == 0 ? o.x : a == 1 ? o.y : o.z, da = a == 0 ? d.x : a == 1 ? d.y : d.z;
        if (fabsf(da) < 1e-8f) { if (oa < 0 || oa > 1) return -1; continue; }
        float ta = (0 - oa) / da, tb = (1 - oa) / da;
        if (ta > tb) { float s = ta; ta = tb; tb = s; }
        t0 = lx_maxf(t0, ta);
        t1 = lx_minf(t1, tb);
    }
    if (t0 > t1) return -1;
    int face = 0;
    /* starting inside the object (the surface it reflects from): ignored until the ray is out of it */
    bool started_inside = t0 <= 0 && hull_inside(depth, o, &face);
    float prev = t0;
    for (int i = t0 > 0 ? 0 : 1; i <= steps; i++) {
        float t = t0 + (t1 - t0) * (float)i / (float)steps;
        bool in = hull_inside(depth, lx_madd3(o, d, t), &face);
        if (started_inside) { if (!in) started_inside = false; prev = t; continue; }   /* leaving the object it starts in */
        if (in && i == 0) {   /* it meets the object right at its box */
            prev = t;
        }
        if (in) {
            float lo = prev, hi = t;
            for (int k = 0; k < 5; k++) {
                float mid = 0.5f * (lo + hi);
                if (hull_inside(depth, lx_madd3(o, d, mid), &face)) hi = mid; else lo = mid;
            }
            hull_inside(depth, lx_madd3(o, d, hi), &face);
            LxVec3 q = lx_madd3(o, d, hi);
            float u, v, dd;
            face_uvd(face, q, &u, &v, &dd);
            int x = (int)lx_clampf(u * R_, 0, R_ - 1), y = (int)lx_clampf(v * R_, 0, R_ - 1);
            const uint8_t *p = albedo + ((size_t)face * R_ * R_ + (size_t)y * R_ + (size_t)x) * 4;
            if (rgb_out) { rgb_out[0] = p[0]; rgb_out[1] = p[1]; rgb_out[2] = p[2]; }
            if (face_out) *face_out = face;
            return hi;
        }
        prev = t;
    }
    return -1;
}
