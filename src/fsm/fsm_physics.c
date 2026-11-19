/* Physics: Rigidbody, Collider, Character Controller, collisions and triggers, and the query actions
 * (raycasts, sphere casts, overlaps).
 *
 * Luxora's physics is 3D only, so there is no 2D counterpart to any of this. Colliders carry their own
 * friction and bounciness rather than pointing at a separate material asset, so those settings live on
 * Set Collider. */
#include "fsm_actions.h"

#define FSM_FORCE_MODE "Force|Impulse|Velocity Change|Acceleration"

/* ==========================================================================================
 *  Rigidbody
 * ========================================================================================== */
static void rb_add_force(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxRigidbody *rb = lx_get_rigidbody(c->scene, e);
    if (!rb) return;
    LxVec3 f = lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 2) == 0) f = lx_entity_transform_direction(c->scene, e, f);   /* Self: along its own facing */
    float mass = lx_maxf(0.0001f, rb->mass);
    switch (lx_fsm_int(c, 3)) {
    case 0: lx_rigidbody_add_force(c->scene, e, f); break;                       /* Force: newtons */
    case 1: lx_rigidbody_add_impulse(c->scene, e, f); break;                     /* Impulse: newton-seconds */
    case 2: lx_rigidbody_add_impulse(c->scene, e, lx_scale3(f, mass)); break;    /* Velocity Change: mass cancels out */
    default: lx_rigidbody_add_force(c->scene, e, lx_scale3(f, mass)); break;     /* Acceleration: mass cancels out */
    }
}
static const LxFsmActionDesc A_RB_ADD_FORCE = {"Add Force", "Rigidbody", "Pushes a physics body. Impulse is an instant shove (jumps, explosions); Force builds up over time.",
    "push {0} by {1}",
    {POBJ("Object"), P("Force", LX_FSM_VEC3, "0 5 0"), PENUMT("Space", FSM_SPACE, "1", "Self pushes along the object's own facing"),
     PENUMT("Mode", FSM_FORCE_MODE, "1", "Velocity Change and Acceleration ignore the body's mass")},
    0, 0, LX_FSM_FIXED, rb_add_force, rb_add_force, NULL};

static void rb_add_force_at(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    lx_rigidbody_add_impulse_at(c->scene, e, lx_fsm_vec3(c, 1), lx_fsm_vec3(c, 2));
}
static const LxFsmActionDesc A_RB_ADD_FORCE_AT = {"Add Force At Position", "Rigidbody", "Shoves a body at a particular point, so it spins as well as moves.",
    "push {0} at {2}",
    {POBJ("Object"), P("Impulse", LX_FSM_VEC3, "0 5 0"), PT("World Point", LX_FSM_VEC3, "0 0 0", "where on the body the push lands")},
    0, 0, LX_FSM_FIXED, rb_add_force_at, rb_add_force_at, NULL};

static void rb_add_torque(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 t = lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 2) == 0) t = lx_entity_transform_direction(c->scene, e, t);
    lx_rigidbody_add_torque(c->scene, e, t);
}
static const LxFsmActionDesc A_RB_ADD_TORQUE = {"Add Torque", "Rigidbody", "Spins a physics body about an axis.", "spin {0} by {1}",
    {POBJ("Object"), P("Torque", LX_FSM_VEC3, "0 1 0"), PENUM("Space", FSM_SPACE, "1")},
    0, 0, LX_FSM_FIXED, rb_add_torque, rb_add_torque, NULL};

static void rb_explosion(LxFsmCtx *c) {
    LxEntity at = fsm_obj(c, 0);
    LxVec3 center = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 1);
    lx_physics_explosion(c->scene, center, lx_maxf(0, lx_fsm_float(c, 2)), lx_fsm_float(c, 3));
}
static const LxFsmActionDesc A_RB_EXPLOSION = {"Add Explosion Force", "Rigidbody", "Blows every nearby physics body away from a point.",
    "explosion at {0}, r={2}",
    {POBJNT("At Object", "None uses the position below"), P("Position", LX_FSM_VEC3, "0 0 0"),
     P("Radius", LX_FSM_FLOAT, "5"), P("Impulse", LX_FSM_FLOAT, "10")},
    0, 0, LX_FSM_ONCE, rb_explosion, rb_explosion, NULL};

