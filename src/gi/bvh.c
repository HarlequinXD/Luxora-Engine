/* Triangle BVH (binned SAH) over a scene's static geometry.
 * Shared by the GI (its distance fields) and the acoustic ray tracer. */
#include "../lx_internal.h"

#define BINS 12
#define LEAF_MAX 4

typedef struct BuildCtx {
    LxBvh *b;
    LxVec3 *centroids;
    LxAabb *tri_bounds;
} BuildCtx;

static void node_bounds(BuildCtx *c, LxBvhNode *n) {
    LxAabb bb = lx_aabb_empty();
    for (int i = 0; i < n->count; i++) {
        LxAabb t = c->tri_bounds[c->b->tri_index[n->left_or_first + i]];
        bb.min = lx_min3(bb.min, t.min);
        bb.max = lx_max3(bb.max, t.max);
    }
    n->bounds = bb;
}

static float area(LxAabb b) {
    LxVec3 e = lx_sub3(b.max, b.min);
    if (e.x < 0) return 0;
    return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

static void subdivide(BuildCtx *c, int ni) {
    LxBvh *b = c->b;
    LxBvhNode *n = &b->nodes[ni];
    if (n->count <= LEAF_MAX) return;
    LxAabb cb = lx_aabb_empty();
    for (int i = 0; i < n->count; i++) lx_aabb_grow(&cb, c->centroids[b->tri_index[n->left_or_first + i]]);
    float best_cost = 1e30f;
    int best_axis = -1, best_split = 0;
    for (int axis = 0; axis < 3; axis++) {
        float lo = lx_comp3(cb.min, axis), hi = lx_comp3(cb.max, axis);
        if (hi - lo < 1e-6f) continue;
        LxAabb bin_b[BINS];
        int bin_n[BINS] = {0};
        for (int k = 0; k < BINS; k++) bin_b[k] = lx_aabb_empty();
        float scale = BINS / (hi - lo);
        for (int i = 0; i < n->count; i++) {
            int t = b->tri_index[n->left_or_first + i];
            int k = (int)((lx_comp3(c->centroids[t], axis) - lo) * scale);
            if (k >= BINS) k = BINS - 1;
            bin_n[k]++;
            bin_b[k].min = lx_min3(bin_b[k].min, c->tri_bounds[t].min);
            bin_b[k].max = lx_max3(bin_b[k].max, c->tri_bounds[t].max);
        }
        float left_area[BINS - 1], right_area[BINS - 1];
        int left_n[BINS - 1], right_n[BINS - 1];
        LxAabb lb = lx_aabb_empty(), rb = lx_aabb_empty();
        int ls = 0, rs = 0;
        for (int k = 0; k < BINS - 1; k++) {
            ls += bin_n[k];
            left_n[k] = ls;
            lb.min = lx_min3(lb.min, bin_b[k].min); lb.max = lx_max3(lb.max, bin_b[k].max);
            left_area[k] = area(lb);
            rs += bin_n[BINS - 1 - k];
            right_n[BINS - 2 - k] = rs;
            rb.min = lx_min3(rb.min, bin_b[BINS - 1 - k].min); rb.max = lx_max3(rb.max, bin_b[BINS - 1 - k].max);
            right_area[BINS - 2 - k] = area(rb);
        }
        for (int k = 0; k < BINS - 1; k++) {
            float cost = left_n[k] * left_area[k] + right_n[k] * right_area[k];
            if (left_n[k] && right_n[k] && cost < best_cost) { best_cost = cost; best_axis = axis; best_split = k; }
        }
    }
    if (best_axis < 0 || best_cost >= n->count * area(n->bounds)) return;
    float lo = lx_comp3(cb.min, best_axis), hi = lx_comp3(cb.max, best_axis);
    float scale = BINS / (hi - lo);
    int i = n->left_or_first, j = i + n->count - 1;
    while (i <= j) {
        int t = b->tri_index[i];
        int k = (int)((lx_comp3(c->centroids[t], best_axis) - lo) * scale);
        if (k >= BINS) k = BINS - 1;
        if (k <= best_split) i++;
        else { b->tri_index[i] = b->tri_index[j]; b->tri_index[j] = t; j--; }
    }
    int left_count = i - n->left_or_first;
    if (left_count == 0 || left_count == n->count) return;
    int li = b->node_count++, ri = b->node_count++;
    b->nodes[li].left_or_first = n->left_or_first;
    b->nodes[li].count = left_count;
    b->nodes[ri].left_or_first = i;
    b->nodes[ri].count = n->count - left_count;
    n->left_or_first = li;
    n->count = 0;
    node_bounds(c, &b->nodes[li]);
    node_bounds(c, &b->nodes[ri]);
    subdivide(c, li);
    subdivide(c, ri);
}

void lx_bvh_free(LxBvh *b) {
    lx_free(b->v0); lx_free(b->e1); lx_free(b->e2); lx_free(b->normals);
    lx_free(b->mat_index); lx_free(b->tri_index); lx_free(b->nodes); lx_free(b->materials);
    memset(b, 0, sizeof *b);
}

/* which objects a build takes: the level's static geometry, or (moving) everything that moves */
/* the average colour of an albedo texture (linear), so bounced light takes the surface's real colour; white without one */
static LxVec3 texture_average(const char *ref) {
    if (!ref || !ref[0]) return lx_v3(1, 1, 1);
    LxTexture *t = lx_asset_texture(ref);
    if (!t || !t->pixels || t->width <= 0 || t->height <= 0) return lx_v3(1, 1, 1);
    double sum[3] = {0};
    long n = 0;
    int step = (t->width * t->height > 65536) ? (int)sqrt((double)t->width * t->height / 65536.0) : 1;
    for (int y = 0; y < t->height; y += step)
        for (int x = 0; x < t->width; x += step) {
            const uint8_t *p = &t->pixels[((size_t)y * t->width + x) * 4];
            for (int c = 0; c < 3; c++) sum[c] += pow(p[c] / 255.0, 2.2);   /* sRGB -> linear */
            n++;
        }
    if (!n) return lx_v3(1, 1, 1);
    return lx_v3((float)(sum[0] / n), (float)(sum[1] / n), (float)(sum[2] / n));
}

static bool takes(LxScene *s, int i, bool moving) {
    LxEntityData *d = &s->entities[i];
    if (!d->alive || !d->active || !(d->comps & LX_COMP_MESH)) return false;
    bool can_be_static = lx_entity_can_be_static(s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), NULL);
    if (!moving) return d->mesh.static_geometry && can_be_static;
    if (can_be_static) return false;
    for (LxEntity p = d->parent; p; p = lx_entity_data(s, p)->parent) {
        LxEntityData *pd = lx_entity_data(s, p);
        if (!pd->active) return false;                    /* hidden by an inactive parent */
        if (pd->comps & LX_COMP_CAMERA) return false;     /* a first person gun or hands: in front of the eye, not in the world */
    }
    /* small things (pickups, eyes, swords) change the bounce light too little to be worth re-solving it every time
     * they move: only things bigger than about 0.8 m (characters, vehicles, doors) */
    const LxMesh *m = lx_skin_mesh_of(d);
    if (m) {
        LxVec3 e = lx_sub3(m->bounds.max, m->bounds.min);
        LxVec3 sc = lx_v3(lx_len3(lx_v3(d->world.m[0], d->world.m[1], d->world.m[2])), lx_len3(lx_v3(d->world.m[4], d->world.m[5], d->world.m[6])),
                          lx_len3(lx_v3(d->world.m[8], d->world.m[9], d->world.m[10])));
        float size = lx_maxf(e.x * sc.x, lx_maxf(e.y * sc.y, e.z * sc.z));
        if (size < 0.8f) return false;
    }
    return true;
}
bool lx_gi_moving_takes(LxScene *s, int i) { return takes(s, i, true); }
/* the triangles it uses: static geometry its full detail; moving objects their current pose (skinned meshes are
 * deformed on the CPU every frame) at their lowest level of detail, to keep the per-frame build cheap */
