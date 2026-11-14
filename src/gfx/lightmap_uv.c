/* Lightmap UVs: a second UV set where every triangle has its own place, for baked lighting.
 *
 * The automatic unwrap works like the classic lightmap unwrappers (Unity's, xatlas-lite):
 *   1. weld positions and find the triangles that share an edge;
 *   2. grow charts: starting from the largest triangle not yet used, flood across shared edges
 *      while the neighbour faces within 45 degrees of the chart's first triangle, so the chart
 *      can be flattened onto a plane without folding over itself or stretching much;
 *   3. flatten each chart onto its average plane and turn it to its tightest bounding box;
 *   4. pack the charts into a square on shelves, every chart surrounded by padding texels so
 *      the bake's filtering never bleeds light from one chart into another;
 *   5. split vertices on chart borders (a vertex can only have one lightmap UV).
 * The scale is the same for every chart, so texels are spread evenly over the mesh.
 * Everything is deterministic: the same mesh always gets the same UVs. */
#include "../lx_internal.h"

#define LMUV_PAD 2              /* texels of padding on each side of a chart */
#define LMUV_COS_LIMIT 0.7071f  /* 45 degrees */

typedef struct Edge { uint32_t a, b; int tri; } Edge;
typedef struct Chart {
    int first, count;          /* range in the chart-ordered triangle list */
    LxVec3 axis, t, b;         /* projection plane */
    float minx, miny, w, h;    /* metres, after rotation */
    float ca, sa;              /* rotation */
    bool swap;                 /* turned 90 degrees so it lies wide */
    int level;                 /* how tight the angle it was grown with */
    int x, y, tw, th;          /* placement in texels (padding included in tw/th) */
} Chart;

static int edge_cmp(const void *pa, const void *pb) {
    const Edge *a = pa, *b = pb;
    if (a->a != b->a) return a->a < b->a ? -1 : 1;
    if (a->b != b->b) return a->b < b->b ? -1 : 1;
    return a->tri - b->tri;
}

typedef struct AreaIdx { float area; int tri; } AreaIdx;
static int area_cmp(const void *pa, const void *pb) {
    const AreaIdx *a = pa, *b = pb;
    if (a->area != b->area) return a->area > b->area ? -1 : 1;
    return a->tri - b->tri;
}

static const Chart *g_sort_charts;
static int chart_h_cmp(const void *pa, const void *pb) {
    int ia = *(const int *)pa, ib = *(const int *)pb;
    const Chart *a = &g_sort_charts[ia], *b = &g_sort_charts[ib];
    if (a->h != b->h) return a->h > b->h ? -1 : 1;
    if (a->w != b->w) return a->w > b->w ? -1 : 1;
    return ia - ib;
}

/* the unwrap measures shapes in this scale (a stretched object's own unwrap, see lx_mesh_lightmap_variant) */
static LxVec3 g_scale = {1, 1, 1};
static LxVec3 vpos(const LxVertex *v) { return lx_v3(v->px * g_scale.x, v->py * g_scale.y, v->pz * g_scale.z); }

/* welds equal positions so triangles that share a corner see the same id */
static uint32_t *weld_positions(const LxVertex *v, int vc) {
    uint32_t *id = lx_alloc(sizeof(uint32_t) * (size_t)(vc ? vc : 1));
    int cap = 16;
    while (cap < vc * 2) cap <<= 1;
    int32_t *slot = lx_alloc(sizeof(int32_t) * (size_t)cap);
    for (int i = 0; i < cap; i++) slot[i] = -1;
    for (int i = 0; i < vc; i++) {
        float p[3] = {v[i].px, v[i].py, v[i].pz};
        uint32_t h = 2166136261u;
        for (int k = 0; k < 3; k++) {
            float f = p[k] == 0.0f ? 0.0f : p[k];   /* -0 == +0 */
            uint32_t w;
            memcpy(&w, &f, 4);
            h = (h ^ w) * 16777619u;
        }
        h &= (uint32_t)(cap - 1);
        for (;;) {
            if (slot[h] < 0) { slot[h] = i; id[i] = (uint32_t)i; break; }
            const LxVertex *o = &v[slot[h]];
            if (o->px == v[i].px && o->py == v[i].py && o->pz == v[i].pz) { id[i] = (uint32_t)slot[h]; break; }
            h = (h + 1) & (uint32_t)(cap - 1);
        }
    }
    lx_free(slot);
    return id;
}