static void rb_get_velocity(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 v = lx_rigidbody_velocity(c->scene, e);
    LxCharacterController *ch = lx_get_character(c->scene, e);
    if (ch && !lx_get_rigidbody(c->scene, e)) v = ch->velocity;   /* characters keep their own velocity */
    if (lx_fsm_int(c, 1) == 0) v = lx_entity_inverse_transform_direction(c->scene, e, v);
    lx_fsm_set_vec3(c, 2, v);
    lx_fsm_set_float(c, 3, lx_len3(v));
    LxVec3 flat = v;
    flat.y = 0;
    lx_fsm_set_float(c, 4, lx_len3(flat));
}
static const LxFsmActionDesc A_RB_GET_VELOCITY = {"Get Velocity", "Rigidbody", "How fast an object is moving. The flat speed ignores falling, which suits walk animations.",
    "velocity of {0} -> {2}",
    {POBJ("Object"), PENUM("Space", FSM_SPACE, "1"), POPT("Store Velocity", LX_FSM_VEC3),
     POPT("Store Speed", LX_FSM_FLOAT), POPTT("Store Flat Speed", LX_FSM_FLOAT, "speed ignoring up and down")},
    0, 0, LX_FSM_EVERY_FRAME, rb_get_velocity, rb_get_velocity, NULL};

static void rb_set_velocity(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 v = lx_rigidbody_velocity(c->scene, e);
    LxVec3 want = lx_fsm_is_none(c, 1) ? v : lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 5) == 0) want = lx_entity_transform_direction(c->scene, e, want);
    if (!lx_fsm_is_none(c, 2)) want.x = lx_fsm_float(c, 2);
    if (!lx_fsm_is_none(c, 3)) want.y = lx_fsm_float(c, 3);
    if (!lx_fsm_is_none(c, 4)) want.z = lx_fsm_float(c, 4);
    lx_rigidbody_set_velocity(c->scene, e, want);
}
static const LxFsmActionDesc A_RB_SET_VELOCITY = {"Set Velocity", "Rigidbody", "Sets how fast a body moves outright. Leave an axis as None to keep it — setting only Y is how you jump.",
    "{0}.velocity = {1}",
    {POBJ("Object"), PIN_OPT("Velocity", LX_FSM_VEC3), PIN_OPT("X", LX_FSM_FLOAT), PIN_OPT("Y", LX_FSM_FLOAT), PIN_OPT("Z", LX_FSM_FLOAT),
     PENUM("Space", FSM_SPACE, "1")},
    0, 0, LX_FSM_FIXED, rb_set_velocity, rb_set_velocity, NULL};

static void rb_set_properties(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxRigidbody *rb = lx_get_rigidbody(c->scene, e);
    if (!rb) return;
    if (!lx_fsm_is_none(c, 1)) rb->mass = lx_maxf(0.0001f, lx_fsm_float(c, 1));
    if (!lx_fsm_is_none(c, 2)) rb->linear_damping = lx_maxf(0, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) rb->angular_damping = lx_maxf(0, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) rb->use_gravity = lx_fsm_bool(c, 4);
    if (!lx_fsm_is_none(c, 5)) rb->is_kinematic = lx_fsm_bool(c, 5);
    if (!lx_fsm_is_none(c, 6)) rb->freeze_rotation = lx_fsm_bool(c, 6);
}
static const LxFsmActionDesc A_RB_SET_PROPS = {"Set Rigidbody Properties", "Rigidbody", "Changes a body's mass, drag, gravity and whether it is simulated. None leaves a setting alone.",
    "rigidbody of {0}",
    {POBJ("Object"), PIN_OPT("Mass", LX_FSM_FLOAT), PIN_OPT("Linear Damping", LX_FSM_FLOAT), PIN_OPT("Angular Damping", LX_FSM_FLOAT),
     PIN_OPT("Use Gravity", LX_FSM_BOOL), PT("Is Kinematic", LX_FSM_BOOL, "", "moved by your code instead of by physics"),
     PT("Freeze Rotation", LX_FSM_BOOL, "", "never tips over")},
    0, 0, LX_FSM_ONCE, rb_set_properties, rb_set_properties, NULL};

