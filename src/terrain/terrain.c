/* Terrain: one object made of square chunks that join seamlessly.
 *
 * Data. A terrain's chunks live in its folder under Assets: an index (terrain.lxterrain: every chunk's place, height
 * range and a 17x17 simplified copy of its heights and paint) and one file per chunk (c_<x>_<z>.lxchunk: its heights
 * per edit layer, paint weights and holes). The data is shared by every scene that shows the terrain (the editor's
 * scene and the copy that plays), keyed by the folder.
 *
 * Seams. Chunks of R height points share their edge points with their neighbours: point (gx, gz) of the terrain's
 * global lattice is stored in every chunk whose edge or inside holds it (up to four). Every edit writes all copies
 * of a point the same, so neighbouring chunks can never disagree.
 *
 * Objects. In each scene the terrain object gets a child object per chunk (made here, never saved, hidden from the
 * hierarchy): a mesh with four levels of detail and skirts that hide the cracks between levels, the terrain
 * material, and a mesh collider. So shadows, GI, picking, physics, sound and the weather's roof map see the
 * terrain like any other static object. Chunks near the focus (the player, or the editor's camera) are loaded in
 * full; the rest are drawn from their simplified copy, without collision.
 *
 * Painting. Six layers. Each vertex carries its blend weights in channels terrain doesn't otherwise use (uv: layers
 * 0-1, uv2: 2-3, tangent.w: 4, the rest is layer 5); layers can also paint themselves by slope and height. The
 * shader (pbr_body.glsl, MF_TERRAIN) blends the layers' textures by height, triplanar on cliffs.
 *
 * Undo. Every stroke keeps the chunks it touched as they were before and after, numbered by revision; the terrain
 * component saves the revision it shows, so the editor's ordinary undo (which restores the scene's text) walks the
 * terrain backwards and forwards too.
 */
#include "../lx_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAR 17                    /* the simplified copy: 17 x 17 points per chunk */
#define NL LX_TERRAIN_LAYERS
#define NE LX_TERRAIN_EDIT_LAYERS
#define HIST_BYTES (384u << 20)   /* undo memory */
#define TEX 512                   /* layer textures: texels along an edge */

typedef struct Chunk {
    int cx, cz;
    bool loaded;                  /* full data in memory */
    bool dirty_file, dirty_mesh, dirty_far, dirty_grass, dirty_scatter;
    float *layer[NE];             /* 0: the Base's heights; others: what each edit layer adds (NULL = nothing yet) */
    float *h;                     /* what shows: the Base plus every visible edit layer */
    uint8_t *w;                   /* R*R*NL paint weights, each point's summing to 255 */
    uint8_t *holes;               /* (R-1)^2 cells cut out */
    float far_h[FAR * FAR];
    uint8_t far_w[FAR * FAR * NL];
    float hmin, hmax;
    LxMesh *mesh, *far_mesh, *grass;
    uint32_t serial;              /* changes with its meshes */
} Chunk;

typedef struct Snap {             /* a chunk as it was (exists = false: there was none) */
    int cx, cz;
    bool exists;
    float *layer[NE];
    uint8_t *w, *holes;
    size_t bytes;
} Snap;
typedef struct Hist {
    uint32_t rev;                 /* the state after it */
    Snap *before, *after;
    int n;
    size_t bytes;
} Hist;

typedef struct TData {
    char folder[LX_REF_MAX];
    int id;                       /* in "terrain:<id>/..." refs */
    int R;                        /* points along a chunk's edge */
    float size;                   /* metres along a chunk's edge */
    Chunk *ch;
    int nch, cch;
    uint32_t rev;
    Hist *hist;
    int nhist, chist;
    size_t hist_bytes;
    bool stroking;
    Snap *pend;                   /* the stroke's chunks as they were before it */
    int npend, cpend;
    bool index_dirty;
    int refs;                     /* scenes showing it */
    LxTerrainComponent set;       /* the settings last seen (layers, grass...) */
    uint64_t layers_hash, grass_hash;
    LxMaterial material, grass_material;
    LxTerrainGpuLayers gpu;
    uint8_t *gpu_a, *gpu_b;
    LxTexture grass_tex;
} TData;
static TData **g_data;
static int g_ndata, g_cdata, g_next_id = 1;

typedef struct SChunk {           /* a chunk's objects in one scene */
    int cx, cz;
    LxEntity ent, grass;
    LxEntity *scatter;
    int nscatter;
    bool full;                    /* showing the full-detail mesh (and a collider) */
    bool alive;
    uint32_t serial, grass_serial;
} SChunk;
typedef struct TView {
    LxScene *scene;
    LxEntity terrain;
    TData *data;
    SChunk *c;
    int n, cap;
    int used;                     /* 0 free, 1 in use, 2 not yet seen this update */
} TView;
static TView *g_views;
static int g_nviews, g_cviews;
static uint32_t g_mesh_version = 1;

/* ------------------------------------------------------------------ small helpers */
static int N_(const TData *d) { return d->R - 1; }   /* cells along a chunk's edge */
static float cell(const TData *d) { return d->size / (float)N_(d); }
static int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
static int posmod(int a, int b) { int m = a % b; return m < 0 ? m + b : m; }
static float smooth01(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    float t = lx_clampf((x - a) / (b - a), 0, 1);
    return t * t * (3 - 2 * t);
}
static uint32_t hash3(int a, int b, int c) {
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u ^ (uint32_t)c * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
static float hashf(int a, int b, int c) { return (float)(hash3(a, b, c) >> 8) * (1.0f / 16777216.0f); }
static float vnoise(float x, float z, int seed) {
    int ix = (int)floorf(x), iz = (int)floorf(z);
    float fx = x - (float)ix, fz = z - (float)iz;
    fx = fx * fx * (3 - 2 * fx); fz = fz * fz * (3 - 2 * fz);
    float a = hashf(ix, iz, seed), b = hashf(ix + 1, iz, seed), c = hashf(ix, iz + 1, seed), e = hashf(ix + 1, iz + 1, seed);
    return lx_lerpf(lx_lerpf(a, b, fx), lx_lerpf(c, e, fx), fz) * 2.0f - 1.0f;
}
static float fbm(float x, float z, int oct, int seed) {
    float v = 0, a = 0.5f, f = 1, norm = 0;
    for (int i = 0; i < oct; i++) {
        float rx = x * 0.8f - z * 0.6f, rz = x * 0.6f + z * 0.8f;   /* turned each octave: no grid lines */
        x = rx; z = rz;
        v += a * vnoise(x * f, z * f, seed + i * 17);
        norm += a;
        a *= 0.5f; f *= 2.03f;
    }
    return v / norm;
}
static float ridged(float x, float z, int oct, int seed) {
    float v = 0, a = 0.5f, f = 1, norm = 0;
    for (int i = 0; i < oct; i++) {
        float n = 1.0f - fabsf(vnoise(x * f, z * f, seed + i * 31));
        v += a * n * n;
        norm += a;
        a *= 0.5f; f *= 2.1f;
    }
    return v / norm;
}

/* ------------------------------------------------------------------ chunks */
static Chunk *find_chunk(TData *d, int cx, int cz) {
    for (int i = 0; i < d->nch; i++) if (d->ch[i].cx == cx && d->ch[i].cz == cz) return &d->ch[i];
    return NULL;
}
static void chunk_free_data(Chunk *c) {
    for (int l = 0; l < NE; l++) { lx_free(c->layer[l]); c->layer[l] = NULL; }
    lx_free(c->h); c->h = NULL;
    lx_free(c->w); c->w = NULL;
    lx_free(c->holes); c->holes = NULL;
    if (c->mesh) { lx_mesh_destroy(c->mesh); c->mesh = NULL; }
    if (c->grass) { lx_mesh_destroy(c->grass); c->grass = NULL; }
    c->loaded = false;
}
static void chunk_alloc(TData *d, Chunk *c) {
    size_t n = (size_t)d->R * (size_t)d->R;
    c->layer[0] = lx_calloc(n, sizeof(float));
    c->h = lx_calloc(n, sizeof(float));
    c->w = lx_calloc(n * NL, 1);
    for (size_t i = 0; i < n; i++) c->w[i * NL] = 255;
    c->holes = lx_calloc((size_t)N_(d) * (size_t)N_(d), 1);
    c->loaded = true;
}
static void composite(TData *d, Chunk *c) {
    size_t n = (size_t)d->R * (size_t)d->R;
    memcpy(c->h, c->layer[0], n * sizeof(float));
    for (int l = 1; l < NE && l < d->set.edit_layer_count; l++) {
        if (!c->layer[l] || !d->set.edit_layers[l].visible) continue;
        float k = d->set.edit_layers[l].strength;
        for (size_t i = 0; i < n; i++) c->h[i] += c->layer[l][i] * k;
    }
}
static void update_far(TData *d, Chunk *c) {
    int R = d->R, N = N_(d);
    c->hmin = 1e30f; c->hmax = -1e30f;
    for (size_t i = 0; i < (size_t)R * R; i++) { c->hmin = lx_minf(c->hmin, c->h[i]); c->hmax = lx_maxf(c->hmax, c->h[i]); }
    for (int j = 0; j < FAR; j++)
        for (int i = 0; i < FAR; i++) {
            int si = i * N / (FAR - 1), sj = j * N / (FAR - 1);
            c->far_h[j * FAR + i] = c->h[sj * R + si];
            memcpy(&c->far_w[(j * FAR + i) * NL], &c->w[((size_t)sj * R + si) * NL], NL);
        }
    c->dirty_far = false;
}

/* ------------------------------------------------------------------ files */
static void folder_path(const TData *d, char *out, size_t cap) { lx_asset_path(out, cap, d->folder); }
static void chunk_path(const TData *d, int cx, int cz, char *out, size_t cap) {
    char dir[LX_PATH_MAX], name[64];
    folder_path(d, dir, sizeof dir);
    snprintf(name, sizeof name, "c_%d_%d.lxchunk", cx, cz);
    lx_path_join(out, cap, dir, name);
}
typedef struct Wr { uint8_t *p; size_t n, cap; } Wr;
static void wr(Wr *w, const void *src, size_t n) {
    if (w->n + n > w->cap) { w->cap = (w->n + n) * 2 + 1024; w->p = lx_realloc(w->p, w->cap); }
    memcpy(w->p + w->n, src, n);
    w->n += n;
}
static void wr_i(Wr *w, int32_t v) { wr(w, &v, 4); }
static void wr_f(Wr *w, float v) { wr(w, &v, 4); }
typedef struct Rd { const uint8_t *p; size_t n, at; bool bad; } Rd;
static void rd(Rd *r, void *dst, size_t n) {
    if (r->at + n > r->n) { r->bad = true; memset(dst, 0, n); return; }
    memcpy(dst, r->p + r->at, n);
    r->at += n;
}
static int32_t rd_i(Rd *r) { int32_t v; rd(r, &v, 4); return v; }
static float rd_f(Rd *r) { float v; rd(r, &v, 4); return v; }

static bool save_chunk(TData *d, Chunk *c) {
    if (!c->loaded) return true;
    Wr w = {0};
    wr(&w, "LXTC", 4);
    wr_i(&w, 1);
    wr_i(&w, d->R);
    uint32_t mask = 0;
    for (int l = 0; l < NE; l++) if (c->layer[l]) mask |= 1u << l;
    wr_i(&w, (int32_t)mask);
    size_t n = (size_t)d->R * d->R;
    for (int l = 0; l < NE; l++) if (c->layer[l]) wr(&w, c->layer[l], n * sizeof(float));
    wr(&w, c->w, n * NL);
    wr(&w, c->holes, (size_t)N_(d) * N_(d));
    char path[LX_PATH_MAX], dir[LX_PATH_MAX];
    folder_path(d, dir, sizeof dir);
    lx_mkdir_p(dir);
    chunk_path(d, c->cx, c->cz, path, sizeof path);
    bool ok = lx_file_write(path, w.p, w.n);
    lx_free(w.p);
    if (ok) c->dirty_file = false;
    return ok;
}
static bool load_chunk(TData *d, Chunk *c) {
    if (c->loaded) return true;
    char path[LX_PATH_MAX];
    chunk_path(d, c->cx, c->cz, path, sizeof path);
    size_t sz = 0;
    char *buf = lx_file_read(path, &sz);
    chunk_alloc(d, c);
    bool ok = false;
    if (buf) {
        Rd r = {(const uint8_t *)buf, sz, 0, false};
        char magic[4];
        rd(&r, magic, 4);
        int ver = rd_i(&r), R = rd_i(&r);
        uint32_t mask = (uint32_t)rd_i(&r);
        if (!memcmp(magic, "LXTC", 4) && ver == 1 && R == d->R) {
            size_t n = (size_t)R * R;
            for (int l = 0; l < NE; l++)
                if (mask & (1u << l)) {
                    if (!c->layer[l]) c->layer[l] = lx_calloc(n, sizeof(float));
                    rd(&r, c->layer[l], n * sizeof(float));
                }
            rd(&r, c->w, n * NL);
            rd(&r, c->holes, (size_t)N_(d) * N_(d));
            ok = !r.bad;
        }
        lx_free(buf);
    }
    if (!ok) {   /* missing or unreadable: rebuild from the simplified copy, so it at least keeps its shape */
        int R = d->R, N = N_(d);
        for (int j = 0; j < R; j++)
            for (int i = 0; i < R; i++) {
                float fx = (float)i * (FAR - 1) / N, fz = (float)j * (FAR - 1) / N;
                int x0 = (int)fx, z0 = (int)fz, x1 = x0 < FAR - 1 ? x0 + 1 : x0, z1 = z0 < FAR - 1 ? z0 + 1 : z0;
                float tx = fx - x0, tz = fz - z0;
                c->layer[0][j * R + i] = lx_lerpf(lx_lerpf(c->far_h[z0 * FAR + x0], c->far_h[z0 * FAR + x1], tx),
                                                  lx_lerpf(c->far_h[z1 * FAR + x0], c->far_h[z1 * FAR + x1], tx), tz);
                memcpy(&c->w[((size_t)j * R + i) * NL], &c->far_w[(((int)(fz + 0.5f)) * FAR + (int)(fx + 0.5f)) * NL], NL);
            }
        if (buf) LX_WARN("Terrain chunk %d,%d of %s couldn't be read: rebuilt from its simplified copy", c->cx, c->cz, d->folder);
    }
    composite(d, c);
    c->dirty_mesh = c->dirty_grass = c->dirty_scatter = true;
    return true;
}
static bool save_index(TData *d) {
    Wr w = {0};
    wr(&w, "LXTI", 4);
    wr_i(&w, 1);
    wr_i(&w, d->R);
    wr_f(&w, d->size);
    wr_i(&w, d->nch);
    for (int i = 0; i < d->nch; i++) {
        Chunk *c = &d->ch[i];
        if (c->loaded && c->dirty_far) update_far(d, c);
        wr_i(&w, c->cx); wr_i(&w, c->cz);
        wr_f(&w, c->hmin); wr_f(&w, c->hmax);
        wr(&w, c->far_h, sizeof c->far_h);
        wr(&w, c->far_w, sizeof c->far_w);
    }
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    folder_path(d, dir, sizeof dir);
    lx_mkdir_p(dir);
    lx_path_join(path, sizeof path, dir, "terrain.lxterrain");
    bool ok = lx_file_write(path, w.p, w.n);
    lx_free(w.p);
    if (ok) d->index_dirty = false;
    return ok;
}
static void load_index(TData *d) {
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    folder_path(d, dir, sizeof dir);
    lx_path_join(path, sizeof path, dir, "terrain.lxterrain");
    size_t sz = 0;
    char *buf = lx_file_read(path, &sz);
    if (!buf) return;
    Rd r = {(const uint8_t *)buf, sz, 0, false};
    char magic[4];
    rd(&r, magic, 4);
    int ver = rd_i(&r), R = rd_i(&r);
    float size = rd_f(&r);
    int n = rd_i(&r);
    if (!memcmp(magic, "LXTI", 4) && ver == 1 && R >= 9 && R <= 1025 && size > 0 && n >= 0 && n < 1000000) {
        d->R = R;
        d->size = size;
        for (int i = 0; i < n && !r.bad; i++) {
            Chunk c;
            memset(&c, 0, sizeof c);
            c.cx = rd_i(&r); c.cz = rd_i(&r);
            c.hmin = rd_f(&r); c.hmax = rd_f(&r);
            rd(&r, c.far_h, sizeof c.far_h);
            rd(&r, c.far_w, sizeof c.far_w);
            c.dirty_mesh = true;
            if (!r.bad) LX_VEC_PUSH(d->ch, d->nch, d->cch, c);
        }
    } else LX_WARN("Terrain index %s couldn't be read", path);
    lx_free(buf);
}

/* ------------------------------------------------------------------ the data, shared by scenes */
static TData *data_for(const LxTerrainComponent *t) {
    for (int i = 0; i < g_ndata; i++) if (!strcmp(g_data[i]->folder, t->folder)) return g_data[i];
    TData *d = lx_calloc(1, sizeof *d);
    lx_strcpy(d->folder, sizeof d->folder, t->folder);
    d->id = g_next_id++;
    d->R = t->resolution >= 9 ? t->resolution : 129;
    d->size = t->chunk_size > 0.5f ? t->chunk_size : 64.0f;
    d->set = *t;
    d->rev = t->revision;
    load_index(d);
    LX_VEC_PUSH(g_data, g_ndata, g_cdata, d);
    return d;
}
static Chunk *ensure_loaded(TData *d, int cx, int cz) {
    Chunk *c = find_chunk(d, cx, cz);
    if (!c) return NULL;
    if (!c->loaded) load_chunk(d, c);
    return c;
}

/* ------------------------------------------------------------------ the global lattice: every copy of a point */
typedef struct Copy { Chunk *c; int i, j; } Copy;
static int copies_of(TData *d, int gx, int gz, Copy out[4], bool load) {
    int N = N_(d), n = 0;
    int cx0 = floordiv(gx, N), cz0 = floordiv(gz, N);
    int ix = posmod(gx, N), iz = posmod(gz, N);
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) {
            if (a && ix != 0) continue;
            if (b && iz != 0) continue;
            int cx = cx0 - a, cz = cz0 - b;
            Chunk *c = load ? ensure_loaded(d, cx, cz) : find_chunk(d, cx, cz);
            if (!c || !c->loaded) continue;
            out[n].c = c; out[n].i = a ? N : ix; out[n].j = b ? N : iz;
            n++;
        }
    return n;
}
static bool height_g(TData *d, int gx, int gz, float *h) {
    Copy cp[4];
    if (!copies_of(d, gx, gz, cp, false)) return false;
    *h = cp[0].c->h[cp[0].j * d->R + cp[0].i];
    return true;
}
/* clamped to the nearest point that exists (normals at the terrain's outer edge) */
static float height_gc(TData *d, int gx, int gz, float fallback) {
    float h;
    return height_g(d, gx, gz, &h) ? h : fallback;
}