static void chart_basis(LxVec3 n, LxVec3 *t, LxVec3 *b) {
    *t = lx_norm3(fabsf(n.y) < 0.99f ? lx_cross3(lx_v3(0, 1, 0), n) : lx_cross3(lx_v3(1, 0, 0), n));
    *b = lx_cross3(n, *t);
}

/* shelf packing at `s` texels per metre into a square of `res` texels; false if it doesn't fit */
static bool pack(Chart *ch, const int *order, int nc, float s, int res) {
    int x = 0, y = 0, shelf = 0;
    for (int k = 0; k < nc; k++) {
        Chart *c = &ch[order[k]];
        c->tw = (int)ceilf(c->w * s) + 1 + 2 * LMUV_PAD;
        c->th = (int)ceilf(c->h * s) + 1 + 2 * LMUV_PAD;
        if (c->tw > res || c->th > res) return false;
        if (x + c->tw > res) { y += shelf; x = 0; shelf = 0; }
        if (y + c->th > res) return false;
        c->x = x;
        c->y = y;
        x += c->tw;
        if (c->th > shelf) shelf = c->th;
    }
    return true;
}


#define LMUV_LEVELS 4
static const float LMUV_LIMITS[LMUV_LEVELS] = {LMUV_COS_LIMIT, 0.866f, 0.966f, 2.0f};   /* 45, 30, 15 degrees, single triangles */

typedef struct Grow {
    const LxVertex *V;
    const uint32_t *I;
    const LxVec3 *tn;
    const int *start, *adj;
    int *chart_of;
    int *ord;          /* triangles, chart by chart */
    int nord, ordcap;
    Chart *ch;
    int nc, ccap;
} Grow;

static bool is_sliver(LxVec3 n) { return n.x == 0 && n.y == 0 && n.z == 0; }

static void flood(Grow *g, int seed, int level) {
    Chart c;
    memset(&c, 0, sizeof c);
    c.first = g->nord;
    c.level = level;
    int id = g->nc;
    float limit = LMUV_LIMITS[level];
    LxVec3 sn = is_sliver(g->tn[seed]) ? lx_v3(0, 1, 0) : g->tn[seed];
    g->chart_of[seed] = id;
    LX_VEC_PUSH(g->ord, g->nord, g->ordcap, seed);
    for (int q = c.first; q < g->nord; q++) {   /* ord doubles as the flood queue */
        int t = g->ord[q];
        for (int k = g->start[t]; k < g->start[t + 1]; k++) {
            int u = g->adj[k];
            if (g->chart_of[u] >= 0) continue;
            if (!is_sliver(g->tn[u]) && lx_dot3(g->tn[u], sn) < limit) continue;
            g->chart_of[u] = id;
            LX_VEC_PUSH(g->ord, g->nord, g->ordcap, u);
        }
    }
    c.count = g->nord - c.first;
    c.axis = sn;
    LX_VEC_PUSH(g->ch, g->nc, g->ccap, c);
}


/* merges each chart into the neighbour it shares most edges with when all its triangles still face that
 * neighbour's direction within 53 degrees - smallest charts first, so slivers left between big charts go away.
 * Charts keep their triangles in linked lists while merging; ord is rebuilt chart by chart at the end. */
