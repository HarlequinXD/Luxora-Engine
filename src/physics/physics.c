/* Luxora physics: rigid bodies, colliders, triggers, queries and a character controller.
 *
 * Shapes: box, sphere, capsule (dynamic or static) and triangle meshes (static scenery).
 * Each fixed step:
 *   1. gather bodies from the scene (colliders, rigidbodies, character controllers)
 *   2. integrate forces and gravity into velocities
 *   3. broadphase (sort-and-sweep on X) -> narrowphase contact manifolds (up to 4 points per pair;
 *      box-box uses SAT + face clipping), with a small speculative margin so fast bodies don't tunnel
 *   4. sequential-impulse solver (normal + 2-axis friction, restitution, Baumgarte position correction)
 *   5. integrate positions/rotations, put resting bodies to sleep, write poses back to entities
 *   6. diff touching pairs against the previous step -> collision/trigger enter/exit callbacks
 * The body list is rebuilt from the entities every step, so scripts can add, remove, move or
 * resize anything at any time without telling the physics engine.
 */
#include "../lx_internal.h"

#define MARGIN 0.02f        /* speculative contact distance */
#define SLOP 0.004f         /* allowed penetration before correction */
#define BAUMGARTE 0.25f
#define MAX_MANIFOLD 4
#define SLEEP_LINEAR 0.08f
#define SLEEP_ANGULAR 0.12f
#define SLEEP_TIME 0.5f

/* ======================================================================
 *  settings
 * ====================================================================== */
static LxPhysicsSettings g_settings;
static bool g_settings_init;

LxPhysicsSettings lx_physics_defaults(void) {
    LxPhysicsSettings p;
    memset(&p, 0, sizeof p);
    p.gravity = lx_v3(0, -9.81f, 0);
    p.fixed_timestep = 1.0f / 60.0f;
    p.solver_iterations = 10;
    for (int i = 0; i < 32; i++) p.layer_mask[i] = 0xffffffffu;
    return p;
}

LxPhysicsSettings *lx_physics_settings(void) {
    if (!g_settings_init) { g_settings = lx_physics_defaults(); g_settings_init = true; }
    return &g_settings;
}

/* ======================================================================
 *  bodies and the world
 * ====================================================================== */
enum { BODY_STATIC, BODY_KINEMATIC, BODY_DYNAMIC };

typedef struct Body {
    LxEntity e;
    int index;                 /* entity slot */
    int kind;
    bool trigger, character, asleep;
    LxColliderShape shape;
    LxVec3 c;                  /* world centre (centre of mass) */
    LxQuat q;                  /* world rotation */
    LxVec3 ax[3];              /* world axes */
    LxVec3 offset;             /* collider centre in the entity's frame (scaled), for writing back */
    LxVec3 half;               /* box half extents (scaled) */
    float radius, hseg;        /* sphere/capsule radius, capsule half segment length */
    float inv_mass;
    LxVec3 inv_i;              /* local inverse inertia (diagonal) */
    float iw[9];               /* world inverse inertia, row major */
    LxVec3 v, w;
    float friction, restitution;
    int layer;
    LxAabb box;
    int mesh;                  /* mesh cache index or -1 */
} Body;

typedef struct Tri { LxVec3 a, b, c, n; } Tri;
typedef struct MNode { LxAabb box; int left, right, first, count; } MNode;  /* leaf when count > 0 */
typedef struct MeshCache {
    uint32_t entity;           /* entity handle */
    const LxMesh *mesh;
    float key[16];             /* world matrix it was built for */
    Tri *tris;
    int ntris;
    MNode *nodes;
    int nnodes;
    int *order;
    bool used;
} MeshCache;

typedef struct Contact {
    int a, b;
    LxVec3 n;                  /* from a to b */
    LxVec3 p;
    float sep;                 /* < 0 penetrating */
    LxVec3 ra, rb, t1, t2;
    float mn, mt1, mt2, target;
    float jn, jt1, jt2;
    float friction, restitution;
} Contact;

typedef struct Pair {
    uint64_t key;              /* (handle a << 32) | handle b, a < b */
    bool trigger;
    LxVec3 point, normal;      /* normal from a to b */
    float impulse;
    LxVec3 rel_vel;            /* vb - va at the contact */
} Pair;

typedef struct LxPhysicsWorld {
    float accumulator;
    Body *bodies;
    int nb, cb;
    Contact *contacts;
    int nc, cc;
    MeshCache *meshes;
    int nm, cm;
    Pair *pairs, *prev;
    int np, cp, nprev, cprev;
    uint64_t query_frame, built_frame;
    uint64_t frame;
} LxPhysicsWorld;

static LxPhysicsWorld *world_of(LxScene *s) {
    if (!s->physics) s->physics = lx_calloc(1, sizeof(LxPhysicsWorld));
    return s->physics;
}

void lx_physics_world_free(LxScene *s) {
    LxPhysicsWorld *w = s->physics;
    if (!w) return;
    for (int i = 0; i < w->nm; i++) { lx_free(w->meshes[i].tris); lx_free(w->meshes[i].nodes); lx_free(w->meshes[i].order); }
    lx_free(w->meshes);
    lx_free(w->bodies);
    lx_free(w->contacts);
    lx_free(w->pairs);
    lx_free(w->prev);
    lx_free(w);
    s->physics = NULL;
}

/* ======================================================================
 *  small geometry helpers
 * ====================================================================== */
static LxVec3 cross(LxVec3 a, LxVec3 b) { return lx_cross3(a, b); }
static float dot(LxVec3 a, LxVec3 b) { return lx_dot3(a, b); }
static LxVec3 sub(LxVec3 a, LxVec3 b) { return lx_sub3(a, b); }
static LxVec3 add(LxVec3 a, LxVec3 b) { return lx_add3(a, b); }
static LxVec3 scl(LxVec3 a, float s) { return lx_scale3(a, s); }
static LxVec3 mul33(const float *m, LxVec3 v) {
    return lx_v3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z);
}

static void tangents(LxVec3 n, LxVec3 *t1, LxVec3 *t2) {
    *t1 = fabsf(n.x) > 0.57f ? lx_v3(n.y, -n.x, 0) : lx_v3(0, n.z, -n.y);
    *t1 = lx_norm3(*t1);
    *t2 = cross(n, *t1);
}

static LxVec3 closest_on_segment(LxVec3 p, LxVec3 a, LxVec3 b) {
    LxVec3 ab = sub(b, a);
    float d = dot(ab, ab);
    float t = d > 1e-12f ? lx_clampf(dot(sub(p, a), ab) / d, 0, 1) : 0;
    return lx_madd3(a, ab, t);
}

/* closest points between segments p1q1 and p2q2 (Ericson, Real-Time Collision Detection 5.1.9) */
static void closest_segments(LxVec3 p1, LxVec3 q1, LxVec3 p2, LxVec3 q2, LxVec3 *c1, LxVec3 *c2) {
    LxVec3 d1 = sub(q1, p1), d2 = sub(q2, p2), r = sub(p1, p2);
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r), s, t;
    if (a <= 1e-9f && e <= 1e-9f) { *c1 = p1; *c2 = p2; return; }
    if (a <= 1e-9f) { s = 0; t = lx_clampf(f / e, 0, 1); }
    else {
        float c = dot(d1, r);
        if (e <= 1e-9f) { t = 0; s = lx_clampf(-c / a, 0, 1); }
        else {
            float b = dot(d1, d2), den = a * e - b * b;
            s = den > 1e-9f ? lx_clampf((b * f - c * e) / den, 0, 1) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = lx_clampf(-c / a, 0, 1); }
            else if (t > 1) { t = 1; s = lx_clampf((b - c) / a, 0, 1); }
        }
    }
    *c1 = lx_madd3(p1, d1, s);
    *c2 = lx_madd3(p2, d2, t);
}