static void rb_get_properties(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxRigidbody *rb = e ? lx_get_rigidbody(c->scene, e) : NULL;
    if (!rb) { lx_fsm_set_bool(c, 5, false); return; }
    lx_fsm_set_float(c, 1, rb->mass);
    lx_fsm_set_bool(c, 2, rb->use_gravity);
    lx_fsm_set_bool(c, 3, rb->is_kinematic);
    lx_fsm_set_vec3(c, 4, rb->angular_velocity);
    lx_fsm_set_bool(c, 5, true);
    lx_fsm_set_bool(c, 6, lx_rigidbody_sleeping(c->scene, e));
}
static const LxFsmActionDesc A_RB_GET_PROPS = {"Get Rigidbody Properties", "Rigidbody", "Reads a body's mass, gravity, kinematic flag, spin and whether it has gone to sleep.",
    "rigidbody of {0}",
    {POBJ("Object"), POPT("Store Mass", LX_FSM_FLOAT), POPT("Store Use Gravity", LX_FSM_BOOL),
     POPT("Store Is Kinematic", LX_FSM_BOOL), POPT("Store Angular Velocity", LX_FSM_VEC3),
     POPT("Store Has Rigidbody", LX_FSM_BOOL), POPT("Store Is Sleeping", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, rb_get_properties, rb_get_properties, NULL};

static void rb_wake(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_rigidbody_wake(c->scene, e);
}
static const LxFsmActionDesc A_RB_WAKE = {"Wake Rigidbody", "Rigidbody", "Wakes a body that physics has put to sleep, so it reacts again.", "wake {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, rb_wake, rb_wake, NULL};

/* ==========================================================================================
 *  Collider
 * ========================================================================================== */
static void col_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxCollider *col = e ? lx_get_collider(c->scene, e) : NULL;
    if (!col) return;
    if (!lx_fsm_is_none(c, 1)) col->is_trigger = lx_fsm_bool(c, 1);
    if (!lx_fsm_is_none(c, 2)) col->friction = lx_saturate(lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) col->bounciness = lx_saturate(lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) col->center = lx_fsm_vec3(c, 4);
    if (!lx_fsm_is_none(c, 5)) col->size = lx_fsm_vec3(c, 5);
    if (!lx_fsm_is_none(c, 6)) col->radius = lx_maxf(0.0001f, lx_fsm_float(c, 6));
    if (!lx_fsm_is_none(c, 7)) col->height = lx_maxf(0.0001f, lx_fsm_float(c, 7));
    if (!lx_fsm_is_none(c, 8)) lx_entity_set_layer(c->scene, e, lx_fsm_int(c, 8) & 31);
}
static const LxFsmActionDesc A_COL_SET = {"Set Collider", "Collider", "Changes a collider: trigger or solid, how slippery and bouncy it is, its shape and its layer. None leaves a setting alone.",
    "collider of {0}",
    {POBJ("Object"), PT("Is Trigger", LX_FSM_BOOL, "", "triggers report overlaps instead of blocking"),
     PT("Friction", LX_FSM_FLOAT, "", "0 is ice, 1 is rubber"), PT("Bounciness", LX_FSM_FLOAT, "", "0 is dead, 1 is a superball"),
     PIN_OPT("Center", LX_FSM_VEC3), PT("Size", LX_FSM_VEC3, "", "box colliders: the full size"),
     PT("Radius", LX_FSM_FLOAT, "", "sphere and capsule colliders"), PT("Height", LX_FSM_FLOAT, "", "capsule colliders"),
     PT("Layer", LX_FSM_INT, "", "0..31; which layers collide is set in Physics Settings")},
    0, 0, LX_FSM_ONCE, col_set, col_set, NULL};

static void col_get(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxCollider *col = e ? lx_get_collider(c->scene, e) : NULL;
    lx_fsm_set_bool(c, 5, col != NULL);
    if (!col) return;
    lx_fsm_set_bool(c, 1, col->is_trigger);
    lx_fsm_set_vec3(c, 2, col->center);
    lx_fsm_set_vec3(c, 3, col->size);
    lx_fsm_set_float(c, 4, col->radius);
    lx_fsm_set_int(c, 6, lx_entity_layer(c->scene, e));
}
static const LxFsmActionDesc A_COL_GET = {"Get Collider", "Collider", "Reads a collider's trigger flag, shape and layer.", "collider of {0}",
    {POBJ("Object"), POPT("Store Is Trigger", LX_FSM_BOOL), POPT("Store Center", LX_FSM_VEC3), POPT("Store Size", LX_FSM_VEC3),
     POPT("Store Radius", LX_FSM_FLOAT), POPT("Store Has Collider", LX_FSM_BOOL), POPT("Store Layer", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, col_get, col_get, NULL};

/* ==========================================================================================
 *  collisions and triggers
 *
 *  The engine posts COLLISION_ENTER / COLLISION_EXIT / TRIGGER_ENTER / TRIGGER_EXIT into a small ring
 *  on the run. These actions read whatever is new since the state was entered, filter it by tag, and
 *  send an event of your own — so one state can react to "an Enemy touched me" and ignore everything else.
 * ========================================================================================== */
typedef struct { uint32_t seen; } FsmSysSeen;
static void fsm_sys_enter(LxFsmCtx *c) {
    FsmSysSeen *s = c->state;
    if (c->run) s->seen = c->run->system_seq;   /* only what happens from now on counts */
}
static void fsm_sys_poll(LxFsmCtx *c, const char *enter_ev, const char *exit_ev) {
    LxFsmRun *r = c->run;
    FsmSysSeen *s = c->state;
    if (!r || r->system_seq == s->seen) return;
    /* the ring holds the last 8; if more arrived in one frame, start at the oldest we still have */
    uint32_t from = r->system_seq - s->seen > 8 ? r->system_seq - 7 : s->seen + 1;
    uint32_t to = r->system_seq;
    s->seen = to;
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
    int want = lx_fsm_int(c, 0);
    for (uint32_t q = from; q - from <= to - from; q++) {
        const char *evn = r->system_ring[q & 7].event;
        LxEntity o = r->system_ring[q & 7].other;
        bool entered = !strcmp(evn, enter_ev), exited = !strcmp(evn, exit_ev);
        if (!(want == 0 ? entered : want == 1 ? exited : (entered || exited))) continue;
        if (tag[0] && (!o || !lx_entity_valid(c->scene, o) || !lx_entity_has_tag(c->scene, o, tag))) continue;
        lx_fsm_set_object(c, 3, o);
        lx_fsm_set_bool(c, 4, entered);
        fsm_send(c, 2);
        return;   /* one per frame: the graph gets to move on before the next */
    }
}
static void col_event(LxFsmCtx *c) { fsm_sys_poll(c, "COLLISION_ENTER", "COLLISION_EXIT"); }
static void trig_event(LxFsmCtx *c) { fsm_sys_poll(c, "TRIGGER_ENTER", "TRIGGER_EXIT"); }

static const LxFsmActionDesc A_COLLISION_EVENT = {"Collision Event", "Collision", "Sends an event when this object bumps into something, optionally only when it carries a tag.",
    "on collision {0} with \"{1}\" -> {2}",
    {PENUM("When", "Enter|Exit|Either", "0"), PT("Tag", LX_FSM_STRING, "", "empty = anything at all"),
     PEV("Send Event"), POPT("Store Other Object", LX_FSM_OBJECT), POPT("Store Was Enter", LX_FSM_BOOL)},
    sizeof(FsmSysSeen), 0, LX_FSM_EVERY_FRAME, fsm_sys_enter, col_event, NULL};

static const LxFsmActionDesc A_TRIGGER_EVENT = {"Trigger Event", "Collision", "Sends an event when something enters or leaves this trigger, optionally only when it carries a tag.",
    "on trigger {0} with \"{1}\" -> {2}",
    {PENUM("When", "Enter|Exit|Either", "0"), PT("Tag", LX_FSM_STRING, "Player", "empty = anything at all"),
     PEV("Send Event"), POPT("Store Other Object", LX_FSM_OBJECT), POPT("Store Was Enter", LX_FSM_BOOL)},
    sizeof(FsmSysSeen), 0, LX_FSM_EVERY_FRAME, fsm_sys_enter, trig_event, NULL};

static void col_info(LxFsmCtx *c) {
    LxEntity other = c->run ? c->run->last_other : 0;
    lx_fsm_set_object(c, 0, other);
    lx_fsm_set_float(c, 1, lx_fsm_event_value(c));     /* the engine sends the impulse as the event value */
    if (other && lx_entity_valid(c->scene, other)) {
        LxVec3 op = lx_entity_world_position(c->scene, other);
        LxVec3 me = lx_entity_world_position(c->scene, c->self);
        lx_fsm_set_vec3(c, 2, op);
        lx_fsm_set_vec3(c, 3, lx_norm3(lx_sub3(me, op)));
        lx_fsm_set_string(c, 4, lx_entity_tag(c->scene, other));
    }
}
static const LxFsmActionDesc A_COLLISION_INFO = {"Get Collision Info", "Collision", "Details of the latest collision or trigger: what was hit, how hard, and from which side.",
    "collision info",
    {POPT("Store Other Object", LX_FSM_OBJECT), POPTT("Store Impulse", LX_FSM_FLOAT, "how hard the hit was"),
     POPT("Store Other Position", LX_FSM_VEC3), POPTT("Store Normal", LX_FSM_VEC3, "points from the other object towards this one"),
     POPT("Store Other Tag", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, col_info, col_info, NULL};

/* ==========================================================================================
 *  queries
 * ========================================================================================== */
static void q_raycast(LxFsmCtx *c) {
    LxEntity from = fsm_obj(c, 0);
    LxVec3 origin = from ? lx_entity_world_position(c->scene, from) : lx_v3(0, 0, 0);
    LxVec3 offset = lx_fsm_vec3(c, 1);
    LxVec3 dir = lx_fsm_vec3(c, 2);
    if (lx_fsm_int(c, 3) == 0 && from) {
        offset = lx_entity_transform_direction(c->scene, from, offset);
        dir = lx_entity_transform_direction(c->scene, from, dir);
    }
    origin = lx_add3(origin, offset);
    LxRaycastHit hit = {0};
    bool got = lx_physics_raycast_ignore(c->scene, origin, lx_norm3(dir), lx_fsm_float(c, 4),
                                         lx_layer_mask(lx_fsm_string(c, 14)), lx_fsm_bool(c, 5) ? from : 0, &hit);
    if (got) {
        char tag[LX_NAME_MAX];
        lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 6));
        if (tag[0] && !lx_entity_has_tag(c->scene, hit.entity, tag)) got = false;
    }
    lx_fsm_set_bool(c, 7, got);
    lx_fsm_set_object(c, 8, got ? hit.entity : 0);
    lx_fsm_set_vec3(c, 9, got ? hit.point : lx_madd3(origin, lx_norm3(dir), lx_fsm_float(c, 4)));
    lx_fsm_set_vec3(c, 10, got ? hit.normal : lx_v3(0, 1, 0));
    lx_fsm_set_float(c, 11, got ? hit.distance : lx_fsm_float(c, 4));
    fsm_send(c, got ? 12 : 13);
}
static const LxFsmActionDesc A_Q_RAYCAST = {"Raycast", "Physics Queries", "Fires an invisible ray and reports the first collider it meets: shooting, looking, ground checks.",
    "ray from {0} along {2}",
    {POBJT("From Object", "the ray starts here"), PT("Offset", LX_FSM_VEC3, "0 0 0", "moves the start point, e.g. up to eye height"),
     P("Direction", LX_FSM_VEC3, "0 0 1"), PENUMT("Space", FSM_SPACE, "0", "Self fires along the object's own facing"),
     P("Distance", LX_FSM_FLOAT, "50"), PT("Ignore Self", LX_FSM_BOOL, "true", "never hit the firing object or its children"),
     PT("Tag Filter", LX_FSM_STRING, "", "only count hits carrying this tag"),
     POPT("Store Did Hit", LX_FSM_BOOL), POPT("Store Hit Object", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3),
     POPT("Store Hit Normal", LX_FSM_VEC3), POPT("Store Distance", LX_FSM_FLOAT), PEV("Hit Event"), PEV("Miss Event"),
     PT("Layers", LX_FSM_STRING, "", "which layers to look at, by name: \"Ground, Props\". Empty looks at every layer")},
    0, 0, LX_FSM_EVERY_FRAME, q_raycast, q_raycast, NULL};

static void q_linecast(LxFsmCtx *c) {
    LxEntity a = fsm_obj(c, 0), b = fsm_obj(c, 1);
    LxVec3 pa = a ? lx_entity_world_position(c->scene, a) : lx_fsm_vec3(c, 2);
    LxVec3 pb = b ? lx_entity_world_position(c->scene, b) : lx_fsm_vec3(c, 3);
    LxVec3 d = lx_sub3(pb, pa);
    float len = lx_len3(d);
    LxRaycastHit hit = {0};
    bool blocked = len > 1e-6f && lx_physics_raycast_ignore(c->scene, pa, lx_scale3(d, 1.0f / len), len,
                                                            lx_layer_mask(lx_fsm_string(c, 9)), a, &hit);
    lx_fsm_set_bool(c, 4, !blocked);
    lx_fsm_set_object(c, 5, blocked ? hit.entity : 0);
    lx_fsm_set_vec3(c, 6, blocked ? hit.point : pb);
    fsm_send(c, blocked ? 8 : 7);
}
static const LxFsmActionDesc A_Q_LINECAST = {"Line Of Sight", "Physics Queries", "Is there a clear line between two points? The usual way to ask whether an enemy can see you.",
    "{0} sees {1}?",
    {POBJT("From Object", "the eye"), POBJNT("To Object", "None uses the position below"),
     P("From Position", LX_FSM_VEC3, "0 0 0"), P("To Position", LX_FSM_VEC3, "0 0 0"),
     POPT("Store Is Clear", LX_FSM_BOOL), POPT("Store Blocker", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3),
     PEV("Clear Event"), PEV("Blocked Event"),
     PT("Layers", LX_FSM_STRING, "", "which layers to look at, by name: \"Ground, Props\". Empty looks at every layer")},
    0, 0, LX_FSM_EVERY_FRAME, q_linecast, q_linecast, NULL};

static void q_sphere_cast(LxFsmCtx *c) {
    LxEntity from = fsm_obj(c, 0);
    LxVec3 origin = from ? lx_entity_world_position(c->scene, from) : lx_v3(0, 0, 0);
    origin = lx_add3(origin, lx_fsm_vec3(c, 1));
    LxVec3 dir = lx_norm3(lx_fsm_vec3(c, 2));
    LxRaycastHit hit = {0};
    bool got = lx_physics_sphere_cast_ignore(c->scene, origin, lx_maxf(0.001f, lx_fsm_float(c, 3)), dir, lx_fsm_float(c, 4),
                                             lx_layer_mask(lx_fsm_string(c, 11)), fsm_obj(c, 12), &hit);
    lx_fsm_set_bool(c, 5, got);
    lx_fsm_set_object(c, 6, got ? hit.entity : 0);
    lx_fsm_set_vec3(c, 7, got ? hit.point : lx_v3(0, 0, 0));
    lx_fsm_set_float(c, 8, got ? hit.distance : lx_fsm_float(c, 4));
    fsm_send(c, got ? 9 : 10);
}
static const LxFsmActionDesc A_Q_SPHERE_CAST = {"Sphere Cast", "Physics Queries", "Sweeps a ball along a line and reports what it meets: a fat ray that will not slip through gaps.",
    "sphere cast from {0}",
    {POBJ("From Object"), P("Offset", LX_FSM_VEC3, "0 0 0"), P("Direction", LX_FSM_VEC3, "0 0 1"),
     P("Radius", LX_FSM_FLOAT, "0.5"), P("Distance", LX_FSM_FLOAT, "10"),
     POPT("Store Did Hit", LX_FSM_BOOL), POPT("Store Hit Object", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3),
     POPT("Store Distance", LX_FSM_FLOAT), PEV("Hit Event"), PEV("Miss Event"),
     PT("Layers", LX_FSM_STRING, "", "which layers to look at, by name: \"Ground, Props\". Empty looks at every layer"),
     POBJNT("Ignore Object", "skipped with everything under it: set it to the player for a camera's wall check")},
    0, 0, LX_FSM_EVERY_FRAME, q_sphere_cast, q_sphere_cast, NULL};

static void q_overlap_sphere(LxFsmCtx *c) {
    LxEntity at = fsm_obj(c, 0);
    LxVec3 center = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 1);
    if (at && !lx_fsm_is_none(c, 1)) center = lx_add3(center, lx_fsm_vec3(c, 1));
    LxEntity found[256];
    int n = lx_physics_overlap_sphere(c->scene, center, lx_maxf(0, lx_fsm_float(c, 2)),
                                      lx_layer_mask(lx_fsm_string(c, 10)), found, 256);
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 3));
    bool skip_self = lx_fsm_bool(c, 4);
    LxArray list = lx_fsm_array(c, 5);
    if (lx_array_valid(list)) lx_array_clear(list);
    int kept = 0;
    LxEntity closest = 0;
    float best = 1e30f;
    for (int i = 0; i < n; i++) {
        LxEntity e = found[i];
        if (skip_self && (e == c->self)) continue;
        if (tag[0] && !lx_entity_has_tag(c->scene, e, tag)) continue;
        if (lx_array_valid(list)) lx_array_add_object(list, e);
        float d = lx_len3(lx_sub3(lx_entity_world_position(c->scene, e), center));
        if (d < best) { best = d; closest = e; }
        kept++;
    }
    lx_fsm_set_int(c, 6, kept);
    lx_fsm_set_object(c, 7, closest);
    fsm_send(c, kept > 0 ? 8 : 9);
}
static const LxFsmActionDesc A_Q_OVERLAP_SPHERE = {"Overlap Sphere", "Physics Queries", "Everything inside a ball: blast radius, \"what is near me\", pickup range.",
    "overlap r={2} at {0}",
    {POBJT("At Object", "the centre"), PT("Offset", LX_FSM_VEC3, "0 0 0", "moves the centre"),
     P("Radius", LX_FSM_FLOAT, "5"), PT("Tag Filter", LX_FSM_STRING, "", "empty = anything"),
     PT("Ignore Self", LX_FSM_BOOL, "true", "leave this object out"),
     PARRT("Store List", "an Object array"), POPT("Store Count", LX_FSM_INT), POPT("Store Closest", LX_FSM_OBJECT),
     PEV("Found Event"), PEV("None Found Event"),
     PT("Layers", LX_FSM_STRING, "", "which layers to look at, by name: \"Ground, Props\". Empty looks at every layer")},
    0, 0, LX_FSM_EVERY_FRAME, q_overlap_sphere, q_overlap_sphere, NULL};

static void q_overlap_box(LxFsmCtx *c) {
    LxEntity at = fsm_obj(c, 0);
    LxVec3 center = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 1);
    LxQuat rot = at ? lx_entity_world_rotation(c->scene, at) : lx_quat_identity();
    LxEntity found[256];
    int n = lx_physics_overlap_box(c->scene, center, lx_scale3(lx_fsm_vec3(c, 2), 0.5f), rot,
                                   lx_layer_mask(lx_fsm_string(c, 8)), found, 256);
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 3));
    LxArray list = lx_fsm_array(c, 4);
    if (lx_array_valid(list)) lx_array_clear(list);
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (found[i] == c->self) continue;
        if (tag[0] && !lx_entity_has_tag(c->scene, found[i], tag)) continue;
        if (lx_array_valid(list)) lx_array_add_object(list, found[i]);
        kept++;
    }
    lx_fsm_set_int(c, 5, kept);
    fsm_send(c, kept > 0 ? 6 : 7);
}
static const LxFsmActionDesc A_Q_OVERLAP_BOX = {"Overlap Box", "Physics Queries", "Everything inside a box: trigger zones tested by hand, area selection.",
    "overlap box at {0}",
    {POBJT("At Object", "the centre; its rotation turns the box"), P("Position", LX_FSM_VEC3, "0 0 0"),
     P("Size", LX_FSM_VEC3, "2 2 2"), PT("Tag Filter", LX_FSM_STRING, "", "empty = anything"),
     PARRT("Store List", "an Object array"), POPT("Store Count", LX_FSM_INT), PEV("Found Event"), PEV("None Found Event"),
     PT("Layers", LX_FSM_STRING, "", "which layers to look at, by name: \"Ground, Props\". Empty looks at every layer")},
    0, 0, LX_FSM_EVERY_FRAME, q_overlap_box, q_overlap_box, NULL};

