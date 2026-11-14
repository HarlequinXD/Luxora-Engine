/* Built-in State Graph actions, part two: switches and change events, more maths and text, smooth
 * following, objects by tag, physics queries, camera, audio, lights, game UI and flow helpers. */
#include "../lx_internal.h"
#include <stdio.h>
#include <ctype.h>

#define P(n, t, d) {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip) {n, t, d, 0, NULL, tip}
#define POUT(n, t) {n, t, "", LX_FSM_P_OUT, NULL, NULL}
#define POPT(n, t) {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n) {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PENUM(n, opts, d) {n, LX_FSM_ENUM, d, 0, opts, NULL}
#define PASSET(n, kind) {n, LX_FSM_ASSET, "", 0, kind, NULL}
#define POBJ(n) {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJN(n) {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}
#define ANCHORS "Top Left|Top|Top Right|Left|Center|Right|Bottom Left|Bottom|Bottom Right"

static void ev(LxFsmCtx *c, int p) { lx_fsm_send(c, lx_fsm_event(c, p)); }
static LxEntity obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }
static uint32_t rgba(LxVec4 c) {
    return ((uint32_t)(lx_clampf(c.x, 0, 1) * 255) << 0) | ((uint32_t)(lx_clampf(c.y, 0, 1) * 255) << 8) | ((uint32_t)(lx_clampf(c.z, 0, 1) * 255) << 16) |
           ((uint32_t)(lx_clampf(c.w, 0, 1) * 255) << 24);
}
/* how far to close a gap this frame for "speed" (1/s): always 0..1, whatever the speed */
static float smooth_k(float speed, float dt) {
    if (!(speed > 0) || !(dt > 0)) return speed > 0 ? 0 : 1;   /* 0 or negative speed: snap */
    return lx_clampf(1 - expf(-speed * dt), 0, 1);
}
static int ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* ======================================================================
 *  switches and change events
 * ====================================================================== */
static void int_switch(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0);
    for (int k = 0; k < 4; k++)
        if (v == lx_fsm_int(c, 1 + k * 2)) { ev(c, 2 + k * 2); return; }
    ev(c, 9);
}
static const LxFsmActionDesc A_INT_SWITCH = {"Int Switch", "Logic", "Sends the event of the first case that matches the number.", "Switch on {0}",
    {P("Int", LX_FSM_INT, "0"), P("Case 1", LX_FSM_INT, "0"), PEV("Event 1"), P("Case 2", LX_FSM_INT, "1"), PEV("Event 2"), P("Case 3", LX_FSM_INT, "2"),
     PEV("Event 3"), P("Case 4", LX_FSM_INT, "3"), PEV("Event 4"), PEV("Otherwise")},
    0, 0, LX_FSM_ONCE, int_switch, int_switch, NULL};

static void string_switch(LxFsmCtx *c) {
    char v[LX_REF_MAX];
    lx_strcpy(v, sizeof v, lx_fsm_string(c, 0));
    for (int k = 0; k < 3; k++)
        if (!strcmp(v, lx_fsm_string(c, 1 + k * 2))) { ev(c, 2 + k * 2); return; }
    ev(c, 7);
}
static const LxFsmActionDesc A_STRING_SWITCH = {"String Switch", "Logic", "Sends the event of the first text that matches.", "Switch on \"{0}\"",
    {P("String", LX_FSM_STRING, ""), P("Case 1", LX_FSM_STRING, ""), PEV("Event 1"), P("Case 2", LX_FSM_STRING, ""), PEV("Event 2"),
     P("Case 3", LX_FSM_STRING, ""), PEV("Event 3"), PEV("Otherwise")},
    0, 0, LX_FSM_ONCE, string_switch, string_switch, NULL};

static void float_range(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    ev(c, v < lx_fsm_float(c, 1) ? 3 : v > lx_fsm_float(c, 2) ? 5 : 4);
}
static const LxFsmActionDesc A_FLOAT_RANGE = {"Float In Range", "Logic", "Below, inside or above a range (health low / ok / full).", "If {0} in {1}..{2}",
    {P("Float", LX_FSM_FLOAT, "0"), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "1"), PEV("Below"), PEV("Inside"), PEV("Above")},
    0, 0, LX_FSM_EVERY_FRAME, float_range, float_range, NULL};

typedef struct ChangeState { float last; bool have; } ChangeState;
static void changed_update(LxFsmCtx *c) {
    ChangeState *s = c->state;
    float v = lx_fsm_float(c, 0);
    if (s->have && v != s->last) { lx_fsm_set_bool(c, 2, true); ev(c, 1); }
    s->last = v;
    s->have = true;
}
static const LxFsmActionDesc A_CHANGED = {"Value Changed", "Logic", "Sends an event when a number (or bool) variable changes.", "When {0} changes: {1}",
    {P("Value", LX_FSM_FLOAT, "0"), PEV("Changed"), POPT("Store Changed", LX_FSM_BOOL)}, sizeof(ChangeState), 0, LX_FSM_EVERY_FRAME, changed_update, changed_update, NULL};

typedef struct CountState { int n; } CountState;
static void counter_enter(LxFsmCtx *c) {
    int n = lx_fsm_int(c, 0) < 2147483647 ? lx_fsm_int(c, 0) + 1 : 2147483647;
    lx_fsm_set_int(c, 0, n);
    if (n >= lx_fsm_int(c, 1)) { if (lx_fsm_bool(c, 4)) lx_fsm_set_int(c, 0, 0); ev(c, 2); }
    else ev(c, 3);
}
static const LxFsmActionDesc A_COUNTER = {"Counter", "Logic", "Counts how often the state is entered; after N times sends Reached.", "Count {0} to {1}",
    {POUT("Counter Variable", LX_FSM_INT), P("Target", LX_FSM_INT, "3"), PEV("Reached"), PEV("Not Yet"), P("Reset When Reached", LX_FSM_BOOL, "true")},
    0, 0, LX_FSM_ONCE, counter_enter, NULL, NULL};

/* ======================================================================
 *  more maths and text
 * ====================================================================== */
static void int_clamp(LxFsmCtx *c) { int v = lx_fsm_int(c, 0), a = lx_fsm_int(c, 1), b = lx_fsm_int(c, 2); lx_fsm_set_int(c, 0, v < a ? a : v > b ? b : v); }
static const LxFsmActionDesc A_INT_CLAMP = {"Int Clamp", "Math", "Keeps a whole number between Min and Max.", "Clamp {0} to {1}..{2}",
    {POUT("Variable", LX_FSM_INT), P("Min", LX_FSM_INT, "0"), P("Max", LX_FSM_INT, "10")}, 0, 0, LX_FSM_ONCE, int_clamp, int_clamp, NULL};

