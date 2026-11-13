/* Scene graph: generational entity handles, hierarchy, transforms and components. */
#include "../lx_internal.h"

#define LX_INDEX_BITS 20u
#define LX_INDEX_MASK ((1u << LX_INDEX_BITS) - 1u)

LxEntity lx_entity_make(uint32_t index, uint32_t gen) { return (gen << LX_INDEX_BITS) | (index & LX_INDEX_MASK); }

LxEntityData *lx_entity_data(const LxScene *s, LxEntity e) {
    if (!s || !e) return NULL;
    uint32_t idx = e & LX_INDEX_MASK, gen = e >> LX_INDEX_BITS;
    if (idx == 0 || (int)idx >= s->count) return NULL;
    LxEntityData *d = &s->entities[idx];
    if (!d->alive || (d->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u)) != gen) return NULL;
    return d;
}

static LxEntity handle_of(const LxScene *s, const LxEntityData *d) {
    uint32_t idx = (uint32_t)(d - s->entities);
    return lx_entity_make(idx, d->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
}

static LxEnvironment default_env(void) {
    LxEnvironment e;
    e.sky_zenith = lx_v3(0.22f, 0.42f, 0.85f);
    e.sky_horizon = lx_v3(0.72f, 0.82f, 0.95f);
    e.ground_color = lx_v3(0.28f, 0.25f, 0.22f);
    e.ambient_intensity = 0.6f;
    e.exposure = 1.0f;
    e.fog_color = lx_v3(0.7f, 0.78f, 0.88f);
    e.fog_density = 0.0f;
    return e;
}

LxScene *lx_scene_create(const char *name) {
    LxScene *s = lx_calloc(1, sizeof *s);
    lx_strcpy(s->name, sizeof s->name, name ? name : "Untitled");
    s->capacity = 64;
    s->entities = lx_calloc((size_t)s->capacity, sizeof(LxEntityData));
    s->count = 1; /* slot 0 = null entity */
    s->env = default_env();
    s->static_version = 1;
    return s;
}

LxScene *lx_scene_create_default(const char *name) {
    LxScene *s = lx_scene_create(name);
    LxEntity cam = lx_entity_create(s, "Main Camera");
    lx_entity_set_position(s, cam, lx_v3(0, 1.6f, 6.0f));
    lx_entity_set_euler(s, cam, lx_v3(-8.0f, 0, 0));
    lx_add_camera(s, cam);
    lx_add_audio_listener(s, cam);

    LxEntity light = lx_entity_create(s, "Directional Light");
    lx_entity_set_position(s, light, lx_v3(0, 6, 0));
    lx_entity_set_euler(s, light, lx_v3(-50.0f, -30.0f, 0));
    lx_add_light(s, light, LX_LIGHT_DIRECTIONAL);

    LxEntity cube = lx_entity_create(s, "Cube");
    lx_entity_set_position(s, cube, lx_v3(0, 0.5f, 0));
    lx_add_mesh(s, cube, "builtin:cube", "builtin:default");
    return s;
}

void lx_scene_destroy(LxScene *s) {
    if (!s) return;
    lx_scene_stop_simulation(s);
    for (int i = 1; i < s->count; i++)
        if (s->entities[i].crowd_sim) lx_crowd_destroy(s->entities[i].crowd_sim);
    lx_free(s->entities);
    lx_free(s->free_slots);
    lx_free(s->order_cache);
    lx_free(s);
}

const char *lx_scene_name(const LxScene *s) { return s->name; }
void lx_scene_set_name(LxScene *s, const char *n) { lx_strcpy(s->name, sizeof s->name, n); }
LxEnvironment *lx_scene_environment(LxScene *s) { return &s->env; }

LxScene *lx_scene_clone(const LxScene *s) {
    char *text = lx_scene_serialize(s);
    LxScene *c = lx_scene_deserialize(text);
    lx_free(text);
    return c;
}

LxEntity lx_entity_create(LxScene *s, const char *name) {
    uint32_t idx;
    if (s->free_count) idx = s->free_slots[--s->free_count];
    else {
        if (s->count >= s->capacity) {
            int old = s->capacity;
            s->capacity *= 2;
            s->entities = lx_realloc(s->entities, sizeof(LxEntityData) * (size_t)s->capacity);
            memset(s->entities + old, 0, sizeof(LxEntityData) * (size_t)(s->capacity - old));
        }
        idx = (uint32_t)s->count++;
    }
    if (idx > LX_INDEX_MASK) { LX_ERROR("Entity limit reached"); return 0; }
    LxEntityData *d = &s->entities[idx];
    uint32_t gen = d->generation + 1;
    if ((gen & ((1u << (32 - LX_INDEX_BITS)) - 1u)) == 0) gen++;
    memset(d, 0, sizeof *d);
    d->generation = gen;
    d->alive = true;
    d->active = true;
    lx_strcpy(d->name, sizeof d->name, name ? name : "Entity");
    d->scale = lx_v3(1, 1, 1);
    d->rotation = lx_quat_identity();
    d->world = lx_mat4_identity();
    d->audio_voice = -1;
    d->order = s->next_order++;
    LxEntity h = handle_of(s, d);
    s->order_dirty = true;
    /* append to the root list */
    if (!s->first_root) s->first_root = h;
    else {
        LxEntity last = s->first_root;
        while (lx_entity_data(s, last)->next_sibling) last = lx_entity_data(s, last)->next_sibling;
        lx_entity_data(s, last)->next_sibling = h;
        d->prev_sibling = last;
    }
    return h;
}

static void unlink_from_parent(LxScene *s, LxEntityData *d) {
    if (d->prev_sibling) lx_entity_data(s, d->prev_sibling)->next_sibling = d->next_sibling;
    else if (d->parent) {
        LxEntityData *p = lx_entity_data(s, d->parent);
        if (p) p->first_child = d->next_sibling;
    } else if (s->first_root == handle_of(s, d)) s->first_root = d->next_sibling;
    if (d->next_sibling) lx_entity_data(s, d->next_sibling)->prev_sibling = d->prev_sibling;
    d->parent = d->prev_sibling = d->next_sibling = 0;
}

void lx_entity_destroy(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return;
    while (d->first_child) lx_entity_destroy(s, d->first_child);
    if (d->binst.desc && d->binst.desc->on_destroy && d->binst.started) {
        LxBehaviourCtx ctx = {s, e, d->binst.state, d->behaviour.params};
        d->binst.desc->on_destroy(&ctx);
    }
    lx_free(d->binst.state);
    if (d->audio_voice >= 0) lx_audio_stop(s, e);
    if (d->crowd_sim) lx_crowd_destroy(d->crowd_sim);
    if ((d->comps & LX_COMP_MESH) && d->mesh.static_geometry) s->static_version++;
    unlink_from_parent(s, d);
    d->alive = false;
    s->order_dirty = true;
    uint32_t idx = (uint32_t)(d - s->entities);
    LX_VEC_PUSH(s->free_slots, s->free_count, s->free_cap, idx);
}

bool lx_entity_valid(const LxScene *s, LxEntity e) { return lx_entity_data(s, e) != NULL; }

static void copy_components(LxEntityData *dst, const LxEntityData *src) {
    dst->active = src->active;
    dst->position = src->position;
    dst->euler = src->euler;
    dst->rotation = src->rotation;
    dst->scale = src->scale;
    dst->comps = src->comps;
    dst->camera = src->camera;
    dst->light = src->light;
    dst->mesh = src->mesh;
    dst->audio = src->audio;
    dst->behaviour = src->behaviour;
    dst->crowd = src->crowd;
}

static LxEntity duplicate_rec(LxScene *s, LxEntity e, LxEntity parent) {
    LxEntityData *src = lx_entity_data(s, e);
    if (!src) return 0;
    char name[LX_NAME_MAX];
    lx_strcpy(name, sizeof name, src->name);
    LxEntity c = lx_entity_create(s, name);
    src = lx_entity_data(s, e); /* storage may have moved */
    copy_components(lx_entity_data(s, c), src);
    if (parent) lx_entity_set_parent(s, c, parent);
    LxEntity kids[512];
    int n = 0;
    for (LxEntity ch = lx_entity_data(s, e)->first_child; ch && n < 512; ch = lx_entity_data(s, ch)->next_sibling) kids[n++] = ch;
    for (int i = 0; i < n; i++) duplicate_rec(s, kids[i], c);
    return c;
}

LxEntity lx_entity_duplicate(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return 0;
    LxEntity c = duplicate_rec(s, e, d->parent);
    if ((lx_entity_data(s, c)->comps & LX_COMP_MESH)) s->static_version++;
    return c;
}

LxEntity lx_entity_find(const LxScene *s, const char *name) {
    for (int i = 1; i < s->count; i++)
        if (s->entities[i].alive && strcmp(s->entities[i].name, name) == 0) return handle_of(s, &s->entities[i]);
    return 0;
}

const char *lx_entity_name(const LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d ? d->name : "";
}
void lx_entity_set_name(LxScene *s, LxEntity e, const char *n) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) lx_strcpy(d->name, sizeof d->name, n);
}
bool lx_entity_active(const LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && d->active;
}
void lx_entity_set_active(LxScene *s, LxEntity e, bool a) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d && d->active != a) {
        d->active = a;
        if (d->comps & LX_COMP_MESH) s->static_version++;
    }
}
uint32_t lx_entity_components(const LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d ? d->comps : 0;
}