static void q_mouse_pick(LxFsmCtx *c) {
    LxRaycastHit hit = {0};
    bool got = lx_mouse_pick(c->scene, lx_fsm_float(c, 0), &hit);
    if (got) {
        char tag[LX_NAME_MAX];
        lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
        if (tag[0] && !lx_entity_has_tag(c->scene, hit.entity, tag)) got = false;
    }
    lx_fsm_set_bool(c, 2, got);
    lx_fsm_set_object(c, 3, got ? hit.entity : 0);
    lx_fsm_set_vec3(c, 4, got ? hit.point : lx_v3(0, 0, 0));
    lx_fsm_set_vec3(c, 5, got ? hit.normal : lx_v3(0, 1, 0));
    fsm_send(c, got ? 6 : 7);
}
static const LxFsmActionDesc A_Q_MOUSE_PICK = {"Mouse Pick", "Physics Queries", "What is under the mouse pointer (or the first finger): clicking on things in the world.",
    "pick under pointer",
    {P("Max Distance", LX_FSM_FLOAT, "100"), PT("Tag Filter", LX_FSM_STRING, "", "empty = anything"),
     POPT("Store Did Hit", LX_FSM_BOOL), POPT("Store Object", LX_FSM_OBJECT), POPT("Store Point", LX_FSM_VEC3),
     POPT("Store Normal", LX_FSM_VEC3), PEV("Hit Event"), PEV("Miss Event")},
    0, 0, LX_FSM_EVERY_FRAME, q_mouse_pick, q_mouse_pick, NULL};

