/* Behaviour registry, built-in behaviours and per-frame scene simulation. */
#include "../lx_internal.h"

#define LX_MAX_BEHAVIOURS 256
static LxBehaviourDesc g_behaviours[LX_MAX_BEHAVIOURS];
static int g_behaviour_count;

void lx_register_behaviour(const LxBehaviourDesc *desc) {
    for (int i = 0; i < g_behaviour_count; i++)
        if (!strcmp(g_behaviours[i].name, desc->name)) { g_behaviours[i] = *desc; return; }
    if (g_behaviour_count < LX_MAX_BEHAVIOURS) g_behaviours[g_behaviour_count++] = *desc;
}
void lx_behaviours_drop_module(const LxModule *m) {
    int n = 0;
    for (int i = 0; i < g_behaviour_count; i++) {
        const LxBehaviourDesc *d = &g_behaviours[i];
        const void *ptrs[] = {d->name, (const void *)(uintptr_t)d->on_start, (const void *)(uintptr_t)d->on_update,
                              (const void *)(uintptr_t)d->on_destroy, (const void *)(uintptr_t)d->on_fixed_update,
                              (const void *)(uintptr_t)d->on_late_update, (const void *)(uintptr_t)d->on_collision_enter,
                              (const void *)(uintptr_t)d->on_collision_exit, (const void *)(uintptr_t)d->on_trigger_enter,
                              (const void *)(uintptr_t)d->on_trigger_exit};
        bool inside = false;
        for (size_t k = 0; k < sizeof ptrs / sizeof ptrs[0]; k++) inside |= ptrs[k] && lx_module_contains(m, ptrs[k]);
        if (!inside) g_behaviours[n++] = *d;
    }
    g_behaviour_count = n;
}
int lx_behaviour_count(void) { return g_behaviour_count; }
const LxBehaviourDesc *lx_behaviour_at(int i) { return (i >= 0 && i < g_behaviour_count) ? &g_behaviours[i] : NULL; }
const LxBehaviourDesc *lx_behaviour_find(const char *name) {
    for (int i = 0; i < g_behaviour_count; i++)
        if (!strcmp(g_behaviours[i].name, name)) return &g_behaviours[i];
    return NULL;
}

/* ---------------- built-ins ---------------- */
static void rotator_update(LxBehaviourCtx *c, float dt) {
    LxVec3 e = lx_entity_euler(c->scene, c->self);
    e.x += c->params[0] * dt;
    e.y += c->params[1] * dt;
    e.z += c->params[2] * dt;
    lx_entity_set_euler(c->scene, c->self, e);
}

typedef struct BobState { LxVec3 origin; float t; } BobState;
static void bob_start(LxBehaviourCtx *c) { ((BobState *)c->state)->origin = lx_entity_position(c->scene, c->self); }
static void bob_update(LxBehaviourCtx *c, float dt) {
    BobState *s = c->state;
    s->t += dt;
    LxVec3 p = s->origin;
    p.y += sinf(s->t * c->params[1] * 2.0f * LX_PI) * c->params[0];
    lx_entity_set_position(c->scene, c->self, p);
}

typedef struct FlyState { float yaw, pitch; } FlyState;
static void fly_start(LxBehaviourCtx *c) {
    LxVec3 e = lx_entity_euler(c->scene, c->self);
    FlyState *s = c->state;
    s->yaw = e.y;
    s->pitch = e.x;
}
static void fly_update(LxBehaviourCtx *c, float dt) {
    /* driven by the project's Input Manager: MoveX/MoveY/LookX/LookY + Sprint (keyboard, gamepad and touch) */
    FlyState *s = c->state;
    float speed = c->params[0] * (lx_action_down("Sprint") ? 3.0f : 1.0f);
    float look = c->params[1] * 10.0f;
    bool mouse_look = lx_mouse_down(LX_MOUSE_RIGHT) || c->params[2] > 0.5f || lx_input_last_device() != LX_DEVICE_KEYBOARD_MOUSE;
    float lx = lx_axis("LookX"), ly = lx_axis("LookY");
    if (lx_input_last_device() == LX_DEVICE_KEYBOARD_MOUSE && !mouse_look) lx = ly = 0;
    /* mouse deltas are per frame; stick values are rates */
    float rate = lx_input_last_device() == LX_DEVICE_GAMEPAD ? dt * 60.0f : 1.0f;
    s->yaw -= lx * look * rate;
    s->pitch = lx_clampf(s->pitch + ly * look * rate, -89.0f, 89.0f);
    lx_entity_set_euler(c->scene, c->self, lx_v3(s->pitch, s->yaw, 0));
    LxQuat q = lx_entity_rotation(c->scene, c->self);
    LxVec3 fwd = lx_quat_rotate(q, lx_v3(0, 0, -1)), right = lx_quat_rotate(q, lx_v3(1, 0, 0));
    LxVec2 mv = lx_axis2("MoveX", "MoveY");
    float up = (lx_key_down(LX_KEY_E) ? 1.0f : 0.0f) - (lx_key_down(LX_KEY_Q) ? 1.0f : 0.0f);
    LxVec3 p = lx_entity_position(c->scene, c->self);
    p = lx_madd3(p, fwd, mv.y * speed * dt);
    p = lx_madd3(p, right, mv.x * speed * dt);
    p.y += up * speed * dt;
    lx_entity_set_position(c->scene, c->self, p);
}

