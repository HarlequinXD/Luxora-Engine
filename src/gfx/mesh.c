/* Meshes: procedural primitives, OBJ import, tangent generation, automatic LOD chains. */
#include "../lx_internal.h"

typedef struct MeshBuilder {
    LxVertex *v;
    uint32_t *i;
    int vc, vcap, ic, icap;
} MeshBuilder;

static uint32_t mb_vertex(MeshBuilder *b, LxVec3 p, LxVec3 n, float u, float v) {
    LxVertex vx;
    memset(&vx, 0, sizeof vx);
    vx.px = p.x; vx.py = p.y; vx.pz = p.z;
    vx.nx = n.x; vx.ny = n.y; vx.nz = n.z;
    vx.u = u; vx.v = v;
    LX_VEC_PUSH(b->v, b->vc, b->vcap, vx);
    return (uint32_t)(b->vc - 1);
}
static void mb_tri(MeshBuilder *b, uint32_t a, uint32_t c, uint32_t d) {
    LX_VEC_PUSH(b->i, b->ic, b->icap, a);
    LX_VEC_PUSH(b->i, b->ic, b->icap, c);
    LX_VEC_PUSH(b->i, b->ic, b->icap, d);
}

static void compute_tangents(LxVertex *v, int vc, const uint32_t *idx, int ic) {
    LxVec3 *tan = lx_calloc((size_t)vc, sizeof(LxVec3));
    LxVec3 *bit = lx_calloc((size_t)vc, sizeof(LxVec3));
    for (int t = 0; t + 2 < ic; t += 3) {
        LxVertex *a = &v[idx[t]], *b = &v[idx[t + 1]], *c = &v[idx[t + 2]];
        LxVec3 e1 = lx_v3(b->px - a->px, b->py - a->py, b->pz - a->pz);
        LxVec3 e2 = lx_v3(c->px - a->px, c->py - a->py, c->pz - a->pz);
        float du1 = b->u - a->u, dv1 = b->v - a->v, du2 = c->u - a->u, dv2 = c->v - a->v;
        float det = du1 * dv2 - du2 * dv1;
        if (fabsf(det) < 1e-12f) continue;
        float r = 1.0f / det;
        LxVec3 sd = lx_scale3(lx_sub3(lx_scale3(e1, dv2), lx_scale3(e2, dv1)), r);
        LxVec3 td = lx_scale3(lx_sub3(lx_scale3(e2, du1), lx_scale3(e1, du2)), r);
        for (int k = 0; k < 3; k++) {
            tan[idx[t + k]] = lx_add3(tan[idx[t + k]], sd);
            bit[idx[t + k]] = lx_add3(bit[idx[t + k]], td);
        }
    }
    for (int i = 0; i < vc; i++) {
        LxVec3 n = lx_v3(v[i].nx, v[i].ny, v[i].nz);
        LxVec3 t = tan[i];
        t = lx_sub3(t, lx_scale3(n, lx_dot3(n, t)));
        if (lx_len3sq(t) < 1e-12f) t = fabsf(n.x) < 0.9f ? lx_cross3(n, lx_v3(1, 0, 0)) : lx_cross3(n, lx_v3(0, 1, 0));
        t = lx_norm3(t);
        float w = lx_dot3(lx_cross3(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        v[i].tx = t.x; v[i].ty = t.y; v[i].tz = t.z; v[i].tw = w;
    }
    lx_free(tan);
    lx_free(bit);
}

LxMesh *lx_mesh_create(const LxVertex *v, int vc, const uint32_t *idx, int ic, const char *ref) {
    LxMesh *m = lx_calloc(1, sizeof *m);
    lx_strcpy(m->ref, sizeof m->ref, ref);
    LxMeshLod *l0 = &m->lods[0];
    l0->vertices = lx_alloc(sizeof(LxVertex) * (size_t)vc);
    memcpy(l0->vertices, v, sizeof(LxVertex) * (size_t)vc);
    l0->indices = lx_alloc(sizeof(uint32_t) * (size_t)ic);
    memcpy(l0->indices, idx, sizeof(uint32_t) * (size_t)ic);
    l0->vertex_count = vc;
    l0->index_count = ic;
    m->lod_count = 1;
    compute_tangents(l0->vertices, vc, l0->indices, ic);
    m->bounds = lx_aabb_empty();
    for (int i = 0; i < vc; i++) lx_aabb_grow(&m->bounds, lx_v3(v[i].px, v[i].py, v[i].pz));
    return m;
}

static LxMesh *mb_finish(MeshBuilder *b, const char *ref) {
    LxMesh *m = lx_mesh_create(b->v, b->vc, b->i, b->ic, ref);
    lx_free(b->v);
    lx_free(b->i);
    return m;
}

static void build_cube(MeshBuilder *b) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; f++) {
        LxVec3 nn = lx_v3(n[f][0], n[f][1], n[f][2]);
        LxVec3 up = fabsf(nn.y) > 0.5f ? lx_v3(0, 0, nn.y > 0 ? -1.0f : 1.0f) : lx_v3(0, 1, 0);
        LxVec3 right = lx_cross3(up, nn);
        uint32_t base = (uint32_t)b->vc;
        for (int k = 0; k < 4; k++) {
            float sx = (k == 1 || k == 2) ? 0.5f : -0.5f;
            float sy = (k >= 2) ? 0.5f : -0.5f;
            LxVec3 p = lx_add3(lx_add3(lx_scale3(nn, 0.5f), lx_scale3(right, sx)), lx_scale3(up, sy));
            mb_vertex(b, p, nn, sx + 0.5f, sy + 0.5f);
        }
        mb_tri(b, base, base + 1, base + 2);
        mb_tri(b, base, base + 2, base + 3);
    }
}