static void merge_charts(Grow *g, int nt) {
    int nc = g->nc;
    int *head = lx_alloc(sizeof(int) * (size_t)nc), *tail = lx_alloc(sizeof(int) * (size_t)nc), *next = lx_alloc(sizeof(int) * (size_t)nt);
    int *size = lx_alloc(sizeof(int) * (size_t)nc), *order = lx_alloc(sizeof(int) * (size_t)nc);
    for (int ci = 0; ci < nc; ci++) {
        const Chart *c = &g->ch[ci];
        head[ci] = g->ord[c->first];
        for (int q = c->first; q < c->first + c->count; q++) next[g->ord[q]] = q + 1 < c->first + c->count ? g->ord[q + 1] : -1;
        tail[ci] = g->ord[c->first + c->count - 1];
        size[ci] = c->count;
        order[ci] = ci;
    }
    /* ascending size (insertion-free: counting by size) */
    int maxs = 0;
    for (int ci = 0; ci < nc; ci++) maxs = size[ci] > maxs ? size[ci] : maxs;
    int *bucket = lx_calloc((size_t)maxs + 2, sizeof(int));
    for (int ci = 0; ci < nc; ci++) bucket[size[ci] + 1]++;
    for (int k = 0; k <= maxs; k++) bucket[k + 1] += bucket[k];
    for (int ci = 0; ci < nc; ci++) order[bucket[size[ci]]++] = ci;
    lx_free(bucket);
    int *nb = NULL, *nbc = NULL, nn = 0, nbcap = 0, nbccap = 0;
    for (int k = 0; k < nc; k++) {
        int a = order[k];
        if (!size[a]) continue;
        nn = 0;
        int nbc_n = 0;
        for (int t = head[a]; t >= 0; t = next[t])
            for (int e = g->start[t]; e < g->start[t + 1]; e++) {
                int b = g->chart_of[g->adj[e]];
                if (b == a) continue;
                int j = 0;
                while (j < nn && nb[j] != b) j++;
                if (j == nn) { LX_VEC_PUSH(nb, nn, nbcap, b); int zero = 0; LX_VEC_PUSH(nbc, nbc_n, nbccap, zero); }
                nbc[j]++;
            }
        int best = -1, best_edges = 0;
        for (int j = 0; j < nn; j++) {
            int b = nb[j];
            if (size[b] < size[a] || nbc[j] <= best_edges) continue;
            bool ok = true;
            for (int t = head[a]; t >= 0 && ok; t = next[t]) ok = is_sliver(g->tn[t]) || lx_dot3(g->tn[t], g->ch[b].axis) >= 0.6f;
            if (ok) { best = b; best_edges = nbc[j]; }
        }
        if (best < 0) continue;
        for (int t = head[a]; t >= 0; t = next[t]) g->chart_of[t] = best;
        next[tail[best]] = head[a];
        tail[best] = tail[a];
        size[best] += size[a];
        size[a] = 0;
    }
    lx_free(nb);
    lx_free(nbc);
    /* rebuild ord and the chart list */
    int q = 0, live = 0;
    for (int ci = 0; ci < nc; ci++) {
        if (!size[ci]) continue;
        Chart c = g->ch[ci];
        c.first = q;
        c.count = size[ci];
        for (int t = head[ci]; t >= 0; t = next[t]) { g->ord[q++] = t; g->chart_of[t] = live; }
        g->ch[live++] = c;
    }
    g->nc = live;
    lx_free(head); lx_free(tail); lx_free(next); lx_free(size); lx_free(order);
}

/* 2D triangles overlap (more than touching) when no edge of either separates them */
static bool tri2_overlap(const float *a, const float *b, float eps) {
    for (int pass = 0; pass < 2; pass++) {
        const float *p = pass ? b : a;
        float area = (p[2] - p[0]) * (p[5] - p[1]) - (p[3] - p[1]) * (p[4] - p[0]);
        float sg = area >= 0 ? 1.0f : -1.0f;
        for (int e = 0; e < 3; e++) {
            float ex = p[((e + 1) % 3) * 2] - p[e * 2], ey = p[((e + 1) % 3) * 2 + 1] - p[e * 2 + 1];
            float len = sqrtf(ex * ex + ey * ey);
            if (len <= 0) continue;
            float nx = ey * sg / len, ny = -ex * sg / len;   /* outward */
            float edge = nx * p[e * 2] + ny * p[e * 2 + 1];
            const float *o = pass ? a : b;
            float mn = INFINITY;
            for (int k = 0; k < 3; k++) mn = lx_minf(mn, nx * o[k * 2] + ny * o[k * 2 + 1]);
            if (mn >= edge - eps) return false;   /* the other triangle is outside this edge */
        }
    }
    return true;
}