static bool is_ancestor(const LxScene *s, LxEntity maybe_ancestor, LxEntity e) {
    for (LxEntity p = e; p; p = lx_entity_data(s, p)->parent)
        if (p == maybe_ancestor) return true;
    return false;
}

void lx_entity_set_parent(LxScene *s, LxEntity child, LxEntity parent) {
    LxEntityData *c = lx_entity_data(s, child);
    if (!c || child == parent) return;
    if (parent && (!lx_entity_data(s, parent) || is_ancestor(s, child, parent))) return;
    unlink_from_parent(s, c);
    c->parent = parent;
    LxEntityData *p = parent ? lx_entity_data(s, parent) : NULL;
    LxEntity *head = p ? &p->first_child : &s->first_root;
    /* append at the end to keep sibling order stable */
    if (!*head) *head = child;
    else {
        LxEntity last = *head;
        while (lx_entity_data(s, last)->next_sibling) last = lx_entity_data(s, last)->next_sibling;
        lx_entity_data(s, last)->next_sibling = child;
        c->prev_sibling = last;
    }
    s->static_version++;
}

LxEntity lx_entity_parent(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->parent : 0; }
LxEntity lx_entity_first_child(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->first_child : 0; }
LxEntity lx_entity_next_sibling(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->next_sibling : 0; }

