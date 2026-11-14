/* Model import: FBX files (binary or ASCII) into nodes, mesh parts, materials, skins and animation
 * clips. Everything is converted to Luxora's space on import - metres, +Y up - so a model's root
 * object sits at identity and its bones are ordinary objects in the hierarchy. */
#include "model_internal.h"
#include "fbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* ======================================================================
 *  small math (column-major 4x4, like LxMat4)
 * ====================================================================== */
static LxMat4 m_translate(LxVec3 t) { LxMat4 m = lx_mat4_identity(); m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z; return m; }
static LxMat4 m_scale(LxVec3 s) { LxMat4 m = lx_mat4_identity(); m.m[0] = s.x; m.m[5] = s.y; m.m[10] = s.z; return m; }
static LxMat4 m_rot_axis(int axis, float deg) {
    LxVec3 a = lx_v3(axis == 0, axis == 1, axis == 2);
    return lx_mat4_trs(lx_v3(0, 0, 0), lx_quat_axis_angle(a, deg * LX_DEG2RAD), lx_v3(1, 1, 1));
}
/* FBX Euler rotation: order 0 = XYZ means X applied first (R = Rz * Ry * Rx) */
static LxMat4 m_euler(LxVec3 e, int order) {
    LxMat4 x = m_rot_axis(0, e.x), y = m_rot_axis(1, e.y), z = m_rot_axis(2, e.z);
    switch (order) {
    case 1: return lx_mat4_mul(y, lx_mat4_mul(z, x));   /* XZY */
    case 2: return lx_mat4_mul(x, lx_mat4_mul(z, y));   /* YZX */
    case 3: return lx_mat4_mul(z, lx_mat4_mul(x, y));   /* YXZ */
    case 4: return lx_mat4_mul(y, lx_mat4_mul(x, z));   /* ZXY */
    case 5: return lx_mat4_mul(x, lx_mat4_mul(y, z));   /* ZYX */
    default: return lx_mat4_mul(z, lx_mat4_mul(y, x));  /* XYZ */
    }
}
static void m_decompose(LxMat4 m, LxVec3 *t, LxQuat *r, LxVec3 *s) {
    *t = lx_v3(m.m[12], m.m[13], m.m[14]);
    LxVec3 x = lx_v3(m.m[0], m.m[1], m.m[2]), y = lx_v3(m.m[4], m.m[5], m.m[6]), z = lx_v3(m.m[8], m.m[9], m.m[10]);
    float sx = lx_len3(x), sy = lx_len3(y), sz = lx_len3(z);
    if (lx_dot3(lx_cross3(x, y), z) < 0) sx = -sx;   /* mirrored */
    *s = lx_v3(sx, sy, sz);
    if (fabsf(sx) < 1e-12f || sy < 1e-12f || sz < 1e-12f) { *r = lx_quat_identity(); return; }
    x = lx_scale3(x, 1.0f / sx);
    y = lx_scale3(y, 1.0f / sy);
    z = lx_scale3(z, 1.0f / sz);
    /* re-orthonormalise (scale in a parent can leave a little shear) */
    x = lx_norm3(x);
    y = lx_norm3(lx_sub3(y, lx_scale3(x, lx_dot3(x, y))));
    z = lx_cross3(x, y);
    *r = lx_quat_from_basis(x, y, z);
}

/* ======================================================================
 *  FBX object graph
 * ====================================================================== */
typedef struct Obj {
    int64_t id;
    const char *kind;   /* node name: Model, Geometry, Material, Deformer, AnimationCurve... */
    const char *name;   /* object name without the class suffix/prefix */
    const char *sub;    /* "Mesh", "LimbNode", "Skin", "Cluster"... */
    FbxNode *node;
    int index;          /* model: node index; material: material slot ... */
} Obj;

typedef struct Conn { int64_t child, parent; const char *prop; bool op; } Conn;

typedef struct Ctx {
    FbxDoc *doc;
    Obj *objs;
    int nobj;
    Conn *conns;
    int nconn;
    LxModel *m;
    float s;              /* unit scale -> metres */
    LxMat4 axis;          /* file axes -> Luxora axes (rotation only) */
    const char *dir;      /* folder of the .fbx */
    const char *base;     /* file name without extension */
    char assets_dir[LX_PATH_MAX];
    char err[200];
} Ctx;

static int obj_cmp(const void *a, const void *b) {
    int64_t x = ((const Obj *)a)->id, y = ((const Obj *)b)->id;
    return x < y ? -1 : x > y;
}
static Obj *find_obj(Ctx *c, int64_t id) {
    Obj key = {0};
    key.id = id;
    return bsearch(&key, c->objs, (size_t)c->nobj, sizeof(Obj), obj_cmp);
}

/* "Hips\0\1Model" (binary) or "Model::Hips" (ASCII) -> "Hips" */
static const char *clean_name(Ctx *c, const FbxProp *p) {
    const char *raw = fbx_str(c->doc, p);
    if (p->type == 'S') {
        for (uint32_t i = 0; i + 1 < p->len; i++)
            if (p->s[i] == 0 && p->s[i + 1] == 1) {
                char *x = (char *)fbx_str(c->doc, p);
                x[i] = 0;
                return x;
            }
    }
    const char *cc = strstr(raw, "::");
    return cc ? cc + 2 : raw;
}

/* Properties70 lookup: returns the P node for name (values start at props[4]) */
static FbxNode *prop70(FbxNode *obj, const char *name) {
    FbxNode *p70 = fbx_child(obj, "Properties70");
    if (!p70) p70 = fbx_child(obj, "Properties60");
    if (!p70) return NULL;
    size_t n = strlen(name);
    for (int i = 0; i < p70->nkids; i++) {
        FbxNode *p = p70->kids[i];
        if (p->nprops > 0 && p->props[0].type == 'S' && p->props[0].len == n && !memcmp(p->props[0].s, name, n)) return p;
    }
    return NULL;
}
static LxVec3 prop_vec3(FbxNode *obj, const char *name, LxVec3 def) {
    FbxNode *p = prop70(obj, name);
    if (!p || p->nprops < 7) return def;
    return lx_v3((float)fbx_num(&p->props[4]), (float)fbx_num(&p->props[5]), (float)fbx_num(&p->props[6]));
}
static double prop_num(FbxNode *obj, const char *name, double def) {
    FbxNode *p = prop70(obj, name);
    if (!p || p->nprops < 5) return def;
    return fbx_num(&p->props[4]);
}

static FbxProp *array_of(FbxNode *n, const char *name) {
    FbxNode *c = fbx_child(n, name);
    return c && c->nprops && c->props[0].type == 'A' ? &c->props[0] : NULL;
}
static const char *str_of(Ctx *c, FbxNode *n, const char *name) {
    FbxNode *x = fbx_child(n, name);
    return x && x->nprops ? fbx_str(c->doc, &x->props[0]) : "";
}

/* ---- the transform of a Model object (FBX: T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1) ---- */
typedef struct NodeXform {
    LxVec3 t, r, s, pre, post, roff, rp, soff, sp, gt, gr, gs;
    int order;
} NodeXform;

static NodeXform read_xform(FbxNode *o) {
    NodeXform x;
    x.t = prop_vec3(o, "Lcl Translation", lx_v3(0, 0, 0));
    x.r = prop_vec3(o, "Lcl Rotation", lx_v3(0, 0, 0));
    x.s = prop_vec3(o, "Lcl Scaling", lx_v3(1, 1, 1));
    x.pre = prop_vec3(o, "PreRotation", lx_v3(0, 0, 0));
    x.post = prop_vec3(o, "PostRotation", lx_v3(0, 0, 0));
    x.roff = prop_vec3(o, "RotationOffset", lx_v3(0, 0, 0));
    x.rp = prop_vec3(o, "RotationPivot", lx_v3(0, 0, 0));
    x.soff = prop_vec3(o, "ScalingOffset", lx_v3(0, 0, 0));
    x.sp = prop_vec3(o, "ScalingPivot", lx_v3(0, 0, 0));
    x.gt = prop_vec3(o, "GeometricTranslation", lx_v3(0, 0, 0));
    x.gr = prop_vec3(o, "GeometricRotation", lx_v3(0, 0, 0));
    x.gs = prop_vec3(o, "GeometricScaling", lx_v3(1, 1, 1));
    x.order = (int)prop_num(o, "RotationOrder", 0);
    return x;
}