/* picks the chart's plane (its average facing when every triangle still faces it well, else its first
 * triangle's) and says whether the flattened chart folds over itself */
static bool chart_flatten(Grow *g, int ci) {
    Chart *c = &g->ch[ci];
    if (!c->count) return false;
    LxVec3 sum = lx_v3(0, 0, 0);
    for (int q = c->first; q < c->first + c->count; q++) {
        int t = g->ord[q];
        LxVec3 a = vpos(&g->V[g->I[t * 3]]), b = vpos(&g->V[g->I[t * 3 + 1]]), cc = vpos(&g->V[g->I[t * 3 + 2]]);
        sum = lx_add3(sum, lx_cross3(lx_sub3(b, a), lx_sub3(cc, a)));
    }
    LxVec3 axis = lx_len3(sum) > 1e-12f ? lx_norm3(sum) : c->axis;
    for (int q = c->first; q < c->first + c->count; q++) {
        LxVec3 n = g->tn[g->ord[q]];
        if (!is_sliver(n) && lx_dot3(n, axis) < 0.6f) { axis = c->axis; break; }
    }
    c->axis = axis;
    chart_basis(axis, &c->t, &c->b);
    if (c->count < 2) return false;
    /* overlap test: flattened triangles in a grid, neighbours in a cell compared */
    float *p = lx_alloc(sizeof(float) * 6 * (size_t)c->count);
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (int q = 0; q < c->count; q++)
        for (int j = 0; j < 3; j++) {
            LxVec3 v = vpos(&g->V[g->I[g->ord[c->first + q] * 3 + j]]);
            p[q * 6 + j * 2] = lx_dot3(v, c->t);
            p[q * 6 + j * 2 + 1] = lx_dot3(v, c->b);
            x0 = lx_minf(x0, p[q * 6 + j * 2]); x1 = lx_maxf(x1, p[q * 6 + j * 2]);
            y0 = lx_minf(y0, p[q * 6 + j * 2 + 1]); y1 = lx_maxf(y1, p[q * 6 + j * 2 + 1]);
        }
    float size = lx_maxf(x1 - x0, y1 - y0) + 1e-9f, eps = size * 1e-5f;
    int n = (int)sqrtf((float)c->count) + 1;
    if (n > 256) n = 256;
    int *cnt = lx_calloc((size_t)n * n + 1, sizeof(int)), *cells_buf = NULL;
    for (int pass = 0; pass < 2; pass++) {   /* counting, then filling (CSR) */
        int *fill = pass ? lx_calloc((size_t)n * n, sizeof(int)) : NULL;
        if (pass) cells_buf = lx_alloc(sizeof(int) * (size_t)(cnt[n * n] ? cnt[n * n] : 1));
        for (int q = 0; q < c->count; q++) {
            const float *t = &p[q * 6];
            int cx0 = (int)((lx_minf(t[0], lx_minf(t[2], t[4])) - x0) / size * n), cx1 = (int)((lx_maxf(t[0], lx_maxf(t[2], t[4])) - x0) / size * n);
            int cy0 = (int)((lx_minf(t[1], lx_minf(t[3], t[5])) - y0) / size * n), cy1 = (int)((lx_maxf(t[1], lx_maxf(t[3], t[5])) - y0) / size * n);
            for (int cy = lx_maxf(cy0, 0); cy <= cy1 && cy < n; cy++)
                for (int cx = lx_maxf(cx0, 0); cx <= cx1 && cx < n; cx++) {
                    if (!pass) cnt[cy * n + cx + 1]++;
                    else cells_buf[cnt[cy * n + cx] + fill[cy * n + cx]++] = q;
                }
        }
        if (!pass) for (int i = 0; i < n * n; i++) cnt[i + 1] += cnt[i];
        else {
            bool folds = false;
            for (int cell = 0; cell < n * n && !folds; cell++)
                for (int i = cnt[cell]; i < cnt[cell + 1] && !folds; i++)
                    for (int j = i + 1; j < cnt[cell + 1] && !folds; j++)
                        folds = tri2_overlap(&p[cells_buf[i] * 6], &p[cells_buf[j] * 6], eps);
            lx_free(fill);
            lx_free(cells_buf);
            lx_free(cnt);
            lx_free(p);
            return folds;
        }
    }
    return false;
}