/* closest point on triangle abc to p (Ericson 5.1.5) */
static LxVec3 closest_on_triangle(LxVec3 p, LxVec3 a, LxVec3 b, LxVec3 c) {
    LxVec3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    LxVec3 bp = sub(p, b);
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return lx_madd3(a, ab, d1 / (d1 - d3));
    LxVec3 cp = sub(p, c);
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return lx_madd3(a, ac, d2 / (d2 - d6));
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return lx_madd3(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
    float den = 1.0f / (va + vb + vc);
    return add(a, add(scl(ab, vb * den), scl(ac, vc * den)));
}

static LxVec3 box_local(const Body *b, LxVec3 p) {
    LxVec3 d = sub(p, b->c);
    return lx_v3(dot(d, b->ax[0]), dot(d, b->ax[1]), dot(d, b->ax[2]));
}
static LxVec3 box_world(const Body *b, LxVec3 l) {
    return add(b->c, add(scl(b->ax[0], l.x), add(scl(b->ax[1], l.y), scl(b->ax[2], l.z))));
}
static LxVec3 closest_on_box(const Body *b, LxVec3 p) {
    LxVec3 l = box_local(b, p);
    l.x = lx_clampf(l.x, -b->half.x, b->half.x);
    l.y = lx_clampf(l.y, -b->half.y, b->half.y);
    l.z = lx_clampf(l.z, -b->half.z, b->half.z);
    return box_world(b, l);
}
static void capsule_points(const Body *b, LxVec3 *p0, LxVec3 *p1) {
    *p0 = lx_madd3(b->c, b->ax[1], -b->hseg);
    *p1 = lx_madd3(b->c, b->ax[1], b->hseg);
}

/* ======================================================================
 *  mesh colliders: world-space triangles + a small BVH, rebuilt when the entity moves
 * ====================================================================== */
static LxAabb tri_box(const Tri *t) {
    LxAabb b = lx_aabb_empty();
    lx_aabb_grow(&b, t->a); lx_aabb_grow(&b, t->b); lx_aabb_grow(&b, t->c);
    return b;
}

static int mesh_build_node(MeshCache *m, int first, int count) {
    int ni = m->nnodes++;
    MNode *n = &m->nodes[ni];
    n->box = lx_aabb_empty();
    LxAabb cb = lx_aabb_empty();
    for (int i = first; i < first + count; i++) {
        const Tri *t = &m->tris[m->order[i]];
        LxAabb tb = tri_box(t);
        lx_aabb_grow(&n->box, tb.min); lx_aabb_grow(&n->box, tb.max);
        lx_aabb_grow(&cb, scl(add(t->a, add(t->b, t->c)), 1.0f / 3.0f));
    }
    if (count <= 4) { n->first = first; n->count = count; n->left = n->right = -1; return ni; }
    LxVec3 ext = sub(cb.max, cb.min);
    int axis = ext.x > ext.y && ext.x > ext.z ? 0 : (ext.y > ext.z ? 1 : 2);
    float mid = (axis == 0 ? cb.min.x + cb.max.x : axis == 1 ? cb.min.y + cb.max.y : cb.min.z + cb.max.z) * 0.5f;
    int i = first, j = first + count - 1;
    while (i <= j) {
        const Tri *t = &m->tris[m->order[i]];
        LxVec3 c = scl(add(t->a, add(t->b, t->c)), 1.0f / 3.0f);
        float v = axis == 0 ? c.x : axis == 1 ? c.y : c.z;
        if (v < mid) i++;
        else { int tmp = m->order[i]; m->order[i] = m->order[j]; m->order[j] = tmp; j--; }
    }
    int left_count = i - first;
    if (left_count == 0 || left_count == count) left_count = count / 2;
    n->count = 0;
    int l = mesh_build_node(m, first, left_count);
    int r = mesh_build_node(m, first + left_count, count - left_count);
    m->nodes[ni].left = l;
    m->nodes[ni].right = r;
    return ni;
}

static int mesh_cache_get(LxPhysicsWorld *w, LxEntity e, const LxEntityData *d) {
    const LxMesh *mesh = lx_asset_mesh(d->mesh.mesh);
    for (int i = 0; i < w->nm; i++) {
        MeshCache *m = &w->meshes[i];
        if (m->entity == e && m->mesh == mesh && !memcmp(m->key, d->world.m, sizeof m->key)) { m->used = true; return i; }
    }
    /* reuse a slot of the same entity or append */
    int slot = -1;
    for (int i = 0; i < w->nm; i++) if (w->meshes[i].entity == e) { slot = i; break; }
    if (slot < 0) {
        LX_VEC_PUSH(w->meshes, w->nm, w->cm, ((MeshCache){0}));
        slot = w->nm - 1;
    }
    MeshCache *m = &w->meshes[slot];
    lx_free(m->tris); lx_free(m->nodes); lx_free(m->order);
    memset(m, 0, sizeof *m);
    m->entity = e;
    m->mesh = mesh;
    memcpy(m->key, d->world.m, sizeof m->key);
    m->used = true;
    int ic = LX_MESH_ICOUNT(mesh);
    const LxVertex *v = LX_MESH_VERTS(mesh);
    const uint32_t *idx = LX_MESH_IDX(mesh);
    m->ntris = ic / 3;
    m->tris = lx_alloc(sizeof(Tri) * (size_t)(m->ntris ? m->ntris : 1));
    m->order = lx_alloc(sizeof(int) * (size_t)(m->ntris ? m->ntris : 1));
    for (int t = 0; t < m->ntris; t++) {
        Tri *tr = &m->tris[t];
        const LxVertex *va = &v[idx[t * 3]], *vb = &v[idx[t * 3 + 1]], *vc = &v[idx[t * 3 + 2]];
        tr->a = lx_mat4_point(d->world, lx_v3(va->px, va->py, va->pz));
        tr->b = lx_mat4_point(d->world, lx_v3(vb->px, vb->py, vb->pz));
        tr->c = lx_mat4_point(d->world, lx_v3(vc->px, vc->py, vc->pz));
        tr->n = lx_norm3(cross(sub(tr->b, tr->a), sub(tr->c, tr->a)));
        m->order[t] = t;
    }
    m->nodes = lx_alloc(sizeof(MNode) * (size_t)(2 * m->ntris + 1));
    if (m->ntris) mesh_build_node(m, 0, m->ntris);
    return slot;
}

typedef void (*TriFn)(const Tri *t, void *u);
static void mesh_query(const MeshCache *m, LxAabb q, TriFn fn, void *u) {
    if (!m->nnodes) return;
    int stack[128], sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const MNode *n = &m->nodes[stack[--sp]];
        if (n->box.max.x < q.min.x || n->box.min.x > q.max.x || n->box.max.y < q.min.y || n->box.min.y > q.max.y ||
            n->box.max.z < q.min.z || n->box.min.z > q.max.z) continue;
        if (n->count) {
            for (int i = n->first; i < n->first + n->count; i++) fn(&m->tris[m->order[i]], u);
        } else if (sp < 126) {
            stack[sp++] = n->left;
            stack[sp++] = n->right;
        }
    }
}

/* ======================================================================
 *  narrowphase: contact manifolds. Normals always point from A to B.
 * ====================================================================== */
typedef struct Manifold {
    LxVec3 n[24], p[24];
    float sep[24];
    int count;
} Manifold;

static void mf_add(Manifold *m, LxVec3 n, LxVec3 p, float sep) {
    if (sep > MARGIN || m->count >= 24) return;
    for (int i = 0; i < m->count; i++)   /* merge near-duplicates (endpoint + closest point often coincide) */
        if (lx_len3sq(sub(m->p[i], p)) < 1e-4f && dot(m->n[i], n) > 0.99f) {
            if (sep < m->sep[i]) { m->sep[i] = sep; m->p[i] = p; }
            return;
        }
    m->n[m->count] = n;
    m->p[m->count] = p;
    m->sep[m->count] = sep;
    m->count++;
}

/* keep the deepest point, then repeatedly the point farthest from those kept: a stable spread */
static void mf_reduce(Manifold *m, int max) {
    if (m->count <= max) return;
    int keep[8], nk = 0;
    int deepest = 0;
    for (int i = 1; i < m->count; i++) if (m->sep[i] < m->sep[deepest]) deepest = i;
    keep[nk++] = deepest;
    while (nk < max) {
        int best = -1;
        float best_d = -1;
        for (int i = 0; i < m->count; i++) {
            float dmin = 1e30f;
            for (int k = 0; k < nk; k++) dmin = lx_minf(dmin, lx_len3sq(sub(m->p[i], m->p[keep[k]])));
            if (dmin > best_d) { best_d = dmin; best = i; }
        }
        if (best < 0 || best_d < 1e-8f) break;
        keep[nk++] = best;
    }
    Manifold r;
    r.count = nk;
    for (int k = 0; k < nk; k++) { r.n[k] = m->n[keep[k]]; r.p[k] = m->p[keep[k]]; r.sep[k] = m->sep[keep[k]]; }
    *m = r;
}

static void sphere_sphere(LxVec3 ca, float ra, LxVec3 cb, float rb, Manifold *m) {
    LxVec3 d = sub(cb, ca);
    float dist = lx_len3(d), sep = dist - ra - rb;
    if (sep > MARGIN) return;
    LxVec3 n = dist > 1e-6f ? scl(d, 1.0f / dist) : lx_v3(0, 1, 0);
    mf_add(m, n, lx_madd3(ca, n, ra + sep * 0.5f), sep);
}

/* sphere (c, r) against a box; normal from the sphere to the box (negated when flip) */
static void sphere_box(LxVec3 c, float r, const Body *box, Manifold *m, bool flip) {
    LxVec3 l = box_local(box, c);
    LxVec3 h = box->half;
    bool inside = fabsf(l.x) <= h.x && fabsf(l.y) <= h.y && fabsf(l.z) <= h.z;
    LxVec3 n, p;
    float sep;
    if (!inside) {
        LxVec3 q = closest_on_box(box, c);
        LxVec3 d = sub(q, c);
        float dist = lx_len3(d);
        sep = dist - r;
        if (sep > MARGIN) return;
        n = dist > 1e-6f ? scl(d, 1.0f / dist) : lx_norm3(sub(box->c, c));
        p = lx_madd3(q, n, -sep * 0.5f);
    } else {
        float dx = h.x - fabsf(l.x), dy = h.y - fabsf(l.y), dz = h.z - fabsf(l.z);
        int k = dx < dy && dx < dz ? 0 : (dy < dz ? 1 : 2);
        float depth = k == 0 ? dx : k == 1 ? dy : dz, lk = k == 0 ? l.x : k == 1 ? l.y : l.z;
        LxVec3 f = scl(box->ax[k], lk >= 0 ? 1.0f : -1.0f);   /* box face the centre is closest to */
        n = scl(f, -1.0f);
        sep = -(depth + r);
        p = lx_madd3(c, f, depth);
    }
    if (flip) n = scl(n, -1.0f);
    mf_add(m, n, p, sep);
}

static void capsule_sphere(const Body *cap, LxVec3 c, float r, Manifold *m) {
    LxVec3 p0, p1;
    capsule_points(cap, &p0, &p1);
    sphere_sphere(closest_on_segment(c, p0, p1), cap->radius, c, r, m);
}

static void capsule_capsule(const Body *A, const Body *B, Manifold *m) {
    LxVec3 a0, a1, b0, b1, c1, c2;
    capsule_points(A, &a0, &a1);
    capsule_points(B, &b0, &b1);
    closest_segments(a0, a1, b0, b1, &c1, &c2);
    sphere_sphere(c1, A->radius, c2, B->radius, m);
    /* lying side by side: also the ends, so they don't roll on one point */
    sphere_sphere(a0, A->radius, closest_on_segment(a0, b0, b1), B->radius, m);
    sphere_sphere(a1, A->radius, closest_on_segment(a1, b0, b1), B->radius, m);
    sphere_sphere(closest_on_segment(b0, a0, a1), A->radius, b0, B->radius, m);
    sphere_sphere(closest_on_segment(b1, a0, a1), A->radius, b1, B->radius, m);
    mf_reduce(m, 2);
}