static void build_sphere(MeshBuilder *b, int seg, int rings, float radius, float y_offset_top, float y_offset_bottom) {
    /* y_offset parameters let the capsule reuse this: upper hemisphere shifted up, lower shifted down */
    for (int r = 0; r <= rings; r++) {
        float v = (float)r / (float)rings;
        float phi = v * LX_PI;
        float y = cosf(phi), s = sinf(phi);
        float off = (r <= rings / 2) ? y_offset_top : y_offset_bottom;
        for (int i = 0; i <= seg; i++) {
            float u = (float)i / (float)seg;
            float th = u * 2.0f * LX_PI;
            LxVec3 n = lx_v3(s * cosf(th), y, -s * sinf(th));
            mb_vertex(b, lx_v3(n.x * radius, n.y * radius + off, n.z * radius), n, u, 1.0f - v);
        }
    }
    for (int r = 0; r < rings; r++)
        for (int i = 0; i < seg; i++) {
            uint32_t a = (uint32_t)(r * (seg + 1) + i), c = a + (uint32_t)seg + 1;
            mb_tri(b, a, c, a + 1);
            mb_tri(b, a + 1, c, c + 1);
        }
}

static void build_plane(MeshBuilder *b, int div) {
    for (int z = 0; z <= div; z++)
        for (int x = 0; x <= div; x++) {
            float u = (float)x / (float)div, v = (float)z / (float)div;
            mb_vertex(b, lx_v3((u - 0.5f) * 10.0f, 0, (v - 0.5f) * 10.0f), lx_v3(0, 1, 0), u * 10.0f, v * 10.0f);
        }
    for (int z = 0; z < div; z++)
        for (int x = 0; x < div; x++) {
            uint32_t a = (uint32_t)(z * (div + 1) + x), c = a + (uint32_t)div + 1;
            mb_tri(b, a, c, a + 1);
            mb_tri(b, a + 1, c, c + 1);
        }
}

static void build_cylinder(MeshBuilder *b, int seg) {
    for (int i = 0; i <= seg; i++) {
        float u = (float)i / (float)seg, th = u * 2.0f * LX_PI;
        LxVec3 n = lx_v3(cosf(th), 0, -sinf(th));
        mb_vertex(b, lx_v3(n.x * 0.5f, -1.0f, n.z * 0.5f), n, u, 0);
        mb_vertex(b, lx_v3(n.x * 0.5f, 1.0f, n.z * 0.5f), n, u, 1);
    }
    for (int i = 0; i < seg; i++) {
        uint32_t a = (uint32_t)(i * 2);
        mb_tri(b, a, a + 2, a + 1);
        mb_tri(b, a + 1, a + 2, a + 3);
    }
    for (int cap = 0; cap < 2; cap++) {
        float y = cap ? 1.0f : -1.0f;
        LxVec3 n = lx_v3(0, y, 0);
        uint32_t c = mb_vertex(b, lx_v3(0, y, 0), n, 0.5f, 0.5f);
        uint32_t first = (uint32_t)b->vc;
        for (int i = 0; i <= seg; i++) {
            float th = (float)i / (float)seg * 2.0f * LX_PI;
            mb_vertex(b, lx_v3(cosf(th) * 0.5f, y, -sinf(th) * 0.5f), n, 0.5f + cosf(th) * 0.5f, 0.5f + sinf(th) * 0.5f);
        }
        for (int i = 0; i < seg; i++) {
            if (cap) mb_tri(b, c, first + (uint32_t)i, first + (uint32_t)i + 1);
            else mb_tri(b, c, first + (uint32_t)i + 1, first + (uint32_t)i);
        }
    }
}