int lx_scene_entity_count(const LxScene *s) {
    int n = 0;
    for (int i = 1; i < s->count; i++) n += s->entities[i].alive;
    return n;
}

static const LxScene *g_sort_scene;
static int order_cmp_q(const void *a, const void *b) {
    const LxEntityData *x = lx_entity_data(g_sort_scene, *(const LxEntity *)a);
    const LxEntityData *y = lx_entity_data(g_sort_scene, *(const LxEntity *)b);
    return x->order < y->order ? -1 : (x->order > y->order);
}

static void rebuild_order(LxScene *s) {
    s->order_count = 0;
    for (int i = 1; i < s->count; i++)
        if (s->entities[i].alive) {
            LxEntity h = handle_of(s, &s->entities[i]);
            LX_VEC_PUSH(s->order_cache, s->order_count, s->order_cap, h);
        }
    g_sort_scene = s;
    if (s->order_count > 1) qsort(s->order_cache, (size_t)s->order_count, sizeof(LxEntity), order_cmp_q);
    s->order_dirty = false;
}

LxEntity lx_scene_entity_at(const LxScene *cs, int index) {
    LxScene *s = (LxScene *)cs;
    if (s->order_dirty || (!s->order_cache && s->count > 1)) rebuild_order(s);
    return (index >= 0 && index < s->order_count) ? s->order_cache[index] : 0;
}

