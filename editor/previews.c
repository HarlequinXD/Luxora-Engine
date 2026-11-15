/* Rendered thumbnails for assets that aren't pictures: models, meshes, materials, animation clips and prefabs.
 *
 * Each one is a tiny scene (the asset, a sun, a soft sky) drawn once into its own small render target, whose
 * texture the UI shows from then on. They are drawn after the editor's views, a couple per frame, in the
 * renderer's preview mode so they never take the frame's GI, lightmaps or reflection probes from the real
 * scene. Pictures (textures) use thumbs.c instead. */
#include "editor.h"
#include <stdio.h>

#define PREVIEW_MAX 96
#define PREVIEW_SIZE 128
#define PER_FRAME 2

typedef struct Preview {
    char kind[16];
    char ref[LX_REF_MAX];
    LxRenderTarget *rt;
    bool done, failed;
    uint64_t used;
} Preview;
static Preview g_prev[PREVIEW_MAX];
static uint64_t g_clock;

static Preview *find(const char *kind, const char *ref) {
    for (int i = 0; i < PREVIEW_MAX; i++)
        if (g_prev[i].ref[0] && !strcmp(g_prev[i].kind, kind) && !strcmp(g_prev[i].ref, ref)) return &g_prev[i];
    return NULL;
}

bool ed_preview_supported(const char *kind) {
    return !strcmp(kind, "model") || !strcmp(kind, "mesh") || !strcmp(kind, "material") || !strcmp(kind, "animation") || !strcmp(kind, "prefab");
}

uint32_t ed_preview_get(const char *kind, const char *ref) {
    if (!ref || !ref[0] || !ed_preview_supported(kind)) return 0;
    Preview *p = find(kind, ref);
    if (p) {
        p->used = ++g_clock;
        return p->done && !p->failed ? lx_render_target_texture(p->rt) : 0;
    }
    /* a free slot, or the least recently shown one */
    Preview *slot = NULL;
    for (int i = 0; i < PREVIEW_MAX && !slot; i++) if (!g_prev[i].ref[0]) slot = &g_prev[i];
    if (!slot) {
        slot = &g_prev[0];
        for (int i = 1; i < PREVIEW_MAX; i++) if (g_prev[i].used < slot->used) slot = &g_prev[i];
    }
    LxRenderTarget *keep = slot->rt;   /* the render target is reused by whatever goes in the slot */
    memset(slot, 0, sizeof *slot);
    slot->rt = keep;
    lx_strcpy(slot->kind, sizeof slot->kind, kind);
    lx_strcpy(slot->ref, sizeof slot->ref, ref);
    slot->used = ++g_clock;
    return 0;
}

bool ed_preview_failed(const char *kind, const char *ref) {
    Preview *p = find(kind, ref);
    return p && p->failed;
}

/* an asset changed on disk: draw it again */
void ed_previews_forget(const char *ref) {
    for (int i = 0; i < PREVIEW_MAX; i++)
        if (g_prev[i].ref[0] && (!ref || strstr(g_prev[i].ref, ref))) g_prev[i].done = g_prev[i].failed = false;
}

/* everything with a mesh in the little scene, in world space */
static bool scene_bounds(LxScene *s, LxAabb *out) {
    bool any = false;
    LxVec3 lo = lx_v3(1e30f, 1e30f, 1e30f), hi = lx_v3(-1e30f, -1e30f, -1e30f);
    lx_scene_update_transforms(s);
    for (int i = 0; i < lx_scene_entity_count(s); i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        LxEntityData *d = lx_entity_data(s, e);
        if (!d || !(d->comps & LX_COMP_MESH)) continue;
        const LxMesh *m = lx_asset_mesh(d->mesh.mesh);
        if (!m) continue;
        for (int c = 0; c < 8; c++) {
            LxVec3 p = lx_v3(c & 1 ? m->bounds.max.x : m->bounds.min.x, c & 2 ? m->bounds.max.y : m->bounds.min.y, c & 4 ? m->bounds.max.z : m->bounds.min.z);
            p = lx_mat4_point(d->world, p);
            lo = lx_min3(lo, p);
            hi = lx_max3(hi, p);
        }
        any = true;
    }
    out->min = lo;
    out->max = hi;
    return any;
}