void lx_mesh_lightmap_measure(LxMesh *m) {
    const LxMeshLod *l = &m->lods[0];
    double wa = 0, ua = 0;
    for (int t = 0; t + 2 < l->index_count; t += 3) {
        const LxVertex *a = &l->vertices[l->indices[t]], *b = &l->vertices[l->indices[t + 1]], *c = &l->vertices[l->indices[t + 2]];
        wa += 0.5 * lx_len3(lx_cross3(lx_sub3(vpos(b), vpos(a)), lx_sub3(vpos(c), vpos(a))));
        ua += 0.5 * fabs((double)(b->u2 - a->u2) * (c->v2 - a->v2) - (double)(c->u2 - a->u2) * (b->v2 - a->v2));
    }
    m->has_uv2 = ua > 0;
    m->lm_world_size = ua > 0 ? (float)sqrt(wa / ua) : 0;
}

static bool unwrap(LxMesh *m);
LxVec3 lx_mat4_scale_of(const LxMat4 *m) {
    return lx_v3(lx_len3(lx_v3(m->m[0], m->m[1], m->m[2])), lx_len3(lx_v3(m->m[4], m->m[5], m->m[6])), lx_len3(lx_v3(m->m[8], m->m[9], m->m[10])));
}
bool lx_mesh_unwrap_lightmap(LxMesh *m) { return unwrap(m); }

/* ---- stretched objects ----
 * Lightmap UVs belong to a mesh, so a cube stretched into a 16 x 1 x 16 floor would give its thin sides as much of
 * the lightmap as its big top. Static objects scaled unevenly get their own copy of the mesh, unwrapped in their
 * real proportions (the same every time for the same mesh and scale, so bakes and drawing agree). */
typedef struct Variant { const LxMesh *base; int key[3]; LxMesh *mesh; } Variant;
static Variant *g_variants;
static int g_variant_count, g_variant_cap;

static int scale_key(float s) {   /* steps of about 2%, so tiny drifts in a transform don't make new copies */
    s = fabsf(s);
    return s < 1e-6f ? -100000 : (int)lroundf(log2f(s) * 32.0f);
}
static float key_scale(int k) { return k <= -100000 ? 0.0f : exp2f((float)k / 32.0f); }