/* ==========================================================================================
 *  Character Controller
 * ========================================================================================== */
typedef struct { float vy; } FsmCharMove;
static void ch_move(LxFsmCtx *c) {
    FsmCharMove *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 dir = lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 2) == 0) dir = lx_entity_transform_direction(c->scene, e, dir);
    dir.y = 0;
    float speed = lx_fsm_float(c, 3);
    if (!lx_get_character(c->scene, e)) {
        /* no Character Controller: still walk it, just without collisions or gravity, and say so once */
        static bool warned;
        if (!warned) {
            warned = true;
            LX_WARN("Character Move: '%s' has no Character Controller - moving it without collisions. Add Component > Character Controller.",
                    lx_entity_name(c->scene, e));
        }
        LxVec3 step = lx_scale3(lx_norm3(dir), speed * lx_minf(1.0f, lx_len3(dir)) * c->dt);
        lx_entity_set_world_pose(c->scene, e, lx_add3(lx_entity_world_position(c->scene, e), step),
                                 lx_entity_world_rotation(c->scene, e));
        lx_fsm_set_bool(c, 7, false);
        lx_fsm_set_float(c, 8, 0);
        return;
    }
    bool grounded = lx_character_grounded(c->scene, e);
    float gravity = lx_fsm_float(c, 4);
    if (grounded && s->vy < 0) s->vy = -1.0f;        /* a little downward push keeps it stuck to slopes */
    else s->vy -= gravity * c->dt;
    if (grounded && lx_fsm_bool(c, 5)) s->vy = lx_fsm_float(c, 6);   /* jump */
    LxVec3 motion = lx_scale3(lx_norm3(dir), speed * lx_minf(1.0f, lx_len3(dir)));
    motion.y = s->vy;
    int flags = lx_character_move(c->scene, e, lx_scale3(motion, c->dt));
    lx_fsm_set_bool(c, 7, (flags & LX_CHAR_GROUNDED) != 0);
    lx_fsm_set_float(c, 8, s->vy);
    if ((flags & LX_CHAR_GROUNDED) && !grounded) fsm_send(c, 9);
}
static const LxFsmActionDesc A_CH_MOVE = {"Character Move", "Character Controller", "Walks a Character Controller, applying gravity, sliding along walls and climbing steps. Jump while grounded to leap.",
    "move {0} by {1} at {3}",
    {POBJ("Object"), PT("Direction", LX_FSM_VEC3, "0 0 1", "which way to walk; its length scales the speed up to 1"),
     PENUMT("Space", FSM_SPACE, "1", "Self walks along the object's own facing"), P("Speed", LX_FSM_FLOAT, "5"),
     PT("Gravity", LX_FSM_FLOAT, "20", "how fast it falls"), PT("Jump", LX_FSM_BOOL, "false", "true for one frame to jump"),
     PT("Jump Speed", LX_FSM_FLOAT, "7", "how hard the jump is"),
     POPT("Store Grounded", LX_FSM_BOOL), POPT("Store Vertical Speed", LX_FSM_FLOAT), PEV("Landed Event")},
    sizeof(FsmCharMove), 0, LX_FSM_EVERY_FRAME, ch_move, ch_move, NULL};