static LxMat4 xform_matrix(const NodeXform *x, LxVec3 t, LxVec3 r, LxVec3 s) {
    LxMat4 m = m_translate(t);
    m = lx_mat4_mul(m, m_translate(x->roff));
    m = lx_mat4_mul(m, m_translate(x->rp));
    m = lx_mat4_mul(m, m_euler(x->pre, 0));
    m = lx_mat4_mul(m, m_euler(r, x->order));
    m = lx_mat4_mul(m, lx_mat4_inverse(m_euler(x->post, 0)));
    m = lx_mat4_mul(m, m_translate(lx_scale3(x->rp, -1)));
    m = lx_mat4_mul(m, m_translate(x->soff));
    m = lx_mat4_mul(m, m_translate(x->sp));
    m = lx_mat4_mul(m, m_scale(s));
    m = lx_mat4_mul(m, m_translate(lx_scale3(x->sp, -1)));
    return m;
}

/* unit + axis conversion of a node's local matrix: translations scale, top-level nodes rotate */
static LxMat4 convert_local(Ctx *c, LxMat4 m, bool top) {
    m.m[12] *= c->s; m.m[13] *= c->s; m.m[14] *= c->s;
    return top ? lx_mat4_mul(c->axis, m) : m;
}
/* the same for a global (scene space) FBX matrix */
static LxMat4 convert_global(Ctx *c, LxMat4 m) { return convert_local(c, m, true); }

static LxMat4 read_matrix(FbxNode *n, const char *name) {
    FbxProp *a = array_of(n, name);
    LxMat4 m = lx_mat4_identity();
    if (a && a->count >= 16)
        for (int i = 0; i < 16; i++) m.m[i] = (float)fbx_arr_d(a, (uint32_t)i);
    return m;
}

/* ======================================================================
 *  axis system
 * ====================================================================== */
static void setup_axes(Ctx *c, FbxNode *root) {
    FbxNode *gs = fbx_child(root, "GlobalSettings");
    int up = 1, upsign = 1, front = 2, frontsign = 1, coord = 0, coordsign = 1;
    double unit = 1.0;
    if (gs) {
        up = (int)prop_num(gs, "UpAxis", 1);
        upsign = (int)prop_num(gs, "UpAxisSign", 1);
        front = (int)prop_num(gs, "FrontAxis", 2);
        frontsign = (int)prop_num(gs, "FrontAxisSign", 1);
        coord = (int)prop_num(gs, "CoordAxis", 0);
        coordsign = (int)prop_num(gs, "CoordAxisSign", 1);
        unit = prop_num(gs, "UnitScaleFactor", 1.0);
    }
    if (up < 0 || up > 2 || front < 0 || front > 2 || coord < 0 || coord > 2 || up == front || up == coord || front == coord) {
        up = 1; front = 2; coord = 0; upsign = frontsign = coordsign = 1;
    }
    /* file axis `coord` -> +X, `up` -> +Y, `front` -> +Z */
    LxMat4 a = lx_mat4_identity();
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) a.m[i * 4 + k] = 0;
    a.m[coord * 4 + 0] = (float)coordsign;
    a.m[up * 4 + 1] = (float)upsign;
    a.m[front * 4 + 2] = (float)frontsign;
    /* keep it a rotation: a mirrored axis system flips the front axis */
    LxVec3 x = lx_v3(a.m[0], a.m[1], a.m[2]), y = lx_v3(a.m[4], a.m[5], a.m[6]), z = lx_v3(a.m[8], a.m[9], a.m[10]);
    if (lx_dot3(lx_cross3(x, y), z) < 0) { a.m[front * 4 + 2] = -a.m[front * 4 + 2]; }
    c->axis = a;
    c->s = (float)(unit * 0.01) * (c->m->imp.scale > 0 ? c->m->imp.scale : 1.0f);
}

/* ======================================================================
 *  materials and textures
 * ====================================================================== */
static void safe_file_name(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    for (const char *p = src; *p && n + 1 < cap; p++) dst[n++] = (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 32) ? '_' : *p;
    dst[n] = 0;
    if (!dst[0]) lx_strcpy(dst, cap, "Material");
}

/* a texture referenced by the FBX -> an asset ref (embedded images are written next to the model) */
static void texture_ref(Ctx *c, Obj *tex, char *out, size_t cap) {
    out[0] = 0;
    if (!tex) return;
    const char *file = str_of(c, tex->node, "RelativeFilename");
    const char *abs = str_of(c, tex->node, "FileName");
    if (!file[0]) file = abs;
    if (!file[0]) return;
    char fname[LX_NAME_MAX];
    const char *slash = file;
    for (const char *p = file; *p; p++) if (*p == '/' || *p == '\\') slash = p + 1;
    safe_file_name(fname, sizeof fname, slash);
    /* embedded? (a Video object with Content connected to the texture) */
    for (int i = 0; i < c->nconn; i++) {
        if (c->conns[i].parent != tex->id) continue;
        Obj *v = find_obj(c, c->conns[i].child);
        if (!v || strcmp(v->kind, "Video")) continue;
        FbxNode *content = fbx_child(v->node, "Content");
        if (!content || !content->nprops || content->props[0].len < 16) continue;
        char dir[LX_PATH_MAX], sub[LX_NAME_MAX + 16], path[LX_PATH_MAX];
        snprintf(sub, sizeof sub, "%s Textures", c->base);
        lx_path_join(dir, sizeof dir, c->dir, sub);
        lx_path_join(path, sizeof path, dir, fname);
        if (!lx_file_exists(path) && strncmp(path, "apk:", 4)) {
            lx_mkdir_p(dir);
            lx_file_write(path, content->props[0].s, content->props[0].len);
        }
        lx_strcpy(out, cap, path);
        return;
    }
    /* next to the model, in <model>.fbm/, or the path as written */
    char path[LX_PATH_MAX], fbm[LX_PATH_MAX + 8];
    char rel[LX_PATH_MAX];
    lx_strcpy(rel, sizeof rel, file);
    for (char *p = rel; *p; p++) if (*p == '\\') *p = '/';
    lx_path_join(path, sizeof path, c->dir, rel);
    if (!lx_file_exists(path)) lx_path_join(path, sizeof path, c->dir, fname);
    if (!lx_file_exists(path)) {
        snprintf(fbm, sizeof fbm, "%s.fbm", c->base);
        char d2[LX_PATH_MAX];
        lx_path_join(d2, sizeof d2, c->dir, fbm);
        lx_path_join(path, sizeof path, d2, fname);
    }
    if (!lx_file_exists(path)) {
        char d2[LX_PATH_MAX], sub[LX_NAME_MAX + 16];
        snprintf(sub, sizeof sub, "%s Textures", c->base);
        lx_path_join(d2, sizeof d2, c->dir, sub);
        lx_path_join(path, sizeof path, d2, fname);
    }
    if (lx_file_exists(path)) lx_strcpy(out, cap, path);
    else LX_WARN("%s: texture '%s' not found next to the model", c->base, file);
}

/* the texture plugged into a material slot */
static Obj *material_texture(Ctx *c, Obj *mat, const char *const *slots) {
    for (int i = 0; i < c->nconn; i++) {
        Conn *k = &c->conns[i];
        if (k->parent != mat->id || !k->op || !k->prop) continue;
        for (int s = 0; slots[s]; s++)
            if (!strcmp(k->prop, slots[s])) {
                Obj *t = find_obj(c, k->child);
                if (t && !strcmp(t->kind, "Texture")) return t;
                /* layered textures: take the first layer */
                if (t && !strcmp(t->kind, "LayeredTexture"))
                    for (int j = 0; j < c->nconn; j++)
                        if (c->conns[j].parent == t->id) {
                            Obj *u = find_obj(c, c->conns[j].child);
                            if (u && !strcmp(u->kind, "Texture")) return u;
                        }
            }
    }
    return NULL;
}

static void asset_ref_of(Ctx *c, const char *abs_path, char *out, size_t cap) {
    const char *root = c->assets_dir;
    size_t n = strlen(root);
    if (n && !strncmp(abs_path, root, n) && (abs_path[n] == '/' || abs_path[n] == '\\')) lx_strcpy(out, cap, abs_path + n + 1);
    else lx_strcpy(out, cap, abs_path);
}