typedef struct OrbitState { float angle; } OrbitState;
static void orbit_update(LxBehaviourCtx *c, float dt) {
    OrbitState *s = c->state;
    s->angle += c->params[1] * dt * LX_DEG2RAD;
    float r = c->params[0];
    LxVec3 p = lx_v3(sinf(s->angle) * r, c->params[2], cosf(s->angle) * r);
    lx_entity_set_position(c->scene, c->self, p);
    lx_entity_set_euler(c->scene, c->self, lx_v3(-atan2f(p.y, r) * LX_RAD2DEG, s->angle * LX_RAD2DEG, 0));
}

static void flicker_update(LxBehaviourCtx *c, float dt) {
    LxLight *l = lx_get_light(c->scene, c->self);
    float *t = c->state;
    *t += dt;
    if (l) l->intensity = c->params[0] * (0.85f + 0.15f * sinf(*t * 13.0f) * sinf(*t * 7.3f + 1.0f));
}

void lx_register_builtin_behaviours(void) {
    LxBehaviourDesc d;
    memset(&d, 0, sizeof d);
    d.name = "Rotator";
    d.param_names[0] = "Speed X"; d.param_names[1] = "Speed Y"; d.param_names[2] = "Speed Z";
    d.param_defaults[1] = 45.0f;
    d.on_update = rotator_update;
    lx_register_behaviour(&d);

    memset(&d, 0, sizeof d);
    d.name = "Bobbing";
    d.state_size = sizeof(BobState);
    d.param_names[0] = "Height"; d.param_names[1] = "Frequency";
    d.param_defaults[0] = 0.25f; d.param_defaults[1] = 0.5f;
    d.on_start = bob_start;
    d.on_update = bob_update;
    lx_register_behaviour(&d);

    memset(&d, 0, sizeof d);
    d.name = "FlyCamera";
    d.state_size = sizeof(FlyState);
    d.param_names[0] = "Move Speed"; d.param_names[1] = "Look Speed"; d.param_names[2] = "Always Look";
    d.param_defaults[0] = 6.0f; d.param_defaults[1] = 0.15f;
    d.on_start = fly_start;
    d.on_update = fly_update;
    lx_register_behaviour(&d);

    memset(&d, 0, sizeof d);
    d.name = "Orbit";
    d.state_size = sizeof(OrbitState);
    d.param_names[0] = "Radius"; d.param_names[1] = "Degrees/sec"; d.param_names[2] = "Height";
    d.param_defaults[0] = 8.0f; d.param_defaults[1] = 20.0f; d.param_defaults[2] = 3.0f;
    d.on_update = orbit_update;
    lx_register_behaviour(&d);

    memset(&d, 0, sizeof d);
    d.name = "LightFlicker";
    d.state_size = sizeof(float);
    d.param_names[0] = "Intensity";
    d.param_defaults[0] = 20.0f;
    d.on_update = flicker_update;
    lx_register_behaviour(&d);
}

/* ---------------- simulation ---------------- */
static void ensure_crowd(LxScene *s, LxEntityData *d) {
    LxCrowdComponent *cc = &d->crowd;
    if (d->crowd_sim && lx_crowd_count(d->crowd_sim) == cc->agent_count) return;
    if (d->crowd_sim) lx_crowd_destroy(d->crowd_sim);
    d->crowd_sim = lx_crowd_create(cc->agent_count);
    LxRng rng = lx_rng_seed(1234, (uint64_t)(d - s->entities));
    for (int i = 0; i < cc->agent_count; i++) {
        float a = lx_rng_f(&rng) * 2.0f * LX_PI, r = sqrtf(lx_rng_f(&rng)) * cc->area_radius;
        int id = lx_crowd_add(d->crowd_sim, lx_v3(cosf(a) * r, 0, sinf(a) * r), 0.3f, cc->agent_speed * (0.7f + 0.6f * lx_rng_f(&rng)));
        float ta = lx_rng_f(&rng) * 2.0f * LX_PI, tr = sqrtf(lx_rng_f(&rng)) * cc->area_radius;
        lx_crowd_set_target(d->crowd_sim, id, lx_v3(cosf(ta) * tr, 0, sinf(ta) * tr));
    }
}