/* the asset on its own, lit by a sun and a soft grey sky */
static LxScene *build(const Preview *p) {
    LxScene *s = lx_scene_create("Preview");
    LxEnvironment *env = lx_scene_environment(s);
    env->sky_zenith = lx_v3(0.32f, 0.34f, 0.38f);
    env->sky_horizon = lx_v3(0.22f, 0.23f, 0.25f);
    env->ground_color = lx_v3(0.12f, 0.12f, 0.13f);
    env->ambient_intensity = 0.9f;
    env->fog_density = 0;
    LxEntity sun = lx_entity_create(s, "Sun");
    LxLight *l = lx_add_light(s, sun, LX_LIGHT_DIRECTIONAL);
    l->intensity = 2.6f;
    l->color = lx_v3(1, 0.97f, 0.92f);
    l->cast_shadows = false;
    l->contribute_gi = false;
    lx_entity_set_euler(s, sun, lx_v3(-40, 35, 0));
    if (!strcmp(p->kind, "material")) {
        LxEntity e = lx_spawn_primitive(s, LX_PRIM_SPHERE, "Ball", lx_v3(0, 0, 0));
        LxMeshRenderer *mr = lx_get_mesh(s, e);
        if (mr) lx_strcpy(mr->material, sizeof mr->material, p->ref);
    } else if (!strcmp(p->kind, "mesh")) {
        LxEntity e = lx_entity_create(s, "Mesh");
        lx_add_mesh(s, e, p->ref, "builtin:default");
    } else if (!strcmp(p->kind, "model")) {
        lx_model_instantiate(s, p->ref, lx_v3(0, 0, 0), 0);
    } else if (!strcmp(p->kind, "prefab")) {
        lx_prefab_instantiate(s, p->ref, lx_v3(0, 0, 0), 0);
    } else if (!strcmp(p->kind, "animation")) {
        /* the clip's own model, posed a third of the way into the clip */
        char model[LX_REF_MAX];
        lx_strcpy(model, sizeof model, p->ref);
        char *h = strchr(model, '#');
        if (h) *h = 0;
        LxEntity root = lx_model_instantiate(s, model, lx_v3(0, 0, 0), 0);
        LxModel *m = lx_asset_model(model);
        if (root && m && lx_get_animator(s, root)) {
            float len = 1;
            const char *name = h ? strchr(p->ref, '#') + 1 : "";
            for (int i = 0; i < lx_model_clip_count(m); i++)
                if (!strcmp(lx_model_clip_name(m, i), name)) len = lx_model_clip_length(m, i);
            lx_get_animator(s, root)->root_motion = false;
            lx_animator_play(s, root, p->ref, 0);
            lx_animator_update(s, root, len * 0.35f);
        }
    }
    return s;
}

static void render_one(Preview *p) {
    LxScene *s = build(p);
    LxAabb b;
    if (!scene_bounds(s, &b)) {   /* nothing to see (a clip without a mesh, a broken file): the tile keeps its icon */
        p->done = p->failed = true;
        lx_scene_destroy(s);
        return;
    }
    LxVec3 c = lx_scale3(lx_add3(b.min, b.max), 0.5f);
    float r = lx_maxf(0.5f * lx_len3(lx_sub3(b.max, b.min)), 0.01f);
    float fov = 30.0f * LX_DEG2RAD;
    float dist = r / sinf(fov * 0.5f) * 1.05f;
    LxVec3 dir = lx_norm3(lx_v3(0.55f, 0.38f, 1.0f));
    LxRenderView v;
    memset(&v, 0, sizeof v);
    v.eye = lx_add3(c, lx_scale3(dir, dist));
    v.view = lx_mat4_look_at(v.eye, c, lx_v3(0, 1, 0));
    v.near_clip = lx_maxf(dist - r * 2.0f, dist * 0.01f);
    v.far_clip = dist + r * 2.0f;
    v.proj = lx_mat4_perspective(fov, 1.0f, v.near_clip, v.far_clip);
    v.width = PREVIEW_SIZE;
    v.height = PREVIEW_SIZE;
    v.clear_color = lx_v4(0.14f, 0.15f, 0.17f, 1);
    if (!p->rt) p->rt = lx_render_target_create(PREVIEW_SIZE, PREVIEW_SIZE);
    lx_renderer_set_preview(true);
    lx_render_scene(s, &v, p->rt);
    lx_renderer_set_preview(false);
    lx_scene_destroy(s);
    p->done = true;
}

/* inside the frame, after the editor's own views */
void ed_previews_render(void) {
    /* the most recently asked for first: what's on screen now */
    for (int drawn = 0; drawn < PER_FRAME && lx_renderer_can_draw_view(); drawn++) {
        Preview *best = NULL;
        for (int i = 0; i < PREVIEW_MAX; i++)
            if (g_prev[i].ref[0] && !g_prev[i].done && (!best || g_prev[i].used > best->used)) best = &g_prev[i];
        if (!best) break;
        render_one(best);
    }
}

void ed_previews_shutdown(void) {
    for (int i = 0; i < PREVIEW_MAX; i++) {
        if (g_prev[i].rt) lx_render_target_destroy(g_prev[i].rt);
        memset(&g_prev[i], 0, sizeof g_prev[i]);
    }
}