/* Writes "<model> Materials/<name>.lxmat" the first time (afterwards it's yours to edit). */
static void make_material(Ctx *c, Obj *mat, char *out_ref, size_t cap) {
    char dir[LX_PATH_MAX], sub[LX_NAME_MAX + 16], fname[LX_NAME_MAX + 8], path[LX_PATH_MAX], safe[LX_NAME_MAX];
    snprintf(sub, sizeof sub, "%s Materials", c->base);
    lx_path_join(dir, sizeof dir, c->dir, sub);
    safe_file_name(safe, sizeof safe, mat->name);
    snprintf(fname, sizeof fname, "%s.lxmat", safe);
    lx_path_join(path, sizeof path, dir, fname);
    asset_ref_of(c, path, out_ref, cap);
    if (lx_file_exists(path) || !strncmp(path, "apk:", 4)) return;
    LxMaterial m = lx_material_default();
    lx_strcpy(m.name, sizeof m.name, mat->name);
    FbxNode *o = mat->node;
    LxVec3 diffuse = prop_vec3(o, "DiffuseColor", prop_vec3(o, "Diffuse", lx_v3(0.8f, 0.8f, 0.8f)));
    float dfac = (float)prop_num(o, "DiffuseFactor", 1.0);
    m.albedo = lx_v4(diffuse.x * dfac, diffuse.y * dfac, diffuse.z * dfac, 1);
    float opacity = (float)prop_num(o, "Opacity", 1.0);
    m.albedo.w = opacity > 0 ? opacity : 1;
    double shin = prop_num(o, "ShininessExponent", prop_num(o, "Shininess", 20.0));
    m.roughness = lx_clampf(sqrtf(2.0f / (float)(shin + 2.0)), 0.08f, 1.0f);
    LxVec3 em = prop_vec3(o, "EmissiveColor", prop_vec3(o, "Emissive", lx_v3(0, 0, 0)));
    float efac = (float)prop_num(o, "EmissiveFactor", 1.0);
    if (em.x + em.y + em.z > 0.001f && efac > 0) { m.emissive = em; m.emissive_strength = efac; }
    static const char *diff_slots[] = {"DiffuseColor", "Diffuse", "Maya|baseColor", "3dsMax|Parameters|base_color_map", NULL};
    static const char *norm_slots[] = {"NormalMap", "Bump", "Maya|normalCamera", "3dsMax|Parameters|bump_map", NULL};
    static const char *rough_slots[] = {"ShininessExponent", "Maya|specularRoughness", "3dsMax|Parameters|roughness_map", NULL};
    static const char *metal_slots[] = {"ReflectionFactor", "Maya|metalness", "3dsMax|Parameters|metalness_map", NULL};
    static const char *emis_slots[] = {"EmissiveColor", "Maya|emissionColor", NULL};
    char abs[LX_PATH_MAX];
    texture_ref(c, material_texture(c, mat, diff_slots), abs, sizeof abs);
    if (abs[0]) { asset_ref_of(c, abs, m.albedo_map, sizeof m.albedo_map); m.albedo = lx_v4(1, 1, 1, m.albedo.w); }
    texture_ref(c, material_texture(c, mat, norm_slots), abs, sizeof abs);
    if (abs[0]) asset_ref_of(c, abs, m.normal_map, sizeof m.normal_map);
    texture_ref(c, material_texture(c, mat, rough_slots), abs, sizeof abs);
    if (abs[0]) { asset_ref_of(c, abs, m.roughness_map, sizeof m.roughness_map); m.roughness = 1; }
    texture_ref(c, material_texture(c, mat, metal_slots), abs, sizeof abs);
    if (abs[0]) { asset_ref_of(c, abs, m.metallic_map, sizeof m.metallic_map); m.metallic = 1; }
    texture_ref(c, material_texture(c, mat, emis_slots), abs, sizeof abs);
    if (abs[0] && !m.emissive_strength) { m.emissive = lx_v3(1, 1, 1); m.emissive_strength = 1; }
    lx_mkdir_p(dir);
    lx_material_save(&m, path);
}

/* ======================================================================
 *  geometry
 * ====================================================================== */
typedef struct LayerRef { FbxProp *data, *index; int mapping; int comps; } LayerRef;   /* mapping: 0 per polygon-vertex, 1 per control point, 2 per polygon, 3 all same */

static LayerRef layer(Ctx *c, FbxNode *geo, const char *elem, const char *data_name, const char *index_name, int comps) {
    LayerRef r = {0};
    FbxNode *l = fbx_child(geo, elem);
    if (!l) return r;
    r.data = array_of(l, data_name);
    r.index = array_of(l, index_name);
    r.comps = comps;
    const char *map = str_of(c, l, "MappingInformationType");
    const char *ref = str_of(c, l, "ReferenceInformationType");
    r.mapping = !strcmp(map, "ByPolygonVertex") ? 0 : (!strcmp(map, "ByVertice") || !strcmp(map, "ByVertex") || !strcmp(map, "ByControlPoint")) ? 1
              : !strcmp(map, "ByPolygon") ? 2 : !strcmp(map, "AllSame") ? 3 : 0;
    if (strcmp(ref, "IndexToDirect") && strcmp(ref, "Index")) r.index = NULL;
    return r;
}
static int layer_index(const LayerRef *l, int pv, int cp, int poly) {
    int i = l->mapping == 0 ? pv : l->mapping == 1 ? cp : l->mapping == 2 ? poly : 0;
    if (l->index) i = (int)fbx_arr_i(l->index, (uint32_t)i);
    return i;
}

typedef struct Vkey { int cp; float n[3], uv[2]; } Vkey;
typedef struct PartBuild { LxVertex *v; uint32_t *i; int *cp; int vc, vcap, ic, icap, cpcap; uint32_t *hash; int hcap; } PartBuild;

static uint32_t vhash(const LxVertex *v, int cp) {
    uint32_t h = 2166136261u ^ (uint32_t)cp;
    const uint32_t *w = (const uint32_t *)v;
    for (int k = 0; k < 8; k++) { h ^= w[k]; h *= 16777619u; }
    return h;
}
static uint32_t part_vertex(PartBuild *b, const LxVertex *v, int cp) {
    if (b->vc * 2 >= b->hcap) {   /* grow the hash table */
        int nc = b->hcap ? b->hcap * 2 : 1024;
        uint32_t *nh = lx_alloc(sizeof(uint32_t) * (size_t)nc);
        for (int i = 0; i < nc; i++) nh[i] = 0xffffffffu;
        for (int i = 0; i < b->vc; i++) {
            uint32_t h = vhash(&b->v[i], b->cp[i]) & (uint32_t)(nc - 1);
            while (nh[h] != 0xffffffffu) h = (h + 1) & (uint32_t)(nc - 1);
            nh[h] = (uint32_t)i;
        }
        lx_free(b->hash);
        b->hash = nh;
        b->hcap = nc;
    }
    uint32_t h = vhash(v, cp) & (uint32_t)(b->hcap - 1);
    while (b->hash[h] != 0xffffffffu) {
        uint32_t i = b->hash[h];
        if (b->cp[i] == cp && !memcmp(&b->v[i], v, sizeof(float) * 8)) return i;
        h = (h + 1) & (uint32_t)(b->hcap - 1);
    }
    LX_VEC_PUSH(b->v, b->vc, b->vcap, *v);
    int dummy = b->vc - 1;
    if (dummy >= b->cpcap) { b->cpcap = b->vcap; b->cp = lx_realloc(b->cp, sizeof(int) * (size_t)b->cpcap); }
    b->cp[dummy] = cp;
    b->hash[h] = (uint32_t)dummy;
    return (uint32_t)dummy;
}