/* ------------------------------------------------------------------ undo history */
static size_t snap_take(TData *d, Chunk *c, int cx, int cz, Snap *s) {
    memset(s, 0, sizeof *s);
    s->cx = cx; s->cz = cz;
    if (!c) return 0;
    if (!c->loaded) load_chunk(d, c);
    s->exists = true;
    size_t n = (size_t)d->R * d->R;
    for (int l = 0; l < NE; l++)
        if (c->layer[l]) { s->layer[l] = lx_alloc(n * sizeof(float)); memcpy(s->layer[l], c->layer[l], n * sizeof(float)); s->bytes += n * 4; }
    s->w = lx_alloc(n * NL); memcpy(s->w, c->w, n * NL);
    size_t hn = (size_t)N_(d) * N_(d);
    s->holes = lx_alloc(hn); memcpy(s->holes, c->holes, hn);
    s->bytes += n * NL + hn;
    return s->bytes;
}
static void snap_free(Snap *s) {
    for (int l = 0; l < NE; l++) lx_free(s->layer[l]);
    lx_free(s->w); lx_free(s->holes);
    memset(s, 0, sizeof *s);
}
static void hist_free(Hist *h) {
    for (int i = 0; i < h->n; i++) { snap_free(&h->before[i]); snap_free(&h->after[i]); }
    lx_free(h->before); lx_free(h->after);
    memset(h, 0, sizeof *h);
}
static void remove_chunk_raw(TData *d, Chunk *c) {
    chunk_free_data(c);
    if (c->far_mesh) lx_mesh_destroy(c->far_mesh);
    int i = (int)(c - d->ch);
    d->ch[i] = d->ch[--d->nch];
    d->index_dirty = true;   /* (its file goes when the terrain is saved) */
}
static void snap_restore(TData *d, const Snap *s) {
    Chunk *c = find_chunk(d, s->cx, s->cz);
    if (!s->exists) { if (c) remove_chunk_raw(d, c); return; }
    if (!c) {
        Chunk nc;
        memset(&nc, 0, sizeof nc);
        nc.cx = s->cx; nc.cz = s->cz;
        LX_VEC_PUSH(d->ch, d->nch, d->cch, nc);
        c = &d->ch[d->nch - 1];
        chunk_alloc(d, c);
        d->index_dirty = true;
    } else if (!c->loaded) load_chunk(d, c);
    size_t n = (size_t)d->R * d->R;
    for (int l = 0; l < NE; l++) {
        if (s->layer[l]) {
            if (!c->layer[l]) c->layer[l] = lx_alloc(n * sizeof(float));
            memcpy(c->layer[l], s->layer[l], n * sizeof(float));
        } else if (l > 0) { lx_free(c->layer[l]); c->layer[l] = NULL; }
    }
    memcpy(c->w, s->w, n * NL);
    memcpy(c->holes, s->holes, (size_t)N_(d) * N_(d));
    composite(d, c);
    c->dirty_file = c->dirty_mesh = c->dirty_far = c->dirty_grass = c->dirty_scatter = true;
    d->index_dirty = true;
}
/* a chunk is about to change: keep it as it is now (once per stroke) */
static void touch(TData *d, int cx, int cz) {
    if (!d->stroking) return;
    for (int i = 0; i < d->npend; i++) if (d->pend[i].cx == cx && d->pend[i].cz == cz) return;
    Snap s;
    snap_take(d, find_chunk(d, cx, cz), cx, cz, &s);
    LX_VEC_PUSH(d->pend, d->npend, d->cpend, s);
}
static void stroke_begin(TData *d) {
    if (d->stroking) return;
    d->stroking = true;
    d->npend = 0;
}
static bool stroke_end(TData *d) {
    if (!d->stroking) return false;
    d->stroking = false;
    if (!d->npend) return false;
    /* forget the redo steps beyond this one */
    for (int i = 0; i < d->nhist; i++)
        if (d->hist[i].rev > d->rev) { d->hist_bytes -= d->hist[i].bytes; hist_free(&d->hist[i]); d->hist[i--] = d->hist[--d->nhist]; }
    Hist h;
    memset(&h, 0, sizeof h);
    h.rev = ++d->rev;
    h.n = d->npend;
    h.before = lx_alloc(sizeof(Snap) * (size_t)h.n);
    h.after = lx_alloc(sizeof(Snap) * (size_t)h.n);
    for (int i = 0; i < h.n; i++) {
        h.before[i] = d->pend[i];
        h.bytes += h.before[i].bytes + snap_take(d, find_chunk(d, d->pend[i].cx, d->pend[i].cz), d->pend[i].cx, d->pend[i].cz, &h.after[i]);
        Chunk *c = find_chunk(d, d->pend[i].cx, d->pend[i].cz);
        if (c) { c->dirty_file = c->dirty_far = c->dirty_grass = c->dirty_scatter = true; }
    }
    d->npend = 0;
    LX_VEC_PUSH(d->hist, d->nhist, d->chist, h);
    d->hist_bytes += h.bytes;
    d->index_dirty = true;
    /* too much kept: the oldest steps go */
    while (d->hist_bytes > HIST_BYTES && d->nhist > 1) {
        int oldest = 0;
        for (int i = 1; i < d->nhist; i++) if (d->hist[i].rev < d->hist[oldest].rev) oldest = i;
        d->hist_bytes -= d->hist[oldest].bytes;
        hist_free(&d->hist[oldest]);
        d->hist[oldest] = d->hist[--d->nhist];
    }
    return true;
}
static Hist *hist_find(TData *d, uint32_t rev) {
    for (int i = 0; i < d->nhist; i++) if (d->hist[i].rev == rev) return &d->hist[i];
    return NULL;
}
/* walk the history to `want` (as far as it reaches); returns the revision reached */
static uint32_t seek(TData *d, uint32_t want) {
    while (d->rev > want) {
        Hist *h = hist_find(d, d->rev);
        if (!h) break;
        for (int i = h->n - 1; i >= 0; i--) snap_restore(d, &h->before[i]);
        d->rev--;
    }
    while (d->rev < want) {
        Hist *h = hist_find(d, d->rev + 1);
        if (!h) break;
        for (int i = 0; i < h->n; i++) snap_restore(d, &h->after[i]);
        d->rev++;
    }
    return d->rev;
}

/* ------------------------------------------------------------------ paint: the weights a point ends up with */
/* painted weights, then each layer with automatic rules takes over where its slope and height fit */
static void final_weights(const TData *d, const uint8_t *painted, float height, float slope_deg, float out[NL]) {
    float sum = 0;
    for (int l = 0; l < NL; l++) { out[l] = painted[l] / 255.0f; sum += out[l]; }
    if (sum <= 0) { out[0] = 1; sum = 1; }
    for (int l = 0; l < NL; l++) out[l] /= sum;
    int count = d->set.layer_count < 1 ? 1 : d->set.layer_count > NL ? NL : d->set.layer_count;
    for (int l = 0; l < count; l++) {
        const LxTerrainLayer *L = &d->set.layers[l];
        bool by_slope = L->slope_max > L->slope_min, by_height = L->height_max > L->height_min;
        if (!by_slope && !by_height) continue;
        float c = 1;
        if (by_slope) {
            float soft = lx_maxf((L->slope_max - L->slope_min) * 0.15f, 2.0f);
            c *= smooth01(L->slope_min - soft, L->slope_min + soft, slope_deg) * (1.0f - smooth01(L->slope_max - soft, L->slope_max + soft, slope_deg));
        }
        if (by_height) {
            float soft = lx_maxf((L->height_max - L->height_min) * 0.1f, 1.0f);
            c *= smooth01(L->height_min - soft, L->height_min + soft, height) * (1.0f - smooth01(L->height_max - soft, L->height_max + soft, height));
        }
        if (c <= 0) continue;
        for (int k = 0; k < NL; k++) out[k] *= 1.0f - c;
        out[l] += c;
    }
    for (int l = count; l < NL; l++) out[l] = 0;   /* layers that don't exist */
    sum = 0;
    for (int l = 0; l < NL; l++) sum += out[l];
    if (sum <= 0) out[0] = sum = 1;
    for (int l = 0; l < NL; l++) out[l] /= sum;
}