static void ch_grounded(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    bool on = e && lx_character_grounded(c->scene, e);
    lx_fsm_set_bool(c, 1, on);
    fsm_send(c, on ? 2 : 3);
}
static const LxFsmActionDesc A_CH_GROUNDED = {"Is Grounded", "Character Controller", "True when a Character Controller is standing on walkable ground.",
    "{0} grounded?",
    {POBJ("Object"), POPT("Store Result", LX_FSM_BOOL), PEV("Grounded Event"), PEV("Airborne Event")},
    0, 0, LX_FSM_EVERY_FRAME, ch_grounded, ch_grounded, NULL};

static void ch_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxCharacterController *ch = e ? lx_get_character(c->scene, e) : NULL;
    if (!ch) return;
    if (!lx_fsm_is_none(c, 1)) ch->radius = lx_maxf(0.01f, lx_fsm_float(c, 1));
    if (!lx_fsm_is_none(c, 2)) ch->height = lx_maxf(0.02f, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) ch->step_height = lx_maxf(0, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) ch->slope_limit_deg = lx_clampf(lx_fsm_float(c, 4), 0, 89);
    if (!lx_fsm_is_none(c, 5)) ch->push_power = lx_maxf(0, lx_fsm_float(c, 5));
}
static const LxFsmActionDesc A_CH_SET = {"Set Character Properties", "Character Controller", "Changes a Character Controller's capsule, step height, slope limit and push strength. Shrinking the height is how you crouch.",
    "character of {0}",
    {POBJ("Object"), PIN_OPT("Radius", LX_FSM_FLOAT), PT("Height", LX_FSM_FLOAT, "", "shrink this to crouch"),
     PT("Step Height", LX_FSM_FLOAT, "", "the tallest ledge it can walk up"),
     PT("Slope Limit", LX_FSM_FLOAT, "", "degrees; steeper counts as a wall"),
     PT("Push Power", LX_FSM_FLOAT, "", "how hard it shoves physics bodies")},
    0, 0, LX_FSM_ONCE, ch_set, ch_set, NULL};