/* builds the mesh parts of one Mesh model (one part per material) */
static void build_geometry(Ctx *c, Obj *model, Obj *geo, int node_index, Obj **mats, int nmat) {
    FbxNode *g = geo->node;
    FbxProp *verts = array_of(g, "Vertices"), *pvi = array_of(g, "PolygonVertexIndex");
    if (!verts || !pvi || verts->count < 9) return;
    int ncp = (int)(verts->count / 3);
    NodeXform x = read_xform(model->node);
    /* geometric transform (applies to this mesh only) */
    LxMat4 geom = lx_mat4_mul(m_translate(x.gt), lx_mat4_mul(m_euler(x.gr, 0), m_scale(x.gs)));
    geom.m[12] *= c->s; geom.m[13] *= c->s; geom.m[14] *= c->s;
    LxMat4 geom_n = lx_mat4_transpose(lx_mat4_inverse(geom));
    LayerRef ln = layer(c, g, "LayerElementNormal", "Normals", "NormalsIndex", 3);
    if (!ln.index) ln.index = array_of(fbx_child(g, "LayerElementNormal"), "NormalIndex");
    LayerRef luv = layer(c, g, "LayerElementUV", "UV", "UVIndex", 2);
    LayerRef lmat = layer(c, g, "LayerElementMaterial", "Materials", "", 1);
    if (c->m->imp.calculate_normals) ln.data = NULL;
    int parts = nmat > 0 ? nmat : 1;
    PartBuild *pb = lx_calloc((size_t)parts, sizeof(PartBuild));

    /* polygons -> triangle fans */
    int poly = 0, start = 0;
    uint32_t n = pvi->count;
    uint32_t *corner = NULL;
    int ccap = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t raw = fbx_arr_i(pvi, i);
        if (raw >= 0) continue;
        int count = (int)(i - (uint32_t)start + 1);
        int mi = 0;
        if (lmat.data) mi = (int)fbx_arr_i(lmat.data, lmat.mapping == 3 ? 0 : (uint32_t)poly);
        if (mi < 0 || mi >= parts) mi = 0;
        PartBuild *b = &pb[mi];
        if (count > ccap) { ccap = count * 2; corner = lx_realloc(corner, sizeof(uint32_t) * (size_t)ccap); }
        for (int k = 0; k < count; k++) {
            int pv = start + k;
            int64_t ci = fbx_arr_i(pvi, (uint32_t)pv);
            int cp = (int)(ci < 0 ? ~ci : ci);
            if (cp < 0 || cp >= ncp) cp = 0;
            LxVertex v;
            memset(&v, 0, sizeof v);
            LxVec3 p = lx_v3((float)fbx_arr_d(verts, (uint32_t)cp * 3) * c->s, (float)fbx_arr_d(verts, (uint32_t)cp * 3 + 1) * c->s,
                             (float)fbx_arr_d(verts, (uint32_t)cp * 3 + 2) * c->s);
            p = lx_mat4_point(geom, p);
            v.px = p.x; v.py = p.y; v.pz = p.z;
            if (ln.data) {
                int ni = layer_index(&ln, pv, cp, poly);
                LxVec3 nn = lx_v3((float)fbx_arr_d(ln.data, (uint32_t)ni * 3), (float)fbx_arr_d(ln.data, (uint32_t)ni * 3 + 1),
                                  (float)fbx_arr_d(ln.data, (uint32_t)ni * 3 + 2));
                nn = lx_norm3(lx_mat4_dir(geom_n, nn));
                v.nx = nn.x; v.ny = nn.y; v.nz = nn.z;
            }
            if (luv.data) {
                int ui = layer_index(&luv, pv, cp, poly);
                v.u = (float)fbx_arr_d(luv.data, (uint32_t)ui * 2);
                v.v = 1.0f - (float)fbx_arr_d(luv.data, (uint32_t)ui * 2 + 1);   /* FBX v points up */
            }
            corner[k] = ln.data ? part_vertex(b, &v, cp) : 0;
            if (!ln.data) {   /* flat for now: smoothed below */
                LX_VEC_PUSH(b->v, b->vc, b->vcap, v);
                if (b->vc > b->cpcap) { b->cpcap = b->vcap; b->cp = lx_realloc(b->cp, sizeof(int) * (size_t)b->cpcap); }
                b->cp[b->vc - 1] = cp;
                corner[k] = (uint32_t)(b->vc - 1);
            }
        }
        for (int k = 1; k + 1 < count; k++) {
            LX_VEC_PUSH(b->i, b->ic, b->icap, corner[0]);
            LX_VEC_PUSH(b->i, b->ic, b->icap, corner[k]);
            LX_VEC_PUSH(b->i, b->ic, b->icap, corner[k + 1]);
        }
        start = (int)i + 1;
        poly++;
    }
    lx_free(corner);

    /* skin: clusters of the Skin deformer on this geometry */
    typedef struct Influence { int bone; float w; } Influence;
    Influence (*inf)[4] = NULL;
    int nbones = 0;
    char (*bone_names)[64] = NULL;
    LxMat4 *inv_bind = NULL;
    LxMat4 mesh_world = lx_mat4_identity();
    for (int n = node_index; n >= 0; n = c->m->nodes[n].parent) {
        const ModelNode *mn = &c->m->nodes[n];
        mesh_world = lx_mat4_mul(lx_mat4_trs(mn->pos, mn->rot, mn->scale), mesh_world);
    }
    for (int i = 0; i < c->nconn; i++) {
        if (c->conns[i].parent != geo->id) continue;
        Obj *skin = find_obj(c, c->conns[i].child);
        if (!skin || strcmp(skin->kind, "Deformer") || strcmp(skin->sub, "Skin")) continue;
        inf = lx_calloc((size_t)ncp, sizeof *inf);
        for (int j = 0; j < c->nconn; j++) {
            if (c->conns[j].parent != skin->id) continue;
            Obj *cl = find_obj(c, c->conns[j].child);
            if (!cl || strcmp(cl->kind, "Deformer") || strcmp(cl->sub, "Cluster")) continue;
            Obj *bone = NULL;
            for (int k = 0; k < c->nconn && !bone; k++)
                if (c->conns[k].parent == cl->id) {
                    Obj *b = find_obj(c, c->conns[k].child);
                    if (b && !strcmp(b->kind, "Model")) bone = b;
                }
            if (!bone) continue;
            c->m->nodes[bone->index].bone = true;
            FbxProp *idx = array_of(cl->node, "Indexes"), *wts = array_of(cl->node, "Weights");
            LxMat4 tl = convert_global(c, read_matrix(cl->node, "TransformLink"));
            int bi = nbones++;
            bone_names = lx_realloc(bone_names, sizeof *bone_names * (size_t)nbones);
            inv_bind = lx_realloc(inv_bind, sizeof(LxMat4) * (size_t)nbones);
            lx_strcpy(bone_names[bi], 64, c->m->nodes[bone->index].name);
            /* stored vertex (node-local, geometric transform applied) -> bone space at bind. The cluster's
             * "Transform" isn't used: exporters disagree on what it holds (Mixamo stores something else), so the
             * mesh's place at bind is its rest position in the hierarchy - the same as three.js and Blender do. */
            inv_bind[bi] = lx_mat4_mul(lx_mat4_inverse(tl), mesh_world);
            for (uint32_t k = 0; idx && wts && k < idx->count && k < wts->count; k++) {
                int cp = (int)fbx_arr_i(idx, k);
                float w = (float)fbx_arr_d(wts, k);
                if (cp < 0 || cp >= ncp || w <= 0) continue;
                /* keep the 4 strongest */
                int slot = 0;
                for (int s = 1; s < 4; s++) if (inf[cp][s].w < inf[cp][slot].w) slot = s;
                if (w > inf[cp][slot].w) { inf[cp][slot].bone = bi; inf[cp][slot].w = w; }
            }
        }
        break;
    }

    ModelNode *nd = &c->m->nodes[node_index];
    nd->part_first = c->m->part_count;
    for (int p = 0; p < parts; p++) {
        PartBuild *b = &pb[p];
        if (!b->ic) { lx_free(b->v); lx_free(b->i); lx_free(b->cp); lx_free(b->hash); continue; }
        if (!ln.data) {   /* calculate smooth normals: average face normals per control point within the smoothing angle */
            LxVec3 *acc = lx_calloc((size_t)ncp, sizeof(LxVec3));
            LxVec3 *fn = lx_alloc(sizeof(LxVec3) * (size_t)(b->ic / 3));
            for (int t = 0; t + 2 < b->ic; t += 3) {
                LxVertex *a = &b->v[b->i[t]], *bb = &b->v[b->i[t + 1]], *cc = &b->v[b->i[t + 2]];
                LxVec3 e1 = lx_v3(bb->px - a->px, bb->py - a->py, bb->pz - a->pz), e2 = lx_v3(cc->px - a->px, cc->py - a->py, cc->pz - a->pz);
                LxVec3 f = lx_cross3(e1, e2);
                fn[t / 3] = f;
                for (int k = 0; k < 3; k++) acc[b->cp[b->i[t + k]]] = lx_add3(acc[b->cp[b->i[t + k]]], f);
            }
            float cos_lim = cosf(lx_clampf(c->m->imp.smoothing_angle, 0, 180) * LX_DEG2RAD);
            for (int t = 0; t + 2 < b->ic; t += 3) {
                LxVec3 f = lx_norm3(fn[t / 3]);
                for (int k = 0; k < 3; k++) {
                    LxVertex *v = &b->v[b->i[t + k]];
                    LxVec3 sm = lx_norm3(acc[b->cp[b->i[t + k]]]);
                    LxVec3 nn = lx_dot3(sm, f) >= cos_lim ? sm : f;
                    v->nx = nn.x; v->ny = nn.y; v->nz = nn.z;
                }
            }
            lx_free(acc);
            lx_free(fn);
        }
        char ref[LX_REF_MAX];
        snprintf(ref, sizeof ref, "%s#mesh%d", c->m->ref, c->m->part_count);
        LxMesh *mesh = lx_mesh_create(b->v, b->vc, b->i, b->ic, ref);
        if (inf) {
            LxMeshSkin *sk = lx_calloc(1, sizeof *sk);
            sk->bone_count = nbones;
            sk->bones = lx_alloc(sizeof *sk->bones * (size_t)(nbones ? nbones : 1));
            memcpy(sk->bones, bone_names, sizeof *sk->bones * (size_t)nbones);
            sk->inverse_bind = lx_alloc(sizeof(LxMat4) * (size_t)(nbones ? nbones : 1));
            memcpy(sk->inverse_bind, inv_bind, sizeof(LxMat4) * (size_t)nbones);
            sk->joints = lx_calloc((size_t)b->vc, sizeof *sk->joints);
            sk->weights = lx_calloc((size_t)b->vc, sizeof *sk->weights);
            for (int v = 0; v < b->vc; v++) {
                Influence *f = inf[b->cp[v]];
                float sum = f[0].w + f[1].w + f[2].w + f[3].w;
                for (int k = 0; k < 4; k++) {
                    sk->joints[v][k] = (uint16_t)f[k].bone;
                    sk->weights[v][k] = sum > 0 ? f[k].w / sum : 0;
                }
            }
            mesh->skin = sk;
        } else lx_mesh_generate_lods(mesh);
        ModelPart part;
        memset(&part, 0, sizeof part);
        part.node = node_index;
        part.mesh = mesh;
        if (p < nmat && mats[p]) {
            lx_strcpy(part.name, sizeof part.name, mats[p]->name);
            if (c->m->imp.import_materials) make_material(c, mats[p], part.material, sizeof part.material);
        } else lx_strcpy(part.name, sizeof part.name, model->name);
        if (!part.material[0]) lx_strcpy(part.material, sizeof part.material, "builtin:default");
        LX_VEC_PUSH(c->m->parts, c->m->part_count, c->m->part_cap, part);
        lx_free(b->v); lx_free(b->i); lx_free(b->cp); lx_free(b->hash);
    }
    nd->part_count = c->m->part_count - nd->part_first;
    lx_free(pb);
    lx_free(inf);
    lx_free(bone_names);
    lx_free(inv_bind);
}