LxMesh *lx_mesh_lightmap_variant(LxMesh *base, LxVec3 scale) {
    if (!base || !base->has_uv2 || !base->uv2_generated || base->skin || base->dynamic) return base;
    float sx = fabsf(scale.x), sy = fabsf(scale.y), sz = fabsf(scale.z);
    float lo = lx_minf(sx, lx_minf(sy, sz)), hi = lx_maxf(sx, lx_maxf(sy, sz));
    if (lo <= 1e-6f || hi / lo < 1.25f) return base;   /* about even: the mesh's own UVs are fine */
    int key[3] = {scale_key(sx / hi), scale_key(sy / hi), scale_key(sz / hi)};   /* only the proportions matter */
    for (int i = 0; i < g_variant_count; i++)
        if (g_variants[i].base == base && !memcmp(g_variants[i].key, key, sizeof key)) return g_variants[i].mesh;
    const LxMeshLod *l0 = &base->lods[0];
    LxMesh *v = lx_calloc(1, sizeof *v);
    snprintf(v->ref, sizeof v->ref, "%s", base->ref);
    v->lods[0].vertices = lx_alloc(sizeof(LxVertex) * (size_t)l0->vertex_count);
    memcpy(v->lods[0].vertices, l0->vertices, sizeof(LxVertex) * (size_t)l0->vertex_count);
    v->lods[0].indices = lx_alloc(sizeof(uint32_t) * (size_t)l0->index_count);
    memcpy(v->lods[0].indices, l0->indices, sizeof(uint32_t) * (size_t)l0->index_count);
    v->lods[0].vertex_count = l0->vertex_count;
    v->lods[0].index_count = l0->index_count;
    v->lod_count = 1;
    v->bounds = base->bounds;
    g_scale = lx_v3(key_scale(key[0]), key_scale(key[1]), key_scale(key[2]));
    unwrap(v);
    g_scale = lx_v3(1, 1, 1);
    Variant var = {base, {key[0], key[1], key[2]}, v};
    LX_VEC_PUSH(g_variants, g_variant_count, g_variant_cap, var);
    return v;
}

void lx_mesh_lightmap_variants_release(const LxMesh *base) {
    for (int i = 0; i < g_variant_count;) {
        if (g_variants[i].base == base) {
            lx_mesh_destroy(g_variants[i].mesh);
            g_variants[i] = g_variants[--g_variant_count];
        } else i++;
    }
}