/* ==========================================================================================
 *  settings
 * ========================================================================================== */
static void ph_gravity(LxFsmCtx *c) {
    LxPhysicsSettings *p = lx_physics_settings();
    if (!p) return;
    if (lx_fsm_is_none(c, 0)) lx_fsm_set_vec3(c, 1, p->gravity);
    else p->gravity = lx_fsm_vec3(c, 0);
}
static const LxFsmActionDesc A_PH_GRAVITY = {"Physics Gravity", "Physics Queries", "Reads or changes the scene's gravity. Leave Gravity as None to read it instead of setting it.",
    "gravity {0}",
    {PT("Gravity", LX_FSM_VEC3, "", "None reads the current value instead"), POPT("Store Gravity", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, ph_gravity, ph_gravity, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_physics(void) {
    static const LxFsmActionDesc *all[] = {
        &A_RB_ADD_FORCE, &A_RB_ADD_FORCE_AT, &A_RB_ADD_TORQUE, &A_RB_EXPLOSION,
        &A_RB_GET_VELOCITY, &A_RB_SET_VELOCITY, &A_RB_SET_PROPS, &A_RB_GET_PROPS, &A_RB_WAKE,
        &A_COL_SET, &A_COL_GET,
        &A_COLLISION_EVENT, &A_TRIGGER_EVENT, &A_COLLISION_INFO,
        &A_Q_RAYCAST, &A_Q_LINECAST, &A_Q_SPHERE_CAST, &A_Q_OVERLAP_SPHERE, &A_Q_OVERLAP_BOX, &A_Q_MOUSE_PICK,
        &A_CH_MOVE, &A_CH_GROUNDED, &A_CH_SET,
        &A_PH_GRAVITY,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