static float seg_box_dist(const Body *box, LxVec3 p) { return lx_len3(sub(closest_on_box(box, p), p)); }

static void capsule_box(const Body *cap, const Body *box, Manifold *m) {
    LxVec3 p0, p1;
    capsule_points(cap, &p0, &p1);
    sphere_box(p0, cap->radius, box, m, false);
    sphere_box(p1, cap->radius, box, m, false);
    /* closest point of the segment to the box: the distance is convex along the segment */
    float lo = 0, hi = 1;
    for (int it = 0; it < 14; it++) {
        float t1 = lo + (hi - lo) / 3, t2 = hi - (hi - lo) / 3;
        if (seg_box_dist(box, lx_lerp3(p0, p1, t1)) < seg_box_dist(box, lx_lerp3(p0, p1, t2))) hi = t2;
        else lo = t1;
    }
    sphere_box(lx_lerp3(p0, p1, (lo + hi) * 0.5f), cap->radius, box, m, false);
    mf_reduce(m, 3);
}

static int clip_poly(const LxVec3 *in, int n, LxVec3 pn, float pd, LxVec3 *out) {
    /* keep the part with dot(p, pn) <= pd */
    int k = 0;
    for (int i = 0; i < n; i++) {
        LxVec3 a = in[i], b = in[(i + 1) % n];
        float da = dot(a, pn) - pd, db = dot(b, pn) - pd;
        if (da <= 0) out[k++] = a;
        if ((da < 0) != (db < 0) && fabsf(da - db) > 1e-12f) out[k++] = lx_lerp3(a, b, da / (da - db));
    }
    return k;
}

static void box_box(const Body *A, const Body *B, Manifold *m) {
    LxVec3 d = sub(B->c, A->c);
    float ha[3] = {A->half.x, A->half.y, A->half.z}, hb[3] = {B->half.x, B->half.y, B->half.z};
    float best = -1e30f;
    int best_axis = -1;
    LxVec3 best_n = lx_v3(0, 1, 0);
    for (int a = 0; a < 15; a++) {
        LxVec3 L;
        if (a < 3) L = A->ax[a];
        else if (a < 6) L = B->ax[a - 3];
        else {
            L = cross(A->ax[(a - 6) / 3], B->ax[(a - 6) % 3]);
            float len = lx_len3(L);
            if (len < 1e-5f) continue;     /* parallel edges: covered by face axes */
            L = scl(L, 1.0f / len);
        }
        float ra = 0, rb = 0;
        for (int k = 0; k < 3; k++) { ra += ha[k] * fabsf(dot(A->ax[k], L)); rb += hb[k] * fabsf(dot(B->ax[k], L)); }
        float dist = dot(d, L), sep = fabsf(dist) - (ra + rb);
        if (sep > MARGIN) return;
        /* prefer face axes (stable, multi-point contacts) unless an edge axis is clearly better */
        float bias = a < 3 ? 0 : (a < 6 ? 0.001f : 0.02f);
        if (sep > best + bias) {
            best = sep;
            best_axis = a;
            best_n = dist < 0 ? scl(L, -1.0f) : L;
        }
    }
    if (best_axis < 0) return;
    if (best_axis < 6) {
        bool ref_a = best_axis < 3;
        const Body *R = ref_a ? A : B, *I = ref_a ? B : A;
        LxVec3 rn = ref_a ? best_n : scl(best_n, -1.0f);   /* reference face normal, towards the incident box */
        int ri = best_axis % 3;
        float rh[3] = {R->half.x, R->half.y, R->half.z}, ih[3] = {I->half.x, I->half.y, I->half.z};
        int k = 0;
        float bestd = -1;
        for (int j = 0; j < 3; j++) { float v = fabsf(dot(I->ax[j], rn)); if (v > bestd) { bestd = v; k = j; } }
        float sgn = dot(I->ax[k], rn) > 0 ? -1.0f : 1.0f;
        LxVec3 fc = lx_madd3(I->c, I->ax[k], sgn * ih[k]);
        int u = (k + 1) % 3, v = (k + 2) % 3;
        LxVec3 eu = scl(I->ax[u], ih[u]), ev = scl(I->ax[v], ih[v]);
        LxVec3 poly[16], tmp[16];
        poly[0] = add(fc, add(eu, ev));
        poly[1] = add(fc, sub(ev, eu));
        poly[2] = sub(fc, add(eu, ev));
        poly[3] = add(fc, sub(eu, ev));
        int n = 4;
        int a1 = (ri + 1) % 3, a2 = (ri + 2) % 3;
        int axes[2] = {a1, a2};
        for (int t = 0; t < 2 && n; t++) {
            LxVec3 an = R->ax[axes[t]];
            float off = dot(R->c, an);
            n = clip_poly(poly, n, an, off + rh[axes[t]], tmp);
            n = clip_poly(tmp, n, scl(an, -1.0f), -off + rh[axes[t]], poly);
        }
        LxVec3 rc = lx_madd3(R->c, rn, rh[ri]);
        for (int i = 0; i < n; i++) {
            float sep = dot(sub(poly[i], rc), rn);
            if (sep <= MARGIN) mf_add(m, best_n, lx_madd3(poly[i], rn, -sep * 0.5f), sep);
        }
    } else {
        int i = (best_axis - 6) / 3, j = (best_axis - 6) % 3;
        LxVec3 pa = A->c, pb = B->c;
        for (int k = 0; k < 3; k++) {
            if (k != i) pa = lx_madd3(pa, A->ax[k], ha[k] * (dot(A->ax[k], best_n) > 0 ? 1.0f : -1.0f));
            if (k != j) pb = lx_madd3(pb, B->ax[k], hb[k] * (dot(B->ax[k], best_n) > 0 ? -1.0f : 1.0f));
        }
        LxVec3 c1, c2;
        closest_segments(lx_madd3(pa, A->ax[i], -ha[i]), lx_madd3(pa, A->ax[i], ha[i]), lx_madd3(pb, B->ax[j], -hb[j]),
                         lx_madd3(pb, B->ax[j], hb[j]), &c1, &c2);
        mf_add(m, best_n, scl(add(c1, c2), 0.5f), dot(sub(c2, c1), best_n));
    }
    mf_reduce(m, MAX_MANIFOLD);
}

/* ---- convex shape against mesh triangles (mesh is B) ---- */
typedef struct TriCtx { const Body *A; Manifold *m; } TriCtx;

static bool inside_triangle(LxVec3 p, const Tri *t) {
    LxVec3 n = t->n;
    return dot(cross(sub(t->b, t->a), sub(p, t->a)), n) >= -1e-6f && dot(cross(sub(t->c, t->b), sub(p, t->b)), n) >= -1e-6f &&
           dot(cross(sub(t->a, t->c), sub(p, t->c)), n) >= -1e-6f;
}

static void point_vs_triangle(LxVec3 c, float r, const Tri *t, Manifold *m) {
    LxVec3 q = closest_on_triangle(c, t->a, t->b, t->c);
    LxVec3 d = sub(q, c);
    float dist = lx_len3(d), sep = dist - r;
    if (sep > MARGIN) return;
    float side = dot(sub(c, t->a), t->n);
    LxVec3 n = dist > 1e-5f ? scl(d, 1.0f / dist) : scl(t->n, side >= 0 ? -1.0f : 1.0f);
    mf_add(m, n, lx_madd3(q, n, -sep * 0.5f), sep);
}

static void tri_cb(const Tri *t, void *u) {
    TriCtx *x = u;
    const Body *A = x->A;
    if (A->shape == LX_COLLIDER_SPHERE) { point_vs_triangle(A->c, A->radius, t, x->m); return; }
    if (A->shape == LX_COLLIDER_CAPSULE) {
        LxVec3 p0, p1;
        capsule_points(A, &p0, &p1);
        point_vs_triangle(p0, A->radius, t, x->m);
        point_vs_triangle(p1, A->radius, t, x->m);
        /* the segment's closest point to the triangle edges catches capsules lying across an edge */
        LxVec3 e[3][2] = {{t->a, t->b}, {t->b, t->c}, {t->c, t->a}};
        for (int i = 0; i < 3; i++) {
            LxVec3 c1, c2;
            closest_segments(p0, p1, e[i][0], e[i][1], &c1, &c2);
            point_vs_triangle(c1, A->radius, t, x->m);
        }
        return;
    }
    /* box: its corners against the face, the triangle's corners against the box */
    LxVec3 tn = t->n;
    if (dot(sub(A->c, t->a), tn) < 0) tn = scl(tn, -1.0f);    /* two-sided: face the box */
    float reach = lx_len3(A->half) * 2.0f;
    for (int i = 0; i < 8; i++) {
        LxVec3 l = lx_v3((i & 1) ? A->half.x : -A->half.x, (i & 2) ? A->half.y : -A->half.y, (i & 4) ? A->half.z : -A->half.z);
        LxVec3 v = box_world(A, l);
        float dist = dot(sub(v, t->a), tn);
        if (dist > MARGIN || dist < -reach) continue;
        LxVec3 proj = lx_madd3(v, tn, -dist);
        if (!inside_triangle(proj, t)) continue;
        mf_add(x->m, scl(tn, -1.0f), lx_madd3(v, tn, -dist * 0.5f), dist);
    }
    LxVec3 tv[3] = {t->a, t->b, t->c};
    for (int i = 0; i < 3; i++) {
        LxVec3 l = box_local(A, tv[i]);
        float dx = A->half.x - fabsf(l.x), dy = A->half.y - fabsf(l.y), dz = A->half.z - fabsf(l.z);
        if (dx < -MARGIN || dy < -MARGIN || dz < -MARGIN) continue;
        int k = dx < dy && dx < dz ? 0 : (dy < dz ? 1 : 2);
        float depth = k == 0 ? dx : k == 1 ? dy : dz, lk = k == 0 ? l.x : k == 1 ? l.y : l.z;
        mf_add(x->m, scl(A->ax[k], lk >= 0 ? 1.0f : -1.0f), tv[i], -depth);
    }
}