static const LxMeshLod *build_lod(LxEntityData *d, bool moving) {
    const LxMesh *m = moving ? lx_skin_mesh_of(d) : lx_asset_mesh(d->mesh.mesh);
    if (!m) return NULL;
    /* terrain chunks: a lighter level (about 2 m between points) is plenty for bounce light, sound and baking */
    if (d->generated == LX_GEN_TERRAIN_CHUNK && m->lod_count > 2) return &m->lods[2];
    return moving && !m->skin && m->lod_count > 1 ? &m->lods[m->lod_count - 1] : &m->lods[0];
}
#define MOVING_TRI_MAX 400000   /* the most moving triangles GI takes in one frame */

static void build(LxBvh *b, LxScene *s, bool moving) {
    lx_bvh_free(b);
    lx_scene_update_transforms(s);
    /* count */
    int tris = 0;
    for (int i = 1; i < s->count; i++) {
        if (!takes(s, i, moving)) continue;
        const LxMeshLod *l = build_lod(&s->entities[i], moving);
        int n = l ? l->index_count / 3 : 0;
        if (moving && tris + n > MOVING_TRI_MAX) continue;
        tris += n;
    }
    b->bounds = lx_aabb_empty();
    if (!tris) return;
    b->v0 = lx_alloc(sizeof(LxVec3) * (size_t)tris);
    b->e1 = lx_alloc(sizeof(LxVec3) * (size_t)tris);
    b->e2 = lx_alloc(sizeof(LxVec3) * (size_t)tris);
    b->normals = lx_alloc(sizeof(LxVec3) * (size_t)tris);
    b->mat_index = lx_alloc(sizeof(uint16_t) * (size_t)tris);
    b->tri_index = lx_alloc(sizeof(int) * (size_t)tris);
    b->nodes = lx_alloc(sizeof(LxBvhNode) * (size_t)(tris * 2));
    BuildCtx c = {b, lx_alloc(sizeof(LxVec3) * (size_t)tris), lx_alloc(sizeof(LxAabb) * (size_t)tris)};
    const LxMaterial **mats = NULL;
    int mat_cap = 0;
    int t = 0;
    for (int i = 1; i < s->count; i++) {
        if (!takes(s, i, moving)) continue;
        LxEntityData *d = &s->entities[i];
        const LxMeshLod *l0 = build_lod(d, moving);
        if (!l0 || t + l0->index_count / 3 > tris) continue;
        const LxMaterial *mat = lx_asset_material(d->mesh.material);
        int mi = -1;
        for (int k = 0; k < b->material_count; k++)
            if (mats[k] == mat) { mi = k; break; }
        if (mi < 0) {
            if (b->material_count >= 65535) mi = 0;
            else {
                const LxMaterial *mm = mat;
                LX_VEC_PUSH(mats, b->material_count, mat_cap, mm);
                mi = b->material_count - 1;
            }
        }
        for (int k = 0; k + 2 < l0->index_count; k += 3, t++) {
            const LxVertex *va = &l0->vertices[l0->indices[k]], *vb = &l0->vertices[l0->indices[k + 1]], *vc = &l0->vertices[l0->indices[k + 2]];
            LxVec3 a = lx_mat4_point(d->world, lx_v3(va->px, va->py, va->pz));
            LxVec3 bb = lx_mat4_point(d->world, lx_v3(vb->px, vb->py, vb->pz));
            LxVec3 cc = lx_mat4_point(d->world, lx_v3(vc->px, vc->py, vc->pz));
            b->v0[t] = a;
            b->e1[t] = lx_sub3(bb, a);
            b->e2[t] = lx_sub3(cc, a);
            b->normals[t] = lx_norm3(lx_cross3(b->e1[t], b->e2[t]));
            b->mat_index[t] = (uint16_t)mi;
            b->tri_index[t] = t;
            LxAabb tb = lx_aabb_empty();
            lx_aabb_grow(&tb, a); lx_aabb_grow(&tb, bb); lx_aabb_grow(&tb, cc);
            c.tri_bounds[t] = tb;
            c.centroids[t] = lx_scale3(lx_add3(lx_add3(a, bb), cc), 1.0f / 3.0f);
            lx_aabb_grow(&b->bounds, tb.min);
            lx_aabb_grow(&b->bounds, tb.max);
        }
    }
    b->tri_count = t;
    b->materials = lx_alloc(sizeof(LxTriMaterial) * (size_t)(b->material_count ? b->material_count : 1));
    for (int k = 0; k < b->material_count; k++) {
        const LxMaterial *m = mats[k];
        /* metals reflect little diffuse light; approximate their GI albedo */
        float diffuse = 1.0f - m->metallic * 0.9f;
        b->materials[k].albedo = lx_scale3(lx_mul3(lx_v3(m->albedo.x, m->albedo.y, m->albedo.z), texture_average(m->albedo_map)), diffuse);
        b->materials[k].emissive = lx_scale3(m->emissive, m->emissive_strength);
        b->materials[k].absorption = m->acoustic_absorption;
        b->materials[k].transmission = m->acoustic_transmission;
    }
    lx_free((void *)mats);
    b->node_count = 1;
    b->nodes[0].left_or_first = 0;
    b->nodes[0].count = t;
    node_bounds(&c, &b->nodes[0]);
    subdivide(&c, 0);
    lx_free(c.centroids);
    lx_free(c.tri_bounds);
}