static void float_lerp(LxFsmCtx *c) { lx_fsm_set_float(c, 3, lx_lerpf(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2))); }
static const LxFsmActionDesc A_FLOAT_LERP = {"Float Lerp", "Math", "Blends between two numbers (0 = From, 1 = To).", "{3} = lerp {0}..{1} by {2}",
    {P("From", LX_FSM_FLOAT, "0"), P("To", LX_FSM_FLOAT, "1"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, float_lerp, float_lerp, NULL};

typedef struct OscState { float t; } OscState;
static void oscillate(LxFsmCtx *c) {
    OscState *s = c->state;
    s->t += c->dt;
    float w = lx_fsm_int(c, 3) == 0 ? sinf(s->t * lx_fsm_float(c, 2) * 6.2831853f) : (fabsf(fmodf(s->t * lx_fsm_float(c, 2), 1.0f) * 2 - 1) * 2 - 1);
    lx_fsm_set_float(c, 4, lx_fsm_float(c, 0) + w * lx_fsm_float(c, 1));
}
static const LxFsmActionDesc A_OSCILLATE = {"Float Oscillate", "Math", "A number that waves up and down (bobbing, pulsing, swinging).", "{4} = {0} ± {1} at {2} Hz",
    {P("Center", LX_FSM_FLOAT, "0"), P("Amplitude", LX_FSM_FLOAT, "1"), P("Frequency", LX_FSM_FLOAT, "0.5"), PENUM("Shape", "Sine|Ping Pong", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    sizeof(OscState), 0, LX_FSM_EVERY_FRAME, NULL, oscillate, NULL};

static void float_smooth(LxFsmCtx *c) {
    float cur = lx_fsm_float(c, 0), to = lx_fsm_float(c, 1), k = smooth_k(lx_fsm_float(c, 2), c->dt);
    lx_fsm_set_float(c, 0, cur + (to - cur) * k);
}
static const LxFsmActionDesc A_FLOAT_SMOOTH = {"Float Smooth Towards", "Math", "Eases a variable towards a target every frame (bigger Speed = snappier).", "{0} → {1}",
    {POUT("Variable", LX_FSM_FLOAT), P("Target", LX_FSM_FLOAT, "1"), P("Speed", LX_FSM_FLOAT, "5")}, 0, 0, LX_FSM_EVERY_FRAME, NULL, float_smooth, NULL};

static void string_len(LxFsmCtx *c) { lx_fsm_set_int(c, 1, (int)strlen(lx_fsm_string(c, 0))); }
static const LxFsmActionDesc A_STRING_LEN = {"String Length", "Variables", "How many characters a text has.", "{1} = length of {0}",
    {P("String", LX_FSM_STRING, ""), POUT("Store Length", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, string_len, string_len, NULL};
static void string_contains(LxFsmCtx *c) {
    char a[LX_REF_MAX];
    lx_strcpy(a, sizeof a, lx_fsm_string(c, 0));
    ev(c, ci_contains(a, lx_fsm_string(c, 1)) ? 2 : 3);
}
static const LxFsmActionDesc A_STRING_CONTAINS = {"String Contains", "Logic", "Checks whether a text contains another (ignoring case).", "If {0} contains {1}: {2}, else {3}",
    {P("String", LX_FSM_STRING, ""), P("Contains", LX_FSM_STRING, ""), PEV("Yes"), PEV("No")}, 0, 0, LX_FSM_ONCE, string_contains, string_contains, NULL};
static void string_join(LxFsmCtx *c) {
    char out[LX_REF_MAX], a[LX_REF_MAX];
    lx_strcpy(a, sizeof a, lx_fsm_string(c, 0));
    snprintf(out, sizeof out, "%s%s", a, lx_fsm_string(c, 1));
    lx_fsm_set_string(c, 2, out);
}
static const LxFsmActionDesc A_STRING_JOIN = {"String Join", "Variables", "Puts two texts together.", "{2} = {0} + {1}",
    {P("First", LX_FSM_STRING, ""), P("Second", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, string_join, string_join, NULL};

static void vec_distance(LxFsmCtx *c) { lx_fsm_set_float(c, 2, lx_len3(lx_sub3(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1)))); }
static const LxFsmActionDesc A_VEC_DISTANCE = {"Vector3 Distance", "Math", "Distance between two points.", "{2} = distance {0} ↔ {1}",
    {P("Point 1", LX_FSM_VEC3, "0 0 0"), P("Point 2", LX_FSM_VEC3, "0 0 0"), POUT("Store Distance", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, vec_distance, vec_distance, NULL};
static void vec_angle(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    float la = lx_len3(a), lb = lx_len3(b);
    float d = la > 1e-6f && lb > 1e-6f ? lx_clampf(lx_dot3(a, b) / (la * lb), -1, 1) : 1;
    lx_fsm_set_float(c, 2, acosf(d) * LX_RAD2DEG);
}
static const LxFsmActionDesc A_VEC_ANGLE = {"Vector3 Angle", "Math", "The angle between two directions, in degrees.", "{2} = angle {0} ∠ {1}",
    {P("From", LX_FSM_VEC3, "0 0 -1"), P("To", LX_FSM_VEC3, "1 0 0"), POUT("Store Angle", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, vec_angle, vec_angle, NULL};
static void vec_add(LxFsmCtx *c) {
    LxVec3 d = lx_fsm_vec3(c, 1);
    if (lx_fsm_bool(c, 2)) d = lx_scale3(d, c->dt);
    lx_fsm_set_vec3(c, 0, lx_add3(lx_fsm_vec3(c, 0), d));
}
static const LxFsmActionDesc A_VEC_ADD = {"Vector3 Add", "Math", "Adds to a vector variable (optionally per second).", "{0} += {1}",
    {POUT("Variable", LX_FSM_VEC3), P("Add", LX_FSM_VEC3, "0 1 0"), P("Per Second", LX_FSM_BOOL, "false")}, 0, 0, LX_FSM_ONCE, vec_add, vec_add, NULL};
static void random_point(LxFsmCtx *c) {
    LxVec3 p = lx_random_in_sphere(lx_fsm_float(c, 1));
    if (lx_fsm_bool(c, 2)) p.y = 0;
    lx_fsm_set_vec3(c, 3, lx_add3(lx_fsm_vec3(c, 0), p));
}
static const LxFsmActionDesc A_RANDOM_POINT = {"Random Point", "Math", "A random point around a centre (on the ground if Flat).", "{3} = random point near {0}",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Radius", LX_FSM_FLOAT, "5"), P("Flat", LX_FSM_BOOL, "true"), POUT("Store Point", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, random_point, random_point, NULL};

/* ======================================================================
 *  transform, smoothly
 * ====================================================================== */
static void get_directions(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxQuat q = lx_entity_world_rotation(c->scene, e);
    lx_fsm_set_vec3(c, 1, lx_quat_rotate(q, lx_v3(0, 0, -1)));
    lx_fsm_set_vec3(c, 2, lx_quat_rotate(q, lx_v3(1, 0, 0)));
    lx_fsm_set_vec3(c, 3, lx_quat_rotate(q, lx_v3(0, 1, 0)));
}
static const LxFsmActionDesc A_GET_DIRECTIONS = {"Get Directions", "Transform", "Which way an object faces: forward, right and up.", "{1} = forward of {0}",
    {POBJ("Object"), POPT("Store Forward", LX_FSM_VEC3), POPT("Store Right", LX_FSM_VEC3), POPT("Store Up", LX_FSM_VEC3)}, 0, 0, LX_FSM_EVERY_FRAME, get_directions, get_directions, NULL};

static LxQuat nlerp(LxQuat a, LxQuat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    LxQuat r = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    if (r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w < 1e-12f) return b;
    return lx_quat_norm(r);
}
static void smooth_look(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 pos = lx_entity_world_position(c->scene, e);
    LxEntity t = obj(c, 1);
    LxVec3 goal = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 2);
    LxVec3 d = lx_sub3(goal, pos);
    if (lx_fsm_bool(c, 4)) d.y = 0;
    if (lx_len3sq(d) < 1e-8f) return;
    LxQuat want = lx_quat_look(lx_norm3(d), lx_v3(0, 1, 0)), cur = lx_entity_world_rotation(c->scene, e);
    LxQuat q = nlerp(cur, want, smooth_k(lx_fsm_float(c, 3), c->dt));
    lx_entity_set_world_pose(c->scene, e, pos, q);
}
static const LxFsmActionDesc A_SMOOTH_LOOK = {"Smooth Look At", "Transform", "Turns an object towards a target gradually.", "Turn {0} towards {1} (speed {3})",
    {POBJ("Object"), POBJN("Target Object"), P("Target Position", LX_FSM_VEC3, "0 0 0"), P("Speed", LX_FSM_FLOAT, "5"), P("Keep Upright", LX_FSM_BOOL, "true")},
    0, 0, LX_FSM_EVERY_FRAME, smooth_look, smooth_look, NULL};

static void follow(LxFsmCtx *c) {
    LxEntity e = obj(c, 0), t = obj(c, 1);
    if (!e || !t) return;
    LxVec3 goal = lx_add3(lx_entity_world_position(c->scene, t), lx_fsm_vec3(c, 2));
    LxVec3 pos = lx_entity_world_position(c->scene, e);
    LxVec3 np = lx_lerp3(pos, goal, smooth_k(lx_fsm_float(c, 3), c->dt));
    lx_entity_set_world_pose(c->scene, e, np, lx_entity_world_rotation(c->scene, e));
}
static const LxFsmActionDesc A_FOLLOW = {"Follow Object", "Transform", "Keeps an object near another (cameras, pets, health bars). Speed 0 = stick exactly.",
    "{0} follows {1} + {2}",
    {POBJ("Object"), POBJN("Target"), P("Offset", LX_FSM_VEC3, "0 3 6"), P("Smoothing", LX_FSM_FLOAT, "6")}, 0, 0, LX_FSM_LATE, follow, follow, NULL};

typedef struct OrbitState { float angle; } OrbitState;
static void orbit(LxFsmCtx *c) {
    OrbitState *s = c->state;
    LxEntity e = obj(c, 0), t = obj(c, 1);
    if (!e) return;
    s->angle += lx_fsm_float(c, 3) * c->dt * LX_DEG2RAD;
    LxVec3 center = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 4);
    float r = lx_fsm_float(c, 2);
    LxVec3 p = lx_add3(center, lx_v3(sinf(s->angle) * r, lx_fsm_float(c, 5), cosf(s->angle) * r));
    lx_entity_set_world_pose(c->scene, e, p, lx_entity_world_rotation(c->scene, e));
}
static const LxFsmActionDesc A_ORBIT = {"Orbit", "Transform", "Circles an object around a target or point.", "{0} circles {1} at {2} m",
    {POBJ("Object"), POBJN("Around Object"), P("Radius", LX_FSM_FLOAT, "3"), P("Degrees Per Second", LX_FSM_FLOAT, "45"), P("Around Point", LX_FSM_VEC3, "0 0 0"),
     P("Height", LX_FSM_FLOAT, "0")}, sizeof(OrbitState), 0, LX_FSM_EVERY_FRAME, NULL, orbit, NULL};

typedef struct ShakeState { float t; LxVec3 base; bool have; } ShakeState;
static void shake_enter(LxFsmCtx *c) {
    ShakeState *s = c->state;
    LxEntity e = obj(c, 0);
    if (e) { s->base = lx_entity_position(c->scene, e); s->have = true; }
}
static void shake_update(LxFsmCtx *c) {
    ShakeState *s = c->state;
    LxEntity e = obj(c, 0);
    if (!e || !s->have) { lx_fsm_finish(c); return; }
    s->t += c->dt;
    float len = lx_fsm_float(c, 2), k = len > 0 ? 1 - lx_clampf(s->t / len, 0, 1) : 0;
    LxVec3 j = lx_scale3(lx_random_in_sphere(1), lx_fsm_float(c, 1) * k);
    lx_entity_set_position(c->scene, e, lx_add3(s->base, j));
    if (k <= 0) { lx_entity_set_position(c->scene, e, s->base); lx_fsm_finish(c); ev(c, 3); }
}
static void shake_exit(LxFsmCtx *c) {
    ShakeState *s = c->state;
    LxEntity e = obj(c, 0);
    if (e && s->have) lx_entity_set_position(c->scene, e, s->base);
}
static const LxFsmActionDesc A_SHAKE = {"Shake", "Transform", "Shakes an object, fading out (camera shake, hits).", "Shake {0} by {1} for {2} s",
    {POBJ("Object"), P("Strength", LX_FSM_FLOAT, "0.2"), P("Time", LX_FSM_FLOAT, "0.4"), PEV("Finish Event")},
    sizeof(ShakeState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, shake_enter, shake_update, shake_exit};

static void set_pos_axis(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 p = lx_entity_position(c->scene, e);
    float v = lx_fsm_float(c, 2);
    switch (lx_fsm_int(c, 1)) { case 0: p.x = v; break; case 1: p.y = v; break; default: p.z = v; break; }
    lx_entity_set_position(c->scene, e, p);
}
static const LxFsmActionDesc A_SET_POS_AXIS = {"Set Position Axis", "Transform", "Sets just X, Y or Z of an object's position.", "{0}.{1} = {2}",
    {POBJ("Object"), PENUM("Axis", "X|Y|Z", "1"), P("Value", LX_FSM_FLOAT, "0")}, 0, 0, LX_FSM_ONCE, set_pos_axis, set_pos_axis, NULL};

static void clamp_pos(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 p = lx_entity_position(c->scene, e), lo = lx_fsm_vec3(c, 1), hi = lx_fsm_vec3(c, 2);
    lx_entity_set_position(c->scene, e, lx_v3(lx_clampf(p.x, lo.x, hi.x), lx_clampf(p.y, lo.y, hi.y), lx_clampf(p.z, lo.z, hi.z)));
}
static const LxFsmActionDesc A_CLAMP_POS = {"Clamp Position", "Transform", "Keeps an object inside a box (play area limits).", "Keep {0} in {1}..{2}",
    {POBJ("Object"), P("Min", LX_FSM_VEC3, "-10 -10 -10"), P("Max", LX_FSM_VEC3, "10 10 10")}, 0, 0, LX_FSM_LATE, clamp_pos, clamp_pos, NULL};

/* ======================================================================
 *  objects
 * ====================================================================== */
static void closest_tag(LxFsmCtx *c) {
    LxEntity from = obj(c, 0);
    if (!from) return;
    LxEntity list[256];
    int n = lx_entities_with_tag(c->scene, lx_fsm_string(c, 1), list, 256);
    LxVec3 p = lx_entity_world_position(c->scene, from);
    LxEntity best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        if (list[i] == from || !lx_entity_active(c->scene, list[i])) continue;
        float d = lx_len3sq(lx_sub3(lx_entity_world_position(c->scene, list[i]), p));
        if (d < bd) { bd = d; best = list[i]; }
    }
    lx_fsm_set_object(c, 2, best);
    lx_fsm_set_float(c, 3, best ? sqrtf(bd) : 0);
    ev(c, best ? 4 : 5);
}
static const LxFsmActionDesc A_CLOSEST_TAG = {"Find Closest With Tag", "Objects", "The nearest active object with a tag (the closest enemy, pickup...).", "{2} = closest {1} to {0}",
    {POBJ("From"), P("Tag", LX_FSM_STRING, "Enemy"), POUT("Store Object", LX_FSM_OBJECT), POPT("Store Distance", LX_FSM_FLOAT), PEV("Found"), PEV("None Found")},
    0, 0, LX_FSM_ONCE, closest_tag, closest_tag, NULL};
static void random_tag(LxFsmCtx *c) {
    LxEntity list[256];
    int n = lx_entities_with_tag(c->scene, lx_fsm_string(c, 0), list, 256);
    LxEntity e = n ? list[lx_random_int(0, n - 1)] : 0;
    lx_fsm_set_object(c, 1, e);
    ev(c, e ? 2 : 3);
}
static const LxFsmActionDesc A_RANDOM_TAG = {"Random With Tag", "Objects", "Picks a random object with a tag (a spawn point, a waypoint...).", "{1} = random {0}",
    {P("Tag", LX_FSM_STRING, "Waypoint"), POUT("Store Object", LX_FSM_OBJECT), PEV("Found"), PEV("None Found")}, 0, 0, LX_FSM_ONCE, random_tag, random_tag, NULL};
static void get_name(LxFsmCtx *c) { LxEntity e = obj(c, 0); lx_fsm_set_string(c, 1, e ? lx_entity_name(c->scene, e) : ""); lx_fsm_set_string(c, 2, e ? lx_entity_tag(c->scene, e) : ""); }
static const LxFsmActionDesc A_GET_NAME = {"Get Name And Tag", "Objects", "Stores an object's name and tag.", "{1} = name of {0}",
    {POBJ("Object"), POPT("Store Name", LX_FSM_STRING), POPT("Store Tag", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, get_name, get_name, NULL};
static void is_active(LxFsmCtx *c) { LxEntity e = obj(c, 0); bool a = e && lx_entity_active(c->scene, e); lx_fsm_set_bool(c, 1, a); ev(c, a ? 2 : 3); }
static const LxFsmActionDesc A_IS_ACTIVE = {"Is Active", "Objects", "Checks whether an object is shown/enabled.", "If {0} is active: {2}, else {3}",
    {POBJ("Object"), POPT("Store Active", LX_FSM_BOOL), PEV("Active"), PEV("Inactive")}, 0, 0, LX_FSM_ONCE, is_active, is_active, NULL};
static void toggle_active(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_set_active(c->scene, e, !lx_entity_active(c->scene, e)); }
static const LxFsmActionDesc A_TOGGLE_ACTIVE = {"Toggle Active", "Objects", "Shows a hidden object, or hides a shown one.", "Toggle {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, toggle_active, NULL, NULL};
static void child_count(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    int n = 0;
    for (LxEntity k = e ? lx_entity_first_child(c->scene, e) : 0; k; k = lx_entity_next_sibling(c->scene, k)) n++;
    lx_fsm_set_int(c, 1, n);
}
static const LxFsmActionDesc A_CHILD_COUNT = {"Get Child Count", "Objects", "How many objects are attached directly to this one.", "{1} = children of {0}",
    {POBJ("Object"), POUT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, child_count, child_count, NULL};
static void duplicate_obj(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxEntity d = e && lx_fsm_spawn_allowed(c->scene) ? lx_entity_duplicate(c->scene, e) : 0;
    if (d) lx_entity_set_position(c->scene, d, lx_add3(lx_entity_position(c->scene, d), lx_fsm_vec3(c, 1)));
    lx_fsm_set_object(c, 2, d);
}
static const LxFsmActionDesc A_DUPLICATE = {"Duplicate Object", "Objects", "Makes a copy of an object (and its children), moved by an offset.", "{2} = copy of {0}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "1 0 0"), POPT("Store Copy", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, duplicate_obj, duplicate_obj, NULL};

/* ======================================================================
 *  physics
 * ====================================================================== */
static void overlap(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxVec3 at = lx_add3(e ? lx_entity_world_position(c->scene, e) : lx_v3(0, 0, 0), lx_fsm_vec3(c, 1));
    LxEntity hits[64];
    int n = lx_physics_overlap_sphere(c->scene, at, lx_fsm_float(c, 2), LX_ALL_LAYERS, hits, 64);
    const char *tag = lx_fsm_string(c, 3);
    char t[LX_NAME_MAX];
    lx_strcpy(t, sizeof t, tag);
    int count = 0;
    LxEntity first = 0;
    for (int i = 0; i < n; i++) {
        if (hits[i] == e || (t[0] && !lx_entity_has_tag(c->scene, hits[i], t))) continue;
        if (!first) first = hits[i];
        count++;
    }
    lx_fsm_set_int(c, 4, count);
    lx_fsm_set_object(c, 5, first);
    ev(c, count ? 6 : 7);
}
static const LxFsmActionDesc A_OVERLAP = {"Overlap Sphere", "Physics", "Finds colliders within a radius (attack range, pickup radius).", "Anything with tag {3} within {2} m of {0}: {6}",
    {POBJ("Center Object"), P("Offset", LX_FSM_VEC3, "0 0 0"), P("Radius", LX_FSM_FLOAT, "2"), PT("Tag", LX_FSM_STRING, "", "empty = anything"),
     POPT("Store Count", LX_FSM_INT), POPT("Store First", LX_FSM_OBJECT), PEV("Found"), PEV("Nothing")}, 0, 0, LX_FSM_EVERY_FRAME, overlap, overlap, NULL};
static void explosion(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxVec3 at = lx_add3(e ? lx_entity_world_position(c->scene, e) : lx_v3(0, 0, 0), lx_fsm_vec3(c, 1));
    lx_physics_explosion(c->scene, at, lx_fsm_float(c, 2), lx_fsm_float(c, 3));
}
static const LxFsmActionDesc A_EXPLOSION = {"Explosion Force", "Physics", "Pushes every Rigidbody nearby away from a point.", "Blast at {0}, {2} m, {3}",
    {POBJ("At Object"), P("Offset", LX_FSM_VEC3, "0 0 0"), P("Radius", LX_FSM_FLOAT, "5"), P("Impulse", LX_FSM_FLOAT, "10")}, 0, 0, LX_FSM_ONCE, explosion, NULL, NULL};
static void add_torque(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_rigidbody_add_torque(c->scene, e, lx_fsm_vec3(c, 1)); }
static const LxFsmActionDesc A_TORQUE = {"Add Torque", "Physics", "Spins a Rigidbody.", "Spin {0} with {1}",
    {POBJ("Object"), P("Torque", LX_FSM_VEC3, "0 5 0")}, 0, 0, LX_FSM_FIXED, add_torque, add_torque, NULL};
static void set_kinematic(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxRigidbody *rb = e ? lx_get_rigidbody(c->scene, e) : NULL;
    if (!rb) return;
    rb->is_kinematic = lx_fsm_bool(c, 1);
    rb->use_gravity = lx_fsm_bool(c, 2);
}
static const LxFsmActionDesc A_RB_SETTINGS = {"Set Rigidbody", "Physics", "Switches physics simulation (kinematic) and gravity on an object.", "{0}: kinematic {1}, gravity {2}",
    {POBJ("Object"), P("Kinematic", LX_FSM_BOOL, "false"), P("Use Gravity", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_ONCE, set_kinematic, NULL, NULL};
static void set_gravity(LxFsmCtx *c) { lx_physics_settings()->gravity = lx_fsm_vec3(c, 0); }
static const LxFsmActionDesc A_GRAVITY = {"Set Gravity", "Physics", "Changes gravity for the whole scene (moon jumps, upside down).", "Gravity = {0}",
    {P("Gravity", LX_FSM_VEC3, "0 -9.81 0")}, 0, 0, LX_FSM_ONCE, set_gravity, NULL, NULL};
static void grounded(LxFsmCtx *c) { LxEntity e = obj(c, 0); bool g = e && lx_character_grounded(c->scene, e); lx_fsm_set_bool(c, 1, g); ev(c, g ? 2 : 3); }
static const LxFsmActionDesc A_GROUNDED = {"Is Grounded", "Physics", "Whether a Character Controller stands on something.", "If {0} is grounded: {2}, else {3}",
    {POBJ("Object"), POPT("Store Grounded", LX_FSM_BOOL), PEV("Grounded"), PEV("In The Air")}, 0, 0, LX_FSM_EVERY_FRAME, grounded, grounded, NULL};

/* ======================================================================
 *  camera, audio, lights, animation
 * ====================================================================== */
static void camera_fov(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) e = lx_scene_main_camera(c->scene);
    LxCamera *cam = e ? lx_get_camera(c->scene, e) : NULL;
    if (cam) cam->fov_deg = lx_clampf(lx_fsm_float(c, 1), 1, 179);
}
static const LxFsmActionDesc A_CAMERA_FOV = {"Set Field Of View", "Camera", "Zooms a camera in or out (degrees; None = the main camera).", "{0} FOV = {1}°",
    {POBJN("Camera"), P("Degrees", LX_FSM_FLOAT, "60")}, 0, 0, LX_FSM_ONCE, camera_fov, camera_fov, NULL};
static void main_camera(LxFsmCtx *c) { lx_fsm_set_object(c, 0, lx_scene_main_camera(c->scene)); }
static const LxFsmActionDesc A_MAIN_CAMERA = {"Get Main Camera", "Camera", "Stores the camera the game is seen through.", "{0} = main camera",
    {POUT("Store Object", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, main_camera, main_camera, NULL};
static void world_to_screen(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxVec3 w = lx_add3(e ? lx_entity_world_position(c->scene, e) : lx_v3(0, 0, 0), lx_fsm_vec3(c, 1));
    LxVec2 p;
    bool vis = lx_ui_world_to_canvas(c->scene, w, &p);
    lx_fsm_set_vec3(c, 2, lx_v3(p.x, p.y, 0));
    lx_fsm_set_bool(c, 3, vis);
}
static const LxFsmActionDesc A_WORLD_TO_SCREEN = {"World To Screen", "Camera", "Where a 3D point appears on the game UI canvas (for labels over heads).", "{2} = screen spot of {0}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "0 2 0"), POUT("Store Position", LX_FSM_VEC3), POPT("Store On Screen", LX_FSM_BOOL)}, 0, 0, LX_FSM_LATE, world_to_screen, world_to_screen, NULL};

static void audio_settings(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxAudioSource *a = e ? lx_get_audio_source(c->scene, e) : NULL;
    if (!a) return;
    a->volume = lx_clampf(lx_fsm_float(c, 1), 0, 4);
    a->pitch = lx_clampf(lx_fsm_float(c, 2), 0.05f, 4);
}
static const LxFsmActionDesc A_AUDIO_SETTINGS = {"Set Volume And Pitch", "Audio", "Changes an Audio Source's volume and pitch.", "{0}: volume {1}, pitch {2}",
    {POBJ("Object"), P("Volume", LX_FSM_FLOAT, "1"), P("Pitch", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, audio_settings, audio_settings, NULL};
static void play_random_sound(LxFsmCtx *c) {
    int n = 0, pick[3];
    for (int i = 0; i < 3; i++) if (lx_fsm_asset(c, i)[0]) pick[n++] = i;
    if (!n) return;
    LxEntity at = obj(c, 3);
    char clip[LX_REF_MAX];
    lx_strcpy(clip, sizeof clip, lx_fsm_asset(c, pick[lx_random_int(0, n - 1)]));
    lx_audio_play_oneshot(clip, at ? lx_entity_world_position(c->scene, at) : lx_v3(0, 0, 0), lx_fsm_float(c, 4));
}
static const LxFsmActionDesc A_RANDOM_SOUND = {"Play Random Sound", "Audio", "Plays one of up to three sounds (footsteps, hits) so they don't repeat.", "Play {0}, {1} or {2}",
    {PASSET("Sound 1", "sound"), PASSET("Sound 2", "sound"), PASSET("Sound 3", "sound"), POBJ("At Object"), P("Volume", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, play_random_sound, NULL, NULL};
static void rumble(LxFsmCtx *c) { lx_gamepad_rumble(lx_fsm_int(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3)); }
static const LxFsmActionDesc A_RUMBLE = {"Gamepad Rumble", "Input", "Vibrates a gamepad.", "Rumble pad {0} for {3} s",
    {P("Gamepad", LX_FSM_INT, "0"), P("Low", LX_FSM_FLOAT, "0.5"), P("High", LX_FSM_FLOAT, "0.5"), P("Seconds", LX_FSM_FLOAT, "0.25")}, 0, 0, LX_FSM_ONCE, rumble, NULL, NULL};

typedef struct FlickerState { float t, base; bool have; } FlickerState;
static void flicker(LxFsmCtx *c) {
    FlickerState *s = c->state;
    LxEntity e = obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) return;
    if (!s->have) { s->base = l->intensity; s->have = true; }
    s->t += c->dt * lx_fsm_float(c, 2);
    float n = 0.5f + 0.5f * sinf(s->t * 13.0f) * sinf(s->t * 7.3f + 1.0f);
    l->intensity = s->base * (1 - lx_fsm_float(c, 1) * n);
}
static void flicker_exit(LxFsmCtx *c) {
    FlickerState *s = c->state;
    LxEntity e = obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (l && s->have) l->intensity = s->base;
}
static const LxFsmActionDesc A_FLICKER = {"Light Flicker", "Rendering", "Makes a light flicker like a flame or a broken lamp.", "Flicker {0}",
    {POBJ("Object"), P("Amount", LX_FSM_FLOAT, "0.4"), P("Speed", LX_FSM_FLOAT, "1")}, sizeof(FlickerState), 0, LX_FSM_EVERY_FRAME, NULL, flicker, flicker_exit};

typedef struct LightTween { float t, from; bool have; } LightTween;
static void light_fade(LxFsmCtx *c) {
    LightTween *s = c->state;
    LxEntity e = obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) { lx_fsm_finish(c); return; }
    if (!s->have) { s->from = l->intensity; s->have = true; }
    s->t += c->dt;
    float len = lx_fsm_float(c, 2), k = len > 0 ? lx_clampf(s->t / len, 0, 1) : 1;
    l->intensity = s->from + (lx_fsm_float(c, 1) - s->from) * k;
    if (k >= 1) { lx_fsm_finish(c); ev(c, 3); }
}
static const LxFsmActionDesc A_LIGHT_FADE = {"Fade Light", "Rendering", "Changes a light's brightness over time.", "Fade {0} to {1} in {2} s",
    {POBJ("Object"), P("To Intensity", LX_FSM_FLOAT, "0"), P("Time", LX_FSM_FLOAT, "1"), PEV("Finish Event")}, sizeof(LightTween), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, light_fade, NULL};

static void anim_time(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    if (lx_fsm_int(c, 2) == 0) lx_animator_set_time(c->scene, e, lx_fsm_float(c, 1));
    lx_fsm_set_float(c, 3, lx_animator_time(c->scene, e));
    lx_fsm_set_string(c, 4, lx_animator_current_clip(c->scene, e));
}
static const LxFsmActionDesc A_ANIM_TIME = {"Animation Time", "Animation", "Jumps to a time in the current clip, or reads where it is.", "{0} animation time",
    {POBJ("Object"), P("Time", LX_FSM_FLOAT, "0"), PENUM("Mode", "Set|Get", "1"), POPT("Store Time", LX_FSM_FLOAT), POPT("Store Clip", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, anim_time, anim_time, NULL};

/* ======================================================================
 *  input
 * ====================================================================== */
static void mouse_pos(LxFsmCtx *c) {
    LxVec2 p = lx_ui_pointer(), d = lx_mouse_delta();
    lx_fsm_set_vec3(c, 0, lx_v3(p.x, p.y, 0));
    lx_fsm_set_vec3(c, 1, lx_v3(d.x, d.y, 0));
    lx_fsm_set_float(c, 2, lx_mouse_wheel());
}
static const LxFsmActionDesc A_MOUSE_POS = {"Get Mouse", "Input", "Mouse or touch position on the game UI canvas, how far it moved and the scroll wheel.", "{0} = pointer",
    {POPT("Store Position", LX_FSM_VEC3), POPT("Store Movement", LX_FSM_VEC3), POPT("Store Wheel", LX_FSM_FLOAT)}, 0, 0, LX_FSM_EVERY_FRAME, mouse_pos, mouse_pos, NULL};
static void any_key(LxFsmCtx *c) {
    for (int k = 1; k < LX_KEY_COUNT; k++) if (lx_key_pressed((LxKey)k)) { ev(c, 0); return; }
    for (int b = 0; b < 3; b++) if (lx_mouse_pressed((LxMouseButton)b)) { ev(c, 0); return; }
}
static void set_mouse(LxFsmCtx *c) {
    int lock = lx_fsm_int(c, 0), show = lx_fsm_int(c, 1);
    if (lock == 1) lx_mouse_lock(true);
    else if (lock == 2) lx_mouse_lock(false);
    if (show == 1) lx_mouse_show(true);
    else if (show == 2) lx_mouse_show(false);
    if (lx_fsm_bool(c, 2)) {
        LxVec3 hot = lx_fsm_vec3(c, 4);
        char img[LX_REF_MAX];
        lx_strcpy(img, sizeof img, lx_fsm_asset(c, 3));
        lx_mouse_set_image(img, lx_clampf(hot.x, 0, 1), lx_clampf(hot.y, 0, 1), lx_fsm_float(c, 5));
    }
    lx_fsm_set_bool(c, 6, lx_mouse_lock_wanted());
}
static const LxFsmActionDesc A_SET_MOUSE = {"Set Mouse", "Input",
    "Locks the mouse for mouse look (hidden and held in the game view), shows or hides the pointer, or swaps the pointer for your own picture. "
    "In the editor, Escape gives the mouse back; click the game view to take it again.",
    "Mouse: {0}, pointer {1}",
    {PENUM("Lock", "Don't Change|Lock|Unlock", "1"), PENUM("Pointer", "Don't Change|Show|Hide", "0"), PT("Change Image", LX_FSM_BOOL, "false", "use the Image below (empty = the normal arrow)"),
     PASSET("Image", "texture"), PT("Hotspot", LX_FSM_VEC3, "0 0 0", "where in the picture it points: 0 0 = top-left, 0.5 0.5 = centre"), PT("Size", LX_FSM_FLOAT, "32", "in game UI units (the screen is 1080 tall)"),
     POPT("Store Locked", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, set_mouse, set_mouse, NULL};

static const LxFsmActionDesc A_ANY_KEY = {"Any Key Event", "Input", "Sends an event when any key or mouse button is pressed (\"press any key\").", "Any key: {0}",
    {PEV("Send Event")}, 0, 0, LX_FSM_EVERY_FRAME, NULL, any_key, NULL};

/* ======================================================================
 *  time, flow, scenes
 * ====================================================================== */
typedef struct RepeatState { float t; int fired; } RepeatState;
static void repeat_update(LxFsmCtx *c) {
    RepeatState *s = c->state;
    float every = lx_fsm_float(c, 0);
    if (every <= 0) return;
    s->t += c->dt;
    if (s->t >= every) {   /* at most once a frame, however small Every is */
        s->t = every > 0 ? fmodf(s->t, every) : 0;
        s->fired++;
        int max = lx_fsm_int(c, 2);
        if (max > 0 && s->fired >= max) { lx_fsm_finish(c); ev(c, 1); ev(c, 3); return; }
        ev(c, 1);
    }
}
static const LxFsmActionDesc A_REPEAT = {"Repeat Event", "Time", "Sends an event every few seconds (spawners, ticking clocks), optionally N times.", "Every {0} s: {1}",
    {P("Every", LX_FSM_FLOAT, "1"), PEV("Event"), PT("Times", LX_FSM_INT, "0", "0 = forever"), PEV("Done Event")}, sizeof(RepeatState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, repeat_update, NULL};
static void countdown(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0) - c->dt;
    if (v <= 0) { lx_fsm_set_float(c, 0, 0); lx_fsm_finish(c); ev(c, 1); return; }
    lx_fsm_set_float(c, 0, v);
}
static const LxFsmActionDesc A_COUNTDOWN = {"Countdown", "Time", "Counts a variable down to zero in seconds, then sends an event (round timers).", "{0} counts down → {1}",
    {POUT("Seconds Variable", LX_FSM_FLOAT), PEV("At Zero")}, 0, LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, countdown, NULL};
static void finish_action(LxFsmCtx *c) { LX_UNUSED(c); }
static const LxFsmActionDesc A_FINISH = {"Finish", "Events", "Does nothing and finishes: the state sends FINISHED when its other actions are done.", "Finish",
    {{0}}, 0, 0, LX_FSM_ONCE, finish_action, NULL, NULL};
static void restart_scene(LxFsmCtx *c) { char n[LX_REF_MAX]; snprintf(n, sizeof n, "Scenes/%s.lxscene", lx_scene_name(c->scene)); lx_load_scene(n); }
static const LxFsmActionDesc A_RESTART = {"Restart Scene", "Scenes", "Loads the current scene again (Scenes/<name>.lxscene).", "Restart the scene",
    {{0}}, 0, 0, LX_FSM_ONCE, restart_scene, NULL, NULL};
static void scene_name(LxFsmCtx *c) { lx_fsm_set_string(c, 0, lx_scene_name(c->scene)); }
static const LxFsmActionDesc A_SCENE_NAME = {"Get Scene Name", "Scenes", "The name of the scene that's playing.", "{0} = scene name",
    {POUT("Store Name", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, scene_name, NULL, NULL};
static void broadcast_global(LxFsmCtx *c) {
    const char *e = lx_fsm_event(c, 0);
    if (e[0]) lx_fsm_broadcast_event(c->scene, e, lx_fsm_float(c, 1), c->self);
}
static const LxFsmActionDesc A_BROADCAST = {"Broadcast Event", "Events", "Sends an event to every State Graph in the scene (game over, level complete).", "Broadcast {0}",
    {PEV("Event"), P("Value", LX_FSM_FLOAT, "0")}, 0, 0, LX_FSM_ONCE, broadcast_global, NULL, NULL};
static void send_owner_parent(LxFsmCtx *c) {
    const char *e = lx_fsm_event(c, 0);
    LxEntity p = lx_entity_parent(c->scene, c->self);
    if (e[0] && p) lx_fsm_send_event(c->scene, p, e, lx_fsm_float(c, 1), c->self);
}
static const LxFsmActionDesc A_SEND_PARENT = {"Send Event To Parent", "Events", "Sends an event to the object this one is attached to.", "Send {0} to parent",
    {PEV("Event"), P("Value", LX_FSM_FLOAT, "0")}, 0, 0, LX_FSM_ONCE, send_owner_parent, NULL, NULL};

/* ======================================================================
 *  game UI
 * ====================================================================== */
static void ui_rect(LxFsmCtx *c) {
    LxVec3 p = lx_fsm_vec3(c, 0), s = lx_fsm_vec3(c, 1);
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    lx_ui_rect(p.x, p.y, s.x, s.y, rgba(lx_fsm_color(c, 2)), lx_fsm_float(c, 3));
}
static const LxFsmActionDesc A_UI_RECT = {"UI Panel", "Game UI", "Draws a filled rectangle every frame (backgrounds, overlays).", "Panel at {0}, size {1}",
    {P("Position", LX_FSM_VEC3, "40 40 0"), P("Size", LX_FSM_VEC3, "400 200 0"), P("Color", LX_FSM_COLOR, "0 0 0 0.6"), P("Corner Radius", LX_FSM_FLOAT, "12"),
     PENUM("Anchor", ANCHORS, "0")}, 0, 0, LX_FSM_EVERY_FRAME, ui_rect, ui_rect, NULL};
static void ui_image(LxFsmCtx *c) {
    LxVec3 p = lx_fsm_vec3(c, 1), s = lx_fsm_vec3(c, 2);
    char tex[LX_REF_MAX];
    lx_strcpy(tex, sizeof tex, lx_fsm_asset(c, 0));
    if (!tex[0]) return;
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    lx_ui_image(tex, p.x, p.y, s.x, s.y, rgba(lx_fsm_color(c, 3)));
}
static const LxFsmActionDesc A_UI_IMAGE = {"UI Image", "Game UI", "Draws a picture every frame (icons, logos, crosshairs).", "Image {0} at {1}",
    {PASSET("Texture", "texture"), P("Position", LX_FSM_VEC3, "40 40 0"), P("Size", LX_FSM_VEC3, "128 128 0"), P("Tint", LX_FSM_COLOR, "1 1 1 1"), PENUM("Anchor", ANCHORS, "0")},
    0, 0, LX_FSM_EVERY_FRAME, ui_image, ui_image, NULL};
typedef struct FadeState { float t; bool done; } FadeState;
static void screen_fade(LxFsmCtx *c) {
    FadeState *s = c->state;
    s->t += c->dt;
    float len = lx_fsm_float(c, 1), k = len > 0 ? lx_clampf(s->t / len, 0, 1) : 1;
    if (lx_fsm_int(c, 0) == 0) k = 1 - k;   /* fade in: from the colour to clear */
    LxVec4 col = lx_fsm_color(c, 2);
    col.w *= k;
    LxVec2 sz = lx_ui_canvas_size();
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    lx_ui_rect(0, 0, sz.x, sz.y, rgba(col), 0);
    if (s->t >= len && !s->done) { s->done = true; lx_fsm_finish(c); ev(c, 3); }
}
static const LxFsmActionDesc A_FADE = {"Screen Fade", "Game UI", "Fades the whole screen to or from a colour (keeps drawing while the state lasts).", "Fade {0} over {1} s",
    {PENUM("Direction", "In|Out", "1"), P("Time", LX_FSM_FLOAT, "1"), P("Color", LX_FSM_COLOR, "0 0 0 1"), PEV("Finish Event")}, sizeof(FadeState), 0, LX_FSM_EVERY_FRAME, NULL, screen_fade, NULL};
static void ui_label_world(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec2 p;
    if (!lx_ui_world_to_canvas(c->scene, lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 1)), &p)) return;
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 2));
    float size = lx_fsm_float(c, 3);
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    lx_ui_text(p.x - lx_ui_text_width(size, text) * 0.5f, p.y - size, size, rgba(lx_fsm_color(c, 4)), "%s", text);
}
static const LxFsmActionDesc A_UI_WORLD_TEXT = {"UI Text Over Object", "Game UI", "Draws text above an object (names, damage numbers, \"Press E\").", "\"{2}\" over {0}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "0 2 0"), P("Text", LX_FSM_STRING, "Hello"), P("Size", LX_FSM_FLOAT, "36"), P("Color", LX_FSM_COLOR, "1 1 1 1")},
    0, 0, LX_FSM_LATE, ui_label_world, ui_label_world, NULL};

void lx_fsm_register_more_actions(void) {
    static const LxFsmActionDesc *all[] = {
        &A_INT_SWITCH, &A_STRING_SWITCH, &A_FLOAT_RANGE, &A_CHANGED, &A_COUNTER, &A_STRING_CONTAINS,
        &A_INT_CLAMP, &A_FLOAT_LERP, &A_OSCILLATE, &A_FLOAT_SMOOTH, &A_VEC_DISTANCE, &A_VEC_ANGLE, &A_VEC_ADD, &A_RANDOM_POINT,
        &A_STRING_LEN, &A_STRING_JOIN,
        &A_GET_DIRECTIONS, &A_SMOOTH_LOOK, &A_FOLLOW, &A_ORBIT, &A_SHAKE, &A_SET_POS_AXIS, &A_CLAMP_POS,
        &A_CLOSEST_TAG, &A_RANDOM_TAG, &A_GET_NAME, &A_IS_ACTIVE, &A_TOGGLE_ACTIVE, &A_CHILD_COUNT, &A_DUPLICATE,
        &A_OVERLAP, &A_EXPLOSION, &A_TORQUE, &A_RB_SETTINGS, &A_GRAVITY, &A_GROUNDED,
        &A_CAMERA_FOV, &A_MAIN_CAMERA, &A_WORLD_TO_SCREEN,
        &A_AUDIO_SETTINGS, &A_RANDOM_SOUND, &A_RUMBLE, &A_FLICKER, &A_LIGHT_FADE, &A_ANIM_TIME,
        &A_MOUSE_POS, &A_ANY_KEY, &A_SET_MOUSE,
        &A_REPEAT, &A_COUNTDOWN, &A_FINISH, &A_RESTART, &A_SCENE_NAME, &A_BROADCAST, &A_SEND_PARENT,
        &A_UI_RECT, &A_UI_IMAGE, &A_FADE, &A_UI_WORLD_TEXT,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