static void convex_mesh(LxPhysicsWorld *w, const Body *A, const Body *M, Manifold *m) {
    if (M->mesh < 0) return;
    TriCtx x = {A, m};
    LxAabb q = A->box;
    q.min = sub(q.min, lx_v3(MARGIN, MARGIN, MARGIN));
    q.max = add(q.max, lx_v3(MARGIN, MARGIN, MARGIN));
    mesh_query(&w->meshes[M->mesh], q, tri_cb, &x);
    mf_reduce(m, 8);
}

static int shape_rank(LxColliderShape s) {
    switch (s) {
    case LX_COLLIDER_SPHERE: return 0;
    case LX_COLLIDER_CAPSULE: return 1;
    case LX_COLLIDER_BOX: return 2;
    default: return 3;
    }
}

/* contacts between two bodies, normals from A to B */
static void collide(LxPhysicsWorld *w, const Body *A, const Body *B, Manifold *m) {
    m->count = 0;
    bool flip = shape_rank(A->shape) > shape_rank(B->shape);
    if (flip) { const Body *t = A; A = B; B = t; }
    LxColliderShape sa = A->shape, sb = B->shape;
    if (sa == LX_COLLIDER_SPHERE && sb == LX_COLLIDER_SPHERE) sphere_sphere(A->c, A->radius, B->c, B->radius, m);
    else if (sa == LX_COLLIDER_SPHERE && sb == LX_COLLIDER_CAPSULE) {
        capsule_sphere(B, A->c, A->radius, m);
        for (int i = 0; i < m->count; i++) m->n[i] = scl(m->n[i], -1.0f);
    } else if (sa == LX_COLLIDER_SPHERE && sb == LX_COLLIDER_BOX) sphere_box(A->c, A->radius, B, m, false);
    else if (sa == LX_COLLIDER_CAPSULE && sb == LX_COLLIDER_CAPSULE) capsule_capsule(A, B, m);
    else if (sa == LX_COLLIDER_CAPSULE && sb == LX_COLLIDER_BOX) capsule_box(A, B, m);
    else if (sa == LX_COLLIDER_BOX && sb == LX_COLLIDER_BOX) box_box(A, B, m);
    else if (sb == LX_COLLIDER_MESH && sa != LX_COLLIDER_MESH) convex_mesh(w, A, B, m);
    if (flip)
        for (int i = 0; i < m->count; i++) m->n[i] = scl(m->n[i], -1.0f);
}

/* ======================================================================
 *  gathering bodies from the scene
 * ====================================================================== */
static bool hidden_by_parent(LxScene *s, const LxEntityData *d) {
    for (LxEntity p = d->parent; p; p = lx_entity_data(s, p)->parent)
        if (!lx_entity_data(s, p)->active) return true;
    return false;
}

static LxEntity handle_at(const LxScene *s, int i) {
    return lx_entity_make((uint32_t)i, s->entities[i].generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
}

static void body_aabb(LxPhysicsWorld *w, Body *b) {
    LxVec3 e;
    switch (b->shape) {
    case LX_COLLIDER_SPHERE: e = lx_v3(b->radius, b->radius, b->radius); break;
    case LX_COLLIDER_CAPSULE: {
        LxVec3 a = scl(b->ax[1], b->hseg);
        e = lx_v3(fabsf(a.x) + b->radius, fabsf(a.y) + b->radius, fabsf(a.z) + b->radius);
        break;
    }
    case LX_COLLIDER_BOX:
        e = lx_v3(0, 0, 0);
        for (int k = 0; k < 3; k++) {
            float h = k == 0 ? b->half.x : k == 1 ? b->half.y : b->half.z;
            e = add(e, lx_v3(fabsf(b->ax[k].x) * h, fabsf(b->ax[k].y) * h, fabsf(b->ax[k].z) * h));
        }
        break;
    default:
        if (b->mesh >= 0 && w->meshes[b->mesh].nnodes) { b->box = w->meshes[b->mesh].nodes[0].box; return; }
        e = lx_v3(0, 0, 0);
    }
    b->box.min = sub(b->c, e);
    b->box.max = add(b->c, e);
}

static void body_inertia_world(Body *b) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float v = 0;
            for (int k = 0; k < 3; k++) {
                float ik = k == 0 ? b->inv_i.x : k == 1 ? b->inv_i.y : b->inv_i.z;
                float rk = r == 0 ? b->ax[k].x : r == 1 ? b->ax[k].y : b->ax[k].z;
                float ck = c == 0 ? b->ax[k].x : c == 1 ? b->ax[k].y : b->ax[k].z;
                v += rk * ik * ck;
            }
            b->iw[r * 3 + c] = v;
        }
}

static void set_axes(Body *b) {
    b->ax[0] = lx_quat_rotate(b->q, lx_v3(1, 0, 0));
    b->ax[1] = lx_quat_rotate(b->q, lx_v3(0, 1, 0));
    b->ax[2] = lx_quat_rotate(b->q, lx_v3(0, 0, 1));
}

/* Builds the body list. With side_effects, also estimates kinematic velocities and wakes bodies a
 * script moved (only the simulation step does that; queries and character moves just look). */
static void gather(LxScene *s, LxPhysicsWorld *w, float dt, bool side_effects) {
    w->nb = 0;
    for (int i = 0; i < w->nm; i++) w->meshes[i].used = false;
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & (LX_COMP_COLLIDER | LX_COMP_CHARACTER)) || hidden_by_parent(s, d)) continue;
        Body b;
        memset(&b, 0, sizeof b);
        b.e = handle_at(s, i);
        b.index = i;
        b.mesh = -1;
        LxVec3 c0 = lx_v3(d->world.m[0], d->world.m[1], d->world.m[2]);
        LxVec3 c1 = lx_v3(d->world.m[4], d->world.m[5], d->world.m[6]);
        LxVec3 c2 = lx_v3(d->world.m[8], d->world.m[9], d->world.m[10]);
        LxVec3 sc = lx_v3(lx_len3(c0), lx_len3(c1), lx_len3(c2));
        LxVec3 pos = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
        if (sc.x < 1e-6f || sc.y < 1e-6f || sc.z < 1e-6f) continue;
        LxQuat q = lx_quat_from_basis(scl(c0, 1.0f / sc.x), scl(c1, 1.0f / sc.y), scl(c2, 1.0f / sc.z));
        if (d->comps & LX_COMP_CHARACTER) {
            const LxCharacterController *ch = &d->character;
            float r = ch->radius * lx_maxf(sc.x, sc.z), h = lx_maxf(ch->height * sc.y, 2 * r);
            b.shape = LX_COLLIDER_CAPSULE;
            b.radius = r;
            b.hseg = h * 0.5f - r;
            b.q = lx_quat_identity();          /* characters stay upright */
            b.offset = lx_v3(0, h * 0.5f, 0);
            b.c = add(pos, b.offset);
            b.kind = BODY_KINEMATIC;
            b.character = true;
            b.friction = 0;
            b.layer = d->comps & LX_COMP_COLLIDER ? d->collider.layer : 0;
        } else {
            const LxCollider *col = &d->collider;
            b.shape = col->shape;
            b.q = q;
            b.trigger = col->is_trigger;
            b.friction = lx_clampf(col->friction, 0, 2);
            b.restitution = lx_clampf(col->bounciness, 0, 1);
            b.layer = col->layer & 31;
            b.offset = lx_v3(col->center.x * sc.x, col->center.y * sc.y, col->center.z * sc.z);
            bool dynamic = (d->comps & LX_COMP_RIGIDBODY) && !d->body.is_kinematic;
            if (b.shape == LX_COLLIDER_MESH && dynamic) {
                /* moving triangle meshes aren't supported: simulate their bounding box */
                const LxMesh *mesh = (d->comps & LX_COMP_MESH) ? lx_asset_mesh(d->mesh.mesh) : NULL;
                LxAabb mb = mesh ? mesh->bounds : (LxAabb){{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
                b.shape = LX_COLLIDER_BOX;
                b.half = lx_v3((mb.max.x - mb.min.x) * 0.5f * sc.x, (mb.max.y - mb.min.y) * 0.5f * sc.y, (mb.max.z - mb.min.z) * 0.5f * sc.z);
                LxVec3 mc = lx_aabb_center(mb);
                b.offset = lx_v3(mc.x * sc.x, mc.y * sc.y, mc.z * sc.z);
            }
            switch (b.shape) {
            case LX_COLLIDER_BOX:
                if (col->shape == LX_COLLIDER_BOX)
                    b.half = lx_v3(fabsf(col->size.x * sc.x) * 0.5f, fabsf(col->size.y * sc.y) * 0.5f, fabsf(col->size.z * sc.z) * 0.5f);
                break;
            case LX_COLLIDER_SPHERE: b.radius = col->radius * lx_maxf(sc.x, lx_maxf(sc.y, sc.z)); break;
            case LX_COLLIDER_CAPSULE:
                b.radius = col->radius * lx_maxf(sc.x, sc.z);
                b.hseg = lx_maxf(col->height * sc.y * 0.5f - b.radius, 0);
                break;
            case LX_COLLIDER_MESH:
                if (!(d->comps & LX_COMP_MESH)) continue;
                b.mesh = mesh_cache_get(w, b.e, d);
                break;
            }
            b.c = add(pos, lx_quat_rotate(q, b.offset));
            if (d->comps & LX_COMP_RIGIDBODY) b.kind = d->body.is_kinematic ? BODY_KINEMATIC : BODY_DYNAMIC;
        }
        set_axes(&b);
        LxPhysState *ps = &d->phys;
        bool moved = ps->has_last && (lx_len3sq(sub(pos, ps->last_pos)) > 1e-10f ||
                                      fabsf(ps->last_rot.x - q.x) + fabsf(ps->last_rot.y - q.y) + fabsf(ps->last_rot.z - q.z) +
                                              fabsf(ps->last_rot.w - q.w) > 1e-5f);
        if (b.kind == BODY_DYNAMIC) {
            const LxRigidbody *rb = &d->body;
            float m = lx_maxf(rb->mass, 1e-3f);
            b.inv_mass = 1.0f / m;
            LxVec3 I;
            if (b.shape == LX_COLLIDER_SPHERE) { float v = 0.4f * m * b.radius * b.radius; I = lx_v3(v, v, v); }
            else if (b.shape == LX_COLLIDER_CAPSULE) {
                float r = b.radius, L = 2 * b.hseg + 2 * r;
                float side = m * (3 * r * r + L * L) / 12.0f;
                I = lx_v3(side, 0.5f * m * r * r, side);
            } else {
                LxVec3 f = scl(b.half, 2.0f);
                I = lx_v3(m * (f.y * f.y + f.z * f.z) / 12, m * (f.x * f.x + f.z * f.z) / 12, m * (f.x * f.x + f.y * f.y) / 12);
            }
            b.inv_i = rb->freeze_rotation ? lx_v3(0, 0, 0)
                                          : lx_v3(I.x > 1e-9f ? 1 / I.x : 0, I.y > 1e-9f ? 1 / I.y : 0, I.z > 1e-9f ? 1 / I.z : 0);
            b.v = rb->velocity;
            b.w = rb->freeze_rotation ? lx_v3(0, 0, 0) : rb->angular_velocity;
            b.asleep = ps->asleep;
            if (side_effects && (moved || lx_len3sq(ps->force) > 0 || lx_len3sq(ps->torque) > 0 ||
                                 lx_len3sq(rb->velocity) > SLEEP_LINEAR * SLEEP_LINEAR)) {
                b.asleep = ps->asleep = false;
                ps->sleep_timer = 0;
            }
            body_inertia_world(&b);
        } else if (b.kind == BODY_KINEMATIC && side_effects && ps->has_last && dt > 0) {
            b.v = scl(sub(pos, ps->last_pos), 1.0f / dt);   /* moved by code: pushes what it touches */
        }
        if (side_effects && b.kind != BODY_DYNAMIC) { ps->last_pos = pos; ps->last_rot = q; ps->has_last = true; }
        body_aabb(w, &b);
        LX_VEC_PUSH(w->bodies, w->nb, w->cb, b);
    }
    /* drop mesh caches of entities that are gone */
    for (int i = 0; i < w->nm;) {
        if (!w->meshes[i].used) {
            lx_free(w->meshes[i].tris); lx_free(w->meshes[i].nodes); lx_free(w->meshes[i].order);
            w->meshes[i] = w->meshes[--w->nm];
            for (int k = 0; k < w->nb; k++) if (w->bodies[k].mesh == w->nm) w->bodies[k].mesh = i;
        } else i++;
    }
}