void lx_bvh_build_from_scene(LxBvh *b, LxScene *s) { build(b, s, false); }

/* Moving objects keep their triangles from frame to frame (an animated character just moves its vertices), so
 * the BVH built for them is refitted instead of rebuilt: new triangle positions, then every node's box from the
 * bottom up. Linear and many times cheaper than a build. Returns false when the objects or their triangle
 * counts changed (the caller builds again). */
bool lx_bvh_refit_moving(LxBvh *b, LxScene *s) {
    if (!b->tri_count) return false;
    lx_scene_update_transforms(s);
    int t = 0;
    for (int i = 1; i < s->count; i++) {
        if (!takes(s, i, true)) continue;
        LxEntityData *d = &s->entities[i];
        const LxMeshLod *l0 = build_lod(d, true);
        if (!l0) continue;
        if (t + l0->index_count / 3 > b->tri_count) return false;
        for (int k = 0; k + 2 < l0->index_count; k += 3, t++) {
            const LxVertex *va = &l0->vertices[l0->indices[k]], *vb = &l0->vertices[l0->indices[k + 1]], *vc = &l0->vertices[l0->indices[k + 2]];
            LxVec3 a = lx_mat4_point(d->world, lx_v3(va->px, va->py, va->pz));
            LxVec3 bb = lx_mat4_point(d->world, lx_v3(vb->px, vb->py, vb->pz));
            LxVec3 cc = lx_mat4_point(d->world, lx_v3(vc->px, vc->py, vc->pz));
            b->v0[t] = a;
            b->e1[t] = lx_sub3(bb, a);
            b->e2[t] = lx_sub3(cc, a);
            LxVec3 nn = lx_cross3(b->e1[t], b->e2[t]);
            float len = lx_len3(nn);
            b->normals[t] = len > 1e-12f ? lx_scale3(nn, 1.0f / len) : lx_v3(0, 1, 0);
        }
    }
    if (t != b->tri_count) return false;
    /* children always come after their parent in the node array: walk it backwards */
    for (int i = b->node_count - 1; i >= 0; i--) {
        LxBvhNode *n = &b->nodes[i];
        LxAabb bb = lx_aabb_empty();
        if (n->count > 0) {
            for (int k = 0; k < n->count; k++) {
                int tri = b->tri_index[n->left_or_first + k];
                LxVec3 a = b->v0[tri];
                lx_aabb_grow(&bb, a);
                lx_aabb_grow(&bb, lx_add3(a, b->e1[tri]));
                lx_aabb_grow(&bb, lx_add3(a, b->e2[tri]));
            }
        } else {
            const LxAabb *l = &b->nodes[n->left_or_first].bounds, *r = &b->nodes[n->left_or_first + 1].bounds;
            bb.min = lx_min3(l->min, r->min);
            bb.max = lx_max3(l->max, r->max);
        }
        n->bounds = bb;
    }
    b->bounds = b->nodes[0].bounds;
    return true;
}
/* moving objects as they are this frame (skinned characters in their current pose): GI rebuilds it when they move */
void lx_bvh_build_moving(LxBvh *b, LxScene *s) { build(b, s, true); }