LxMesh *lx_mesh_build_primitive(LxPrimitive p) {
    MeshBuilder b;
    memset(&b, 0, sizeof b);
    char ref[64];
    snprintf(ref, sizeof ref, "builtin:%s", lx_primitive_name(p));
    switch (p) {
    case LX_PRIM_CUBE: build_cube(&b); break;
    case LX_PRIM_SPHERE: build_sphere(&b, 48, 32, 0.5f, 0, 0); break;
    case LX_PRIM_PLANE: build_plane(&b, 10); break;
    case LX_PRIM_CYLINDER: build_cylinder(&b, 40); break;
    case LX_PRIM_CAPSULE: build_sphere(&b, 32, 32, 0.5f, 0.5f, -0.5f); break;
    default: build_cube(&b); break;
    }
    LxMesh *m = mb_finish(&b, ref);
    lx_mesh_generate_lods(m);
    return m;
}

const char *lx_primitive_name(LxPrimitive p) {
    static const char *names[] = {"cube", "sphere", "plane", "cylinder", "capsule"};
    return (p >= 0 && p < LX_PRIM_COUNT) ? names[p] : "cube";
}

/* ---------------- OBJ import ---------------- */
LxMesh *lx_mesh_load_obj(const char *path) {
    char *text = lx_file_read(path, NULL);
    if (!text) return NULL;
    LxVec3 *pos = NULL, *nrm = NULL;
    LxVec2 *uv = NULL;
    int pc = 0, pcap = 0, nc = 0, ncap = 0, uc = 0, ucap = 0;
    MeshBuilder b;
    memset(&b, 0, sizeof b);
    char *line = text;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == 'v' && line[1] == ' ') {
            LxVec3 p;
            sscanf(line + 2, "%f %f %f", &p.x, &p.y, &p.z);
            LX_VEC_PUSH(pos, pc, pcap, p);
        } else if (line[0] == 'v' && line[1] == 'n') {
            LxVec3 n;
            sscanf(line + 3, "%f %f %f", &n.x, &n.y, &n.z);
            LX_VEC_PUSH(nrm, nc, ncap, n);
        } else if (line[0] == 'v' && line[1] == 't') {
            LxVec2 t;
            sscanf(line + 3, "%f %f", &t.x, &t.y);
            LX_VEC_PUSH(uv, uc, ucap, t);
        } else if (line[0] == 'f' && line[1] == ' ') {
            uint32_t face[64];
            int fc = 0;
            char *tok = line + 2;
            while (*tok && fc < 64) {
                while (*tok == ' ' || *tok == '\t') tok++;
                if (!*tok || *tok == '\r') break;
                int vi = 0, ti = 0, ni = 0;
                vi = (int)strtol(tok, &tok, 10);
                if (*tok == '/') {
                    tok++;
                    if (*tok != '/') ti = (int)strtol(tok, &tok, 10);
                    if (*tok == '/') { tok++; ni = (int)strtol(tok, &tok, 10); }
                }
                while (*tok && *tok != ' ' && *tok != '\t') tok++;
                if (vi < 0) vi = pc + vi + 1;
                if (ti < 0) ti = uc + ti + 1;
                if (ni < 0) ni = nc + ni + 1;
                if (vi < 1 || vi > pc) continue;
                LxVec3 p = pos[vi - 1];
                LxVec3 n = (ni >= 1 && ni <= nc) ? nrm[ni - 1] : lx_v3(0, 0, 0);
                LxVec2 t = (ti >= 1 && ti <= uc) ? uv[ti - 1] : lx_v2(0, 0);
                face[fc++] = mb_vertex(&b, p, n, t.x, t.y);
            }
            for (int k = 1; k + 1 < fc; k++) mb_tri(&b, face[0], face[k], face[k + 1]);
        }
        line = next;
    }
    lx_free(text);
    lx_free(pos); lx_free(uv);
    if (!b.ic) { lx_free(nrm); lx_free(b.v); lx_free(b.i); return NULL; }
    if (!nc) { /* flat normals */
        for (int t = 0; t + 2 < b.ic; t += 3) {
            LxVertex *a = &b.v[b.i[t]], *c = &b.v[b.i[t + 1]], *d = &b.v[b.i[t + 2]];
            LxVec3 n = lx_norm3(lx_cross3(lx_v3(c->px - a->px, c->py - a->py, c->pz - a->pz),
                                          lx_v3(d->px - a->px, d->py - a->py, d->pz - a->pz)));
            a->nx = c->nx = d->nx = n.x; a->ny = c->ny = d->ny = n.y; a->nz = c->nz = d->nz = n.z;
        }
    }
    lx_free(nrm);
    LxMesh *m = mb_finish(&b, path);
    lx_mesh_generate_lods(m);
    return m;
}

/* ---------------- LOD generation (vertex clustering) ----------------
 * Each LOD snaps vertices to a coarser grid (split by normal octant to keep hard
 * edges) and drops collapsed triangles. Cheap, robust and good for distant geometry. */