LxEntity lx_scene_root_first(const LxScene *s) { return s->first_root; }

/* ---------------- transforms ---------------- */
LxVec3 lx_entity_position(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->position : lx_v3(0, 0, 0); }
LxVec3 lx_entity_euler(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->euler : lx_v3(0, 0, 0); }
LxQuat lx_entity_rotation(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->rotation : lx_quat_identity(); }
LxVec3 lx_entity_scale(const LxScene *s, LxEntity e) { LxEntityData *d = lx_entity_data(s, e); return d ? d->scale : lx_v3(1, 1, 1); }

static void touch_static(LxScene *s, LxEntityData *d) {
    if ((d->comps & LX_COMP_MESH) && d->mesh.static_geometry && !s->simulating) s->static_version++;
}
void lx_entity_set_position(LxScene *s, LxEntity e, LxVec3 p) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) { d->position = p; touch_static(s, d); }
}
void lx_entity_set_euler(LxScene *s, LxEntity e, LxVec3 deg) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) { d->euler = deg; d->rotation = lx_quat_euler_deg(deg); touch_static(s, d); }
}
void lx_entity_set_scale(LxScene *s, LxEntity e, LxVec3 sc) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) { d->scale = sc; touch_static(s, d); }
}

static void update_rec(LxScene *s, LxEntityData *d, const LxMat4 *parent) {
    LxMat4 local = lx_mat4_trs(d->position, d->rotation, d->scale);
    d->world = parent ? lx_mat4_mul(*parent, local) : local;
    for (LxEntity c = d->first_child; c; ) {
        LxEntityData *cd = lx_entity_data(s, c);
        update_rec(s, cd, &d->world);
        c = cd->next_sibling;
    }
}

void lx_scene_update_transforms(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (d->alive && !d->parent) update_rec(s, d, NULL);
    }
}

LxMat4 lx_entity_world_matrix(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return lx_mat4_identity();
    LxMat4 m = lx_mat4_trs(d->position, d->rotation, d->scale);
    for (LxEntity p = d->parent; p; p = lx_entity_data(s, p)->parent) {
        LxEntityData *pd = lx_entity_data(s, p);
        m = lx_mat4_mul(lx_mat4_trs(pd->position, pd->rotation, pd->scale), m);
    }
    d->world = m;
    return m;
}
LxVec3 lx_entity_world_position(LxScene *s, LxEntity e) {
    LxMat4 m = lx_entity_world_matrix(s, e);
    return lx_v3(m.m[12], m.m[13], m.m[14]);
}
LxVec3 lx_entity_forward(LxScene *s, LxEntity e) {
    return lx_norm3(lx_mat4_dir(lx_entity_world_matrix(s, e), lx_v3(0, 0, -1)));
}
LxVec3 lx_entity_right(LxScene *s, LxEntity e) {
    return lx_norm3(lx_mat4_dir(lx_entity_world_matrix(s, e), lx_v3(1, 0, 0)));
}

/* ---------------- components ---------------- */
LxCamera *lx_add_camera(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    if (!(d->comps & LX_COMP_CAMERA)) {
        d->comps |= LX_COMP_CAMERA;
        d->camera.fov_deg = 60.0f;
        d->camera.near_clip = 0.1f;
        d->camera.far_clip = 1000.0f;
        d->camera.ortho_size = 5.0f;
        d->camera.clear_color = lx_v4(0.1f, 0.1f, 0.12f, 1);
        d->camera.priority = 0;
    }
    return &d->camera;
}