/* ------------------------------------------------------------------ meshes */
static void pack_vertex(LxVertex *v, float x, float y, float z, LxVec3 n, const float w[NL]) {
    v->px = x; v->py = y; v->pz = z;
    v->nx = n.x; v->ny = n.y; v->nz = n.z;
    v->u = w[0]; v->v = w[1];
    v->u2 = w[2]; v->v2 = w[3];
    v->tx = 1; v->ty = 0; v->tz = 0; v->tw = w[4];   /* the shader takes layer 5 as what's left */
}
/* one level of detail of a chunk: every `step`-th point, with skirts hanging down from its edges */
static void build_lod(TData *d, Chunk *c, int step, LxMeshLod *lod, LxVec3 world_origin) {
    int R = d->R, N = N_(d), n = N / step + 1;
    float cs = cell(d), skirt = lx_maxf(d->size * 0.03f, 1.0f);
    int vcount = n * n + 4 * n;
    LxVertex *v = lx_calloc((size_t)vcount, sizeof(LxVertex));
    int gx0 = c->cx * N, gz0 = c->cz * N;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int si = i * step, sj = j * step;
            float y = c->h[sj * R + si];
            /* the normal from the full-detail neighbours, across chunk edges too */
            int gx = gx0 + si, gz = gz0 + sj;
            float hl = height_gc(d, gx - 1, gz, y), hr = height_gc(d, gx + 1, gz, y), hd = height_gc(d, gx, gz - 1, y), hu = height_gc(d, gx, gz + 1, y);
            LxVec3 nrm = lx_norm3(lx_v3(hl - hr, 2.0f * cs, hd - hu));
            float slope = acosf(lx_clampf(nrm.y, -1, 1)) * LX_RAD2DEG;
            float w[NL];
            final_weights(d, &c->w[((size_t)sj * R + si) * NL], y + world_origin.y, slope, w);
            pack_vertex(&v[j * n + i], (float)si * cs, y, (float)sj * cs, nrm, w);
        }
    /* skirts: a copy of each edge, hanging down */
    int sk = n * n;
    for (int e = 0; e < 4; e++)
        for (int k = 0; k < n; k++) {
            int i = e == 0 ? k : e == 1 ? n - 1 : e == 2 ? n - 1 - k : 0;
            int j = e == 0 ? 0 : e == 1 ? k : e == 2 ? n - 1 : n - 1 - k;
            v[sk + e * n + k] = v[j * n + i];
            v[sk + e * n + k].py -= skirt;
        }
    uint32_t *idx = lx_alloc(sizeof(uint32_t) * (size_t)((n - 1) * (n - 1) * 6 + 4 * (n - 1) * 6));
    int ic = 0;
    for (int j = 0; j < n - 1; j++)
        for (int i = 0; i < n - 1; i++) {
            bool hole = false;   /* any full-detail cell under this one cut out */
            for (int b = 0; b < step && !hole; b++)
                for (int a = 0; a < step && !hole; a++) hole = c->holes[(j * step + b) * N + i * step + a] != 0;
            if (hole) continue;
            uint32_t q0 = (uint32_t)(j * n + i), q1 = q0 + 1, q2 = q0 + (uint32_t)n, q3 = q2 + 1;
            if (((i + j) & 1) == 0) { idx[ic++] = q0; idx[ic++] = q2; idx[ic++] = q1; idx[ic++] = q1; idx[ic++] = q2; idx[ic++] = q3; }
            else { idx[ic++] = q0; idx[ic++] = q2; idx[ic++] = q3; idx[ic++] = q0; idx[ic++] = q3; idx[ic++] = q1; }
        }
    lod->solid_index_count = ic > 0 ? ic : -1;   /* the skirts below aren't walls a character can bump into under the ground */
    for (int e = 0; e < 4; e++)
        for (int k = 0; k < n - 1; k++) {
            int i0 = e == 0 ? k : e == 1 ? n - 1 : e == 2 ? n - 1 - k : 0, j0 = e == 0 ? 0 : e == 1 ? k : e == 2 ? n - 1 : n - 1 - k;
            int i1 = e == 0 ? k + 1 : e == 1 ? n - 1 : e == 2 ? n - 2 - k : 0, j1 = e == 0 ? 0 : e == 1 ? k + 1 : e == 2 ? n - 1 : n - 2 - k;
            uint32_t a = (uint32_t)(j0 * n + i0), b = (uint32_t)(j1 * n + i1), sa = (uint32_t)(sk + e * n + k), sb = sa + 1;
            idx[ic++] = a; idx[ic++] = b; idx[ic++] = sa;
            idx[ic++] = b; idx[ic++] = sb; idx[ic++] = sa;
        }
    lod->vertices = v;
    lod->vertex_count = vcount;
    lod->indices = idx;
    lod->index_count = ic;
}
static LxMesh *finish_mesh(LxMesh *m, const char *ref) {
    m->version = g_mesh_version++;
    lx_strcpy(m->ref, sizeof m->ref, ref);
    LxAabb b = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    const LxMeshLod *l0 = &m->lods[0];
    for (int i = 0; i < l0->vertex_count; i++) {
        LxVec3 p = lx_v3(l0->vertices[i].px, l0->vertices[i].py, l0->vertices[i].pz);
        b.min = lx_min3(b.min, p);
        b.max = lx_max3(b.max, p);
    }
    if (l0->vertex_count == 0) b.min = b.max = lx_v3(0, 0, 0);
    m->bounds = b;
    return m;
}
static void build_full_mesh(TData *d, Chunk *c, LxVec3 origin) {
    LxMesh *m = lx_calloc(1, sizeof *m);
    static const float SIZES[4] = {0.45f, 0.2f, 0.09f, 0.0f};
    float k = d->set.lod_distance > 0.05f ? d->set.lod_distance : 1.0f;
    for (int step = 1; step <= N_(d) / 4 && m->lod_count < LX_MAX_LODS; step *= 2) {
        build_lod(d, c, step, &m->lods[m->lod_count], origin);
        m->lods[m->lod_count].screen_size = SIZES[m->lod_count] / k;
        m->lod_count++;
    }
    m->lods[m->lod_count - 1].screen_size = 0;
    char ref[64];
    snprintf(ref, sizeof ref, "terrain:%d/%d_%d", d->id, c->cx, c->cz);
    if (c->mesh) lx_mesh_destroy(c->mesh);
    c->mesh = finish_mesh(m, ref);
    c->dirty_mesh = false;
    c->serial++;
}
static void build_far_mesh(TData *d, Chunk *c, LxVec3 origin) {
    LxMesh *m = lx_calloc(1, sizeof *m);
    int n = FAR;
    float cs = d->size / (FAR - 1), skirt = lx_maxf(d->size * 0.03f, 1.0f);
    int vcount = n * n + 4 * n;
    LxVertex *v = lx_calloc((size_t)vcount, sizeof(LxVertex));
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float y = c->far_h[j * n + i];
            float hl = c->far_h[j * n + (i > 0 ? i - 1 : i)], hr = c->far_h[j * n + (i < n - 1 ? i + 1 : i)];
            float hd = c->far_h[(j > 0 ? j - 1 : j) * n + i], hu = c->far_h[(j < n - 1 ? j + 1 : j) * n + i];
            LxVec3 nrm = lx_norm3(lx_v3((hl - hr) / (float)((i > 0) + (i < n - 1)), cs, (hd - hu) / (float)((j > 0) + (j < n - 1))));
            float w[NL];
            final_weights(d, &c->far_w[(j * n + i) * NL], y + origin.y, acosf(lx_clampf(nrm.y, -1, 1)) * LX_RAD2DEG, w);
            pack_vertex(&v[j * n + i], (float)i * cs, y, (float)j * cs, nrm, w);
        }
    int sk = n * n;
    for (int e = 0; e < 4; e++)
        for (int k = 0; k < n; k++) {
            int i = e == 0 ? k : e == 1 ? n - 1 : e == 2 ? n - 1 - k : 0;
            int j = e == 0 ? 0 : e == 1 ? k : e == 2 ? n - 1 : n - 1 - k;
            v[sk + e * n + k] = v[j * n + i];
            v[sk + e * n + k].py -= skirt;
        }
    uint32_t *idx = lx_alloc(sizeof(uint32_t) * (size_t)((n - 1) * (n - 1) * 6 + 4 * (n - 1) * 6));
    int ic = 0;
    for (int j = 0; j < n - 1; j++)
        for (int i = 0; i < n - 1; i++) {
            uint32_t q0 = (uint32_t)(j * n + i), q1 = q0 + 1, q2 = q0 + (uint32_t)n, q3 = q2 + 1;
            idx[ic++] = q0; idx[ic++] = q2; idx[ic++] = q1; idx[ic++] = q1; idx[ic++] = q2; idx[ic++] = q3;
        }
    for (int e = 0; e < 4; e++)
        for (int k = 0; k < n - 1; k++) {
            int i0 = e == 0 ? k : e == 1 ? n - 1 : e == 2 ? n - 1 - k : 0, j0 = e == 0 ? 0 : e == 1 ? k : e == 2 ? n - 1 : n - 1 - k;
            int i1 = e == 0 ? k + 1 : e == 1 ? n - 1 : e == 2 ? n - 2 - k : 0, j1 = e == 0 ? 0 : e == 1 ? k + 1 : e == 2 ? n - 1 : n - 2 - k;
            uint32_t a = (uint32_t)(j0 * n + i0), b = (uint32_t)(j1 * n + i1), sa = (uint32_t)(sk + e * n + k), sb = sa + 1;
            idx[ic++] = a; idx[ic++] = b; idx[ic++] = sa;
            idx[ic++] = b; idx[ic++] = sb; idx[ic++] = sa;
        }
    m->lods[0].vertices = v; m->lods[0].vertex_count = vcount;
    m->lods[0].indices = idx; m->lods[0].index_count = ic;
    m->lod_count = 1;
    char ref[64];
    snprintf(ref, sizeof ref, "terrain:%d/%d_%d/far", d->id, c->cx, c->cz);
    if (c->far_mesh) lx_mesh_destroy(c->far_mesh);
    c->far_mesh = finish_mesh(m, ref);
    c->serial++;
}

/* bilinear height and weights inside a loaded chunk, at local (x, z) metres from its corner */
static float chunk_height(TData *d, Chunk *c, float x, float z) {
    int R = d->R, N = N_(d);
    float fx = lx_clampf(x / cell(d), 0, (float)N), fz = lx_clampf(z / cell(d), 0, (float)N);
    int x0 = lx_mini((int)fx, N - 1), z0 = lx_mini((int)fz, N - 1);
    float tx = fx - x0, tz = fz - z0;
    const float *h = c->h;
    return lx_lerpf(lx_lerpf(h[z0 * R + x0], h[z0 * R + x0 + 1], tx), lx_lerpf(h[(z0 + 1) * R + x0], h[(z0 + 1) * R + x0 + 1], tx), tz);
}
static float chunk_weight(TData *d, Chunk *c, float x, float z, int layer, LxVec3 origin) {
    int R = d->R, N = N_(d);
    int i = lx_clampi((int)(x / cell(d) + 0.5f), 0, N), j = lx_clampi((int)(z / cell(d) + 0.5f), 0, N);
    float y = c->h[j * R + i];
    int gx = c->cx * N + i, gz = c->cz * N + j;
    float hl = height_gc(d, gx - 1, gz, y), hr = height_gc(d, gx + 1, gz, y), hd = height_gc(d, gx, gz - 1, y), hu = height_gc(d, gx, gz + 1, y);
    LxVec3 nrm = lx_norm3(lx_v3(hl - hr, 2.0f * cell(d), hd - hu));
    float w[NL];
    final_weights(d, &c->w[((size_t)j * R + i) * NL], y + origin.y, acosf(lx_clampf(nrm.y, -1, 1)) * LX_RAD2DEG, w);
    return layer >= 0 && layer < NL ? w[layer] : 1.0f;
}

/* grass: tufts of thin blades where its layer is painted. uv.y runs root to tip (the texture darkens the root),
 * uv2.x how much a vertex sways (0 at the root), uv2.y its phase. */
static void build_grass(TData *d, Chunk *c, LxVec3 origin) {
    if (c->grass) { lx_mesh_destroy(c->grass); c->grass = NULL; }
    const LxTerrainComponent *t = &d->set;
    c->dirty_grass = false;
    c->serial++;
    if (t->grass_layer < 0 || t->grass_layer >= NL || t->grass_density <= 0.001f) return;
    float spacing = 1.0f / sqrtf(lx_minf(t->grass_density, 16.0f));
    int n = (int)(d->size / spacing);
    if (n < 1) return;
    LxVertex *v = NULL;
    uint32_t *idx = NULL;
    int nv = 0, cv = 0, ni = 0, ci = 0;
    float hgt = t->grass_height > 0.02f ? t->grass_height : 0.4f;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float x = ((float)i + hashf(i, j, c->cx * 7 + c->cz * 131)) * spacing;
            float z = ((float)j + hashf(j, i, c->cz * 7 + c->cx * 131 + 5)) * spacing;
            if (x >= d->size || z >= d->size) continue;
            int ci_ = lx_clampi((int)(x / cell(d)), 0, N_(d) - 1), cj = lx_clampi((int)(z / cell(d)), 0, N_(d) - 1);
            if (c->holes[cj * N_(d) + ci_]) continue;
            float wgt = chunk_weight(d, c, x, z, t->grass_layer, origin);
            if (wgt < 0.25f || hashf(i + 9, j, c->cx ^ c->cz) > wgt * 1.2f) continue;
            float y = chunk_height(d, c, x, z);
            float phase = hashf(i, j, 77) * 6.28f;
            for (int b = 0; b < 4; b++) {
                float a = hashf(i * 4 + b, j, 3) * 6.2831853f, lean = 0.15f + hashf(i, j * 4 + b, 4) * 0.25f;
                float bh = hgt * (0.55f + 0.7f * hashf(i * 4 + b, j * 4, 9)) * lx_clampf(wgt * 1.3f, 0.4f, 1.0f);
                float bw = 0.035f + 0.03f * hashf(i, j, b);
                float ox = cosf(a) * 0.12f * hashf(b, i, j), oz = sinf(a) * 0.12f * hashf(j, b, i);
                LxVec3 side = lx_v3(-sinf(a) * bw, 0, cosf(a) * bw), dir = lx_v3(cosf(a), 0, sinf(a));
                LxVec3 base = lx_v3(x + ox, y - 0.02f, z + oz);
                LxVec3 tip = lx_add3(base, lx_add3(lx_v3(0, bh, 0), lx_scale3(dir, bh * lean)));
                LxVec3 nrm = lx_norm3(lx_add3(lx_v3(0, 1.2f, 0), lx_scale3(dir, -0.5f)));
                if (nv + 3 > cv) { cv = (nv + 3) * 2; v = lx_realloc(v, sizeof(LxVertex) * (size_t)cv); }
                if (ni + 3 > ci) { ci = (ni + 3) * 2; idx = lx_realloc(idx, sizeof(uint32_t) * (size_t)ci); }
                LxVertex *q = &v[nv];
                memset(q, 0, sizeof(LxVertex) * 3);
                LxVec3 ps[3] = {lx_sub3(base, side), lx_add3(base, side), tip};
                for (int k = 0; k < 3; k++) {
                    q[k].px = ps[k].x; q[k].py = ps[k].y; q[k].pz = ps[k].z;
                    q[k].nx = nrm.x; q[k].ny = nrm.y; q[k].nz = nrm.z;
                    q[k].u = 0.5f; q[k].v = k == 2 ? 1.0f : 0.0f;
                    q[k].tx = 1; q[k].tw = 1;
                    q[k].u2 = k == 2 ? bh : 0.0f;   /* sways by its height */
                    q[k].v2 = phase + (float)b;
                }
                idx[ni++] = (uint32_t)nv; idx[ni++] = (uint32_t)nv + 1; idx[ni++] = (uint32_t)nv + 2;
                nv += 3;
            }
        }
    if (!nv) { lx_free(v); lx_free(idx); return; }
    LxMesh *m = lx_calloc(1, sizeof *m);
    m->lods[0].vertices = v; m->lods[0].vertex_count = nv;
    m->lods[0].indices = idx; m->lods[0].index_count = ni;
    m->lod_count = 1;
    char ref[64];
    snprintf(ref, sizeof ref, "terrain:%d/%d_%d/grass", d->id, c->cx, c->cz);
    c->grass = finish_mesh(m, ref);
}