static bool cluster_lod(const LxMesh *m, int grid, LxMeshLod *out) {
    const LxMeshLod *src = &m->lods[0];
    LxVec3 size = lx_sub3(m->bounds.max, m->bounds.min);
    float cell = lx_maxf(size.x, lx_maxf(size.y, size.z)) / (float)grid;
    if (cell <= 0) return false;
    int hash_cap = 1;
    while (hash_cap < src->vertex_count * 2) hash_cap <<= 1;
    int64_t *keys = lx_alloc(sizeof(int64_t) * (size_t)hash_cap);
    int *vals = lx_alloc(sizeof(int) * (size_t)hash_cap);
    for (int i = 0; i < hash_cap; i++) keys[i] = -1;
    int *remap = lx_alloc(sizeof(int) * (size_t)src->vertex_count);
    LxVertex *nv = lx_alloc(sizeof(LxVertex) * (size_t)src->vertex_count);
    float *wsum = lx_calloc((size_t)src->vertex_count, sizeof(float));
    int nvc = 0;
    for (int i = 0; i < src->vertex_count; i++) {
        const LxVertex *v = &src->vertices[i];
        int64_t cx = (int64_t)((v->px - m->bounds.min.x) / cell), cy = (int64_t)((v->py - m->bounds.min.y) / cell),
                cz = (int64_t)((v->pz - m->bounds.min.z) / cell);
        int64_t oct = (v->nx > 0) | ((v->ny > 0) << 1) | ((v->nz > 0) << 2);
        int64_t key = (cx << 42) ^ (cy << 21) ^ cz ^ (oct << 61);
        uint32_t h = (uint32_t)(((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 32) & (uint32_t)(hash_cap - 1);
        while (keys[h] != -1 && keys[h] != key) h = (h + 1) & (uint32_t)(hash_cap - 1);
        if (keys[h] == -1) {
            keys[h] = key;
            vals[h] = nvc;
            nv[nvc] = *v;
            nv[nvc].px = nv[nvc].py = nv[nvc].pz = 0;
            nvc++;
        }
        int c = vals[h];
        remap[i] = c;
        nv[c].px += v->px; nv[c].py += v->py; nv[c].pz += v->pz;
        wsum[c] += 1.0f;
    }
    for (int i = 0; i < nvc; i++) { nv[i].px /= wsum[i]; nv[i].py /= wsum[i]; nv[i].pz /= wsum[i]; }
    uint32_t *idx = lx_alloc(sizeof(uint32_t) * (size_t)src->index_count);
    int ic = 0;
    for (int t = 0; t + 2 < src->index_count; t += 3) {
        int a = remap[src->indices[t]], b = remap[src->indices[t + 1]], c = remap[src->indices[t + 2]];
        if (a == b || b == c || a == c) continue;
        idx[ic++] = (uint32_t)a; idx[ic++] = (uint32_t)b; idx[ic++] = (uint32_t)c;
    }
    lx_free(keys); lx_free(vals); lx_free(remap); lx_free(wsum);
    out->vertices = nv; out->vertex_count = nvc;
    out->indices = idx; out->index_count = ic;
    return true;
}

void lx_mesh_generate_lods(LxMesh *m) {
    m->lod_count = 1;
    m->lods[0].screen_size = 0.0f;
    if (m->lods[0].index_count / 3 < 512) return;
    static const int grids[] = {48, 20, 8};
    static const float sizes[] = {0.20f, 0.07f, 0.02f};
    int prev_tris = m->lods[0].index_count / 3;
    for (int l = 0; l < 3 && m->lod_count < LX_MAX_LODS; l++) {
        LxMeshLod lod;
        memset(&lod, 0, sizeof lod);
        if (!cluster_lod(m, grids[l], &lod)) continue;
        if (lod.index_count < 12 || lod.index_count / 3 > prev_tris * 3 / 4) {
            lx_free(lod.indices); lx_free(lod.vertices);
            continue;
        }
        prev_tris = lod.index_count / 3;
        m->lods[m->lod_count - 1].screen_size = sizes[l];
        lod.screen_size = 0.0f;
        m->lods[m->lod_count++] = lod;
    }
}

void lx_mesh_destroy(LxMesh *m) {
    if (!m) return;
    lx_renderer_mesh_release(m);
    for (int l = 0; l < m->lod_count; l++) {
        lx_free(m->lods[l].vertices);
        lx_free(m->lods[l].indices);
    }
    lx_free(m);
}

LxAabb lx_mesh_bounds(const LxMesh *m) { return m->bounds; }
int lx_mesh_triangle_count(const LxMesh *m) { return m->lods[0].index_count / 3; }
int lx_mesh_lod_count(const LxMesh *m) { return m->lod_count; }