static inline float ray_box(const LxAabb *bb, LxVec3 o, LxVec3 inv, float tmax) {
    float tx1 = (bb->min.x - o.x) * inv.x, tx2 = (bb->max.x - o.x) * inv.x;
    float tmin = lx_minf(tx1, tx2), tmx = lx_maxf(tx1, tx2);
    float ty1 = (bb->min.y - o.y) * inv.y, ty2 = (bb->max.y - o.y) * inv.y;
    tmin = lx_maxf(tmin, lx_minf(ty1, ty2)); tmx = lx_minf(tmx, lx_maxf(ty1, ty2));
    float tz1 = (bb->min.z - o.z) * inv.z, tz2 = (bb->max.z - o.z) * inv.z;
    tmin = lx_maxf(tmin, lx_minf(tz1, tz2)); tmx = lx_minf(tmx, lx_maxf(tz1, tz2));
    return (tmx >= tmin && tmin < tmax && tmx > 0) ? tmin : 1e30f;
}

static bool trace(const LxBvh *b, LxRay r, float tmax, LxHit *hit, bool any) {
    if (!b->node_count) return false;
    LxVec3 inv = lx_v3(1.0f / (fabsf(r.dir.x) > 1e-9f ? r.dir.x : 1e-9f), 1.0f / (fabsf(r.dir.y) > 1e-9f ? r.dir.y : 1e-9f),
                       1.0f / (fabsf(r.dir.z) > 1e-9f ? r.dir.z : 1e-9f));
    int stack[64], sp = 0;
    int node = 0;
    bool found = false;
    float best = tmax;
    if (ray_box(&b->nodes[0].bounds, r.origin, inv, best) >= 1e30f) return false;
    for (;;) {
        const LxBvhNode *n = &b->nodes[node];
        if (n->count) {
            for (int i = 0; i < n->count; i++) {
                int t = b->tri_index[n->left_or_first + i];
                LxVec3 e1 = b->e1[t], e2 = b->e2[t];
                LxVec3 p = lx_cross3(r.dir, e2);
                float det = lx_dot3(e1, p);
                if (fabsf(det) < 1e-12f) continue;
                float id = 1.0f / det;
                LxVec3 tv = lx_sub3(r.origin, b->v0[t]);
                float u = lx_dot3(tv, p) * id;
                if (u < 0 || u > 1) continue;
                LxVec3 q = lx_cross3(tv, e1);
                float v = lx_dot3(r.dir, q) * id;
                if (v < 0 || u + v > 1) continue;
                float d = lx_dot3(e2, q) * id;
                if (d > 1e-4f && d < best) {
                    best = d;
                    found = true;
                    if (hit) { hit->t = d; hit->tri = t; hit->u = u; hit->v = v; }
                    if (any) return true;
                }
            }
            if (!sp) break;
            node = stack[--sp];
            continue;
        }
        int c1 = n->left_or_first, c2 = c1 + 1;
        float d1 = ray_box(&b->nodes[c1].bounds, r.origin, inv, best);
        float d2 = ray_box(&b->nodes[c2].bounds, r.origin, inv, best);
        if (d1 > d2) { float td = d1; d1 = d2; d2 = td; int ti = c1; c1 = c2; c2 = ti; }
        if (d1 >= 1e30f) {
            if (!sp) break;
            node = stack[--sp];
        } else {
            node = c1;
            if (d2 < 1e30f && sp < 64) stack[sp++] = c2;
        }
    }
    return found;
}

bool lx_bvh_intersect(const LxBvh *b, LxRay r, float tmax, LxHit *hit) { return trace(b, r, tmax, hit, false); }
bool lx_bvh_occluded(const LxBvh *b, LxRay r, float tmax) { return trace(b, r, tmax, NULL, true); }