/* ------------------------------------------------------------------ layers: the textures the shader blends */
static void layer_procedural(const char *name, uint8_t *a, uint8_t *b, int S, LxVec3 *avg) {
    /* built-in ground textures, made here so a new terrain looks like ground at once */
    LxVec3 c0, c1;
    float rough, scale, contrast;
    int kind = 0;
    if (strstr(name, "rock")) { c0 = lx_v3(0.32f, 0.30f, 0.28f); c1 = lx_v3(0.52f, 0.50f, 0.47f); rough = 0.85f; scale = 6; contrast = 1.0f; kind = 1; }
    else if (strstr(name, "dirt")) { c0 = lx_v3(0.30f, 0.22f, 0.15f); c1 = lx_v3(0.45f, 0.35f, 0.25f); rough = 0.95f; scale = 10; contrast = 0.7f; kind = 2; }
    else if (strstr(name, "snow")) { c0 = lx_v3(0.82f, 0.85f, 0.9f); c1 = lx_v3(0.95f, 0.96f, 0.98f); rough = 0.55f; scale = 5; contrast = 0.4f; kind = 3; }
    else if (strstr(name, "sand")) { c0 = lx_v3(0.70f, 0.60f, 0.42f); c1 = lx_v3(0.82f, 0.74f, 0.56f); rough = 0.9f; scale = 14; contrast = 0.5f; kind = 4; }
    else { c0 = lx_v3(0.12f, 0.22f, 0.06f); c1 = lx_v3(0.28f, 0.40f, 0.12f); rough = 0.9f; scale = 12; contrast = 0.8f; }
    float *hf = lx_alloc(sizeof(float) * (size_t)S * S);
    double sum[3] = {0, 0, 0};
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float u = (float)x / S * scale, v = (float)y / S * scale;
            /* tileable: blend the noise with copies shifted by a whole period */
            float wx = (float)x / S, wy = (float)y / S;
            float n00 = fbm(u, v, 5, 11), n10 = fbm(u - scale, v, 5, 11), n01 = fbm(u, v - scale, 5, 11), n11 = fbm(u - scale, v - scale, 5, 11);
            float n = lx_lerpf(lx_lerpf(n00, n10, wx), lx_lerpf(n01, n11, wx), wy);
            if (kind == 1) {   /* rock: cracked plates */
                float r00 = ridged(u * 0.7f, v * 0.7f, 4, 5), r10 = ridged((u - scale) * 0.7f, v * 0.7f, 4, 5);
                float r01 = ridged(u * 0.7f, (v - scale) * 0.7f, 4, 5), r11 = ridged((u - scale) * 0.7f, (v - scale) * 0.7f, 4, 5);
                n = n * 0.5f + (lx_lerpf(lx_lerpf(r00, r10, wx), lx_lerpf(r01, r11, wx), wy) - 0.5f) * 1.4f;
            }
            float t = lx_clampf(0.5f + n * contrast, 0, 1);
            hf[y * S + x] = t;
            LxVec3 c = lx_lerp3(c0, c1, t);
            if (kind == 0) c = lx_lerp3(c, lx_v3(0.30f, 0.30f, 0.12f), smooth01(0.75f, 0.95f, hashf(x / 3, y / 3, 1)) * 0.5f);   /* dry blades */
            sum[0] += c.x; sum[1] += c.y; sum[2] += c.z;
            uint8_t *pa = &a[((size_t)y * S + x) * 4];
            for (int k = 0; k < 3; k++) pa[k] = (uint8_t)(255.0f * powf(lx_clampf((&c.x)[k], 0, 1), 1.0f / 2.2f) + 0.5f);   /* stored sRGB */
            pa[3] = (uint8_t)(255.0f * lx_clampf(rough - 0.1f * t, 0, 1));
        }
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float hl = hf[y * S + (x + S - 1) % S], hr = hf[y * S + (x + 1) % S], hd = hf[((y + S - 1) % S) * S + x], hu = hf[((y + 1) % S) * S + x];
            float k = kind == 1 ? 6.0f : 3.0f;
            LxVec3 n = lx_norm3(lx_v3((hl - hr) * k, (hd - hu) * k, 1.0f));
            uint8_t *pb = &b[((size_t)y * S + x) * 4];
            pb[0] = (uint8_t)(255.0f * (n.x * 0.5f + 0.5f));
            pb[1] = (uint8_t)(255.0f * (n.y * 0.5f + 0.5f));
            pb[2] = (uint8_t)(255.0f * hf[y * S + x]);
            pb[3] = (uint8_t)(255.0f * (0.75f + 0.25f * hf[y * S + x]));
        }
    lx_free(hf);
    double inv = 1.0 / ((double)S * S);
    *avg = lx_v3((float)(sum[0] * inv), (float)(sum[1] * inv), (float)(sum[2] * inv));
}
static void sample_tex(const LxTexture *t, float u, float v, float out[4]) {
    float fx = u * t->width - 0.5f, fy = v * t->height - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - x0, ty = fy - y0;
    for (int c = 0; c < 4; c++) out[c] = 0;
    for (int k = 0; k < 4; k++) {
        int x = posmod(x0 + (k & 1), t->width), y = posmod(y0 + (k >> 1), t->height);
        float w = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
        const uint8_t *p = &t->pixels[((size_t)y * t->width + x) * 4];
        for (int c = 0; c < 4; c++) out[c] += p[c] * w;
    }
}
static void layer_from_material(const LxMaterial *m, uint8_t *a, uint8_t *b, int S, LxVec3 *avg) {
    LxTexture *ta = m->albedo_map[0] ? lx_asset_texture(m->albedo_map) : NULL;
    LxTexture *tn = m->normal_map[0] ? lx_asset_texture(m->normal_map) : NULL;
    LxTexture *tr = m->roughness_map[0] ? lx_asset_texture(m->roughness_map) : NULL;
    LxTexture *th = m->height_map[0] ? lx_asset_texture(m->height_map) : NULL;
    LxTexture *to = m->ao_map[0] ? lx_asset_texture(m->ao_map) : NULL;
    double sum[3] = {0, 0, 0};
    /* sRGB albedo: tint by the material's colour in linear, store sRGB again */
    LxVec3 tint = lx_v3(m->albedo.x, m->albedo.y, m->albedo.z);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float u = ((float)x + 0.5f) / S, v = ((float)y + 0.5f) / S;
            float pa[4] = {255, 255, 255, 255}, pn[4] = {128, 128, 255, 255}, pr[4] = {255, 255, 255, 255}, ph[4] = {128, 128, 128, 255}, po[4] = {255, 255, 255, 255};
            if (ta && ta->pixels) sample_tex(ta, u, v, pa);
            if (tn && tn->pixels) sample_tex(tn, u, v, pn);
            if (tr && tr->pixels) sample_tex(tr, u, v, pr);
            if (th && th->pixels) sample_tex(th, u, v, ph);
            if (to && to->pixels) sample_tex(to, u, v, po);
            uint8_t *qa = &a[((size_t)y * S + x) * 4], *qb = &b[((size_t)y * S + x) * 4];
            float lum = 0;
            for (int c = 0; c < 3; c++) {
                float lin = powf(pa[c] / 255.0f, 2.2f) * (&tint.x)[c];
                sum[c] += lin;
                lum += lin / 3.0f;
                qa[c] = (uint8_t)(255.0f * powf(lx_clampf(lin, 0, 1), 1.0f / 2.2f) + 0.5f);
            }
            float r = m->roughness * (tr ? pr[1] / 255.0f : 1.0f);
            if (tr && m->roughness_is_smoothness) r = m->roughness * (1.0f - pr[1] / 255.0f);
            qa[3] = (uint8_t)(255.0f * lx_clampf(r, 0.04f, 1));
            float ny = pn[1];
            if (m->normal_flip_green) ny = 255.0f - ny;
            qb[0] = (uint8_t)pn[0];
            qb[1] = (uint8_t)ny;
            qb[2] = (uint8_t)(th ? ph[0] : 255.0f * lx_clampf(powf(lum, 0.45f), 0, 1));   /* no height map: brighter is higher */
            qb[3] = (uint8_t)(to ? 255.0f - (255.0f - po[0]) * lx_clampf(m->ao_strength, 0, 1) : 255.0f);
        }
    double inv = 1.0 / ((double)S * S);
    *avg = lx_v3((float)(sum[0] * inv), (float)(sum[1] * inv), (float)(sum[2] * inv));
}
static uint64_t hash_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static void refresh_layers(TData *d) {
    const LxTerrainComponent *t = &d->set;
    int count = t->layer_count < 1 ? 1 : t->layer_count > NL ? NL : t->layer_count;
    uint64_t h = 1469598103934665603ull;
    h = hash_bytes(h, &count, sizeof count);
    for (int l = 0; l < count; l++) {
        h = hash_bytes(h, t->layers[l].material, strlen(t->layers[l].material));
        h = hash_bytes(h, t->layers[l].texture, strlen(t->layers[l].texture));
        h = hash_bytes(h, t->layers[l].normal_map, strlen(t->layers[l].normal_map));
        if (t->layers[l].texture[0]) {   /* an image loads in the background: look again once it's there */
            LxTexture *tx = lx_asset_texture(t->layers[l].texture);
            h = hash_bytes(h, &(int){tx && tx->pixels ? tx->width : 0}, sizeof(int));
            continue;
        }
        LxMaterial *m = t->layers[l].material[0] && strncmp(t->layers[l].material, "builtin:terrain_", 16) ? lx_asset_material(t->layers[l].material) : NULL;
        if (m) h = hash_bytes(h, m, sizeof *m);   /* a material edited in its inspector shows at once */
    }
    for (int l = 0; l < NL; l++) d->gpu.tiling[l] = l < count && t->layers[l].tiling > 0.05f ? t->layers[l].tiling : 4.0f;
    if (h == d->layers_hash && d->gpu_a) return;
    d->layers_hash = h;
    int S = TEX;
    lx_free(d->gpu_a); lx_free(d->gpu_b);
    d->gpu_a = lx_alloc((size_t)count * S * S * 4);
    d->gpu_b = lx_alloc((size_t)count * S * S * 4);
    LxVec3 total = lx_v3(0, 0, 0);
    for (int l = 0; l < count; l++) {
        const char *ref = t->layers[l].material;
        uint8_t *a = d->gpu_a + (size_t)l * S * S * 4, *b = d->gpu_b + (size_t)l * S * S * 4;
        LxVec3 avg;
        if (t->layers[l].texture[0]) {   /* just an image */
            LxMaterial m = lx_material_default();
            lx_strcpy(m.albedo_map, sizeof m.albedo_map, t->layers[l].texture);
            lx_strcpy(m.normal_map, sizeof m.normal_map, t->layers[l].normal_map);
            m.roughness = 0.92f;
            layer_from_material(&m, a, b, S, &avg);
        } else if (!ref[0] || !strncmp(ref, "builtin:terrain_", 16)) layer_procedural(ref[0] ? ref : "grass", a, b, S, &avg);
        else layer_from_material(lx_asset_material(ref), a, b, S, &avg);
        total = lx_add3(total, l == 0 ? lx_scale3(avg, 2) : avg);
    }
    d->gpu.count = count;
    d->gpu.size = S;
    d->gpu.a = d->gpu_a;
    d->gpu.b = d->gpu_b;
    d->gpu.version++;
    /* the terrain's material: what GI and baking see is the layers' average colour */
    d->material = lx_material_default();
    snprintf(d->material.name, sizeof d->material.name, "Terrain %d", d->id);
    LxVec3 a = lx_scale3(total, 1.0f / (float)(count + 1));
    d->material.albedo = lx_v4(a.x, a.y, a.z, 1);
    d->material.roughness = 1;
    d->material.metallic = 0;
    d->material.acoustic_absorption = 0.3f;
}
static void refresh_grass_material(TData *d) {
    LxVec3 g = d->set.grass_color;
    d->grass_material = lx_material_default();
    snprintf(d->grass_material.name, sizeof d->grass_material.name, "Terrain %d grass", d->id);
    d->grass_material.albedo = lx_v4(g.x, g.y, g.z, 1);
    d->grass_material.roughness = 0.75f;
    snprintf(d->grass_material.albedo_map, sizeof d->grass_material.albedo_map, "terrain:%d/grass_tex", d->id);
    if (!d->grass_tex.pixels) {   /* root to tip: dark to light */
        d->grass_tex.width = 4;
        d->grass_tex.height = 64;
        d->grass_tex.pixels = lx_alloc(4 * 64 * 4);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 4; x++) {
                float t = (float)y / 63.0f, k = 0.35f + 0.65f * powf(t, 0.7f);
                uint8_t *p = &d->grass_tex.pixels[(y * 4 + x) * 4];
                p[0] = p[1] = p[2] = (uint8_t)(255.0f * powf(k, 1.0f / 2.2f));
                p[3] = 255;
            }
        snprintf(d->grass_tex.ref, sizeof d->grass_tex.ref, "terrain:%d/grass_tex", d->id);
    }
}

const LxTerrainGpuLayers *lx_terrain_gpu_layers(const LxMaterial *m) {
    for (int i = 0; i < g_ndata; i++) if (m == &g_data[i]->material && g_data[i]->gpu_a) return &g_data[i]->gpu;
    return NULL;
}

/* "terrain:<id>/<x>_<z>[/far|/grass]" and "terrain:<id>[/grass]" */
static TData *data_by_ref(const char *ref, const char **rest) {
    if (strncmp(ref, "terrain:", 8) != 0) return NULL;
    int id = atoi(ref + 8);
    const char *p = strchr(ref + 8, '/');
    *rest = p ? p + 1 : "";
    for (int i = 0; i < g_ndata; i++) if (g_data[i]->id == id) return g_data[i];
    return NULL;
}
LxMesh *lx_terrain_mesh_ref(const char *ref) {
    const char *rest;
    TData *d = data_by_ref(ref, &rest);
    if (!d) return NULL;
    int cx, cz;
    if (sscanf(rest, "%d_%d", &cx, &cz) != 2) return NULL;
    Chunk *c = find_chunk(d, cx, cz);
    if (!c) return NULL;
    if (strstr(rest, "/grass")) return c->grass;
    if (strstr(rest, "/far")) return c->far_mesh;
    return c->mesh ? c->mesh : c->far_mesh;
}
LxMaterial *lx_terrain_material_ref(const char *ref) {
    const char *rest;
    TData *d = data_by_ref(ref, &rest);
    if (!d) return NULL;
    return strstr(rest, "grass") ? &d->grass_material : &d->material;
}
LxTexture *lx_terrain_texture_ref(const char *ref) {
    const char *rest;
    TData *d = data_by_ref(ref, &rest);
    return d && strstr(rest, "grass_tex") ? &d->grass_tex : NULL;
}

/* ------------------------------------------------------------------ component */
LxTerrainComponent *lx_get_terrain(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && (d->comps & LX_COMP_TERRAIN) ? d->terrain : NULL;
}
void lx_terrain_component_free(LxTerrainComponent *t) { lx_free(t); }

static void default_layers(LxTerrainComponent *t) {
    static const char *M[4] = {"builtin:terrain_grass", "builtin:terrain_rock", "builtin:terrain_dirt", "builtin:terrain_snow"};
    t->layer_count = 4;
    for (int l = 0; l < 4; l++) {
        memset(&t->layers[l], 0, sizeof t->layers[l]);
        lx_strcpy(t->layers[l].material, sizeof t->layers[l].material, M[l]);
        t->layers[l].tiling = l == 1 ? 8.0f : 4.0f;
    }
    t->layers[1].slope_min = 38; t->layers[1].slope_max = 90;      /* rock on steep slopes */
    t->layers[3].height_min = 180; t->layers[3].height_max = 10000; /* snow up high */
}
/* the component with its defaults, and nothing else (scene loading fills it in) */
LxTerrainComponent *lx_terrain_component_ensure(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    if (!(d->comps & LX_COMP_TERRAIN) || !d->terrain) {
        LxTerrainComponent *t = lx_calloc(1, sizeof *t);
        t->chunk_size = 64;
        t->resolution = 129;
        t->load_radius = 320;
        t->lod_distance = 1;
        default_layers(t);
        t->grass_layer = 0;
        t->grass_density = 3;
        t->grass_height = 0.45f;
        t->grass_color = lx_v3(0.32f, 0.5f, 0.16f);
        t->grass_distance = 60;
        t->edit_layer_count = 1;
        lx_strcpy(t->edit_layers[0].name, sizeof t->edit_layers[0].name, "Base");
        t->edit_layers[0].visible = true;
        t->edit_layers[0].strength = 1;
        lx_free(d->terrain);
        d->terrain = t;
    }
    d->comps |= LX_COMP_TERRAIN;
    return d->terrain;
}
LxTerrainComponent *lx_add_terrain(LxScene *s, LxEntity e, const char *folder) {
    LxTerrainComponent *t = lx_terrain_component_ensure(s, e);
    if (!t) return NULL;
    LxEntityData *d = lx_entity_data(s, e);
    if (folder && folder[0]) lx_strcpy(t->folder, sizeof t->folder, folder);
    if (!t->folder[0]) {
        char name[LX_NAME_MAX];
        lx_strcpy(name, sizeof name, d->name[0] ? d->name : "Terrain");
        for (char *p = name; *p; p++) if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '_';
        snprintf(t->folder, sizeof t->folder, "Terrains/%s", name);
    }
    TData *td = data_for(t);
    if (!td->nch) {   /* a new terrain: one flat chunk, centred on the object */
        stroke_begin(td);
        touch(td, 0, 0);
        Chunk c;
        memset(&c, 0, sizeof c);
        LX_VEC_PUSH(td->ch, td->nch, td->cch, c);
        chunk_alloc(td, &td->ch[td->nch - 1]);
        composite(td, &td->ch[td->nch - 1]);
        update_far(td, &td->ch[td->nch - 1]);
        td->ch[td->nch - 1].dirty_file = td->ch[td->nch - 1].dirty_mesh = true;
        td->index_dirty = true;
        stroke_end(td);
        t->revision = td->rev;
    }
    return t;
}
LxEntity lx_terrain_of(LxScene *s, LxEntity e) {
    for (int guard = 0; e && guard < 64; guard++) {
        LxEntityData *d = lx_entity_data(s, e);
        if (!d) return 0;
        if ((d->comps & LX_COMP_TERRAIN) && d->terrain && !d->generated) return e;
        if (!d->generated) return 0;
        e = d->parent;
    }
    return 0;
}