/* ======================================================================
 *  animation
 * ====================================================================== */
#define FBX_TICKS 46186158000.0

typedef struct Curve { FbxProp *t, *v; float def; } Curve;
static float eval_curve(const Curve *c, int64_t time) {
    if (!c->t || !c->v || !c->t->count) return c->def;
    uint32_t n = c->t->count < c->v->count ? c->t->count : c->v->count;
    if (time <= fbx_arr_i(c->t, 0)) return (float)fbx_arr_d(c->v, 0);
    if (time >= fbx_arr_i(c->t, n - 1)) return (float)fbx_arr_d(c->v, n - 1);
    uint32_t lo = 0, hi = n - 1;
    while (hi - lo > 1) { uint32_t mid = (lo + hi) / 2; if (fbx_arr_i(c->t, mid) <= time) lo = mid; else hi = mid; }
    double t0 = (double)fbx_arr_i(c->t, lo), t1 = (double)fbx_arr_i(c->t, hi);
    double a = t1 > t0 ? ((double)time - t0) / (t1 - t0) : 0;
    return (float)(fbx_arr_d(c->v, lo) + (fbx_arr_d(c->v, hi) - fbx_arr_d(c->v, lo)) * a);
}

typedef struct NodeAnim { int node; Curve ch[3][3]; bool has[3]; } NodeAnim;   /* [T,R,S][x,y,z] */