/* ======================================================================
 *  solver
 * ====================================================================== */
static bool layers_collide(int a, int b) {
    const LxPhysicsSettings *ps = lx_physics_settings();
    return (ps->layer_mask[a & 31] >> (b & 31)) & 1u;
}

static int cmp_minx(const void *a, const void *b) {
    float x = ((const Body *)a)->box.min.x, y = ((const Body *)b)->box.min.x;
    return x < y ? -1 : x > y;
}

static void pair_record(LxPhysicsWorld *w, const Body *A, const Body *B, bool trigger, const Manifold *m) {
    bool swap = A->e > B->e;
    Pair p;
    memset(&p, 0, sizeof p);
    p.key = swap ? ((uint64_t)B->e << 32) | A->e : ((uint64_t)A->e << 32) | B->e;
    p.trigger = trigger;
    if (m && m->count) {
        int k = 0;
        for (int i = 1; i < m->count; i++) if (m->sep[i] < m->sep[k]) k = i;
        p.point = m->p[k];
        p.normal = swap ? scl(m->n[k], -1.0f) : m->n[k];
        LxVec3 ra = sub(p.point, A->c), rb = sub(p.point, B->c);
        LxVec3 rv = sub(add(B->v, cross(B->w, rb)), add(A->v, cross(A->w, ra)));
        p.rel_vel = swap ? scl(rv, -1.0f) : rv;
    }
    LX_VEC_PUSH(w->pairs, w->np, w->cp, p);
}

static void wake(Body *b, LxScene *s) {
    if (b->kind != BODY_DYNAMIC || !b->asleep) return;
    b->asleep = false;
    s->entities[b->index].phys.asleep = false;
    s->entities[b->index].phys.sleep_timer = 0;
}

static void apply_impulse(Body *A, Body *B, LxVec3 ra, LxVec3 rb, LxVec3 P) {
    if (A->kind == BODY_DYNAMIC && !A->asleep) {
        A->v = lx_madd3(A->v, P, -A->inv_mass);
        A->w = sub(A->w, mul33(A->iw, cross(ra, P)));
    }
    if (B->kind == BODY_DYNAMIC && !B->asleep) {
        B->v = lx_madd3(B->v, P, B->inv_mass);
        B->w = add(B->w, mul33(B->iw, cross(rb, P)));
    }
}

static float eff_mass(const Body *A, const Body *B, LxVec3 ra, LxVec3 rb, LxVec3 n) {
    float k = 0;
    if (A->kind == BODY_DYNAMIC && !A->asleep) k += A->inv_mass + dot(cross(mul33(A->iw, cross(ra, n)), ra), n);
    if (B->kind == BODY_DYNAMIC && !B->asleep) k += B->inv_mass + dot(cross(mul33(B->iw, cross(rb, n)), rb), n);
    return k > 1e-9f ? 1.0f / k : 0;
}

static int cmp_pair(const void *a, const void *b) {
    uint64_t x = ((const Pair *)a)->key, y = ((const Pair *)b)->key;
    return x < y ? -1 : x > y;
}

static void dispatch_events(LxScene *s, LxPhysicsWorld *w) {
    if (w->np > 1) qsort(w->pairs, (size_t)w->np, sizeof(Pair), cmp_pair);
    /* enter: in pairs, not in prev; exit: in prev, not in pairs */
    int i = 0, j = 0;
    Pair *enter = NULL, *exitp = NULL;
    int ne = 0, ce = 0, nx = 0, cx = 0;
    while (i < w->np || j < w->nprev) {
        if (j >= w->nprev || (i < w->np && w->pairs[i].key < w->prev[j].key)) { LX_VEC_PUSH(enter, ne, ce, w->pairs[i]); i++; }
        else if (i >= w->np || w->prev[j].key < w->pairs[i].key) { LX_VEC_PUSH(exitp, nx, cx, w->prev[j]); j++; }
        else { i++; j++; }
    }
    /* keep this step's pairs for the next diff */
    Pair *t = w->prev; w->prev = w->pairs; w->pairs = t;
    int tc = w->cprev; w->cprev = w->cp; w->cp = tc;
    w->nprev = w->np;
    w->np = 0;
    for (int k = 0; k < ne; k++) {
        LxEntity a = (LxEntity)(enter[k].key >> 32), b = (LxEntity)(enter[k].key & 0xffffffffu);
        if (enter[k].trigger) {
            lx_behaviour_trigger(s, a, b, true);
            lx_behaviour_trigger(s, b, a, true);
            continue;
        }
        LxCollision ca = {b, enter[k].point, scl(enter[k].normal, -1.0f), enter[k].impulse, enter[k].rel_vel};
        LxCollision cb = {a, enter[k].point, enter[k].normal, enter[k].impulse, scl(enter[k].rel_vel, -1.0f)};
        lx_behaviour_collision_enter(s, a, &ca);
        lx_behaviour_collision_enter(s, b, &cb);
    }
    for (int k = 0; k < nx; k++) {
        LxEntity a = (LxEntity)(exitp[k].key >> 32), b = (LxEntity)(exitp[k].key & 0xffffffffu);
        if (exitp[k].trigger) {
            lx_behaviour_trigger(s, a, b, false);
            lx_behaviour_trigger(s, b, a, false);
        } else {
            lx_behaviour_collision_exit(s, a, b);
            lx_behaviour_collision_exit(s, b, a);
        }
    }
    lx_free(enter);
    lx_free(exitp);
}