/* the terrain's data and its local frame */
typedef struct Ctx { LxScene *s; LxEntity e; LxTerrainComponent *t; TData *d; LxMat4 world, inv; } Ctx;
static bool ctx_of(LxScene *s, LxEntity e, Ctx *c) {
    LxTerrainComponent *t = lx_get_terrain(s, e);
    if (!t) return false;
    c->s = s; c->e = e; c->t = t;
    c->d = data_for(t);
    c->d->set = *t;
    lx_scene_update_transforms(s);
    c->world = lx_entity_data(s, e)->world;
    c->inv = lx_mat4_inverse(c->world);
    return true;
}

/* ------------------------------------------------------------------ queries */
static bool local_height(TData *d, float x, float z, float *y) {
    int N = N_(d);
    float cs = cell(d);
    float gxf = x / cs, gzf = z / cs;
    int gx = (int)floorf(gxf), gz = (int)floorf(gzf);
    int cx = floordiv(gx, N), cz = floordiv(gz, N);
    Chunk *c = find_chunk(d, cx, cz);
    if (!c) return false;
    int ix = gx - cx * N, iz = gz - cz * N;
    if (c->holes && c->loaded && c->holes[iz * N + ix]) return false;
    if (c->loaded) { *y = chunk_height(d, c, x - (float)cx * d->size, z - (float)cz * d->size); return true; }
    /* simplified copy */
    float fx = (x - (float)cx * d->size) / d->size * (FAR - 1), fz = (z - (float)cz * d->size) / d->size * (FAR - 1);
    int x0 = lx_clampi((int)fx, 0, FAR - 2), z0 = lx_clampi((int)fz, 0, FAR - 2);
    float tx = lx_clampf(fx - x0, 0, 1), tz = lx_clampf(fz - z0, 0, 1);
    const float *h = c->far_h;
    *y = lx_lerpf(lx_lerpf(h[z0 * FAR + x0], h[z0 * FAR + x0 + 1], tx), lx_lerpf(h[(z0 + 1) * FAR + x0], h[(z0 + 1) * FAR + x0 + 1], tx), tz);
    return true;
}
bool lx_terrain_sample(LxScene *s, float x, float z, float *y, LxVec3 *normal, int *layer) {
    if (!s) return false;
    for (int i = 1; i < s->count; i++) {
        LxEntityData *ed = &s->entities[i];
        if (!ed->alive || !ed->active || ed->generated || !(ed->comps & LX_COMP_TERRAIN) || !ed->terrain) continue;
        Ctx c;
        if (!ctx_of(s, lx_entity_make((uint32_t)i, ed->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u)), &c)) continue;
        LxVec3 lp = lx_mat4_point(c.inv, lx_v3(x, 0, z));
        float ly;
        if (!local_height(c.d, lp.x, lp.z, &ly)) continue;
        LxVec3 wp = lx_mat4_point(c.world, lx_v3(lp.x, ly, lp.z));
        if (y) *y = wp.y;
        if (normal || layer) {
            float e = cell(c.d), hx0 = ly, hx1 = ly, hz0 = ly, hz1 = ly;
            local_height(c.d, lp.x - e, lp.z, &hx0); local_height(c.d, lp.x + e, lp.z, &hx1);
            local_height(c.d, lp.x, lp.z - e, &hz0); local_height(c.d, lp.x, lp.z + e, &hz1);
            LxVec3 n = lx_norm3(lx_v3(hx0 - hx1, 2 * e, hz0 - hz1));
            if (normal) *normal = lx_norm3(lx_mat4_dir(c.world, n));
            if (layer) {
                *layer = 0;
                int N = N_(c.d);
                int cx = floordiv((int)floorf(lp.x / e), N), cz = floordiv((int)floorf(lp.z / e), N);
                Chunk *ch = find_chunk(c.d, cx, cz);
                if (ch && ch->loaded) {
                    float best = -1;
                    LxVec3 org = lx_mat4_point(c.world, lx_v3(0, 0, 0));
                    for (int l = 0; l < NL; l++) {
                        float w = chunk_weight(c.d, ch, lp.x - (float)cx * c.d->size, lp.z - (float)cz * c.d->size, l, org);
                        if (w > best) { best = w; *layer = l; }
                    }
                }
            }
        }
        return true;
    }
    return false;
}
bool lx_terrain_height(LxScene *s, float x, float z, float *y) { return lx_terrain_sample(s, x, z, y, NULL, NULL); }
bool lx_terrain_raycast(LxScene *s, LxEntity e, LxRay ray, float max_dist, LxVec3 *hit) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return false;
    LxVec3 o = lx_mat4_point(c.inv, ray.origin), dir = lx_norm3(lx_mat4_dir(c.inv, ray.dir));
    float step = cell(c.d) * 0.5f, t = 0, prev_t = 0;
    bool prev_above = true, have_prev = false;
    /* only march where the land is: the box around every chunk and its heights */
    if (c.d->nch == 0) return false;
    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < c.d->nch; i++) {
        const Chunk *ch = &c.d->ch[i];
        bmin[0] = lx_minf(bmin[0], (float)ch->cx * c.d->size); bmax[0] = lx_maxf(bmax[0], (float)(ch->cx + 1) * c.d->size);
        bmin[2] = lx_minf(bmin[2], (float)ch->cz * c.d->size); bmax[2] = lx_maxf(bmax[2], (float)(ch->cz + 1) * c.d->size);
        if (ch->hmax >= ch->hmin) { bmin[1] = lx_minf(bmin[1], ch->hmin); bmax[1] = lx_maxf(bmax[1], ch->hmax); }
    }
    if (bmax[1] < bmin[1]) { bmin[1] = 0; bmax[1] = 0; }
    bmin[1] -= 1; bmax[1] += 1;
    float t0 = 0, t1 = max_dist;
    const float *po = &o.x, *pd = &dir.x;
    for (int a = 0; a < 3; a++) {
        if (fabsf(pd[a]) < 1e-9f) {
            if (po[a] < bmin[a] || po[a] > bmax[a]) return false;
            continue;
        }
        float ta = (bmin[a] - po[a]) / pd[a], tb = (bmax[a] - po[a]) / pd[a];
        if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
        t0 = lx_maxf(t0, ta);
        t1 = lx_minf(t1, tb);
        if (t0 > t1) return false;
    }
    t = prev_t = t0;
    max_dist = t1 + step;
    for (int k = 0; k < 200000 && t <= max_dist; k++) {
        LxVec3 p = lx_madd3(o, dir, t);
        float h;
        if (local_height(c.d, p.x, p.z, &h)) {
            bool above = p.y >= h;
            if (have_prev && prev_above && !above) {   /* crossed: refine */
                float a = prev_t, b = t;
                for (int i = 0; i < 24; i++) {
                    float m = 0.5f * (a + b);
                    LxVec3 q = lx_madd3(o, dir, m);
                    float hq;
                    if (local_height(c.d, q.x, q.z, &hq) && q.y < hq) b = m; else a = m;
                }
                LxVec3 q = lx_madd3(o, dir, b);
                if (hit) *hit = lx_mat4_point(c.world, q);
                return true;
            }
            prev_above = above;
            have_prev = true;
        } else have_prev = false;
        prev_t = t;
        t += lx_maxf(step, t * 0.002f);
    }
    return false;
}
bool lx_terrain_has_chunk(LxScene *s, LxEntity e, int cx, int cz) {
    Ctx c;
    return ctx_of(s, e, &c) && find_chunk(c.d, cx, cz) != NULL;
}
int lx_terrain_chunks(LxScene *s, LxEntity e, int *xz, int max) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return 0;
    int n = lx_mini(max, c.d->nch);
    for (int i = 0; i < n; i++) { xz[i * 2] = c.d->ch[i].cx; xz[i * 2 + 1] = c.d->ch[i].cz; }
    return c.d->nch;
}
int lx_terrain_chunk_count(LxScene *s, LxEntity e) {
    Ctx c;
    return ctx_of(s, e, &c) ? c.d->nch : 0;
}
int lx_terrain_loaded_count(LxScene *s, LxEntity e) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return 0;
    int n = 0;
    for (int i = 0; i < c.d->nch; i++) n += c.d->ch[i].loaded;
    return n;
}
bool lx_terrain_chunk_at(LxScene *s, LxEntity e, LxVec3 world, int *cx, int *cz) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return false;
    LxVec3 p = lx_mat4_point(c.inv, world);
    *cx = (int)floorf(p.x / c.d->size);
    *cz = (int)floorf(p.z / c.d->size);
    return true;
}

/* ------------------------------------------------------------------ editing */
static void mark_scenes_static(TData *d) {
    for (int i = 0; i < g_nviews; i++)
        if (g_views[i].used && g_views[i].data == d) lx_scene_mark_static_dirty(g_views[i].scene);
}
void lx_terrain_begin_stroke(LxScene *s, LxEntity e) {
    Ctx c;
    if (ctx_of(s, e, &c)) stroke_begin(c.d);
}
void lx_terrain_end_stroke(LxScene *s, LxEntity e) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return;
    if (stroke_end(c.d)) {
        c.t->revision = c.d->rev;
        for (int i = 0; i < c.d->nch; i++) if (c.d->ch[i].loaded && c.d->ch[i].dirty_far) update_far(c.d, &c.d->ch[i]);
        mark_scenes_static(c.d);
    }
}
/* auto-stroke for one-off calls made outside begin/end */
static bool auto_begin(TData *d) {
    if (d->stroking) return false;
    stroke_begin(d);
    return true;
}
static void auto_end(Ctx *c, bool began) {
    if (began) lx_terrain_end_stroke(c->s, c->e);
}

bool lx_terrain_add_chunk(LxScene *s, LxEntity e, int cx, int cz) {
    Ctx c;
    if (!ctx_of(s, e, &c) || find_chunk(c.d, cx, cz)) return false;
    TData *d = c.d;
    bool began = auto_begin(d);
    touch(d, cx, cz);
    Chunk nc;
    memset(&nc, 0, sizeof nc);
    nc.cx = cx; nc.cz = cz;
    LX_VEC_PUSH(d->ch, d->nch, d->cch, nc);
    Chunk *ch = &d->ch[d->nch - 1];
    chunk_alloc(d, ch);
    /* fill it from its neighbours : each edge takes its neighbour's edge, the inside
       blends between them, so the new chunk joins without a step */
    int R = d->R, N = N_(d);
    Chunk *nb[4] = {ensure_loaded(d, cx - 1, cz), ensure_loaded(d, cx + 1, cz), ensure_loaded(d, cx, cz - 1), ensure_loaded(d, cx, cz + 1)};
    ch = find_chunk(d, cx, cz);   /* (loading may have moved it) */
    for (int j = 0; j < R; j++)
        for (int i = 0; i < R; i++) {
            float sum = 0, wsum = 0;
            float u = (float)i / N, v = (float)j / N;
            if (nb[0]) { float w = powf(1 - u, 2) + 1e-4f; sum += nb[0]->h[j * R + N] * w; wsum += w; }
            if (nb[1]) { float w = powf(u, 2) + 1e-4f; sum += nb[1]->h[j * R + 0] * w; wsum += w; }
            if (nb[2]) { float w = powf(1 - v, 2) + 1e-4f; sum += nb[2]->h[N * R + i] * w; wsum += w; }
            if (nb[3]) { float w = powf(v, 2) + 1e-4f; sum += nb[3]->h[0 * R + i] * w; wsum += w; }
            ch->layer[0][j * R + i] = wsum > 0 ? sum / wsum : 0.0f;
            /* paint: the nearest neighbour's edge paint */
            const uint8_t *src = NULL;
            float best = 1e9f;
            if (nb[0] && u < best) { best = u; src = &nb[0]->w[((size_t)j * R + N) * NL]; }
            if (nb[1] && 1 - u < best) { best = 1 - u; src = &nb[1]->w[((size_t)j * R + 0) * NL]; }
            if (nb[2] && v < best) { best = v; src = &nb[2]->w[((size_t)N * R + i) * NL]; }
            if (nb[3] && 1 - v < best) { best = 1 - v; src = &nb[3]->w[((size_t)0 * R + i) * NL]; }
            if (src) memcpy(&ch->w[((size_t)j * R + i) * NL], src, NL);
        }
    /* the shared edges themselves must match exactly */
    if (nb[0]) for (int j = 0; j < R; j++) ch->layer[0][j * R + 0] = nb[0]->h[j * R + N];
    if (nb[1]) for (int j = 0; j < R; j++) ch->layer[0][j * R + N] = nb[1]->h[j * R + 0];
    if (nb[2]) for (int i = 0; i < R; i++) ch->layer[0][0 * R + i] = nb[2]->h[N * R + i];
    if (nb[3]) for (int i = 0; i < R; i++) ch->layer[0][N * R + i] = nb[3]->h[0 * R + i];
    /* edit layers: copy the neighbours' edge offsets the same way, so hiding a layer keeps the edges joined */
    for (int l = 1; l < NE; l++) {
        bool any = false;
        for (int k = 0; k < 4; k++) any |= nb[k] && nb[k]->layer[l];
        if (!any) continue;
        size_t n = (size_t)R * R;
        if (!ch->layer[l]) ch->layer[l] = lx_calloc(n, sizeof(float));
        if (nb[0] && nb[0]->layer[l]) for (int j = 0; j < R; j++) ch->layer[l][j * R] = nb[0]->layer[l][j * R + N];
        if (nb[1] && nb[1]->layer[l]) for (int j = 0; j < R; j++) ch->layer[l][j * R + N] = nb[1]->layer[l][j * R];
        if (nb[2] && nb[2]->layer[l]) for (int i = 0; i < R; i++) ch->layer[l][i] = nb[2]->layer[l][N * R + i];
        if (nb[3] && nb[3]->layer[l]) for (int i = 0; i < R; i++) ch->layer[l][N * R + i] = nb[3]->layer[l][i];
    }
    /* the Base keeps the edges where the visible heights meet: subtract what the edit layers add there */
    for (int l = 1; l < NE && l < d->set.edit_layer_count; l++) {
        if (!ch->layer[l] || !d->set.edit_layers[l].visible) continue;
        float k = d->set.edit_layers[l].strength;
        for (int j = 0; j < R; j++)
            for (int i = 0; i < R; i++)
                if (i == 0 || j == 0 || i == N || j == N) ch->layer[0][j * R + i] -= ch->layer[l][j * R + i] * k;
    }
    composite(d, ch);
    update_far(d, ch);
    ch->dirty_file = ch->dirty_mesh = ch->dirty_grass = ch->dirty_scatter = true;
    /* neighbours' normals along the shared edges change */
    for (int k = 0; k < 4; k++) if (nb[k]) nb[k]->dirty_mesh = true;
    d->index_dirty = true;
    auto_end(&c, began);
    return true;
}
bool lx_terrain_remove_chunk(LxScene *s, LxEntity e, int cx, int cz) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return false;
    Chunk *ch = find_chunk(c.d, cx, cz);
    if (!ch || c.d->nch <= 1) return false;   /* a terrain keeps at least one chunk */
    bool began = auto_begin(c.d);
    touch(c.d, cx, cz);
    remove_chunk_raw(c.d, find_chunk(c.d, cx, cz));
    for (int k = 0; k < 4; k++) {
        Chunk *nb = find_chunk(c.d, cx + (k == 0 ? -1 : k == 1 ? 1 : 0), cz + (k == 2 ? -1 : k == 3 ? 1 : 0));
        if (nb) nb->dirty_mesh = true;
    }
    auto_end(&c, began);
    return true;
}