static void build_clip(Ctx *c, Obj *stack, Obj **model_objs) {
    LxModel *m = c->m;
    /* curve nodes of the first layer */
    Obj *layer = NULL;
    for (int i = 0; i < c->nconn && !layer; i++)
        if (c->conns[i].parent == stack->id) {
            Obj *l = find_obj(c, c->conns[i].child);
            if (l && !strcmp(l->kind, "AnimationLayer")) layer = l;
        }
    if (!layer) return;
    NodeAnim *na = NULL;
    int nna = 0, cap = 0;
    int64_t tmin = INT64_MAX, tmax = INT64_MIN;
    for (int i = 0; i < c->nconn; i++) {
        if (c->conns[i].parent != layer->id) continue;
        Obj *cn = find_obj(c, c->conns[i].child);
        if (!cn || strcmp(cn->kind, "AnimationCurveNode")) continue;
        /* which model property it drives */
        int ch = -1, node = -1;
        for (int k = 0; k < c->nconn; k++) {
            Conn *q = &c->conns[k];
            if (q->child != cn->id || !q->op || !q->prop) continue;
            Obj *mo = find_obj(c, q->parent);
            if (!mo || strcmp(mo->kind, "Model")) continue;
            ch = !strcmp(q->prop, "Lcl Translation") ? 0 : !strcmp(q->prop, "Lcl Rotation") ? 1 : !strcmp(q->prop, "Lcl Scaling") ? 2 : -1;
            node = mo->index;
            break;
        }
        if (ch < 0 || node < 0) continue;
        int slot = -1;
        for (int k = 0; k < nna; k++) if (na[k].node == node) slot = k;
        if (slot < 0) {
            NodeAnim a;
            memset(&a, 0, sizeof a);
            a.node = node;
            LX_VEC_PUSH(na, nna, cap, a);
            slot = nna - 1;
        }
        NodeAnim *a = &na[slot];
        a->has[ch] = true;
        static const char *dn[3] = {"d|X", "d|Y", "d|Z"};
        for (int k = 0; k < 3; k++) a->ch[ch][k].def = (float)prop_num(cn->node, dn[k], ch == 2 ? 1.0 : 0.0);
        for (int k = 0; k < c->nconn; k++) {
            Conn *q = &c->conns[k];
            if (q->parent != cn->id || !q->prop) continue;
            Obj *cv = find_obj(c, q->child);
            if (!cv || strcmp(cv->kind, "AnimationCurve")) continue;
            int axis = !strcmp(q->prop, "d|X") ? 0 : !strcmp(q->prop, "d|Y") ? 1 : !strcmp(q->prop, "d|Z") ? 2 : -1;
            if (axis < 0) continue;
            Curve *cu = &a->ch[ch][axis];
            cu->t = array_of(cv->node, "KeyTime");
            cu->v = array_of(cv->node, "KeyValueFloat");
            if (!cu->v) cu->v = array_of(cv->node, "KeyValue");
            if (cu->t && cu->t->count) {
                int64_t t0 = fbx_arr_i(cu->t, 0), t1 = fbx_arr_i(cu->t, cu->t->count - 1);
                if (t0 < tmin) tmin = t0;
                if (t1 > tmax) tmax = t1;
            }
        }
    }
    if (!nna || tmax < tmin) { lx_free(na); return; }
    /* the stack's own span when it has one */
    double ls = prop_num(stack->node, "LocalStart", 0), le = prop_num(stack->node, "LocalStop", 0);
    if (le > ls) { tmin = (int64_t)ls; tmax = (int64_t)le; }
    float fps = 30.0f;
    double secs = (double)(tmax - tmin) / FBX_TICKS;
    int frames = (int)(secs * fps + 0.5) + 1;
    if (frames < 2) frames = 2;
    if (frames > 30 * 60 * 10) frames = 30 * 60 * 10;
    ModelClip clip;
    memset(&clip, 0, sizeof clip);
    lx_strcpy(clip.name, sizeof clip.name, stack->name);
    /* "Armature|Run" (Blender) -> "Run" */
    const char *bar = strrchr(clip.name, '|');
    if (bar && bar[1]) memmove(clip.name, bar + 1, strlen(bar + 1) + 1);
    clip.fps = fps;
    clip.frames = frames;
    clip.length = (float)(frames - 1) / fps;
    clip.tracks = nna;
    clip.node = lx_alloc(sizeof(int) * (size_t)nna);
    clip.pos = lx_alloc(sizeof(LxVec3) * (size_t)nna * (size_t)frames);
    clip.rot = lx_alloc(sizeof(LxQuat) * (size_t)nna * (size_t)frames);
    clip.scl = lx_alloc(sizeof(LxVec3) * (size_t)nna * (size_t)frames);
    for (int t = 0; t < nna; t++) {
        NodeAnim *a = &na[t];
        clip.node[t] = a->node;
        Obj *mo = model_objs[a->node];
        NodeXform x = read_xform(mo->node);
        bool top = m->nodes[a->node].parent < 0;
        LxQuat prev = lx_quat_identity();
        for (int f = 0; f < frames; f++) {
            int64_t time = tmin + (int64_t)((double)f / fps * FBX_TICKS);
            LxVec3 v[3] = {x.t, x.r, x.s};
            for (int ch = 0; ch < 3; ch++)
                if (a->has[ch]) v[ch] = lx_v3(eval_curve(&a->ch[ch][0], time), eval_curve(&a->ch[ch][1], time), eval_curve(&a->ch[ch][2], time));
            LxMat4 L = convert_local(c, xform_matrix(&x, v[0], v[1], v[2]), top);
            LxVec3 p, s;
            LxQuat q;
            m_decompose(L, &p, &q, &s);
            /* keep quaternions in one hemisphere so interpolation takes the short way */
            if (f && q.x * prev.x + q.y * prev.y + q.z * prev.z + q.w * prev.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
            prev = q;
            size_t k = (size_t)f * (size_t)nna + (size_t)t;
            clip.pos[k] = p;
            clip.rot[k] = q;
            clip.scl[k] = s;
        }
    }
    lx_free(na);
    LX_VEC_PUSH(m->clips, m->clip_count, m->clip_cap, clip);
}

/* ======================================================================
 *  loading
 * ====================================================================== */

LxModel *lx_model_load_file(const char *path, const char *ref, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = 0;
    size_t size;
    uint8_t *data = (uint8_t *)lx_file_read(path, &size);
    if (!data) { if (err) snprintf(err, err_cap, "File not found"); return NULL; }
    FbxDoc *doc = fbx_parse(data, size, err, err_cap);
    if (!doc) { lx_free(data); return NULL; }
    LxModel *m = lx_calloc(1, sizeof *m);
    lx_strcpy(m->ref, sizeof m->ref, ref);
    if (!lx_model_import_load(path, &m->imp)) m->imp = lx_model_import_defaults();
    for (int i = 0; i < LX_HB_COUNT; i++) m->avatar[i] = -1;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.doc = doc;
    c.m = m;
    char dir[LX_PATH_MAX], base[LX_NAME_MAX];
    lx_path_parent(dir, sizeof dir, path);
    lx_strcpy(base, sizeof base, lx_path_filename(path));
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    c.dir = dir;
    c.base = base;
    lx_strcpy(c.assets_dir, sizeof c.assets_dir, lx_assets_root());
    FbxNode *root = fbx_root(doc);
    setup_axes(&c, root);

    /* objects */
    FbxNode *objs = fbx_child(root, "Objects");
    int nobj = objs ? objs->nkids : 0;
    c.objs = lx_calloc((size_t)(nobj ? nobj : 1), sizeof(Obj));
    for (int i = 0; i < nobj; i++) {
        FbxNode *o = objs->kids[i];
        if (o->nprops < 2) continue;
        Obj *ob = &c.objs[c.nobj++];
        ob->id = o->props[0].type == 'I' ? o->props[0].i : (int64_t)fbx_num(&o->props[0]);
        ob->kind = o->name;
        ob->name = clean_name(&c, &o->props[1]);
        ob->sub = o->nprops > 2 ? fbx_str(doc, &o->props[2]) : "";
        ob->node = o;
        ob->index = -1;
    }
    qsort(c.objs, (size_t)c.nobj, sizeof(Obj), obj_cmp);
    /* connections */
    FbxNode *cs = fbx_child(root, "Connections");
    int conn_cap = 0;
    for (int i = 0; cs && i < cs->nkids; i++) {
        FbxNode *k = cs->kids[i];
        if (k->nprops < 3) continue;
        Conn q;
        const char *type = fbx_str(doc, &k->props[0]);
        q.op = !strcmp(type, "OP");
        q.child = k->props[1].type == 'I' ? k->props[1].i : (int64_t)fbx_num(&k->props[1]);
        q.parent = k->props[2].type == 'I' ? k->props[2].i : (int64_t)fbx_num(&k->props[2]);
        q.prop = k->nprops > 3 ? fbx_str(doc, &k->props[3]) : NULL;
        LX_VEC_PUSH(c.conns, c.nconn, conn_cap, q);
    }

    /* models -> nodes, parents first */
    int nmodels = 0;
    for (int i = 0; i < c.nobj; i++) if (!strcmp(c.objs[i].kind, "Model")) nmodels++;
    Obj **order = lx_calloc((size_t)(nmodels ? nmodels : 1), sizeof(Obj *));
    int *parent_of = lx_alloc(sizeof(int) * (size_t)(nmodels ? nmodels : 1));
    Obj **mobj = lx_alloc(sizeof(Obj *) * (size_t)(nmodels ? nmodels : 1));
    int k = 0;
    for (int i = 0; i < c.nobj; i++) if (!strcmp(c.objs[i].kind, "Model")) { mobj[k] = &c.objs[i]; c.objs[i].index = k; k++; }
    for (int i = 0; i < nmodels; i++) {
        parent_of[i] = -1;
        for (int j = 0; j < c.nconn; j++)
            if (c.conns[j].child == mobj[i]->id && !c.conns[j].op) {
                Obj *p = find_obj(&c, c.conns[j].parent);
                if (p && !strcmp(p->kind, "Model")) { parent_of[i] = p->index; break; }
            }
    }
    /* breadth-first so parents come before children */
    int count = 0;
    char *placed = lx_calloc((size_t)(nmodels ? nmodels : 1), 1);
    for (int pass = 0; pass < nmodels && count < nmodels; pass++)
        for (int i = 0; i < nmodels; i++)
            if (!placed[i] && (parent_of[i] < 0 || placed[parent_of[i]])) { order[count++] = mobj[i]; placed[i] = 1; }
    for (int i = 0; i < nmodels; i++) if (!placed[i]) order[count++] = mobj[i];   /* cycles: just append */
    int *remap = lx_alloc(sizeof(int) * (size_t)(nmodels ? nmodels : 1));
    for (int i = 0; i < count; i++) remap[order[i]->index] = i;
    m->nodes = lx_calloc((size_t)(count ? count : 1), sizeof(ModelNode));
    m->node_count = count;
    Obj **node_obj = lx_alloc(sizeof(Obj *) * (size_t)(count ? count : 1));
    for (int i = 0; i < count; i++) {
        Obj *o = order[i];
        int old = o->index;
        node_obj[i] = o;
        ModelNode *nd = &m->nodes[i];
        lx_strcpy(nd->name, sizeof nd->name, o->name);
        nd->parent = parent_of[old] >= 0 ? remap[parent_of[old]] : -1;
        nd->bone = !strcmp(o->sub, "LimbNode") || !strcmp(o->sub, "Root");
        NodeXform x = read_xform(o->node);
        LxMat4 L = convert_local(&c, xform_matrix(&x, x.t, x.r, x.s), nd->parent < 0);
        m_decompose(L, &nd->pos, &nd->rot, &nd->scale);
    }
    for (int i = 0; i < count; i++) node_obj[i]->index = i;
    lx_free(placed);
    lx_free(remap);
    lx_free(parent_of);
    lx_free(mobj);
    lx_free(order);

    /* meshes */
    for (int i = 0; i < count; i++) {
        Obj *o = node_obj[i];
        Obj *geo = NULL;
        Obj *mats[64];
        int nmat = 0;
        for (int j = 0; j < c.nconn; j++) {
            if (c.conns[j].parent != o->id) continue;
            Obj *ch = find_obj(&c, c.conns[j].child);
            if (!ch) continue;
            if (!strcmp(ch->kind, "Geometry") && !strcmp(ch->sub, "Mesh") && !geo) geo = ch;
            else if (!strcmp(ch->kind, "Material") && nmat < 64) mats[nmat++] = ch;
        }
        if (geo) build_geometry(&c, o, geo, i, mats, nmat);
    }
    /* bones: anything with a bone below it is part of the skeleton too (e.g. Blender's Armature) */
    for (int pass = 0; pass < 2; pass++)
        for (int i = count - 1; i >= 0; i--)
            if (m->nodes[i].bone && m->nodes[i].parent >= 0 && !m->nodes[m->nodes[i].parent].part_count) m->nodes[m->nodes[i].parent].bone = true;
    for (int i = 0; i < count; i++) if (m->nodes[i].bone) { LX_VEC_PUSH(m->bones, m->bone_count, m->bone_cap, i); }

    /* animation */
    if (m->imp.import_animations)
        for (int i = 0; i < c.nobj; i++)
            if (!strcmp(c.objs[i].kind, "AnimationStack")) build_clip(&c, &c.objs[i], node_obj);
    /* clip names and loop flags from the import settings */
    for (int i = 0; i < m->clip_count && i < LX_MAX_CLIPS; i++) {
        if (i < m->imp.clip_count && m->imp.clips[i].name[0]) lx_strcpy(m->clips[i].name, sizeof m->clips[i].name, m->imp.clips[i].name);
    }
    if (m->imp.clip_count < m->clip_count) {
        for (int i = m->imp.clip_count; i < m->clip_count && i < LX_MAX_CLIPS; i++) {
            lx_strcpy(m->imp.clips[i].name, sizeof m->imp.clips[i].name, m->clips[i].name);
            m->imp.clips[i].loop = true;
        }
        m->imp.clip_count = m->clip_count < LX_MAX_CLIPS ? m->clip_count : LX_MAX_CLIPS;
    }

    /* rest pose in model space + bounds */
    m->rest_wpos = lx_alloc(sizeof(LxVec3) * (size_t)(count ? count : 1));
    m->rest_wrot = lx_alloc(sizeof(LxQuat) * (size_t)(count ? count : 1));
    LxMat4 *world = lx_alloc(sizeof(LxMat4) * (size_t)(count ? count : 1));
    m->bounds = lx_aabb_empty();
    for (int i = 0; i < count; i++) {
        ModelNode *nd = &m->nodes[i];
        LxMat4 L = lx_mat4_trs(nd->pos, nd->rot, nd->scale);
        world[i] = nd->parent >= 0 ? lx_mat4_mul(world[nd->parent], L) : L;
        m->rest_wpos[i] = lx_v3(world[i].m[12], world[i].m[13], world[i].m[14]);
        m->rest_wrot[i] = nd->parent >= 0 ? lx_quat_norm(lx_quat_mul(m->rest_wrot[nd->parent], nd->rot)) : nd->rot;
        for (int p = nd->part_first; p < nd->part_first + nd->part_count; p++) {
            LxAabb b = m->parts[p].mesh->bounds;
            for (int corner = 0; corner < 8; corner++) {
                LxVec3 q = lx_v3(corner & 1 ? b.max.x : b.min.x, corner & 2 ? b.max.y : b.min.y, corner & 4 ? b.max.z : b.min.z);
                lx_aabb_grow(&m->bounds, lx_mat4_point(world[i], q));
            }
        }
    }
    lx_free(world);
    lx_free(node_obj);
    lx_free(c.objs);
    lx_free(c.conns);
    fbx_free(doc);
    lx_free(data);
    if (m->imp.rig == LX_RIG_HUMANOID) lx_model_prepare_avatar(m);
    LX_INFO("Imported %s: %d nodes, %d bones, %d mesh parts, %d clips", ref, m->node_count, m->bone_count, m->part_count, m->clip_count);
    return m;
}

void lx_model_free(LxModel *m) {
    if (!m) return;
    for (int i = 0; i < m->part_count; i++) {
        LxMesh *mesh = m->parts[i].mesh;
        if (mesh && mesh->skin) {
            lx_free(mesh->skin->bones);
            lx_free(mesh->skin->inverse_bind);
            lx_free(mesh->skin->joints);
            lx_free(mesh->skin->weights);
            lx_free(mesh->skin);
            mesh->skin = NULL;
        }
        lx_mesh_destroy(mesh);
    }
    for (int i = 0; i < m->clip_count; i++) {
        lx_free(m->clips[i].node);
        lx_free(m->clips[i].pos);
        lx_free(m->clips[i].rot);
        lx_free(m->clips[i].scl);
    }
    lx_free(m->clips);
    lx_free(m->parts);
    lx_free(m->nodes);
    lx_free(m->bones);
    lx_free(m->rest_wpos);
    lx_free(m->rest_wrot);
    lx_free(m);
}

/* ======================================================================
 *  import settings (<model>.lximport)
 * ====================================================================== */
LxModelImport lx_model_import_defaults(void) {
    LxModelImport i;
    memset(&i, 0, sizeof i);
    i.scale = 1.0f;
    i.import_materials = true;
    i.import_animations = true;
    i.smoothing_angle = 60.0f;
    i.rig = LX_RIG_GENERIC;
    return i;
}

bool lx_model_import_load(const char *model_path, LxModelImport *out) {
    char path[LX_PATH_MAX + 16];
    snprintf(path, sizeof path, "%s.lximport", model_path);
    *out = lx_model_import_defaults();
    char *text = lx_file_read(path, NULL);
    if (!text) return false;
    const char *cur = text;
    char k[96], v[256];
    int r;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r != 1) continue;
        if (!strcmp(k, "scale")) out->scale = (float)atof(v);
        else if (!strcmp(k, "import_materials")) out->import_materials = atoi(v) != 0;
        else if (!strcmp(k, "import_animations")) out->import_animations = atoi(v) != 0;
        else if (!strcmp(k, "calculate_normals")) out->calculate_normals = atoi(v) != 0;
        else if (!strcmp(k, "smoothing_angle")) out->smoothing_angle = (float)atof(v);
        else if (!strcmp(k, "rig")) out->rig = !strcmp(v, "humanoid") ? LX_RIG_HUMANOID : !strcmp(v, "none") ? LX_RIG_NONE : LX_RIG_GENERIC;
        else if (!strncmp(k, "avatar.", 7)) {
            for (int b = 0; b < LX_HB_COUNT; b++)
                if (!strcmp(k + 7, lx_human_bone_name((LxHumanBone)b))) lx_strcpy(out->avatar[b], sizeof out->avatar[b], v);
        } else if (!strncmp(k, "clip.", 5)) {
            int idx = atoi(k + 5);
            const char *f = strchr(k + 5, '.');
            if (idx < 0 || idx >= LX_MAX_CLIPS || !f) continue;
            if (idx >= out->clip_count) out->clip_count = idx + 1;
            if (!strcmp(f + 1, "name")) lx_strcpy(out->clips[idx].name, sizeof out->clips[idx].name, v);
            else if (!strcmp(f + 1, "loop")) out->clips[idx].loop = atoi(v) != 0;
        }
    }
    lx_free(text);
    return true;
}