static void step(LxScene *s, LxPhysicsWorld *w, float dt) {
    lx_behaviour_fixed_update(s, dt);
    lx_scene_update_transforms(s);
    gather(s, w, dt, true);
    const LxPhysicsSettings *ps = lx_physics_settings();
    /* forces and gravity */
    for (int i = 0; i < w->nb; i++) {
        Body *b = &w->bodies[i];
        if (b->kind != BODY_DYNAMIC || b->asleep) continue;
        LxEntityData *d = &s->entities[b->index];
        LxVec3 acc = d->body.use_gravity ? ps->gravity : lx_v3(0, 0, 0);
        acc = lx_madd3(acc, d->phys.force, b->inv_mass);
        b->v = lx_madd3(b->v, acc, dt);
        b->w = add(b->w, scl(mul33(b->iw, d->phys.torque), dt));
        b->v = scl(b->v, 1.0f / (1.0f + dt * lx_maxf(d->body.linear_damping, 0)));
        b->w = scl(b->w, 1.0f / (1.0f + dt * lx_maxf(d->body.angular_damping, 0)));
        d->phys.force = d->phys.torque = lx_v3(0, 0, 0);
    }
    /* broadphase: sort and sweep on X */
    if (w->nb > 1) qsort(w->bodies, (size_t)w->nb, sizeof(Body), cmp_minx);
    w->nc = 0;
    w->np = 0;
    Manifold m;
    for (int i = 0; i < w->nb; i++) {
        Body *A = &w->bodies[i];
        for (int j = i + 1; j < w->nb; j++) {
            Body *B = &w->bodies[j];
            if (B->box.min.x > A->box.max.x + MARGIN) break;
            if (A->box.min.y > B->box.max.y + MARGIN || B->box.min.y > A->box.max.y + MARGIN ||
                A->box.min.z > B->box.max.z + MARGIN || B->box.min.z > A->box.max.z + MARGIN) continue;
            bool trig = A->trigger || B->trigger;
            bool a_moves = A->kind != BODY_STATIC, b_moves = B->kind != BODY_STATIC;
            if (!a_moves && !b_moves) continue;
            if (trig && A->trigger && B->trigger) continue;
            if (!trig && A->kind != BODY_DYNAMIC && B->kind != BODY_DYNAMIC) continue;   /* nothing to solve */
            if ((A->asleep || A->kind != BODY_DYNAMIC) && (B->asleep || B->kind != BODY_DYNAMIC) && !trig &&
                lx_len3sq(A->v) < 1e-8f && lx_len3sq(B->v) < 1e-8f) {
                /* sleeping body against something still: keep the touch for events, skip solving */
                if (A->asleep || B->asleep) {
                    for (int k = 0; k < w->nprev; k++) {
                        uint64_t key = A->e < B->e ? ((uint64_t)A->e << 32) | B->e : ((uint64_t)B->e << 32) | A->e;
                        if (w->prev[k].key == key) { LX_VEC_PUSH(w->pairs, w->np, w->cp, w->prev[k]); break; }
                    }
                }
                continue;
            }
            if (!layers_collide(A->layer, B->layer)) continue;
            collide(w, A, B, &m);
            if (!m.count) continue;
            if (trig) {
                bool overlap = false;
                for (int k = 0; k < m.count; k++) overlap |= m.sep[k] < 0;
                if (overlap) pair_record(w, A, B, true, NULL);
                continue;
            }
            /* something moving touches a sleeper: wake it */
            if (A->asleep && (B->kind == BODY_DYNAMIC ? !B->asleep : lx_len3sq(B->v) > 1e-6f)) wake(A, s);
            if (B->asleep && (A->kind == BODY_DYNAMIC ? !A->asleep : lx_len3sq(A->v) > 1e-6f)) wake(B, s);
            int first = w->nc;
            for (int k = 0; k < m.count; k++) {
                Contact c;
                memset(&c, 0, sizeof c);
                c.a = i;
                c.b = j;
                c.n = m.n[k];
                c.p = m.p[k];
                c.sep = m.sep[k];
                c.friction = sqrtf(lx_maxf(A->friction, 0) * lx_maxf(B->friction, 0));
                c.restitution = lx_maxf(A->restitution, B->restitution);
                LX_VEC_PUSH(w->contacts, w->nc, w->cc, c);
            }
            float min_sep = 1e30f;
            for (int k = 0; k < m.count; k++) min_sep = lx_minf(min_sep, m.sep[k]);
            if (min_sep < 0.01f) pair_record(w, A, B, false, &m);
            LX_UNUSED(first);
        }
    }
    /* prepare contacts */
    for (int k = 0; k < w->nc; k++) {
        Contact *c = &w->contacts[k];
        Body *A = &w->bodies[c->a], *B = &w->bodies[c->b];
        c->ra = sub(c->p, A->c);
        c->rb = sub(c->p, B->c);
        tangents(c->n, &c->t1, &c->t2);
        c->mn = eff_mass(A, B, c->ra, c->rb, c->n);
        c->mt1 = eff_mass(A, B, c->ra, c->rb, c->t1);
        c->mt2 = eff_mass(A, B, c->ra, c->rb, c->t2);
        LxVec3 vr = sub(add(B->v, cross(B->w, c->rb)), add(A->v, cross(A->w, c->ra)));
        float vn = dot(vr, c->n);
        float target = c->sep > 0 ? -c->sep / dt : lx_minf(BAUMGARTE * lx_maxf(-c->sep - SLOP, 0) / dt, 3.0f);
        if (vn < -1.0f && c->restitution > 0) target = lx_maxf(target, -c->restitution * vn);
        c->target = target;
    }
    /* sequential impulses */
    int iters = ps->solver_iterations > 0 ? ps->solver_iterations : 10;
    for (int it = 0; it < iters; it++) {
        for (int k = 0; k < w->nc; k++) {
            Contact *c = &w->contacts[k];
            Body *A = &w->bodies[c->a], *B = &w->bodies[c->b];
            LxVec3 vr = sub(add(B->v, cross(B->w, c->rb)), add(A->v, cross(A->w, c->ra)));
            float lambda = c->mn * (c->target - dot(vr, c->n));
            float old = c->jn;
            c->jn = lx_maxf(old + lambda, 0);
            apply_impulse(A, B, c->ra, c->rb, scl(c->n, c->jn - old));
            float maxf = c->friction * c->jn;
            vr = sub(add(B->v, cross(B->w, c->rb)), add(A->v, cross(A->w, c->ra)));
            old = c->jt1;
            c->jt1 = lx_clampf(old - c->mt1 * dot(vr, c->t1), -maxf, maxf);
            apply_impulse(A, B, c->ra, c->rb, scl(c->t1, c->jt1 - old));
            vr = sub(add(B->v, cross(B->w, c->rb)), add(A->v, cross(A->w, c->ra)));
            old = c->jt2;
            c->jt2 = lx_clampf(old - c->mt2 * dot(vr, c->t2), -maxf, maxf);
            apply_impulse(A, B, c->ra, c->rb, scl(c->t2, c->jt2 - old));
        }
    }
    /* total impulse per pair, for the collision callback */
    for (int k = 0; k < w->nc; k++) {
        const Contact *c = &w->contacts[k];
        const Body *A = &w->bodies[c->a], *B = &w->bodies[c->b];
        uint64_t key = A->e < B->e ? ((uint64_t)A->e << 32) | B->e : ((uint64_t)B->e << 32) | A->e;
        for (int p = 0; p < w->np; p++)
            if (w->pairs[p].key == key) { w->pairs[p].impulse += c->jn; break; }
    }
    /* integrate, sleep, write back */
    for (int i = 0; i < w->nb; i++) {
        Body *b = &w->bodies[i];
        if (b->kind != BODY_DYNAMIC) continue;
        LxEntityData *d = &s->entities[b->index];
        if (!b->asleep) {
            b->c = lx_madd3(b->c, b->v, dt);
            LxQuat wq = {b->w.x, b->w.y, b->w.z, 0};
            LxQuat dq = lx_quat_mul(wq, b->q);
            b->q = lx_quat_norm((LxQuat){b->q.x + dq.x * 0.5f * dt, b->q.y + dq.y * 0.5f * dt, b->q.z + dq.z * 0.5f * dt,
                                         b->q.w + dq.w * 0.5f * dt});
            if (lx_len3sq(b->v) < SLEEP_LINEAR * SLEEP_LINEAR && lx_len3sq(b->w) < SLEEP_ANGULAR * SLEEP_ANGULAR) {
                d->phys.sleep_timer += dt;
                if (d->phys.sleep_timer > SLEEP_TIME) { d->phys.asleep = true; b->v = b->w = lx_v3(0, 0, 0); }
            } else d->phys.sleep_timer = 0;
        }
        d->body.velocity = b->v;
        d->body.angular_velocity = b->w;
        LxVec3 pos = sub(b->c, lx_quat_rotate(b->q, b->offset));
        lx_entity_set_world_pose(s, b->e, pos, b->q);
        d = &s->entities[b->index];
        d->phys.last_pos = pos;
        d->phys.last_rot = b->q;
        d->phys.has_last = true;
    }
    lx_scene_update_transforms(s);
    dispatch_events(s, w);
}

void lx_physics_simulate(LxScene *s, float dt) {
    LxPhysicsWorld *w = world_of(s);
    w->frame++;
    float h = lx_physics_settings()->fixed_timestep;
    if (h < 1.0f / 1000.0f) h = 1.0f / 60.0f;
    w->accumulator += dt;
    int steps = 0;
    while (w->accumulator >= h && steps < 6) {
        step(s, w, h);
        w->accumulator -= h;
        steps++;
    }
    if (steps == 6) w->accumulator = 0;   /* running too slowly: drop time instead of spiralling */
}

/* ======================================================================
 *  queries
 * ====================================================================== */
static LxPhysicsWorld *query_world(LxScene *s) {
    LxPhysicsWorld *w = world_of(s);
    lx_scene_update_transforms(s);
    gather(s, w, 0, false);
    return w;
}

static bool ray_sphere(LxVec3 o, LxVec3 d, LxVec3 c, float r, float *t) {
    LxVec3 m = sub(o, c);
    float b = dot(m, d), cc = dot(m, m) - r * r;
    if (cc > 0 && b > 0) return false;
    float disc = b * b - cc;
    if (disc < 0) return false;
    *t = lx_maxf(-b - sqrtf(disc), 0);
    return true;
}

static bool ray_capsule(LxVec3 o, LxVec3 d, const Body *b, float *t_out, LxVec3 *n_out) {
    LxVec3 p0, p1;
    capsule_points(b, &p0, &p1);
    float best = 1e30f;
    LxVec3 bn = lx_v3(0, 1, 0);
    float t;
    if (ray_sphere(o, d, p0, b->radius, &t) && t < best) { best = t; bn = lx_norm3(sub(lx_madd3(o, d, t), p0)); }
    if (ray_sphere(o, d, p1, b->radius, &t) && t < best) { best = t; bn = lx_norm3(sub(lx_madd3(o, d, t), p1)); }
    /* infinite cylinder around the segment, clipped to it */
    LxVec3 ax = b->ax[1];
    LxVec3 m = sub(o, p0);
    LxVec3 dp = sub(d, scl(ax, dot(d, ax))), mp = sub(m, scl(ax, dot(m, ax)));
    float A = dot(dp, dp), B = 2 * dot(dp, mp), C = dot(mp, mp) - b->radius * b->radius;
    float disc = B * B - 4 * A * C;
    if (A > 1e-9f && disc >= 0) {
        float tc = (-B - sqrtf(disc)) / (2 * A);
        if (tc < 0 && C <= 0) tc = 0;
        if (tc >= 0) {
            LxVec3 hit = lx_madd3(o, d, tc);
            float h = dot(sub(hit, p0), ax);
            if (h >= 0 && h <= 2 * b->hseg && tc < best) {
                best = tc;
                bn = lx_norm3(sub(hit, lx_madd3(p0, ax, h)));
            }
        }
    }
    if (best == 1e30f) return false;
    *t_out = best;
    *n_out = bn;
    return true;
}