/* a brush works on the terrain's lattice of points as one surface: it reads what shows (composite heights) from
 * before it changes anything, works out each point's change, then adds it to the active edit layer in every copy */
typedef struct Area { int gx0, gz0, w, h; float *val; bool *has; } Area;
static void area_gather(TData *d, float cx, float cz, float r, Area *a) {
    float cs = cell(d);
    a->gx0 = (int)floorf((cx - r) / cs) - 1;
    a->gz0 = (int)floorf((cz - r) / cs) - 1;
    a->w = (int)ceilf((cx + r) / cs) + 2 - a->gx0;
    a->h = (int)ceilf((cz + r) / cs) + 2 - a->gz0;
    a->val = lx_alloc(sizeof(float) * (size_t)a->w * a->h);
    a->has = lx_calloc((size_t)a->w * a->h, 1);
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            Copy cp[4];
            int n = copies_of(d, a->gx0 + i, a->gz0 + j, cp, true);
            if (n) { a->val[j * a->w + i] = cp[0].c->h[cp[0].j * d->R + cp[0].i]; a->has[j * a->w + i] = true; }
        }
}
static void area_free(Area *a) { lx_free(a->val); lx_free(a->has); }
static void apply_delta(TData *d, int gx, int gz, float delta) {
    if (delta == 0) return;
    Copy cp[4];
    int n = copies_of(d, gx, gz, cp, true);
    int L = lx_clampi(d->set.active_edit_layer, 0, lx_maxi(d->set.edit_layer_count, 1) - 1);
    if (L > 0 && !d->set.edit_layers[L].visible) return;   /* a hidden layer isn't sculpted blind */
    float k = L > 0 ? lx_maxf(d->set.edit_layers[L].strength, 0.05f) : 1.0f;
    for (int q = 0; q < n; q++) {
        Chunk *c = cp[q].c;
        touch(d, c->cx, c->cz);
        if (!c->layer[L]) c->layer[L] = lx_calloc((size_t)d->R * d->R, sizeof(float));
        int at = cp[q].j * d->R + cp[q].i;
        c->layer[L][at] += delta / k;
        c->h[at] += delta;
        c->dirty_mesh = c->dirty_file = c->dirty_far = c->dirty_grass = c->dirty_scatter = true;
    }
}
static float falloff_at(float dist, float r, float soft) {
    if (dist >= r) return 0;
    float inner = r * (1.0f - lx_clampf(soft, 0, 1));
    if (dist <= inner) return 1;
    float t = (dist - inner) / lx_maxf(r - inner, 1e-4f);
    return 1.0f - t * t * (3 - 2 * t);
}
static void erode_area(Area *a, float cs, float amount, uint32_t seed) {
    /* hydraulic erosion: droplets run downhill picking up soil where they speed up, dropping it where they slow */
    int drops = (int)(amount * (float)a->w * (float)a->h * 0.3f) + 1;
    uint32_t r = seed * 2654435761u + 1;
    for (int k = 0; k < drops; k++) {
        r = r * 1664525u + 1013904223u;
        float x = 1 + (float)(r >> 8) / 16777216.0f * (float)(a->w - 3);
        r = r * 1664525u + 1013904223u;
        float z = 1 + (float)(r >> 8) / 16777216.0f * (float)(a->h - 3);
        float dx = 0, dz = 0, speed = 1, water = 1, sediment = 0;
        for (int step = 0; step < 40; step++) {
            int ix = (int)x, iz = (int)z;
            if (ix < 1 || iz < 1 || ix >= a->w - 2 || iz >= a->h - 2) break;
            int at = iz * a->w + ix;
            if (!a->has[at] || !a->has[at + 1] || !a->has[at + a->w] || !a->has[at + a->w + 1]) break;
            float fx = x - ix, fz = z - iz;
            float h00 = a->val[at], h10 = a->val[at + 1], h01 = a->val[at + a->w], h11 = a->val[at + a->w + 1];
            float gx = (h10 - h00) * (1 - fz) + (h11 - h01) * fz, gz = (h01 - h00) * (1 - fx) + (h11 - h10) * fx;
            float hold = h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz;
            dx = dx * 0.3f - gx * 0.7f;
            dz = dz * 0.3f - gz * 0.7f;
            float len = sqrtf(dx * dx + dz * dz);
            if (len < 1e-6f) break;
            dx /= len; dz /= len;
            x += dx; z += dz;
            int nx = (int)x, nz = (int)z;
            if (nx < 1 || nz < 1 || nx >= a->w - 2 || nz >= a->h - 2) break;
            int nat = nz * a->w + nx;
            float nfx = x - nx, nfz = z - nz;
            float hnew = a->val[nat] * (1 - nfx) * (1 - nfz) + a->val[nat + 1] * nfx * (1 - nfz) + a->val[nat + a->w] * (1 - nfx) * nfz + a->val[nat + a->w + 1] * nfx * nfz;
            float dh = hnew - hold;
            float cap = lx_maxf(-dh, 0.01f * cs) * speed * water * 4.0f;
            float w4[4] = {(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
            int idx4[4] = {at, at + 1, at + a->w, at + a->w + 1};
            if (sediment > cap || dh > 0) {   /* drop soil */
                float drop = dh > 0 ? lx_minf(dh, sediment) : (sediment - cap) * 0.3f;
                sediment -= drop;
                for (int q = 0; q < 4; q++) a->val[idx4[q]] += drop * w4[q];
            } else {                          /* pick it up */
                float take = lx_minf((cap - sediment) * 0.3f, -dh);
                for (int q = 0; q < 4; q++) a->val[idx4[q]] -= take * w4[q];
                sediment += take;
            }
            speed = sqrtf(lx_maxf(speed * speed + dh * -9.8f * 0.1f, 0.01f));
            water *= 0.97f;
        }
    }
}
void lx_terrain_brush(LxScene *s, LxEntity e, LxVec3 world, const LxTerrainBrush *b, float dt) {
    Ctx c;
    if (!ctx_of(s, e, &c) || !b || b->radius <= 0) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    LxVec3 p = lx_mat4_point(c.inv, world);
    float r = b->radius, cs = cell(d), st = lx_clampf(b->strength, 0, 1);
    float rate = lx_clampf(dt, 0, 0.1f);
    if (b->tool == LX_TERRAIN_PAINT || b->tool == LX_TERRAIN_HOLE || b->tool == LX_TERRAIN_FILL_HOLE) {
        int gx0 = (int)floorf((p.x - r) / cs), gx1 = (int)ceilf((p.x + r) / cs);
        int gz0 = (int)floorf((p.z - r) / cs), gz1 = (int)ceilf((p.z + r) / cs);
        int L = lx_clampi(b->layer, 0, NL - 1);
        for (int gz = gz0; gz <= gz1; gz++)
            for (int gx = gx0; gx <= gx1; gx++) {
                float f = falloff_at(sqrtf(powf((float)gx * cs - p.x, 2) + powf((float)gz * cs - p.z, 2)), r, b->falloff);
                if (f <= 0) continue;
                if (b->tool == LX_TERRAIN_PAINT) {
                    Copy cp[4];
                    int n = copies_of(d, gx, gz, cp, true);
                    for (int q = 0; q < n; q++) {
                        Chunk *ch = cp[q].c;
                        touch(d, ch->cx, ch->cz);
                        uint8_t *w = &ch->w[((size_t)cp[q].j * d->R + cp[q].i) * NL];
                        float add = f * st * rate * 6.0f * 255.0f;
                        float cur[NL];
                        for (int l = 0; l < NL; l++) cur[l] = w[l];
                        cur[L] = lx_minf(255.0f, cur[L] + add);
                        float others = 0;
                        for (int l = 0; l < NL; l++) if (l != L) others += cur[l];
                        float room = 255.0f - cur[L];
                        for (int l = 0; l < NL; l++) if (l != L) cur[l] = others > 0 ? cur[l] * room / others : 0;
                        int acc = 0;
                        for (int l = 0; l < NL; l++) { w[l] = (uint8_t)lx_clampf(cur[l] + 0.5f, 0, 255); acc += w[l]; }
                        w[L] = (uint8_t)lx_clampi((int)w[L] + (255 - acc), 0, 255);   /* exactly 255 */
                        ch->dirty_mesh = ch->dirty_file = ch->dirty_far = ch->dirty_grass = ch->dirty_scatter = true;
                    }
                } else {   /* holes: the cell to the +x +z of a point */
                    int N = N_(d);
                    int cx = floordiv(gx, N), cz = floordiv(gz, N);
                    Chunk *ch = ensure_loaded(d, cx, cz);
                    if (!ch || f < 0.5f) continue;
                    touch(d, cx, cz);
                    uint8_t *hc = &ch->holes[(gz - cz * N) * N + (gx - cx * N)];
                    uint8_t want = b->tool == LX_TERRAIN_HOLE;
                    if (*hc != want) { *hc = want; ch->dirty_mesh = ch->dirty_file = ch->dirty_grass = ch->dirty_scatter = true; }
                }
            }
        auto_end(&c, began);
        return;
    }
    Area a;
    area_gather(d, p.x, p.z, r, &a);
    float *before = NULL;
    if (b->tool == LX_TERRAIN_ERODE) {
        before = lx_alloc(sizeof(float) * (size_t)a.w * a.h);
        memcpy(before, a.val, sizeof(float) * (size_t)a.w * a.h);
        erode_area(&a, cs, st * rate * 8.0f, (uint32_t)(lx_time() * 1000.0));
    }
    float target = lx_mat4_point(c.inv, lx_v3(world.x, b->target, world.z)).y;
    float speed = r * 0.6f + 2.0f;   /* metres a second at full strength */
    for (int j = 1; j < a.h - 1; j++)
        for (int i = 1; i < a.w - 1; i++) {
            int at = j * a.w + i;
            if (!a.has[at]) continue;
            int gx = a.gx0 + i, gz = a.gz0 + j;
            float f = falloff_at(sqrtf(powf((float)gx * cs - p.x, 2) + powf((float)gz * cs - p.z, 2)), r, b->falloff);
            if (f <= 0) continue;
            float h = a.val[at], delta = 0;
            switch (b->tool) {
            case LX_TERRAIN_RAISE: delta = speed * st * rate * f; break;
            case LX_TERRAIN_LOWER: delta = -speed * st * rate * f; break;
            case LX_TERRAIN_SMOOTH: {
                float sum = 0, n = 0;
                for (int dz = -2; dz <= 2; dz++)
                    for (int dx = -2; dx <= 2; dx++) {
                        int q = (j + dz) * a.w + i + dx;
                        if (i + dx < 0 || i + dx >= a.w || j + dz < 0 || j + dz >= a.h || !a.has[q]) continue;
                        float wgt = 1.0f / (1.0f + (float)(dx * dx + dz * dz));
                        sum += a.val[q] * wgt; n += wgt;
                    }
                delta = (sum / n - h) * lx_minf(1.0f, st * rate * 12.0f) * f;
                break;
            }
            case LX_TERRAIN_FLATTEN: delta = (target - h) * lx_minf(1.0f, st * rate * 8.0f) * f; break;
            case LX_TERRAIN_SET_HEIGHT: delta = (target - h) * lx_minf(1.0f, st * rate * 30.0f) * f; break;
            case LX_TERRAIN_NOISE: delta = fbm((float)gx * cs / 9.0f, (float)gz * cs / 9.0f, 4, 7) * speed * 0.6f * st * rate * f; break;
            case LX_TERRAIN_TERRACE: {
                float sp = b->spacing > 0.1f ? b->spacing : 4.0f;
                float step = floorf(h / sp + 0.5f) * sp;
                float frac = (h - step) / sp;   /* -0.5..0.5: a steep riser between flat treads */
                float shaped = step + sp * 0.5f * (frac > 0 ? powf(frac * 2, 3) : -powf(-frac * 2, 3)) * 0.5f;
                delta = (shaped - h) * lx_minf(1.0f, st * rate * 8.0f) * f;
                break;
            }
            case LX_TERRAIN_ERODE: delta = (a.val[at] - before[at]) * f; break;
            case LX_TERRAIN_ERASE_LAYER: {   /* the active edit layer back to nothing here */
                Copy cp[4];
                int n = copies_of(d, gx, gz, cp, true);
                int L = d->set.active_edit_layer;
                if (n && L > 0 && cp[0].c->layer[L]) delta = -cp[0].c->layer[L][cp[0].j * d->R + cp[0].i] * d->set.edit_layers[L].strength * lx_minf(1.0f, st * rate * 10.0f) * f;
                break;
            }
            default: break;
            }
            apply_delta(d, gx, gz, delta);
        }
    lx_free(before);
    area_free(&a);
    auto_end(&c, began);
}
static float stamp_shape(LxTerrainStamp shape, float d01, float x, float z) {
    switch (shape) {
    case LX_STAMP_HILL: return powf(lx_maxf(0, 1 - d01 * d01), 2.0f);
    case LX_STAMP_MOUNTAIN: { float base = powf(lx_maxf(0, 1 - d01), 1.6f); return base * (0.75f + 0.5f * ridged(x * 3, z * 3, 4, 21)); }
    case LX_STAMP_CRATER: { float rim = expf(-powf((d01 - 0.75f) / 0.12f, 2)); float bowl = d01 < 0.75f ? -(1 - powf(d01 / 0.75f, 2)) * 0.8f : 0; return (rim * 0.5f + bowl) * smooth01(1.0f, 0.85f, d01); }
    case LX_STAMP_MESA: return 1.0f - smooth01(0.55f, 0.75f, d01);
    case LX_STAMP_VOLCANO: { float cone = powf(lx_maxf(0, 1 - d01), 1.3f); float pit = 1.0f - smooth01(0.08f, 0.18f, d01); return cone - pit * 0.45f; }
    case LX_STAMP_VALLEY: return -powf(lx_maxf(0, 1 - d01 * d01), 2.0f);
    default: return 0;
    }
}
void lx_terrain_stamp(LxScene *s, LxEntity e, LxVec3 world, LxTerrainStamp shape, float radius, float height) {
    Ctx c;
    if (!ctx_of(s, e, &c) || radius <= 0) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    LxVec3 p = lx_mat4_point(c.inv, world);
    float cs = cell(d);
    int gx0 = (int)floorf((p.x - radius) / cs), gx1 = (int)ceilf((p.x + radius) / cs);
    int gz0 = (int)floorf((p.z - radius) / cs), gz1 = (int)ceilf((p.z + radius) / cs);
    for (int gz = gz0; gz <= gz1; gz++)
        for (int gx = gx0; gx <= gx1; gx++) {
            float dx = (float)gx * cs - p.x, dz = (float)gz * cs - p.z;
            float d01 = sqrtf(dx * dx + dz * dz) / radius;
            if (d01 >= 1) continue;
            apply_delta(d, gx, gz, stamp_shape(shape, d01, dx / radius, dz / radius) * height);
        }
    auto_end(&c, began);
}
void lx_terrain_ramp(LxScene *s, LxEntity e, LxVec3 wa, LxVec3 wb, float width, float soft) {
    Ctx c;
    if (!ctx_of(s, e, &c) || width <= 0) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    LxVec3 a = lx_mat4_point(c.inv, wa), b = lx_mat4_point(c.inv, wb);
    float cs = cell(d), half = width * 0.5f, outer = half * (1 + lx_clampf(soft, 0, 1) * 1.5f);
    float minx = lx_minf(a.x, b.x) - outer, maxx = lx_maxf(a.x, b.x) + outer, minz = lx_minf(a.z, b.z) - outer, maxz = lx_maxf(a.z, b.z) + outer;
    float sx = b.x - a.x, sz = b.z - a.z, len2 = sx * sx + sz * sz;
    Area ar;
    area_gather(d, 0.5f * (minx + maxx), 0.5f * (minz + maxz), 0.5f * lx_maxf(maxx - minx, maxz - minz) + cs, &ar);
    for (int j = 0; j < ar.h; j++)
        for (int i = 0; i < ar.w; i++) {
            int at = j * ar.w + i;
            if (!ar.has[at]) continue;
            int gx = ar.gx0 + i, gz = ar.gz0 + j;
            float x = (float)gx * cs, z = (float)gz * cs;
            float t = len2 > 1e-6f ? lx_clampf(((x - a.x) * sx + (z - a.z) * sz) / len2, 0, 1) : 0;
            float px = a.x + sx * t, pz = a.z + sz * t;
            float dist = sqrtf((x - px) * (x - px) + (z - pz) * (z - pz));
            if (dist >= outer) continue;
            float k = dist <= half ? 1.0f : 1.0f - smooth01(half, outer, dist);
            float ts = t * t * (3 - 2 * t) * 0.35f + t * 0.65f;   /* eases in and out at the ends */
            float y = lx_lerpf(a.y, b.y, ts);
            apply_delta(d, gx, gz, (y - ar.val[at]) * k);
        }
    area_free(&ar);
    auto_end(&c, began);
}
void lx_terrain_generate(LxScene *s, LxEntity e, LxTerrainPreset preset, float height, uint32_t seed) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    int N = N_(d);
    float cs = cell(d);
    int sd = (int)(seed % 100000u);
    /* the island fills the chunks' bounds */
    int cx0 = 1 << 30, cx1 = -(1 << 30), cz0 = 1 << 30, cz1 = -(1 << 30);
    for (int i = 0; i < d->nch; i++) { cx0 = lx_mini(cx0, d->ch[i].cx); cx1 = lx_maxi(cx1, d->ch[i].cx); cz0 = lx_mini(cz0, d->ch[i].cz); cz1 = lx_maxi(cz1, d->ch[i].cz); }
    float mx = ((float)cx0 + (float)cx1 + 1) * 0.5f * d->size, mz = ((float)cz0 + (float)cz1 + 1) * 0.5f * d->size;
    float rad = 0.5f * lx_maxf((float)(cx1 - cx0 + 1), (float)(cz1 - cz0 + 1)) * d->size;
    for (int k = 0; k < d->nch; k++) {
        Chunk *ch = ensure_loaded(d, d->ch[k].cx, d->ch[k].cz);
        touch(d, ch->cx, ch->cz);
        for (int j = 0; j <= N; j++)
            for (int i = 0; i <= N; i++) {
                float x = (float)(ch->cx * N + i) * cs, z = (float)(ch->cz * N + j) * cs;
                float u = x / 400.0f, v = z / 400.0f, hgt = 0;
                switch (preset) {
                case LX_TERRAIN_HILLS: hgt = (fbm(u * 1.5f, v * 1.5f, 6, sd) * 0.5f + 0.5f) * 0.6f; break;
                case LX_TERRAIN_MOUNTAINS: hgt = powf(ridged(u, v, 7, sd), 2.2f) * 1.3f + fbm(u * 4, v * 4, 4, sd + 3) * 0.04f; break;
                case LX_TERRAIN_ISLAND: {
                    float dd = sqrtf((x - mx) * (x - mx) + (z - mz) * (z - mz)) / lx_maxf(rad, 1);
                    float land = 1.0f - smooth01(0.45f, 0.95f, dd + fbm(u * 2, v * 2, 4, sd + 9) * 0.25f);
                    hgt = land * (0.25f + powf(ridged(u * 1.3f, v * 1.3f, 6, sd), 2.0f) * 0.75f) - (1 - land) * 0.15f;
                    break;
                }
                case LX_TERRAIN_CANYONS: {
                    float n = fbm(u * 1.2f, v * 1.2f, 5, sd) * 0.5f + 0.5f;
                    float steps = floorf(n * 6) / 6;
                    hgt = lx_lerpf(steps, n, 0.25f) * 0.8f - powf(lx_maxf(0, 0.25f - fabsf(fbm(u * 0.8f, v * 0.8f, 3, sd + 5))), 1.5f) * 3.0f;
                    break;
                }
                default: hgt = 0; break;
                }
                ch->layer[0][j * d->R + i] = hgt * height;
            }
        composite(d, ch);
        update_far(d, ch);
        ch->dirty_mesh = ch->dirty_file = ch->dirty_grass = ch->dirty_scatter = true;
    }
    d->index_dirty = true;
    auto_end(&c, began);
}
int lx_terrain_add_edit_layer(LxScene *s, LxEntity e, const char *name) {
    LxTerrainComponent *t = lx_get_terrain(s, e);
    if (!t || t->edit_layer_count >= NE) return -1;
    if (t->edit_layer_count < 1) {
        t->edit_layer_count = 1;
        lx_strcpy(t->edit_layers[0].name, sizeof t->edit_layers[0].name, "Base");
        t->edit_layers[0].visible = true;
        t->edit_layers[0].strength = 1;
    }
    int i = t->edit_layer_count++;
    memset(&t->edit_layers[i], 0, sizeof t->edit_layers[i]);
    lx_strcpy(t->edit_layers[i].name, sizeof t->edit_layers[i].name, name && name[0] ? name : "Layer");
    t->edit_layers[i].visible = true;
    t->edit_layers[i].strength = 1;
    t->active_edit_layer = i;
    return i;
}
void lx_terrain_remove_edit_layer(LxScene *s, LxEntity e, int index) {
    Ctx c;
    if (!ctx_of(s, e, &c) || index <= 0 || index >= c.t->edit_layer_count) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    for (int k = 0; k < d->nch; k++) {
        Chunk *ch = ensure_loaded(d, d->ch[k].cx, d->ch[k].cz);
        touch(d, ch->cx, ch->cz);
        lx_free(ch->layer[index]);
        for (int l = index; l < NE - 1; l++) ch->layer[l] = ch->layer[l + 1];
        ch->layer[NE - 1] = NULL;
    }
    for (int l = index; l < c.t->edit_layer_count - 1; l++) c.t->edit_layers[l] = c.t->edit_layers[l + 1];
    c.t->edit_layer_count--;
    if (c.t->active_edit_layer >= c.t->edit_layer_count) c.t->active_edit_layer = c.t->edit_layer_count - 1;
    d->set = *c.t;
    for (int k = 0; k < d->nch; k++) {
        Chunk *ch = &d->ch[k];
        composite(d, ch);
        ch->dirty_mesh = ch->dirty_file = ch->dirty_far = ch->dirty_grass = ch->dirty_scatter = true;
    }
    auto_end(&c, began);
}
void lx_terrain_collapse_edit_layers(LxScene *s, LxEntity e) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    size_t n = (size_t)d->R * d->R;
    for (int k = 0; k < d->nch; k++) {
        Chunk *ch = ensure_loaded(d, d->ch[k].cx, d->ch[k].cz);
        touch(d, ch->cx, ch->cz);
        memcpy(ch->layer[0], ch->h, n * sizeof(float));   /* what shows becomes the Base */
        for (int l = 1; l < NE; l++) { lx_free(ch->layer[l]); ch->layer[l] = NULL; }
        ch->dirty_file = true;
    }
    c.t->edit_layer_count = 1;
    c.t->active_edit_layer = 0;
    d->set = *c.t;
    auto_end(&c, began);
}
void lx_terrain_remove_layer(LxScene *s, LxEntity e, int layer) {
    Ctx c;
    if (!ctx_of(s, e, &c) || layer < 0 || layer >= c.t->layer_count || c.t->layer_count <= 1) return;
    TData *d = c.d;
    bool began = auto_begin(d);
    size_t n = (size_t)d->R * d->R;
    for (int k = 0; k < d->nch; k++) {
        Chunk *ch = ensure_loaded(d, d->ch[k].cx, d->ch[k].cz);
        touch(d, ch->cx, ch->cz);
        for (size_t i = 0; i < n; i++) {
            uint8_t *w = &ch->w[i * NL];
            if (layer > 0) w[0] = (uint8_t)lx_mini(255, w[0] + w[layer]);   /* what it covered goes back to the first layer */
            else w[1] = (uint8_t)lx_mini(255, w[1] + w[0]);
            memmove(&w[layer], &w[layer + 1], (size_t)(NL - 1 - layer));
            w[NL - 1] = 0;
        }
        update_far(d, ch);
        ch->dirty_mesh = ch->dirty_file = ch->dirty_far = ch->dirty_grass = ch->dirty_scatter = true;
    }
    LxTerrainComponent *t = c.t;
    memmove(&t->layers[layer], &t->layers[layer + 1], sizeof t->layers[0] * (size_t)(t->layer_count - 1 - layer));
    t->layer_count--;
    memset(&t->layers[t->layer_count], 0, sizeof t->layers[0]);
    if (t->grass_layer == layer) t->grass_layer = -1;
    else if (t->grass_layer > layer) t->grass_layer--;
    for (int k = 0; k < t->scatter_count; k++) {
        if (t->scatter[k].layer == layer) t->scatter[k].layer = -1;
        else if (t->scatter[k].layer > layer) t->scatter[k].layer--;
    }
    d->index_dirty = true;
    auto_end(&c, began);
}
void lx_terrain_layers_changed(LxScene *s, LxEntity e) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return;
    for (int k = 0; k < c.d->nch; k++) {
        Chunk *ch = &c.d->ch[k];
        if (ch->loaded) composite(c.d, ch);
        ch->dirty_mesh = ch->dirty_far = ch->dirty_grass = ch->dirty_scatter = true;
        if (!ch->loaded && ch->far_mesh) { lx_mesh_destroy(ch->far_mesh); ch->far_mesh = NULL; }
    }
    c.d->layers_hash = 0;
}