LxLight *lx_add_light(LxScene *s, LxEntity e, LxLightType type) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    d->comps |= LX_COMP_LIGHT;
    d->light.type = type;
    d->light.color = lx_v3(1.0f, 0.96f, 0.9f);
    d->light.intensity = type == LX_LIGHT_DIRECTIONAL ? 3.0f : 20.0f;
    d->light.range = 10.0f;
    d->light.spot_angle_deg = 45.0f;
    d->light.cast_shadows = type == LX_LIGHT_DIRECTIONAL;
    d->light.contribute_gi = true;
    s->static_version++;
    return &d->light;
}

LxMeshRenderer *lx_add_mesh(LxScene *s, LxEntity e, const char *mesh_ref, const char *material_ref) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    d->comps |= LX_COMP_MESH;
    lx_strcpy(d->mesh.mesh, sizeof d->mesh.mesh, mesh_ref ? mesh_ref : "builtin:cube");
    lx_strcpy(d->mesh.material, sizeof d->mesh.material, material_ref ? material_ref : "builtin:default");
    d->mesh.cast_shadows = true;
    d->mesh.static_geometry = true;
    s->static_version++;
    return &d->mesh;
}

LxAudioSource *lx_add_audio_source(LxScene *s, LxEntity e, const char *clip) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    d->comps |= LX_COMP_AUDIO_SOURCE;
    lx_strcpy(d->audio.clip, sizeof d->audio.clip, clip ? clip : "");
    d->audio.volume = 1.0f;
    d->audio.pitch = 1.0f;
    d->audio.loop = false;
    d->audio.play_on_start = true;
    d->audio.spatial = true;
    d->audio.min_distance = 1.0f;
    d->audio.max_distance = 60.0f;
    d->audio.bus = LX_BUS_SFX;
    return &d->audio;
}

void lx_add_audio_listener(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) d->comps |= LX_COMP_AUDIO_LISTENER;
}

LxBehaviourRef *lx_add_behaviour(LxScene *s, LxEntity e, const char *name) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    d->comps |= LX_COMP_BEHAVIOUR;
    lx_strcpy(d->behaviour.name, sizeof d->behaviour.name, name);
    const LxBehaviourDesc *desc = lx_behaviour_find(name);
    for (int i = 0; i < LX_BEHAVIOUR_PARAMS; i++) d->behaviour.params[i] = desc ? desc->param_defaults[i] : 0.0f;
    return &d->behaviour;
}

LxCrowdComponent *lx_add_crowd(LxScene *s, LxEntity e, int agents) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    d->comps |= LX_COMP_CROWD;
    d->crowd.agent_count = agents;
    d->crowd.area_radius = 40.0f;
    d->crowd.agent_speed = 1.6f;
    lx_strcpy(d->crowd.mesh, sizeof d->crowd.mesh, "builtin:capsule");
    lx_strcpy(d->crowd.material, sizeof d->crowd.material, "builtin:default");
    return &d->crowd;
}

#define GETTER(fn, type, bit, field)                                  \
    type *fn(LxScene *s, LxEntity e) {                                \
        LxEntityData *d = lx_entity_data(s, e);                       \
        return (d && (d->comps & (bit))) ? &d->field : NULL;          \
    }
GETTER(lx_get_camera, LxCamera, LX_COMP_CAMERA, camera)
GETTER(lx_get_light, LxLight, LX_COMP_LIGHT, light)
GETTER(lx_get_mesh, LxMeshRenderer, LX_COMP_MESH, mesh)
GETTER(lx_get_audio_source, LxAudioSource, LX_COMP_AUDIO_SOURCE, audio)
GETTER(lx_get_behaviour, LxBehaviourRef, LX_COMP_BEHAVIOUR, behaviour)
GETTER(lx_get_crowd, LxCrowdComponent, LX_COMP_CROWD, crowd)