/* ---- calling into behaviours from physics ---- */
static const LxBehaviourDesc *started_desc(LxScene *s, LxEntity e, LxBehaviourCtx *ctx) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !d->active || !(d->comps & LX_COMP_BEHAVIOUR) || !d->binst.desc) return NULL;
    ctx->scene = s;
    ctx->self = e;
    ctx->state = d->binst.state;
    ctx->params = d->behaviour.params;
    return d->binst.desc;
}
void lx_behaviour_collision_enter(LxScene *s, LxEntity self, const LxCollision *c) {
    LxBehaviourCtx ctx;
    const LxBehaviourDesc *d = started_desc(s, self, &ctx);
    if (d && d->on_collision_enter) d->on_collision_enter(&ctx, c);
}
void lx_behaviour_collision_exit(LxScene *s, LxEntity self, LxEntity other) {
    LxBehaviourCtx ctx;
    const LxBehaviourDesc *d = started_desc(s, self, &ctx);
    if (d && d->on_collision_exit) d->on_collision_exit(&ctx, other);
}
void lx_behaviour_trigger(LxScene *s, LxEntity self, LxEntity other, bool enter) {
    LxBehaviourCtx ctx;
    const LxBehaviourDesc *d = started_desc(s, self, &ctx);
    if (!d) return;
    if (enter && d->on_trigger_enter) d->on_trigger_enter(&ctx, other);
    if (!enter && d->on_trigger_exit) d->on_trigger_exit(&ctx, other);
}
void lx_behaviour_fixed_update(LxScene *s, float fixed_dt) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_BEHAVIOUR) || !d->binst.started || !d->binst.desc ||
            !d->binst.desc->on_fixed_update) continue;
        LxBehaviourCtx ctx = {s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), d->binst.state, d->behaviour.params};
        d->binst.desc->on_fixed_update(&ctx, fixed_dt);
    }
}

static void late_update(LxScene *s, float dt) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_BEHAVIOUR) || !d->binst.started || !d->binst.desc ||
            !d->binst.desc->on_late_update) continue;
        LxBehaviourCtx ctx = {s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), d->binst.state, d->behaviour.params};
        d->binst.desc->on_late_update(&ctx, dt);
    }
}

void lx_scene_simulate(LxScene *s, float dt) {
    s->simulating = true;
    s->last_dt = dt;
    int n = s->count;
    for (int i = 1; i < n; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active) continue;
        LxEntity e = lx_entity_make((uint32_t)i, d->generation & 0xfffu);
        if (d->comps & LX_COMP_BEHAVIOUR) {
            if (!d->binst.desc || strcmp(d->binst.desc->name, d->behaviour.name) != 0) {
                lx_free(d->binst.state);
                memset(&d->binst, 0, sizeof d->binst);
                d->binst.desc = lx_behaviour_find(d->behaviour.name);
                if (d->binst.desc && d->binst.desc->state_size) d->binst.state = lx_calloc(1, d->binst.desc->state_size);
            }
            const LxBehaviourDesc *desc = d->binst.desc;
            if (desc) {
                LxBehaviourCtx ctx = {s, e, d->binst.state, d->behaviour.params};
                if (!d->binst.started) {
                    d->binst.started = true;
                    if (desc->on_start) desc->on_start(&ctx);
                    d = &s->entities[i]; /* callbacks may create entities */
                    ctx.params = d->behaviour.params;
                }
                if (desc->on_update) desc->on_update(&ctx, dt);
                d = &s->entities[i];
            }
        }
        if (d->comps & LX_COMP_CROWD) {
            ensure_crowd(s, d);
            LxCrowd *c = d->crowd_sim;
            /* agents that arrive pick a new random target */
            LxRng rng = lx_rng_seed((uint64_t)(lx_time() * 1000.0), (uint64_t)i);
            const LxVec3 *pos, *vel;
            int count = lx_crowd_positions(c, &pos, &vel);
            for (int a = 0; a < count; a += 97) {
                int k = (a + (int)(lx_rng_u32(&rng) % 97u)) % count;
                if (lx_len3sq(vel[k]) < 0.01f) {
                    float ta = lx_rng_f(&rng) * 2.0f * LX_PI, tr = sqrtf(lx_rng_f(&rng)) * d->crowd.area_radius;
                    lx_crowd_set_target(c, k, lx_v3(cosf(ta) * tr, 0, sinf(ta) * tr));
                }
            }
            lx_crowd_update(c, dt);
        }
    }
    lx_scene_update_transforms(s);
    lx_physics_simulate(s, dt);
    late_update(s, dt);
    lx_scene_update_transforms(s);
}

void lx_scene_stop_simulation(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive) continue;
        if (d->binst.desc && d->binst.started && d->binst.desc->on_destroy) {
            LxBehaviourCtx ctx = {s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), d->binst.state, d->behaviour.params};
            d->binst.desc->on_destroy(&ctx);
        }
        lx_free(d->binst.state);
        memset(&d->binst, 0, sizeof d->binst);
        memset(&d->phys, 0, sizeof d->phys);
    }
    lx_physics_world_free(s);
    s->simulating = false;
}