/* ------------------------------------------------------------------ saving */
bool lx_terrain_save(LxScene *s) {
    bool ok = true;
    for (int v = 0; v < g_nviews; v++) {
        if (!g_views[v].used || g_views[v].scene != s) continue;
        TData *d = g_views[v].data;
        for (int i = 0; i < d->nch; i++) {
            Chunk *c = &d->ch[i];
            if (c->loaded && c->dirty_far) update_far(d, c);
            if (c->dirty_file) ok = save_chunk(d, c) && ok;
        }
        /* files of chunks that are gone */
        char dir[LX_PATH_MAX];
        folder_path(d, dir, sizeof dir);
        LxDirEntry *ents = NULL;
        int ne = lx_dir_list(dir, &ents);
        for (int k = 0; k < ne; k++) {
            int cx, cz;
            if (sscanf(ents[k].name, "c_%d_%d.lxchunk", &cx, &cz) == 2 && !find_chunk(d, cx, cz)) {
                char path[LX_PATH_MAX];
                lx_path_join(path, sizeof path, dir, ents[k].name);
                lx_path_remove(path);
            }
        }
        lx_free(ents);
        if (d->index_dirty || ok) ok = save_index(d) && ok;
    }
    return ok;
}
bool lx_terrain_unsaved(LxScene *s) {
    for (int v = 0; v < g_nviews; v++) {
        if (!g_views[v].used || g_views[v].scene != s) continue;
        TData *d = g_views[v].data;
        if (d->index_dirty) return true;
        for (int i = 0; i < d->nch; i++) if (d->ch[i].dirty_file) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ each scene's chunk objects */
static TView *view_for(LxScene *s, LxEntity e, TData *d) {
    for (int i = 0; i < g_nviews; i++)
        if (g_views[i].used && g_views[i].scene == s && g_views[i].terrain == e) {
            if (g_views[i].data != d) {   /* the component now points at another folder */
                g_views[i].data->refs--;
                g_views[i].data = d;
                d->refs++;
                for (int k = 0; k < g_views[i].n; k++) g_views[i].c[k].alive = false;
            }
            return &g_views[i];
        }
    TView v;
    memset(&v, 0, sizeof v);
    v.scene = s; v.terrain = e; v.data = d; v.used = true;
    d->refs++;
    for (int i = 0; i < g_nviews; i++) if (!g_views[i].used) { g_views[i] = v; return &g_views[i]; }
    LX_VEC_PUSH(g_views, g_nviews, g_cviews, v);
    return &g_views[g_nviews - 1];
}
static void mark_generated(LxScene *s, LxEntity e, uint8_t kind) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return;
    d->generated = kind;
    for (LxEntity c = d->first_child; c; c = lx_entity_next_sibling(s, c)) mark_generated(s, c, kind);
}
static void destroy_ent(LxScene *s, LxEntity *e) {
    if (*e && lx_entity_valid(s, *e)) lx_entity_destroy(s, *e);
    *e = 0;
}
static void schunk_clear(LxScene *s, SChunk *sc) {
    destroy_ent(s, &sc->ent);
    destroy_ent(s, &sc->grass);
    for (int k = 0; k < sc->nscatter; k++) destroy_ent(s, &sc->scatter[k]);
    lx_free(sc->scatter);
    sc->scatter = NULL;
    sc->nscatter = 0;
}
static void scatter_chunk(LxScene *s, TView *v, SChunk *sc, Chunk *c, LxVec3 origin) {
    for (int k = 0; k < sc->nscatter; k++) destroy_ent(s, &sc->scatter[k]);
    sc->nscatter = 0;
    TData *d = v->data;
    const LxTerrainComponent *t = &d->set;
    int cap = 0;
    for (int r = 0; r < t->scatter_count && r < LX_TERRAIN_SCATTERS; r++) {
        const LxTerrainScatter *S = &t->scatter[r];
        if (!S->prefab[0] || S->density <= 0) continue;
        float spacing = 10.0f / sqrtf(lx_minf(S->density, 400.0f));
        int n = (int)(d->size / spacing);
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) {
                if (sc->nscatter >= 600) break;
                float x = ((float)i + hashf(i, j, c->cx * 31 + c->cz * 17 + r * 7)) * spacing;
                float z = ((float)j + hashf(j, i, c->cz * 31 + c->cx * 17 + r * 7 + 3)) * spacing;
                if (x >= d->size || z >= d->size) continue;
                int hi = lx_clampi((int)(x / cell(d)), 0, N_(d) - 1), hj = lx_clampi((int)(z / cell(d)), 0, N_(d) - 1);
                if (c->holes[hj * N_(d) + hi]) continue;
                float wgt = S->layer >= 0 ? chunk_weight(d, c, x, z, S->layer, origin) : 1.0f;
                if (hashf(i * 3 + r, j * 5, c->cx - c->cz) > wgt) continue;
                float y = chunk_height(d, c, x, z);
                float e2 = cell(d);
                float hx = chunk_height(d, c, lx_minf(x + e2, d->size), z), hz = chunk_height(d, c, x, lx_minf(z + e2, d->size));
                float slope = atanf(sqrtf(powf(hx - y, 2) + powf(hz - y, 2)) / e2) * LX_RAD2DEG;
                if (S->max_slope > 0 && slope > S->max_slope) continue;
                LxVec3 local = lx_v3((float)c->cx * d->size + x, y, (float)c->cz * d->size + z);
                LxVec3 wp = lx_mat4_point(lx_entity_data(s, v->terrain)->world, local);
                LxEntity inst = 0;
                const char *ext = lx_path_ext(S->prefab);
                if (!strcmp(ext, ".lxprefab")) inst = lx_prefab_instantiate(s, S->prefab, wp, 0);
                else {
                    inst = lx_entity_create(s, "Scatter");
                    lx_add_mesh(s, inst, S->prefab, "");
                    lx_entity_set_position(s, inst, wp);
                }
                if (!inst) continue;
                float smin = S->scale_min > 0 ? S->scale_min : 1, smax = S->scale_max > smin ? S->scale_max : smin;
                float sc_ = lx_lerpf(smin, smax, hashf(i, j, r + 99));
                LxEntityData *id = lx_entity_data(s, inst);
                if (id) {
                    id->scale = lx_scale3(id->scale, sc_);
                    lx_entity_set_rotation(s, inst, lx_quat_mul(lx_quat_axis_angle(lx_v3(0, 1, 0), hashf(j, i, r + 5) * 6.2831853f), id->rotation));
                }
                lx_entity_set_parent(s, inst, v->terrain);
                lx_entity_set_position(s, inst, local);
                mark_generated(s, inst, LX_GEN_SCATTER);
                if (sc->nscatter >= cap) { cap = cap ? cap * 2 : 16; sc->scatter = lx_realloc(sc->scatter, sizeof(LxEntity) * (size_t)cap); }
                sc->scatter[sc->nscatter++] = inst;
            }
    }
}