static bool ray_box(LxVec3 o, LxVec3 d, const Body *b, float *t_out, LxVec3 *n_out) {
    LxVec3 lo = box_local(b, o);
    LxVec3 ld = lx_v3(dot(d, b->ax[0]), dot(d, b->ax[1]), dot(d, b->ax[2]));
    float tmin = 0, tmax = 1e30f;
    int axis = -1;
    float sign = 1;
    float oo[3] = {lo.x, lo.y, lo.z}, dd[3] = {ld.x, ld.y, ld.z}, hh[3] = {b->half.x, b->half.y, b->half.z};
    for (int k = 0; k < 3; k++) {
        if (fabsf(dd[k]) < 1e-9f) {
            if (oo[k] < -hh[k] || oo[k] > hh[k]) return false;
            continue;
        }
        float t1 = (-hh[k] - oo[k]) / dd[k], t2 = (hh[k] - oo[k]) / dd[k];
        float s = -1;
        if (t1 > t2) { float tt = t1; t1 = t2; t2 = tt; s = 1; }
        if (t1 > tmin) { tmin = t1; axis = k; sign = s; }
        tmax = lx_minf(tmax, t2);
        if (tmin > tmax) return false;
    }
    *t_out = tmin;
    *n_out = axis >= 0 ? scl(b->ax[axis], sign) : scl(d, -1.0f);
    return true;
}

typedef struct RayMesh { LxVec3 o, d; float best; LxVec3 n; } RayMesh;
static bool ray_tri(LxVec3 o, LxVec3 d, const Tri *t, float *out) {
    LxVec3 e1 = sub(t->b, t->a), e2 = sub(t->c, t->a), p = cross(d, e2);
    float det = dot(e1, p);
    if (fabsf(det) < 1e-12f) return false;
    float inv = 1.0f / det;
    LxVec3 s = sub(o, t->a);
    float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    LxVec3 q = cross(s, e1);
    float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    float tt = dot(e2, q) * inv;
    if (tt < 0) return false;
    *out = tt;
    return true;
}
static bool ray_mesh(const MeshCache *m, LxVec3 o, LxVec3 d, float max_t, float *t_out, LxVec3 *n_out) {
    if (!m->nnodes) return false;
    LxVec3 inv = lx_v3(1.0f / (fabsf(d.x) > 1e-12f ? d.x : 1e-12f), 1.0f / (fabsf(d.y) > 1e-12f ? d.y : 1e-12f),
                       1.0f / (fabsf(d.z) > 1e-12f ? d.z : 1e-12f));
    float best = max_t;
    LxVec3 bn = lx_v3(0, 1, 0);
    bool hit = false;
    int stack[128], sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const MNode *n = &m->nodes[stack[--sp]];
        float t1 = (n->box.min.x - o.x) * inv.x, t2 = (n->box.max.x - o.x) * inv.x;
        float tmin = lx_minf(t1, t2), tmax = lx_maxf(t1, t2);
        t1 = (n->box.min.y - o.y) * inv.y; t2 = (n->box.max.y - o.y) * inv.y;
        tmin = lx_maxf(tmin, lx_minf(t1, t2)); tmax = lx_minf(tmax, lx_maxf(t1, t2));
        t1 = (n->box.min.z - o.z) * inv.z; t2 = (n->box.max.z - o.z) * inv.z;
        tmin = lx_maxf(tmin, lx_minf(t1, t2)); tmax = lx_minf(tmax, lx_maxf(t1, t2));
        if (tmax < lx_maxf(tmin, 0) || tmin > best) continue;
        if (n->count) {
            for (int i = n->first; i < n->first + n->count; i++) {
                const Tri *t = &m->tris[m->order[i]];
                float tt;
                if (ray_tri(o, d, t, &tt) && tt < best) {
                    best = tt;
                    bn = dot(t->n, d) > 0 ? scl(t->n, -1.0f) : t->n;
                    hit = true;
                }
            }
        } else if (sp < 126) {
            stack[sp++] = n->left;
            stack[sp++] = n->right;
        }
    }
    if (hit) { *t_out = best; *n_out = bn; }
    return hit;
}

bool lx_physics_raycast(LxScene *s, LxVec3 origin, LxVec3 dir, float max_dist, uint32_t mask, LxRaycastHit *hit) {
    if (!s) return false;
    LxPhysicsWorld *w = query_world(s);
    float len = lx_len3(dir);
    if (len < 1e-9f) return false;
    LxVec3 d = scl(dir, 1.0f / len);
    float best = max_dist;
    bool found = false;
    LxRaycastHit h;
    memset(&h, 0, sizeof h);
    for (int i = 0; i < w->nb; i++) {
        const Body *b = &w->bodies[i];
        if (b->trigger || !((mask >> (b->layer & 31)) & 1u)) continue;
        float t = 0;
        LxVec3 n = lx_v3(0, 1, 0);
        bool ok = false;
        switch (b->shape) {
        case LX_COLLIDER_SPHERE:
            ok = ray_sphere(origin, d, b->c, b->radius, &t);
            if (ok) n = lx_norm3(sub(lx_madd3(origin, d, t), b->c));
            break;
        case LX_COLLIDER_CAPSULE: ok = ray_capsule(origin, d, b, &t, &n); break;
        case LX_COLLIDER_BOX: ok = ray_box(origin, d, b, &t, &n); break;
        case LX_COLLIDER_MESH: ok = b->mesh >= 0 && ray_mesh(&w->meshes[b->mesh], origin, d, best, &t, &n); break;
        }
        if (ok && t <= best) {
            best = t;
            found = true;
            h.entity = b->e;
            h.distance = t;
            h.point = lx_madd3(origin, d, t);
            h.normal = n;
        }
    }
    if (found && hit) *hit = h;
    return found;
}

/* a temporary probe body for overlap tests */
static Body probe_sphere(LxVec3 c, float r) {
    Body b;
    memset(&b, 0, sizeof b);
    b.shape = LX_COLLIDER_SPHERE;
    b.c = c;
    b.radius = r;
    b.q = lx_quat_identity();
    b.mesh = -1;
    set_axes(&b);
    b.box.min = sub(c, lx_v3(r, r, r));
    b.box.max = add(c, lx_v3(r, r, r));
    return b;
}

static bool aabb_overlap(LxAabb a, LxAabb b) {
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y && a.min.z <= b.max.z &&
           b.min.z <= a.max.z;
}

static int overlap_body(LxPhysicsWorld *w, const Body *probe, uint32_t mask, LxEntity *out, int max_out, LxEntity skip) {
    int n = 0;
    Manifold m;
    for (int i = 0; i < w->nb; i++) {
        const Body *b = &w->bodies[i];
        if (b->trigger || b->e == skip || !((mask >> (b->layer & 31)) & 1u) || !aabb_overlap(probe->box, b->box)) continue;
        collide(w, probe, b, &m);
        bool hit = false;
        for (int k = 0; k < m.count; k++) hit |= m.sep[k] < 0;
        if (hit) {
            if (n < max_out && out) out[n] = b->e;
            n++;
        }
    }
    return n;
}

int lx_physics_overlap_sphere(LxScene *s, LxVec3 center, float radius, uint32_t mask, LxEntity *out, int max_out) {
    if (!s) return 0;
    LxPhysicsWorld *w = query_world(s);
    Body p = probe_sphere(center, radius);
    return overlap_body(w, &p, mask, out, max_out, 0);
}

int lx_physics_overlap_box(LxScene *s, LxVec3 center, LxVec3 half, LxQuat rot, uint32_t mask, LxEntity *out, int max_out) {
    if (!s) return 0;
    LxPhysicsWorld *w = query_world(s);
    Body p;
    memset(&p, 0, sizeof p);
    p.shape = LX_COLLIDER_BOX;
    p.c = center;
    p.half = half;
    p.q = rot;
    p.mesh = -1;
    set_axes(&p);
    body_aabb(w, &p);
    return overlap_body(w, &p, mask, out, max_out, 0);
}

bool lx_physics_sphere_cast(LxScene *s, LxVec3 origin, float radius, LxVec3 dir, float max_dist, uint32_t mask, LxRaycastHit *hit) {
    if (!s) return false;
    LxPhysicsWorld *w = query_world(s);
    float len = lx_len3(dir);
    if (len < 1e-9f) return false;
    LxVec3 d = scl(dir, 1.0f / len);
    /* march, then bisect to the first touching position */
    float step = lx_maxf(radius * 0.5f, 0.02f), prev = 0;
    for (float t = 0;; t += step) {
        if (t > max_dist) t = max_dist;
        Body p = probe_sphere(lx_madd3(origin, d, t), radius);
        LxEntity e;
        if (overlap_body(w, &p, mask, &e, 1, 0)) {
            float lo = prev, hi = t;
            for (int i = 0; i < 10; i++) {
                float mid = (lo + hi) * 0.5f;
                Body q = probe_sphere(lx_madd3(origin, d, mid), radius);
                if (overlap_body(w, &q, mask, NULL, 0, 0)) hi = mid; else lo = mid;
            }
            if (hit) {
                Body q = probe_sphere(lx_madd3(origin, d, hi), radius);
                Manifold m;
                memset(hit, 0, sizeof *hit);
                for (int i = 0; i < w->nb; i++) {
                    const Body *b = &w->bodies[i];
                    if (b->trigger || !((mask >> (b->layer & 31)) & 1u) || !aabb_overlap(q.box, b->box)) continue;
                    collide(w, &q, b, &m);
                    for (int k = 0; k < m.count; k++)
                        if (m.sep[k] < 0) {
                            hit->entity = b->e;
                            hit->point = m.p[k];
                            hit->normal = scl(m.n[k], -1.0f);
                        }
                }
                hit->distance = lo;
            }
            return true;
        }
        if (t >= max_dist) return false;
        prev = t;
    }
}

/* ======================================================================
 *  rigidbody API
 * ====================================================================== */