bool lx_model_import_save(const char *model_path, const LxModelImport *imp) {
    char path[LX_PATH_MAX + 16];
    snprintf(path, sizeof path, "%s.lximport", model_path);
    LxStrBuf sb = {0};
    static const char *rigs[] = {"none", "generic", "humanoid"};
    lx_sb_printf(&sb, "# Luxora model import settings\nscale = %g\nimport_materials = %d\nimport_animations = %d\ncalculate_normals = %d\n"
                      "smoothing_angle = %g\nrig = %s\n",
                 imp->scale, imp->import_materials, imp->import_animations, imp->calculate_normals, imp->smoothing_angle,
                 rigs[imp->rig < 0 || imp->rig > 2 ? 1 : imp->rig]);
    for (int b = 0; b < LX_HB_COUNT; b++)
        if (imp->avatar[b][0]) lx_sb_printf(&sb, "avatar.%s = %s\n", lx_human_bone_name((LxHumanBone)b), imp->avatar[b]);
    for (int i = 0; i < imp->clip_count && i < LX_MAX_CLIPS; i++)
        lx_sb_printf(&sb, "clip.%d.name = %s\nclip.%d.loop = %d\n", i, imp->clips[i].name, i, imp->clips[i].loop);
    bool ok = lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
    return ok;
}