void lx_terrain_update(LxScene *s, LxVec3 focus, bool editing) {
    if (!s) return;
    for (int v = 0; v < g_nviews; v++) if (g_views[v].scene == s) g_views[v].used = g_views[v].used ? 2 : 0;   /* 2: not yet seen this time */
    bool any = false;
    for (int i = 1; i < s->count; i++) {
        LxEntityData *ed = &s->entities[i];
        if (!ed->alive || ed->generated || !(ed->comps & LX_COMP_TERRAIN) || !ed->terrain) continue;
        any = true;
        LxEntity e = lx_entity_make((uint32_t)i, ed->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
        LxTerrainComponent *t = ed->terrain;
        if (!t->folder[0]) lx_add_terrain(s, e, "");
        TData *d = data_for(t);
        TView *view = view_for(s, e, d);
        view->used = 1;
        ed = lx_entity_data(s, e);
        t = ed->terrain;
        /* undo / redo: the scene shows another revision of the terrain */
        if (editing && !s->simulating && !d->stroking && t->revision != d->rev) {
            t->revision = seek(d, t->revision);
            mark_scenes_static(d);
        }
        /* settings that change how chunks look */
        uint64_t gh = hash_bytes(1469598103934665603ull, &t->grass_layer, sizeof(int));
        gh = hash_bytes(gh, &t->grass_density, sizeof(float) * 2);
        gh = hash_bytes(gh, t->scatter, sizeof t->scatter);
        gh = hash_bytes(gh, &t->scatter_count, sizeof(int));
        gh = hash_bytes(gh, &t->grass_color, sizeof t->grass_color);
        bool layers_differ = memcmp(d->set.layers, t->layers, sizeof t->layers) != 0 || d->set.layer_count != t->layer_count ||
                             d->set.edit_layer_count != t->edit_layer_count || memcmp(d->set.edit_layers, t->edit_layers, sizeof t->edit_layers) != 0 ||
                             d->set.lod_distance != t->lod_distance;
        d->set = *t;
        if (layers_differ) lx_terrain_layers_changed(s, e);
        if (gh != d->grass_hash) {
            d->grass_hash = gh;
            for (int k = 0; k < d->nch; k++) d->ch[k].dirty_grass = d->ch[k].dirty_scatter = true;
        }
        refresh_layers(d);
        refresh_grass_material(d);
        /* streaming: full detail within the load radius of the focus */
        LxMat4 world = ed->world, inv = lx_mat4_inverse(world);
        LxVec3 lf = lx_mat4_point(inv, focus);
        LxVec3 origin = lx_mat4_point(world, lx_v3(0, 0, 0));
        float radius = t->load_radius;
        int loads = editing ? 16 : 3;   /* a few a frame while playing, so walking never stalls */
        for (int k = 0; k < d->nch; k++) {
            Chunk *c = &d->ch[k];
            float x0 = (float)c->cx * d->size, z0 = (float)c->cz * d->size;
            float dx = lx_maxf(lx_maxf(x0 - lf.x, lf.x - (x0 + d->size)), 0), dz = lx_maxf(lx_maxf(z0 - lf.z, lf.z - (z0 + d->size)), 0);
            float dist = sqrtf(dx * dx + dz * dz);
            bool want = radius <= 0 || dist <= radius;
            if (want && !c->loaded && loads > 0) { load_chunk(d, c); loads--; }
            else if (!want && c->loaded && !c->dirty_file && !d->stroking && dist > radius * 1.15f + d->size * 0.25f) {
                chunk_free_data(c);   /* unsaved changes stay in memory */
                c->serial++;
            }
        }
        /* meshes */
        for (int k = 0; k < d->nch; k++) {
            Chunk *c = &d->ch[k];
            if (c->loaded && (c->dirty_mesh || !c->mesh)) build_full_mesh(d, c, origin);
            if (c->loaded && c->dirty_far) update_far(d, c);
            if (!c->far_mesh || (c->loaded && c->dirty_far)) build_far_mesh(d, c, origin);
        }
        /* objects: one per chunk, the right mesh, collision when in full detail */
        for (int k = 0; k < view->n; k++) view->c[k].alive = false;
        for (int k = 0; k < d->nch; k++) {
            Chunk *c = &d->ch[k];
            SChunk *sc = NULL;
            for (int q = 0; q < view->n; q++) if (view->c[q].cx == c->cx && view->c[q].cz == c->cz) { sc = &view->c[q]; break; }
            if (!sc) {
                SChunk nsc;
                memset(&nsc, 0, sizeof nsc);
                nsc.cx = c->cx; nsc.cz = c->cz;
                LX_VEC_PUSH(view->c, view->n, view->cap, nsc);
                sc = &view->c[view->n - 1];
            }
            sc->alive = true;
            if (sc->ent && !lx_entity_valid(s, sc->ent)) sc->ent = 0;
            bool full = c->loaded && c->mesh;
            char mref[64], matref[32];
            snprintf(mref, sizeof mref, full ? "terrain:%d/%d_%d" : "terrain:%d/%d_%d/far", d->id, c->cx, c->cz);
            snprintf(matref, sizeof matref, "terrain:%d", d->id);
            if (!sc->ent) {
                char name[48];
                snprintf(name, sizeof name, "Chunk %d,%d", c->cx, c->cz);
                sc->ent = lx_entity_create(s, name);
                LxEntityData *cd = lx_entity_data(s, sc->ent);
                cd->generated = LX_GEN_TERRAIN_CHUNK;
                lx_entity_set_parent(s, sc->ent, e);
                lx_entity_set_position(s, sc->ent, lx_v3((float)c->cx * d->size, 0, (float)c->cz * d->size));
                LxMeshRenderer *mr = lx_add_mesh(s, sc->ent, mref, matref);
                mr->cast_shadows = true;
                mr->static_geometry = true;
                sc->full = !full;   /* forces the collider decision below */
                ed = lx_entity_data(s, e);
            }
            LxEntityData *cd = lx_entity_data(s, sc->ent);
            cd->layer = ed->layer;
            memcpy(cd->tag, ed->tag, sizeof cd->tag);
            if (strcmp(cd->mesh.mesh, mref)) { lx_strcpy(cd->mesh.mesh, sizeof cd->mesh.mesh, mref); lx_scene_mark_static_dirty(s); }
            if (sc->full != full || sc->serial != c->serial) {
                if (full && !(cd->comps & LX_COMP_COLLIDER)) lx_add_collider(s, sc->ent, LX_COLLIDER_MESH);
                if (!full) cd->comps &= ~(uint32_t)LX_COMP_COLLIDER;
                if (sc->full != full) lx_scene_mark_static_dirty(s);
                sc->full = full;
                sc->serial = c->serial;
            }
            /* grass near the focus */
            float gx0 = (float)c->cx * d->size, gz0 = (float)c->cz * d->size;
            float gdx = lx_maxf(lx_maxf(gx0 - lf.x, lf.x - (gx0 + d->size)), 0), gdz = lx_maxf(lx_maxf(gz0 - lf.z, lf.z - (gz0 + d->size)), 0);
            bool want_grass = full && t->grass_layer >= 0 && t->grass_density > 0.001f && sqrtf(gdx * gdx + gdz * gdz) <= t->grass_distance;
            if (want_grass && (c->dirty_grass || !c->grass)) build_grass(d, c, origin);
            if (sc->grass && !lx_entity_valid(s, sc->grass)) sc->grass = 0;
            if (want_grass && c->grass && !sc->grass) {
                sc->grass = lx_entity_create(s, "Grass");
                lx_entity_data(s, sc->grass)->generated = LX_GEN_GRASS;
                lx_entity_set_parent(s, sc->grass, sc->ent);
                char gref[64], gmat[32];
                snprintf(gref, sizeof gref, "terrain:%d/%d_%d/grass", d->id, c->cx, c->cz);
                snprintf(gmat, sizeof gmat, "terrain:%d/grass", d->id);
                LxMeshRenderer *gm = lx_add_mesh(s, sc->grass, gref, gmat);
                gm->cast_shadows = false;
                gm->static_geometry = false;
            } else if ((!want_grass || !c->grass) && sc->grass) destroy_ent(s, &sc->grass);
            /* scattered objects on chunks in full detail */
            if (full && (c->dirty_scatter || (!sc->nscatter && sc->grass_serial != c->serial))) {
                scatter_chunk(s, view, sc, c, origin);
                c->dirty_scatter = false;
                sc->grass_serial = c->serial;
            } else if (!full && sc->nscatter) {
                for (int q = 0; q < sc->nscatter; q++) destroy_ent(s, &sc->scatter[q]);
                sc->nscatter = 0;
            }
            ed = lx_entity_data(s, e);
        }
        for (int q = 0; q < view->n; q++)
            if (!view->c[q].alive) { schunk_clear(s, &view->c[q]); view->c[q--] = view->c[--view->n]; }
    }
    /* terrains gone from the scene */
    for (int v = 0; v < g_nviews; v++)
        if (g_views[v].used == 2 && g_views[v].scene == s) {
            for (int k = 0; k < g_views[v].n; k++) schunk_clear(s, &g_views[v].c[k]);
            lx_free(g_views[v].c);
            g_views[v].data->refs--;
            memset(&g_views[v], 0, sizeof g_views[v]);
        }
    LX_UNUSED(any);
}

/* tests: the biggest difference between two copies of a shared edge point, over loaded neighbours */
float lx_terrain_test_seams(LxScene *s, LxEntity e) {
    Ctx c;
    if (!ctx_of(s, e, &c)) return -1;
    TData *d = c.d;
    int R = d->R, N = N_(d);
    float worst = 0;
    for (int k = 0; k < d->nch; k++) {
        Chunk *a = &d->ch[k];
        if (!a->loaded) continue;
        Chunk *rx = find_chunk(d, a->cx + 1, a->cz), *rz = find_chunk(d, a->cx, a->cz + 1);
        if (rx && rx->loaded)
            for (int j = 0; j < R; j++) worst = lx_maxf(worst, fabsf(a->h[j * R + N] - rx->h[j * R]));
        if (rz && rz->loaded)
            for (int i = 0; i < R; i++) worst = lx_maxf(worst, fabsf(a->h[N * R + i] - rz->h[i]));
    }
    return worst;
}

/* where chunks stream around: the listener (usually the player's camera) while playing, else what the caller sees */
void lx_terrain_stream(LxScene *s, LxVec3 focus) {
    if (s) lx_terrain_update(s, focus, !s->simulating);
}
LxVec3 lx_terrain_focus(LxScene *s, LxVec3 fallback) {
    if (!s || !s->simulating) return fallback;
    LxEntity le = lx_scene_listener(s);
    if (!le) le = lx_scene_main_camera(s);
    return le ? lx_entity_world_position(s, le) : fallback;
}

void lx_terrain_scene_destroyed(LxScene *s) {
    for (int v = 0; v < g_nviews; v++)
        if (g_views[v].used && g_views[v].scene == s) {
            for (int k = 0; k < g_views[v].n; k++) lx_free(g_views[v].c[k].scatter);
            lx_free(g_views[v].c);
            g_views[v].data->refs--;
            memset(&g_views[v], 0, sizeof g_views[v]);
        }
}

void lx_terrain_shutdown(void) {
    for (int i = 0; i < g_ndata; i++) {
        TData *d = g_data[i];
        for (int k = 0; k < d->nch; k++) {
            chunk_free_data(&d->ch[k]);
            if (d->ch[k].far_mesh) lx_mesh_destroy(d->ch[k].far_mesh);
        }
        lx_free(d->ch);
        for (int k = 0; k < d->nhist; k++) hist_free(&d->hist[k]);
        lx_free(d->hist);
        for (int k = 0; k < d->npend; k++) snap_free(&d->pend[k]);
        lx_free(d->pend);
        lx_free(d->gpu_a); lx_free(d->gpu_b);
        lx_free(d->grass_tex.pixels);
        lx_free(d);
    }
    lx_free(g_data);
    g_data = NULL;
    g_ndata = g_cdata = 0;
    for (int v = 0; v < g_nviews; v++) if (g_views[v].used) { for (int k = 0; k < g_views[v].n; k++) lx_free(g_views[v].c[k].scatter); lx_free(g_views[v].c); }
    lx_free(g_views);
    g_views = NULL;
    g_nviews = g_cviews = 0;
}