static LxEntityData *rb_data(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return (d && (d->comps & LX_COMP_RIGIDBODY)) ? d : NULL;
}
static void rb_wake(LxEntityData *d) { d->phys.asleep = false; d->phys.sleep_timer = 0; }

void lx_rigidbody_add_force(LxScene *s, LxEntity e, LxVec3 f) {
    LxEntityData *d = rb_data(s, e);
    if (d) { d->phys.force = add(d->phys.force, f); rb_wake(d); }
}
void lx_rigidbody_add_torque(LxScene *s, LxEntity e, LxVec3 t) {
    LxEntityData *d = rb_data(s, e);
    if (d) { d->phys.torque = add(d->phys.torque, t); rb_wake(d); }
}
void lx_rigidbody_add_impulse(LxScene *s, LxEntity e, LxVec3 p) {
    LxEntityData *d = rb_data(s, e);
    if (!d || d->body.is_kinematic) return;
    d->body.velocity = lx_madd3(d->body.velocity, p, 1.0f / lx_maxf(d->body.mass, 1e-3f));
    rb_wake(d);
}
void lx_rigidbody_add_impulse_at(LxScene *s, LxEntity e, LxVec3 p, LxVec3 at) {
    LxEntityData *d = rb_data(s, e);
    if (!d || d->body.is_kinematic) return;
    lx_rigidbody_add_impulse(s, e, p);
    if (d->body.freeze_rotation) return;
    /* angular part, using a sphere-like inertia estimate from the collider size */
    lx_scene_update_transforms(s);
    LxVec3 c = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
    float r = 0.5f;
    if (d->comps & LX_COMP_COLLIDER) {
        const LxCollider *col = &d->collider;
        r = col->shape == LX_COLLIDER_BOX ? lx_len3(col->size) * 0.35f : lx_maxf(col->radius, 0.05f);
    }
    float inv_i = 1.0f / (0.4f * lx_maxf(d->body.mass, 1e-3f) * r * r);
    d->body.angular_velocity = lx_madd3(d->body.angular_velocity, cross(sub(at, c), p), inv_i);
}
void lx_rigidbody_set_velocity(LxScene *s, LxEntity e, LxVec3 v) {
    LxEntityData *d = rb_data(s, e);
    if (d) { d->body.velocity = v; rb_wake(d); }
}
LxVec3 lx_rigidbody_velocity(LxScene *s, LxEntity e) {
    LxEntityData *d = rb_data(s, e);
    return d ? d->body.velocity : lx_v3(0, 0, 0);
}
void lx_rigidbody_wake(LxScene *s, LxEntity e) {
    LxEntityData *d = rb_data(s, e);
    if (d) rb_wake(d);
}
bool lx_rigidbody_sleeping(LxScene *s, LxEntity e) {
    LxEntityData *d = rb_data(s, e);
    return d && d->phys.asleep;
}

void lx_physics_explosion(LxScene *s, LxVec3 center, float radius, float impulse) {
    if (!s || radius <= 0) return;
    lx_scene_update_transforms(s);
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_RIGIDBODY) || d->body.is_kinematic) continue;
        LxVec3 p = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
        LxVec3 off = sub(p, center);
        float dist = lx_len3(off);
        if (dist > radius) continue;
        LxVec3 dir = dist > 1e-4f ? scl(off, 1.0f / dist) : lx_v3(0, 1, 0);
        dir = lx_norm3(add(dir, lx_v3(0, 0.35f, 0)));   /* a little lift looks right */
        LxEntity e = handle_at(s, i);
        lx_rigidbody_add_impulse(s, e, scl(dir, impulse * (1.0f - dist / radius)));
    }
}

/* ======================================================================
 *  character controller
 * ====================================================================== */
typedef struct CharCtx {
    LxScene *s;
    LxPhysicsWorld *w;
    int self;
    float radius, hseg, half_h;
    float cos_slope, step_height, push;
} CharCtx;

static Body char_body(const CharCtx *k, LxVec3 feet) {
    Body b;
    memset(&b, 0, sizeof b);
    b.shape = LX_COLLIDER_CAPSULE;
    b.radius = k->radius;
    b.hseg = k->hseg;
    b.q = lx_quat_identity();
    b.mesh = -1;
    b.c = add(feet, lx_v3(0, k->half_h, 0));
    set_axes(&b);
    LxVec3 e = lx_v3(k->radius, k->hseg + k->radius, k->radius);
    b.box.min = sub(b.c, e);
    b.box.max = add(b.c, e);
    return b;
}

/* pushes the capsule out of everything it overlaps; returns flags, slides `motion` along what it hit */
static int char_resolve(CharCtx *k, LxVec3 *feet, LxVec3 *motion, bool allow_step, bool push_bodies) {
    int flags = 0;
    Manifold m;
    for (int iter = 0; iter < 4; iter++) {
        Body cap = char_body(k, *feet);
        bool any = false;
        for (int i = 0; i < k->w->nb; i++) {
            Body *b = &k->w->bodies[i];
            if (b->index == k->self || b->trigger || !aabb_overlap(cap.box, b->box)) continue;
            if (!layers_collide(0, b->layer)) continue;
            collide(k->w, &cap, b, &m);
            for (int c = 0; c < m.count; c++) {
                float sep = m.sep[c];
                LxVec3 n = scl(m.n[c], -1.0f);           /* from the obstacle to the character */
                if (sep > 0.002f) {
                    if (n.y >= k->cos_slope && sep < 0.03f) flags |= LX_CHAR_GROUNDED;   /* resting on it */
                    continue;
                }
                any = true;
                float rise = m.p[c].y - feet->y;
                bool walkable = n.y >= k->cos_slope;
                if (!walkable && allow_step && n.y > -0.3f && rise > 0.01f && rise <= k->step_height) {
                    feet->y += rise + 0.01f;                    /* climb the step */
                    flags |= LX_CHAR_GROUNDED;
                    break;
                }
                if (walkable) {
                    /* push straight up on walkable ground, so standing on a slope doesn't slide */
                    feet->y += -sep / lx_maxf(n.y, 0.2f);
                    flags |= LX_CHAR_GROUNDED;
                    if (motion->y < 0) motion->y = 0;
                } else {
                    *feet = lx_madd3(*feet, n, -sep);
                    flags |= n.y < -0.5f ? LX_CHAR_ABOVE : LX_CHAR_SIDES;
                    float into = dot(*motion, n);
                    if (into < 0) *motion = lx_madd3(*motion, n, -into);
                }
                if (push_bodies && b->kind == BODY_DYNAMIC && k->push > 0) {
                    LxEntity be = b->e;
                    lx_rigidbody_add_impulse(k->s, be, scl(n, -k->push * (-sep) * 60.0f * lx_minf(1.0f / lx_maxf(b->inv_mass, 1e-3f), 20.0f) * 0.05f));
                }
            }
        }
        if (!any) break;
    }
    return flags;
}

int lx_character_move(LxScene *s, LxEntity e, LxVec3 disp) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !(d->comps & LX_COMP_CHARACTER)) return 0;
    LxPhysicsWorld *w = query_world(s);
    d = lx_entity_data(s, e);
    LxCharacterController *ch = &d->character;
    LxVec3 sc = lx_v3(lx_len3(lx_v3(d->world.m[0], d->world.m[1], d->world.m[2])), lx_len3(lx_v3(d->world.m[4], d->world.m[5], d->world.m[6])),
                      lx_len3(lx_v3(d->world.m[8], d->world.m[9], d->world.m[10])));
    CharCtx k;
    k.s = s;
    k.w = w;
    k.self = (int)(d - s->entities);
    k.radius = ch->radius * lx_maxf(sc.x, sc.z);
    float h = lx_maxf(ch->height * sc.y, 2 * k.radius);
    k.half_h = h * 0.5f;
    k.hseg = k.half_h - k.radius;
    k.cos_slope = cosf(lx_clampf(ch->slope_limit_deg, 1, 89) * LX_DEG2RAD);
    k.step_height = lx_maxf(ch->step_height, 0);
    k.push = ch->push_power;
    LxVec3 start = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]), feet = start;
    bool was_grounded = ch->grounded;
    float len = lx_len3(disp);
    int steps = (int)ceilf(len / lx_maxf(k.radius * 0.5f, 0.01f));
    steps = steps < 1 ? 1 : (steps > 32 ? 32 : steps);
    LxVec3 motion = scl(disp, 1.0f / (float)steps);
    int flags = 0;
    for (int i = 0; i < steps; i++) {
        feet = add(feet, motion);
        bool horizontal = fabsf(motion.x) + fabsf(motion.z) > 1e-6f;
        flags |= char_resolve(&k, &feet, &motion, (was_grounded || (flags & LX_CHAR_GROUNDED)) && horizontal, true);
    }
    /* stick to the ground going down slopes and stairs */
    if (was_grounded && !(flags & LX_CHAR_GROUNDED) && disp.y <= 0) {
        LxVec3 probe = sub(feet, lx_v3(0, k.step_height + 0.05f, 0)), m0 = lx_v3(0, 0, 0);
        int f = char_resolve(&k, &probe, &m0, false, false);
        bool straight_down = fabsf(probe.x - feet.x) + fabsf(probe.z - feet.z) < 1e-3f;   /* never snap sideways off a ledge */
        if ((f & LX_CHAR_GROUNDED) && straight_down && probe.y < feet.y) { feet = probe; flags |= LX_CHAR_GROUNDED; }
    }
    d = lx_entity_data(s, e);
    LxQuat rot = lx_entity_world_rotation(s, e);
    lx_entity_set_world_pose(s, e, feet, rot);
    d = lx_entity_data(s, e);
    d->character.grounded = (flags & LX_CHAR_GROUNDED) != 0;
    float dt = s->last_dt > 1e-5f ? s->last_dt : 1.0f / 60.0f;
    d->character.velocity = scl(sub(feet, start), 1.0f / dt);
    lx_scene_update_transforms(s);
    return flags;
}

bool lx_character_grounded(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && (d->comps & LX_COMP_CHARACTER) && d->character.grounded;
}