void lx_remove_component(LxScene *s, LxEntity e, LxComponentBits c) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return;
    if ((c & (LX_COMP_MESH | LX_COMP_LIGHT)) && (d->comps & c)) s->static_version++;
    if ((c & LX_COMP_AUDIO_SOURCE) && d->audio_voice >= 0) lx_audio_stop(s, e);
    if ((c & LX_COMP_CROWD) && d->crowd_sim) { lx_crowd_destroy(d->crowd_sim); d->crowd_sim = NULL; }
    if (c & LX_COMP_BEHAVIOUR) { lx_free(d->binst.state); memset(&d->binst, 0, sizeof d->binst); }
    d->comps &= ~(uint32_t)c;
}

LxEntity lx_spawn_primitive(LxScene *s, LxPrimitive p, const char *name, LxVec3 pos) {
    static const char *names[] = {"Cube", "Sphere", "Plane", "Cylinder", "Capsule"};
    LxEntity e = lx_entity_create(s, name ? name : names[p]);
    lx_entity_set_position(s, e, pos);
    char ref[64];
    snprintf(ref, sizeof ref, "builtin:%s", lx_primitive_name(p));
    lx_add_mesh(s, e, ref, "builtin:default");
    return e;
}

LxEntity lx_scene_main_camera(const LxScene *s) {
    int best_pri = -100000;
    LxEntity best = 0;
    for (int i = 1; i < s->count; i++) {
        const LxEntityData *d = &s->entities[i];
        if (d->alive && d->active && (d->comps & LX_COMP_CAMERA) && d->camera.priority > best_pri) {
            best_pri = d->camera.priority;
            best = handle_of(s, d);
        }
    }
    return best;
}

LxEntity lx_scene_listener(const LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        const LxEntityData *d = &s->entities[i];
        if (d->alive && d->active && (d->comps & LX_COMP_AUDIO_LISTENER)) return handle_of(s, d);
    }
    return lx_scene_main_camera(s);
}

void lx_scene_mark_static_dirty(LxScene *s) { s->static_version++; }
uint32_t lx_scene_static_version(const LxScene *s) { return s->static_version; }

LxEntity lx_scene_raycast(LxScene *s, LxRay ray, float max_dist, float *out_dist) {
    lx_scene_update_transforms(s);
    LxEntity best = 0;
    float best_t = max_dist;
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_MESH)) continue;
        LxMesh *m = lx_asset_mesh(d->mesh.mesh);
        LxAabb wb = lx_aabb_transform(m->bounds, d->world);
        float tb = lx_ray_aabb(ray, wb);
        if (tb < 0 || tb > best_t) continue;
        /* exact test in object space */
        LxMat4 inv = lx_mat4_inverse(d->world);
        LxRay lr = {lx_mat4_point(inv, ray.origin), lx_mat4_dir(inv, ray.dir)};
        const LxMeshLod *l0 = &m->lods[0];
        for (int t = 0; t + 2 < l0->index_count; t += 3) {
            const LxVertex *a = &l0->vertices[l0->indices[t]], *b = &l0->vertices[l0->indices[t + 1]], *c = &l0->vertices[l0->indices[t + 2]];
            float lt = lx_ray_triangle(lr, lx_v3(a->px, a->py, a->pz), lx_v3(b->px, b->py, b->pz), lx_v3(c->px, c->py, c->pz), NULL, NULL);
            if (lt > 0) {
                LxVec3 hit = lx_mat4_point(d->world, lx_add3(lr.origin, lx_scale3(lr.dir, lt)));
                float wt = lx_len3(lx_sub3(hit, ray.origin));
                if (wt < best_t) { best_t = wt; best = handle_of(s, d); }
            }
        }
    }
    if (out_dist) *out_dist = best_t;
    return best;
}