static bool unwrap(LxMesh *m) {
    if (!m || m->skin || m->dynamic) return false;
    LxMeshLod *l0 = &m->lods[0];
    int vc = l0->vertex_count, nt = l0->index_count / 3;
    if (nt <= 0 || vc <= 0) return false;
    const LxVertex *V = l0->vertices;
    const uint32_t *I = l0->indices;

    /* triangle normals and areas */
    LxVec3 *tn = lx_alloc(sizeof(LxVec3) * (size_t)nt);
    AreaIdx *by_area = lx_alloc(sizeof(AreaIdx) * (size_t)nt);
    for (int t = 0; t < nt; t++) {
        LxVec3 a = vpos(&V[I[t * 3]]), b = vpos(&V[I[t * 3 + 1]]), c = vpos(&V[I[t * 3 + 2]]);
        LxVec3 n = lx_cross3(lx_sub3(b, a), lx_sub3(c, a));
        float len = lx_len3(n);
        tn[t] = len > 1e-12f ? lx_scale3(n, 1.0f / len) : lx_v3(0, 0, 0);
        by_area[t].area = 0.5f * len;
        by_area[t].tri = t;
    }
    qsort(by_area, (size_t)nt, sizeof *by_area, area_cmp);

    /* edges -> neighbours */
    uint32_t *pid = weld_positions(V, vc);
    Edge *edges = lx_alloc(sizeof(Edge) * (size_t)nt * 3);
    for (int t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) {
            uint32_t a = pid[I[t * 3 + k]], b = pid[I[t * 3 + (k + 1) % 3]];
            edges[t * 3 + k] = (Edge){a < b ? a : b, a < b ? b : a, t};
        }
    qsort(edges, (size_t)nt * 3, sizeof(Edge), edge_cmp);
    /* adjacency list: for each triangle, the triangles across its edges (CSR) */
    int *deg = lx_calloc((size_t)nt + 1, sizeof(int));
    int ne = nt * 3;
    for (int i = 0; i < ne;) {
        int j = i;
        while (j < ne && edges[j].a == edges[i].a && edges[j].b == edges[i].b) j++;
        if (edges[i].a != edges[i].b)
            for (int p = i; p < j; p++) deg[edges[p].tri] += j - i - 1;
        i = j;
    }
    int *start = lx_alloc(sizeof(int) * ((size_t)nt + 1));
    start[0] = 0;
    for (int t = 0; t < nt; t++) start[t + 1] = start[t] + deg[t];
    int *adj = lx_alloc(sizeof(int) * (size_t)(start[nt] ? start[nt] : 1));
    memset(deg, 0, sizeof(int) * (size_t)nt);
    for (int i = 0; i < ne;) {
        int j = i;
        while (j < ne && edges[j].a == edges[i].a && edges[j].b == edges[i].b) j++;
        if (edges[i].a != edges[i].b)
            for (int p = i; p < j; p++)
                for (int q = i; q < j; q++)
                    if (p != q) { int t = edges[p].tri; adj[start[t] + deg[t]++] = edges[q].tri; }
        i = j;
    }
    lx_free(edges);
    lx_free(pid);

    /* grow charts; a chart that folds over itself once flattened is grown again with a tighter angle */
    Grow g = {V, I, tn, start, adj, lx_alloc(sizeof(int) * (size_t)nt), NULL, 0, 0, NULL, 0, 0};
    for (int t = 0; t < nt; t++) g.chart_of[t] = -1;
    for (int pass = 0; pass < 2; pass++)   /* real triangles seed first, leftover slivers after */
        for (int s = 0; s < nt; s++) {
            int seed = by_area[s].tri;
            if (g.chart_of[seed] < 0 && is_sliver(tn[seed]) == (pass == 1)) flood(&g, seed, 0);
        }
    merge_charts(&g, nt);
    for (int ci = 0; ci < g.nc; ci++) {
        if (!chart_flatten(&g, ci) || g.ch[ci].level >= LMUV_LEVELS - 1) continue;
        int level = g.ch[ci].level + 1, first = g.ch[ci].first, count = g.ch[ci].count;
        for (int q = first; q < first + count; q++) g.chart_of[g.ord[q]] = -1;
        g.ch[ci].count = 0;   /* dead: its triangles go to the new charts */
        for (int s = 0; s < nt; s++)
            if (g.chart_of[by_area[s].tri] < 0) flood(&g, by_area[s].tri, level);
    }
    lx_free(adj);
    lx_free(start);
    lx_free(deg);
    /* drop the dead charts */
    Chart *ch = g.ch;
    int nc = 0;
    for (int ci = 0; ci < g.nc; ci++)
        if (ch[ci].count) ch[nc++] = ch[ci];
    int *tris = g.ord, *chart_of = g.chart_of;

    /* turn each chart to its tightest box (16 angles over a quarter turn), then lie it wide */
    for (int ci = 0; ci < nc; ci++) {
        Chart *c = &ch[ci];
        float best = INFINITY;
        for (int k = 0; k < 16; k++) {
            float ang = (float)k * (LX_PI * 0.5f / 16.0f), ca = cosf(ang), sa = sinf(ang);
            float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (int q = c->first; q < c->first + c->count; q++)
                for (int j = 0; j < 3; j++) {
                    LxVec3 p = vpos(&V[I[tris[q] * 3 + j]]);
                    float px = lx_dot3(p, c->t), py = lx_dot3(p, c->b);
                    float rx = px * ca - py * sa, ry = px * sa + py * ca;
                    x0 = lx_minf(x0, rx); x1 = lx_maxf(x1, rx); y0 = lx_minf(y0, ry); y1 = lx_maxf(y1, ry);
                }
            float area = (x1 - x0 + 1e-4f) * (y1 - y0 + 1e-4f);
            if (area < best - 1e-9f) { best = area; c->ca = ca; c->sa = sa; c->minx = x0; c->miny = y0; c->w = x1 - x0; c->h = y1 - y0; }
        }
        if (c->h > c->w) { c->swap = true; float tw = c->w; c->w = c->h; c->h = tw; }
    }

    /* pack: largest scale whose shelves fit the square */
    double area = 0;
    float maxdim = 0;
    for (int ci = 0; ci < nc; ci++) { area += (double)ch[ci].w * ch[ci].h; maxdim = lx_maxf(maxdim, ch[ci].w); }
    if (maxdim <= 0) maxdim = 1;
    int *order = lx_alloc(sizeof(int) * (size_t)nc);
    for (int ci = 0; ci < nc; ci++) order[ci] = ci;
    g_sort_charts = ch;
    qsort(order, (size_t)nc, sizeof(int), chart_h_cmp);
    g_sort_charts = NULL;
    /* the square the padding is measured in: about 12 texels per metre, at least 64 and more when there
     * are many charts so each one still gets room */
    int res = 64;
    while (res < 1024 && (float)res < sqrtf((float)area) * 12.0f) res <<= 1;
    while (res < 8192 && (double)nc * (2 * LMUV_PAD + 2) * (2 * LMUV_PAD + 2) * 2.0 > (double)res * res) res <<= 1;
    float lo = 1e-6f, hi = (float)(res - 2 * LMUV_PAD - 1) / maxdim, s = 1e-6f;
    for (;;) {   /* make sure something fits; grow the square if even the smallest scale doesn't */
        if (pack(ch, order, nc, 1e-6f, res) || res >= 16384) break;
        res <<= 1;
        hi = (float)(res - 2 * LMUV_PAD - 1) / maxdim;
    }
    for (int it = 0; it < 24; it++) {
        float mid = 0.5f * (lo + hi);
        if (pack(ch, order, nc, mid, res)) { lo = mid; s = mid; } else hi = mid;
    }
    pack(ch, order, nc, s, res);
    lx_free(order);

    /* write the new vertices: one copy of a vertex per chart it appears in */
    LxVertex *nv = NULL;
    int nvc = 0, nvcap = 0;
    uint32_t *ni = lx_alloc(sizeof(uint32_t) * (size_t)nt * 3);
    int *stamp = lx_alloc(sizeof(int) * (size_t)vc), *remap = lx_alloc(sizeof(int) * (size_t)vc);
    for (int i = 0; i < vc; i++) stamp[i] = -1;
    float inv = 1.0f / (float)res;
    for (int ci = 0; ci < nc; ci++) {
        const Chart *c = &ch[ci];
        for (int q = c->first; q < c->first + c->count; q++) {
            int t = tris[q];
            for (int j = 0; j < 3; j++) {
                uint32_t o = I[t * 3 + j];
                if (stamp[o] != ci) {
                    stamp[o] = ci;
                    LxVertex v = V[o];
                    LxVec3 p = vpos(&v);
                    float px = lx_dot3(p, c->t), py = lx_dot3(p, c->b);
                    float rx = px * c->ca - py * c->sa - c->minx, ry = px * c->sa + py * c->ca - c->miny;
                    if (c->swap) { float tmp = rx; rx = ry; ry = tmp; }
                    v.u2 = ((float)(c->x + LMUV_PAD) + 0.5f + rx * s) * inv;
                    v.v2 = ((float)(c->y + LMUV_PAD) + 0.5f + ry * s) * inv;
                    LX_VEC_PUSH(nv, nvc, nvcap, v);
                    remap[o] = nvc - 1;
                }
                ni[t * 3 + j] = (uint32_t)remap[o];
            }
        }
    }
    lx_free(stamp);
    lx_free(remap);
    lx_free(tris);
    lx_free(chart_of);
    lx_free(by_area);
    lx_free(tn);
    lx_free(ch);

    /* replace the mesh's detail level and rebuild the coarser ones from it */
    lx_renderer_mesh_release(m);
    for (int l = 0; l < m->lod_count; l++) {
        lx_free(m->lods[l].vertices);
        lx_free(m->lods[l].indices);
        memset(&m->lods[l], 0, sizeof m->lods[l]);
    }
    m->lods[0].vertices = nv;
    m->lods[0].vertex_count = nvc;
    m->lods[0].indices = ni;
    m->lods[0].index_count = nt * 3;
    m->lod_count = 1;
    lx_mesh_generate_lods(m);
    m->lm_charts = nc;
    m->lm_pack_res = res;
    m->uv2_generated = true;
    lx_mesh_lightmap_measure(m);
    return true;
}