/* ======================================================================
 *  model registry and queries
 * ====================================================================== */
typedef struct ModelEntry { char ref[LX_REF_MAX]; LxModel *m; char err[200]; } ModelEntry;
static ModelEntry *g_models;
static int g_model_count, g_model_cap;
static uint32_t g_model_gen = 1;
uint32_t lx_model_generation(void) { return g_model_gen; }

LxModel *lx_asset_model(const char *ref) {
    if (!ref || !ref[0]) return NULL;
    char base[LX_REF_MAX];
    lx_strcpy(base, sizeof base, ref);
    char *hash = strchr(base, '#');
    if (hash) *hash = 0;
    if (!base[0]) return NULL;
    for (int i = 0; i < g_model_count; i++) if (!strcmp(g_models[i].ref, base)) return g_models[i].m;
    ModelEntry e;
    memset(&e, 0, sizeof e);
    lx_strcpy(e.ref, sizeof e.ref, base);
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, base);
    e.m = lx_model_load_file(path, base, e.err, sizeof e.err);
    if (!e.m) LX_ERROR("Could not import %s: %s", base, e.err);
    LX_VEC_PUSH(g_models, g_model_count, g_model_cap, e);
    return e.m;
}

const char *lx_model_error(const char *ref) {
    for (int i = 0; i < g_model_count; i++) if (!strcmp(g_models[i].ref, ref)) return g_models[i].err;
    return "";
}

void lx_model_reload(const char *ref) {
    for (int i = 0; i < g_model_count; i++)
        if (!strcmp(g_models[i].ref, ref)) {
            lx_renderer_wait_idle();
            lx_model_free(g_models[i].m);
            g_models[i] = g_models[--g_model_count];
            g_model_gen++;
            return;
        }
}

void lx_models_clear(void) {
    for (int i = 0; i < g_model_count; i++) lx_model_free(g_models[i].m);
    g_model_count = 0;
    g_model_gen++;
}

LxMesh *lx_model_part_mesh(LxModel *m, int part) { return m && part >= 0 && part < m->part_count ? m->parts[part].mesh : NULL; }

/* "Models/Hero.fbx#mesh3" */
LxMesh *lx_model_mesh_ref(const char *ref) {
    const char *h = strchr(ref, '#');
    if (!h || strncmp(h + 1, "mesh", 4)) return NULL;
    LxModel *m = lx_asset_model(ref);
    return lx_model_part_mesh(m, atoi(h + 5));
}

int lx_model_find_node(const LxModel *m, const char *name) {
    if (!m || !name || !name[0]) return -1;
    for (int i = 0; i < m->node_count; i++) if (!strcmp(m->nodes[i].name, name)) return i;
    return -1;
}

int lx_model_bone_count(const LxModel *m) { return m ? m->bone_count : 0; }
const char *lx_model_bone_name(const LxModel *m, int i) { return m && i >= 0 && i < m->bone_count ? m->nodes[m->bones[i]].name : ""; }
int lx_model_bone_parent(const LxModel *m, int i) {
    if (!m || i < 0 || i >= m->bone_count) return -1;
    int p = m->nodes[m->bones[i]].parent;
    for (int k = 0; k < m->bone_count; k++) if (m->bones[k] == p) return k;
    return -1;
}
int lx_model_node_count(const LxModel *m) { return m ? m->node_count : 0; }
const char *lx_model_node_name(const LxModel *m, int i) { return m && i >= 0 && i < m->node_count ? m->nodes[i].name : ""; }
int lx_model_node_parent(const LxModel *m, int i) { return m && i >= 0 && i < m->node_count ? m->nodes[i].parent : -1; }
bool lx_model_node_is_bone(const LxModel *m, int i) { return m && i >= 0 && i < m->node_count && m->nodes[i].bone; }
int lx_model_mesh_count(const LxModel *m) { return m ? m->part_count : 0; }
int lx_model_clip_count(const LxModel *m) { return m ? m->clip_count : 0; }
const char *lx_model_clip_name(const LxModel *m, int i) { return m && i >= 0 && i < m->clip_count ? m->clips[i].name : ""; }
float lx_model_clip_length(const LxModel *m, int i) { return m && i >= 0 && i < m->clip_count ? m->clips[i].length : 0; }
const LxModelImport *lx_model_settings(const LxModel *m) { return m ? &m->imp : NULL; }
LxRigType lx_model_rig(const LxModel *m) { return m ? m->imp.rig : LX_RIG_NONE; }
LxAabb lx_model_bounds(const LxModel *m) { return m ? m->bounds : lx_aabb_empty(); }
int lx_model_avatar_bone(const LxModel *m, LxHumanBone b) {
    if (!m || b < 0 || b >= LX_HB_COUNT || m->avatar[b] < 0) return -1;
    for (int k = 0; k < m->bone_count; k++) if (m->bones[k] == m->avatar[b]) return k;
    return -1;
}

/* ======================================================================
 *  placing a model in a scene
 * ====================================================================== */
LxEntity lx_model_instantiate(LxScene *s, const char *ref, LxVec3 position, LxEntity parent) {
    LxModel *m = lx_asset_model(ref);
    if (!m) return 0;
    char name[LX_NAME_MAX];
    lx_strcpy(name, sizeof name, lx_path_filename(ref));
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    LxEntity root = lx_entity_create(s, name);
    if (parent) lx_entity_set_parent(s, root, parent);
    lx_entity_set_position(s, root, position);
    LxEntity *ents = lx_alloc(sizeof(LxEntity) * (size_t)(m->node_count ? m->node_count : 1));
    for (int i = 0; i < m->node_count; i++) {
        ModelNode *nd = &m->nodes[i];
        LxEntity e = lx_entity_create(s, nd->name);
        lx_entity_set_parent(s, e, nd->parent >= 0 ? ents[nd->parent] : root);
        lx_entity_set_position(s, e, nd->pos);
        lx_entity_set_rotation(s, e, nd->rot);
        lx_entity_set_scale(s, e, nd->scale);
        ents[i] = e;
        for (int p = nd->part_first; p < nd->part_first + nd->part_count; p++) {
            /* one part sits on the node itself; more get a child each */
            LxEntity target = e;
            if (nd->part_count > 1) {
                target = lx_entity_create(s, m->parts[p].name);
                lx_entity_set_parent(s, target, e);
            }
            char mref[LX_REF_MAX];
            snprintf(mref, sizeof mref, "%s#mesh%d", m->ref, p);
            LxMeshRenderer *mr = lx_add_mesh(s, target, mref, m->parts[p].material);
            if (mr) mr->static_geometry = m->parts[p].mesh->skin == NULL && !m->clip_count;
        }
    }
    if (m->imp.rig != LX_RIG_NONE && (m->bone_count || m->clip_count)) {
        LxAnimator *a = lx_add_animator(s, root, m->ref);
        if (m->clip_count) snprintf(a->clip, sizeof a->clip, "%s#%s", m->ref, m->clips[0].name);
    }
    lx_free(ents);
    lx_scene_update_transforms(s);
    return root;
}
