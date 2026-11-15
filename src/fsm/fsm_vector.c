/* Vectors, rotations, rectangles and bounding boxes: Vector2, Vector3, Quaternion, Rect, Bounds.
 *
 * Vector2 values are stored in the Vector2 variable type ("x y"); Vector3 in Vector3; rotations in
 * Quaternion variables (written as Euler degrees); rectangles in Rect ("x y width height"). Luxora has
 * no Bounds variable type, so bounding boxes are passed as a centre and a size, both Vector3. */
#include "fsm_actions.h"

/* ==========================================================================================
 *  helpers
 * ========================================================================================== */
static LxVec3 fsm_v_clamp_mag(LxVec3 v, float max_len) {
    float l = lx_len3(v);
    if (max_len < 0) max_len = 0;
    return l > max_len ? lx_scale3(v, max_len / lx_maxf(l, 1e-12f)) : v;
}
static LxVec3 fsm_v_move_towards(LxVec3 cur, LxVec3 target, float max_step) {
    LxVec3 d = lx_sub3(target, cur);
    float l = lx_len3(d);
    if (l <= max_step || l < 1e-12f) return target;
    return lx_add3(cur, lx_scale3(d, max_step / l));
}
static LxVec3 fsm_v_slerp(LxVec3 a, LxVec3 b, float t) {
    float la = lx_len3(a), lb = lx_len3(b);
    if (la < 1e-9f || lb < 1e-9f) return lx_lerp3(a, b, t);
    LxVec3 na = lx_scale3(a, 1 / la), nb = lx_scale3(b, 1 / lb);
    float d = lx_clampf(lx_dot3(na, nb), -1, 1);
    float theta = acosf(d);
    LxVec3 dir;
    if (theta < 1e-4f) {
        dir = lx_lerp3(na, nb, t);
    } else {
        float s = sinf(theta);
        dir = lx_add3(lx_scale3(na, sinf((1 - t) * theta) / s), lx_scale3(nb, sinf(t * theta) / s));
    }
    return lx_scale3(lx_norm3(dir), lx_lerpf(la, lb, t));
}
static float fsm_v_signed_angle(LxVec3 a, LxVec3 b, LxVec3 axis) {
    LxVec3 na = lx_norm3(a), nb = lx_norm3(b);
    float ang = acosf(lx_clampf(lx_dot3(na, nb), -1, 1)) * LX_RAD2DEG;
    return lx_dot3(lx_cross3(na, nb), lx_norm3(axis)) < 0 ? -ang : ang;
}
static float fsm_snap(float v, float step) { return step > 0 ? floorf(v / step + 0.5f) * step : v; }

typedef struct { LxVec3 vel; } FsmVecSpring;
static float fsm_sd1(float cur, float target, float *vel, float st, float dt) {
    st = lx_maxf(0.0001f, st);
    float omega = 2.0f / st, x = omega * dt;
    float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = cur - target, t = cur - change;
    float temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * e;
    return t + (change + temp) * e;
}

/* ==========================================================================================
 *  Vector3
 * ========================================================================================== */
static void v3_set(LxFsmCtx *c) { lx_fsm_set_vec3(c, 1, lx_fsm_vec3(c, 0)); }
static const LxFsmActionDesc A_V3_SET = {"Vector3 Set Value", "Vector3", "Stores a Vector3 in a variable.", "{1} = {0}",
    {P("Value", LX_FSM_VEC3, "0 0 0"), POUT("Store Result", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, v3_set, v3_set, NULL};

static void v3_set_xyz(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    if (!lx_fsm_is_none(c, 1)) v.x = lx_fsm_float(c, 1);
    if (!lx_fsm_is_none(c, 2)) v.y = lx_fsm_float(c, 2);
    if (!lx_fsm_is_none(c, 3)) v.z = lx_fsm_float(c, 3);
    lx_fsm_set_vec3(c, 0, v);
}
static const LxFsmActionDesc A_V3_SET_XYZ = {"Vector3 Set XYZ", "Vector3", "Changes the X, Y or Z of a Vector3 variable; leave one as None to keep it.",
    "{0} = ({1}, {2}, {3})",
    {POUT("Vector3 Variable", LX_FSM_VEC3), PIN_OPT("X", LX_FSM_FLOAT), PIN_OPT("Y", LX_FSM_FLOAT), PIN_OPT("Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_set_xyz, v3_set_xyz, NULL};

static void v3_get_xyz(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    lx_fsm_set_float(c, 1, v.x);
    lx_fsm_set_float(c, 2, v.y);
    lx_fsm_set_float(c, 3, v.z);
}
static const LxFsmActionDesc A_V3_GET_XYZ = {"Vector3 Get XYZ", "Vector3", "Reads the X, Y and Z out of a Vector3.", "{0} -> {1}, {2}, {3}",
    {P("Vector3", LX_FSM_VEC3, "0 0 0"), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT), POPT("Store Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_get_xyz, v3_get_xyz, NULL};

static void v3_get_axis(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    lx_fsm_set_float(c, 2, lx_comp3(v, lx_fsm_int(c, 1)));
}
static const LxFsmActionDesc A_V3_GET_AXIS = {"Vector3 Get Axis", "Vector3", "Reads one axis of a Vector3.", "{0}.{1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 0"), PENUM("Axis", FSM_AXIS, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_get_axis, v3_get_axis, NULL};

static void v3_set_axis(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    float f = lx_fsm_float(c, 2);
    switch (lx_fsm_int(c, 1)) { case 0: v.x = f; break; case 1: v.y = f; break; default: v.z = f; break; }
    lx_fsm_set_vec3(c, 0, v);
}
static const LxFsmActionDesc A_V3_SET_AXIS = {"Vector3 Set Axis", "Vector3", "Changes one axis of a Vector3 variable.", "{0}.{1} = {2}",
    {POUT("Vector3 Variable", LX_FSM_VEC3), PENUM("Axis", FSM_AXIS, "0"), P("Value", LX_FSM_FLOAT, "0")},
    0, 0, LX_FSM_ONCE, v3_set_axis, v3_set_axis, NULL};

static void v3_operator(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1), r;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_add3(a, b); break;
    case 1: r = lx_sub3(a, b); break;
    case 2: r = lx_mul3(a, b); break;
    case 3: r = lx_v3(b.x != 0 ? a.x / b.x : 0, b.y != 0 ? a.y / b.y : 0, b.z != 0 ? a.z / b.z : 0); break;
    case 4: r = lx_cross3(a, b); break;
    case 5: r = lx_min3(a, b); break;
    case 6: r = lx_max3(a, b); break;
    case 7: r = lx_reflect3(a, lx_norm3(b)); break;
    default: r = a; break;
    }
    lx_fsm_set_vec3(c, 3, r);
}
static const LxFsmActionDesc A_V3_OPERATOR = {"Vector3 Operator", "Vector3", "Adds, subtracts, multiplies, divides, crosses, takes the smaller/larger or reflects two Vector3s.",
    "{0} {2} {1} -> {3}",
    {P("Vector3 1", LX_FSM_VEC3, "0 0 0"), P("Vector3 2", LX_FSM_VEC3, "0 0 0"),
     PENUM("Operation", "Add|Subtract|Multiply|Divide|Cross|Min|Max|Reflect", "0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_operator, v3_operator, NULL};

static void v3_add(LxFsmCtx *c) {
    LxVec3 add = lx_fsm_vec3(c, 1);
    if (lx_fsm_bool(c, 2)) add = lx_scale3(add, c->dt);
    lx_fsm_set_vec3(c, 0, lx_add3(lx_fsm_vec3(c, 0), add));
}
static const LxFsmActionDesc A_V3_ADD = {"Vector3 Add", "Vector3", "Adds a Vector3 to a Vector3 variable.", "{0} += {1}",
    {POUT("Vector3 Variable", LX_FSM_VEC3), P("Add", LX_FSM_VEC3, "0 0 0"),
     PT("Per Second", LX_FSM_BOOL, "false", "scale by the frame time so it adds per second")},
    0, 0, LX_FSM_ONCE, v3_add, v3_add, NULL};

static void v3_subtract(LxFsmCtx *c) { lx_fsm_set_vec3(c, 0, lx_sub3(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1))); }
static const LxFsmActionDesc A_V3_SUBTRACT = {"Vector3 Subtract", "Vector3", "Subtracts a Vector3 from a Vector3 variable.", "{0} -= {1}",
    {POUT("Vector3 Variable", LX_FSM_VEC3), P("Subtract", LX_FSM_VEC3, "0 0 0")}, 0, 0, LX_FSM_ONCE, v3_subtract, v3_subtract, NULL};

static void v3_scale(LxFsmCtx *c) { lx_fsm_set_vec3(c, 2, lx_scale3(lx_fsm_vec3(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_V3_SCALE = {"Vector3 Scale", "Vector3", "Multiplies a Vector3 by a single number (making it longer or shorter).",
    "{0} * {1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 0"), P("Multiply By", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_scale, v3_scale, NULL};

static void v3_negate(LxFsmCtx *c) { lx_fsm_set_vec3(c, 1, lx_neg3(lx_fsm_vec3(c, 0))); }
static const LxFsmActionDesc A_V3_NEGATE = {"Vector3 Negate", "Vector3", "Points a Vector3 the opposite way.", "-{0} -> {1}",
    {P("Vector3", LX_FSM_VEC3, "0 0 1"), POUT("Store Result", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, v3_negate, v3_negate, NULL};

static void v3_normalize(LxFsmCtx *c) { lx_fsm_set_vec3(c, 1, lx_norm3(lx_fsm_vec3(c, 0))); }
static const LxFsmActionDesc A_V3_NORMALIZE = {"Vector3 Normalize", "Vector3", "Shortens a Vector3 to length 1, keeping its direction.", "normalize {0} -> {1}",
    {P("Vector3", LX_FSM_VEC3, "0 0 1"), POUT("Store Result", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, v3_normalize, v3_normalize, NULL};

static void v3_magnitude(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    lx_fsm_set_float(c, 1, lx_len3(v));
    lx_fsm_set_float(c, 2, lx_len3sq(v));
}
static const LxFsmActionDesc A_V3_MAGNITUDE = {"Vector3 Magnitude", "Vector3", "How long a Vector3 is. The squared length is cheaper, for comparing distances.",
    "|{0}| -> {1}",
    {P("Vector3", LX_FSM_VEC3, "0 0 1"), POUT("Store Length", LX_FSM_FLOAT), POPT("Store Squared Length", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_magnitude, v3_magnitude, NULL};

static void v3_clamp_mag(LxFsmCtx *c) { lx_fsm_set_vec3(c, 2, fsm_v_clamp_mag(lx_fsm_vec3(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_V3_CLAMP_MAG = {"Vector3 Clamp Magnitude", "Vector3", "Caps how long a Vector3 can be (maximum speed, for example).",
    "clamp |{0}| to {1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 0"), P("Max Length", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_clamp_mag, v3_clamp_mag, NULL};

static void v3_distance(LxFsmCtx *c) {
    LxVec3 d = lx_sub3(lx_fsm_vec3(c, 1), lx_fsm_vec3(c, 0));
    lx_fsm_set_float(c, 2, lx_len3(d));
    lx_fsm_set_float(c, 3, lx_len3sq(d));
}
static const LxFsmActionDesc A_V3_DISTANCE = {"Vector3 Distance", "Vector3", "The distance between two points.", "dist({0}, {1}) -> {2}",
    {P("Point 1", LX_FSM_VEC3, "0 0 0"), P("Point 2", LX_FSM_VEC3, "0 0 0"),
     POUT("Store Distance", LX_FSM_FLOAT), POPT("Store Squared", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_distance, v3_distance, NULL};

static void v3_dot(LxFsmCtx *c) {
    bool norm = lx_fsm_bool(c, 2);
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    if (norm) { a = lx_norm3(a); b = lx_norm3(b); }
    lx_fsm_set_float(c, 3, lx_dot3(a, b));
}
static const LxFsmActionDesc A_V3_DOT = {"Vector3 Dot Product", "Vector3", "How much two directions agree: 1 the same way, 0 at right angles, -1 opposite.",
    "dot({0}, {1}) -> {3}",
    {P("Vector3 1", LX_FSM_VEC3, "0 0 1"), P("Vector3 2", LX_FSM_VEC3, "0 0 1"),
     PT("Normalize", LX_FSM_BOOL, "true", "treat both as pure directions"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_dot, v3_dot, NULL};

static void v3_cross(LxFsmCtx *c) { lx_fsm_set_vec3(c, 2, lx_cross3(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1))); }
static const LxFsmActionDesc A_V3_CROSS = {"Vector3 Cross Product", "Vector3", "A direction at right angles to both inputs (surface normals, sideways directions).",
    "cross({0}, {1}) -> {2}",
    {P("Vector3 1", LX_FSM_VEC3, "1 0 0"), P("Vector3 2", LX_FSM_VEC3, "0 1 0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_cross, v3_cross, NULL};

static void v3_angle(LxFsmCtx *c) {
    LxVec3 a = lx_norm3(lx_fsm_vec3(c, 0)), b = lx_norm3(lx_fsm_vec3(c, 1));
    lx_fsm_set_float(c, 2, acosf(lx_clampf(lx_dot3(a, b), -1, 1)) * LX_RAD2DEG);
}
static const LxFsmActionDesc A_V3_ANGLE = {"Vector3 Angle", "Vector3", "The angle in degrees between two directions (0..180).", "angle({0}, {1}) -> {2}",
    {P("Vector3 1", LX_FSM_VEC3, "0 0 1"), P("Vector3 2", LX_FSM_VEC3, "1 0 0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_angle, v3_angle, NULL};

static void v3_signed_angle(LxFsmCtx *c) {
    lx_fsm_set_float(c, 3, fsm_v_signed_angle(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1), lx_fsm_vec3(c, 2)));
}
static const LxFsmActionDesc A_V3_SIGNED_ANGLE = {"Vector3 Signed Angle", "Vector3", "The angle between two directions, negative when the turn is anticlockwise about the axis.",
    "signed angle({0}, {1}) -> {3}",
    {P("From", LX_FSM_VEC3, "0 0 1"), P("To", LX_FSM_VEC3, "1 0 0"), PT("Axis", LX_FSM_VEC3, "0 1 0", "the axis the turn is measured about"),
     POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v3_signed_angle, v3_signed_angle, NULL};

static void v3_direction(LxFsmCtx *c) {
    LxVec3 d = lx_sub3(lx_fsm_vec3(c, 1), lx_fsm_vec3(c, 0));
    lx_fsm_set_float(c, 3, lx_len3(d));
    lx_fsm_set_vec3(c, 2, lx_fsm_bool(c, 4) ? lx_norm3(d) : d);
}
static const LxFsmActionDesc A_V3_DIRECTION = {"Vector3 Get Direction", "Vector3", "The direction from one point to another, and how far apart they are.",
    "{0} -> {1} = {2}",
    {P("From", LX_FSM_VEC3, "0 0 0"), P("To", LX_FSM_VEC3, "0 0 0"), POUT("Store Direction", LX_FSM_VEC3),
     POPT("Store Distance", LX_FSM_FLOAT), P("Normalize", LX_FSM_BOOL, "true")},
    0, 0, LX_FSM_ONCE, v3_direction, v3_direction, NULL};

static void v3_lerp(LxFsmCtx *c) {
    float t = lx_fsm_float(c, 2);
    if (!lx_fsm_bool(c, 3)) t = lx_saturate(t);
    lx_fsm_set_vec3(c, 4, lx_lerp3(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1), t));
}
static const LxFsmActionDesc A_V3_LERP = {"Vector3 Lerp", "Vector3", "Blends between two Vector3s in a straight line.", "lerp {0}..{1} at {2} -> {4}",
    {P("From", LX_FSM_VEC3, "0 0 0"), P("To", LX_FSM_VEC3, "0 0 0"), P("Amount", LX_FSM_FLOAT, "0.5"),
     P("Unclamped", LX_FSM_BOOL, "false"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_lerp, v3_lerp, NULL};

static void v3_slerp(LxFsmCtx *c) {
    lx_fsm_set_vec3(c, 3, fsm_v_slerp(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1), lx_saturate(lx_fsm_float(c, 2))));
}
static const LxFsmActionDesc A_V3_SLERP = {"Vector3 Slerp", "Vector3", "Blends between two directions along an arc, so the turn is even.",
    "slerp {0}..{1} at {2} -> {3}",
    {P("From", LX_FSM_VEC3, "0 0 1"), P("To", LX_FSM_VEC3, "1 0 0"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_slerp, v3_slerp, NULL};

static void v3_move_towards(LxFsmCtx *c) {
    float step = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) step *= c->dt;
    LxVec3 target = lx_fsm_vec3(c, 1);
    LxVec3 r = fsm_v_move_towards(lx_fsm_vec3(c, 0), target, step);
    lx_fsm_set_vec3(c, 4, r);
    if (lx_len3sq(lx_sub3(r, target)) < 1e-10f) fsm_send(c, 5);
}
static const LxFsmActionDesc A_V3_MOVE_TOWARDS = {"Vector3 Move Towards", "Vector3", "Steps a Vector3 towards a target at a fixed speed, never overshooting.",
    "{0} -> {1} by {2}",
    {P("Current", LX_FSM_VEC3, "0 0 0"), P("Target", LX_FSM_VEC3, "0 0 0"), P("Max Distance", LX_FSM_FLOAT, "1"),
     P("Per Second", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_VEC3), PEV("Finish Event")},
    0, 0, LX_FSM_EVERY_FRAME, v3_move_towards, v3_move_towards, NULL};

static void v3_smooth_damp(LxFsmCtx *c) {
    FsmVecSpring *s = c->state;
    LxVec3 cur = lx_fsm_vec3(c, 0), target = lx_fsm_vec3(c, 1);
    float st = lx_fsm_float(c, 2);
    LxVec3 r;
    r.x = fsm_sd1(cur.x, target.x, &s->vel.x, st, c->dt);
    r.y = fsm_sd1(cur.y, target.y, &s->vel.y, st, c->dt);
    r.z = fsm_sd1(cur.z, target.z, &s->vel.z, st, c->dt);
    lx_fsm_set_vec3(c, 3, r);
    lx_fsm_set_vec3(c, 4, s->vel);
}
static const LxFsmActionDesc A_V3_SMOOTH_DAMP = {"Vector3 Smooth Damp", "Vector3", "Eases a Vector3 towards a target like a spring: the usual way to make a camera follow.",
    "{0} -> {1} over {2}s",
    {P("Current", LX_FSM_VEC3, "0 0 0"), P("Target", LX_FSM_VEC3, "0 0 0"), P("Smooth Time", LX_FSM_FLOAT, "0.3"),
     POUT("Store Result", LX_FSM_VEC3), POPT("Store Velocity", LX_FSM_VEC3)},
    sizeof(FsmVecSpring), 0, LX_FSM_EVERY_FRAME, v3_smooth_damp, v3_smooth_damp, NULL};

static void v3_project(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0), onto = lx_fsm_vec3(c, 1);
    float d = lx_dot3(onto, onto);
    lx_fsm_set_vec3(c, 2, d > 1e-12f ? lx_scale3(onto, lx_dot3(v, onto) / d) : lx_v3(0, 0, 0));
}
static const LxFsmActionDesc A_V3_PROJECT = {"Vector3 Project", "Vector3", "The part of a Vector3 that points along another one.", "project {0} on {1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 1"), P("Onto", LX_FSM_VEC3, "1 0 0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_project, v3_project, NULL};

static void v3_project_plane(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0), n = lx_norm3(lx_fsm_vec3(c, 1));
    lx_fsm_set_vec3(c, 2, lx_sub3(v, lx_scale3(n, lx_dot3(v, n))));
}
static const LxFsmActionDesc A_V3_PROJECT_PLANE = {"Vector3 Project On Plane", "Vector3", "Flattens a Vector3 onto a plane: movement along a slope, or a direction with the vertical taken out.",
    "flatten {0} on {1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 1"), PT("Plane Normal", LX_FSM_VEC3, "0 1 0", "the plane's facing direction"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_project_plane, v3_project_plane, NULL};

static void v3_reflect(LxFsmCtx *c) {
    lx_fsm_set_vec3(c, 2, lx_reflect3(lx_fsm_vec3(c, 0), lx_norm3(lx_fsm_vec3(c, 1))));
}
static const LxFsmActionDesc A_V3_REFLECT = {"Vector3 Reflect", "Vector3", "Bounces a direction off a surface (ricochets).", "reflect {0} off {1} -> {2}",
    {P("Direction", LX_FSM_VEC3, "0 0 1"), P("Normal", LX_FSM_VEC3, "0 1 0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_reflect, v3_reflect, NULL};

static void v3_rotate(LxFsmCtx *c) {
    LxQuat q = lx_quat_euler_deg(lx_fsm_vec3(c, 1));
    lx_fsm_set_vec3(c, 2, lx_quat_rotate(q, lx_fsm_vec3(c, 0)));
}
static const LxFsmActionDesc A_V3_ROTATE = {"Vector3 Rotate", "Vector3", "Turns a Vector3 by an angle on each axis.", "rotate {0} by {1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 1"), P("Euler Angles", LX_FSM_VEC3, "0 90 0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_rotate, v3_rotate, NULL};

static void v3_snap_grid(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0), g = lx_fsm_vec3(c, 1);
    lx_fsm_set_vec3(c, 2, lx_v3(fsm_snap(v.x, g.x), fsm_snap(v.y, g.y), fsm_snap(v.z, g.z)));
}
static const LxFsmActionDesc A_V3_SNAP_GRID = {"Vector3 Snap To Grid", "Vector3", "Rounds a position onto a grid (tile placement, building).",
    "snap {0} to {1} -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 0"), P("Grid Size", LX_FSM_VEC3, "1 1 1"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_snap_grid, v3_snap_grid, NULL};

static void v3_equals(LxFsmCtx *c) {
    float tol = lx_fsm_float(c, 2);
    bool eq = lx_len3(lx_sub3(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1))) <= tol;
    lx_fsm_set_bool(c, 3, eq);
    fsm_send(c, eq ? 4 : 5);
}
static const LxFsmActionDesc A_V3_EQUALS = {"Vector3 Equals", "Vector3", "Tests whether two Vector3s are the same, within a tolerance.", "{0} == {1}?",
    {P("Vector3 1", LX_FSM_VEC3, "0 0 0"), P("Vector3 2", LX_FSM_VEC3, "0 0 0"), P("Tolerance", LX_FSM_FLOAT, "0.001"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, v3_equals, v3_equals, NULL};

static void v3_select(LxFsmCtx *c) { lx_fsm_set_vec3(c, 3, lx_fsm_bool(c, 0) ? lx_fsm_vec3(c, 1) : lx_fsm_vec3(c, 2)); }
static const LxFsmActionDesc A_V3_SELECT = {"Vector3 Select Value", "Vector3", "Picks one of two Vector3s depending on a bool.", "{3} = {0} ? {1} : {2}",
    {P("Condition", LX_FSM_BOOL, "true"), P("If True", LX_FSM_VEC3, "0 0 0"), P("If False", LX_FSM_VEC3, "0 0 0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_select, v3_select, NULL};

static void v3_constant(LxFsmCtx *c) {
    static const LxVec3 k[] = {{0, 0, 0}, {1, 1, 1}, {0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    int i = lx_fsm_int(c, 0);
    lx_fsm_set_vec3(c, 1, k[(i < 0 || i > 7) ? 0 : i]);
}
static const LxFsmActionDesc A_V3_CONSTANT = {"Vector3 Get Constant", "Vector3", "One of the standard directions: zero, one, up, down, right, left, forward or back.",
    "{0} -> {1}",
    {PENUM("Constant", "Zero|One|Up|Down|Right|Left|Forward|Back", "0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_constant, v3_constant, NULL};

static void v3_list_sum(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    LxVec3 sum = lx_v3(0, 0, 0);
    for (int i = 0; i < n; i++) sum = lx_add3(sum, lx_array_get_vec3(a, i));
    lx_fsm_set_vec3(c, 1, sum);
    lx_fsm_set_vec3(c, 2, n > 0 ? lx_scale3(sum, 1.0f / (float)n) : lx_v3(0, 0, 0));
}
static const LxFsmActionDesc A_V3_LIST_SUM = {"Vector3 List Sum", "Vector3", "Adds up every Vector3 in an array; the average is the middle of a group of points.",
    "sum {0} -> {1}",
    {PARR("Array"), POUT("Store Sum", LX_FSM_VEC3), POPT("Store Average", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, v3_list_sum, v3_list_sum, NULL};

/* ==========================================================================================
 *  Vector2  (stored as a Vector2 variable; z is ignored)
 * ========================================================================================== */
static void v2_set(LxFsmCtx *c) { lx_fsm_set_vec3(c, 1, lx_fsm_vec3(c, 0)); }
static const LxFsmActionDesc A_V2_SET = {"Vector2 Set Value", "Vector2", "Stores a Vector2 in a variable.", "{1} = {0}",
    {P("Value", LX_FSM_VEC2, "0 0"), POUT("Store Result", LX_FSM_VEC2)}, 0, 0, LX_FSM_ONCE, v2_set, v2_set, NULL};

static void v2_set_xy(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    if (!lx_fsm_is_none(c, 1)) v.x = lx_fsm_float(c, 1);
    if (!lx_fsm_is_none(c, 2)) v.y = lx_fsm_float(c, 2);
    lx_fsm_set_vec3(c, 0, v);
}
static const LxFsmActionDesc A_V2_SET_XY = {"Vector2 Set XY", "Vector2", "Changes the X or Y of a Vector2 variable; leave one as None to keep it.", "{0} = ({1}, {2})",
    {POUT("Vector2 Variable", LX_FSM_VEC2), PIN_OPT("X", LX_FSM_FLOAT), PIN_OPT("Y", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v2_set_xy, v2_set_xy, NULL};

static void v2_get_xy(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    lx_fsm_set_float(c, 1, v.x);
    lx_fsm_set_float(c, 2, v.y);
}
static const LxFsmActionDesc A_V2_GET_XY = {"Vector2 Get XY", "Vector2", "Reads the X and Y out of a Vector2.", "{0} -> {1}, {2}",
    {P("Vector2", LX_FSM_VEC2, "0 0"), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v2_get_xy, v2_get_xy, NULL};

static void v2_operator(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1), r;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_add3(a, b); break;
    case 1: r = lx_sub3(a, b); break;
    case 2: r = lx_mul3(a, b); break;
    case 3: r = lx_v3(b.x != 0 ? a.x / b.x : 0, b.y != 0 ? a.y / b.y : 0, 0); break;
    case 4: r = lx_min3(a, b); break;
    default: r = lx_max3(a, b); break;
    }
    r.z = 0;
    lx_fsm_set_vec3(c, 3, r);
}
static const LxFsmActionDesc A_V2_OPERATOR = {"Vector2 Operator", "Vector2", "Adds, subtracts, multiplies, divides or takes the smaller/larger of two Vector2s.",
    "{0} {2} {1} -> {3}",
    {P("Vector2 1", LX_FSM_VEC2, "0 0"), P("Vector2 2", LX_FSM_VEC2, "0 0"),
     PENUM("Operation", "Add|Subtract|Multiply|Divide|Min|Max", "0"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, v2_operator, v2_operator, NULL};

static void v2_scale(LxFsmCtx *c) {
    LxVec3 r = lx_scale3(lx_fsm_vec3(c, 0), lx_fsm_float(c, 1));
    r.z = 0;
    lx_fsm_set_vec3(c, 2, r);
}
static const LxFsmActionDesc A_V2_SCALE = {"Vector2 Scale", "Vector2", "Multiplies a Vector2 by a single number.", "{0} * {1} -> {2}",
    {P("Vector2", LX_FSM_VEC2, "0 0"), P("Multiply By", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, v2_scale, v2_scale, NULL};

static void v2_normalize(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    v.z = 0;
    lx_fsm_set_vec3(c, 1, lx_norm3(v));
}
static const LxFsmActionDesc A_V2_NORMALIZE = {"Vector2 Normalize", "Vector2", "Shortens a Vector2 to length 1, keeping its direction.", "normalize {0} -> {1}",
    {P("Vector2", LX_FSM_VEC2, "1 0"), POUT("Store Result", LX_FSM_VEC2)}, 0, 0, LX_FSM_ONCE, v2_normalize, v2_normalize, NULL};

static void v2_magnitude(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    v.z = 0;
    lx_fsm_set_float(c, 1, lx_len3(v));
    lx_fsm_set_float(c, 2, lx_len3sq(v));
}
static const LxFsmActionDesc A_V2_MAGNITUDE = {"Vector2 Magnitude", "Vector2", "How long a Vector2 is.", "|{0}| -> {1}",
    {P("Vector2", LX_FSM_VEC2, "1 0"), POUT("Store Length", LX_FSM_FLOAT), POPT("Store Squared Length", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v2_magnitude, v2_magnitude, NULL};

static void v2_distance(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    a.z = b.z = 0;
    lx_fsm_set_float(c, 2, lx_len3(lx_sub3(b, a)));
}
static const LxFsmActionDesc A_V2_DISTANCE = {"Vector2 Distance", "Vector2", "The distance between two Vector2 points.", "dist({0}, {1}) -> {2}",
    {P("Point 1", LX_FSM_VEC2, "0 0"), P("Point 2", LX_FSM_VEC2, "0 0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v2_distance, v2_distance, NULL};

static void v2_dot(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    a.z = b.z = 0;
    if (lx_fsm_bool(c, 2)) { a = lx_norm3(a); b = lx_norm3(b); }
    lx_fsm_set_float(c, 3, lx_dot3(a, b));
}
static const LxFsmActionDesc A_V2_DOT = {"Vector2 Dot Product", "Vector2", "How much two Vector2 directions agree.", "dot({0}, {1}) -> {3}",
    {P("Vector2 1", LX_FSM_VEC2, "1 0"), P("Vector2 2", LX_FSM_VEC2, "1 0"), P("Normalize", LX_FSM_BOOL, "true"),
     POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, v2_dot, v2_dot, NULL};

static void v2_angle(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    float ang = atan2f(b.y, b.x) - atan2f(a.y, a.x);
    ang *= LX_RAD2DEG;
    while (ang > 180) ang -= 360;
    while (ang < -180) ang += 360;
    lx_fsm_set_float(c, 2, lx_fsm_bool(c, 3) ? ang : fabsf(ang));
}
static const LxFsmActionDesc A_V2_ANGLE = {"Vector2 Angle", "Vector2", "The angle in degrees between two Vector2 directions.", "angle({0}, {1}) -> {2}",
    {P("Vector2 1", LX_FSM_VEC2, "1 0"), P("Vector2 2", LX_FSM_VEC2, "0 1"), POUT("Store Result", LX_FSM_FLOAT),
     PT("Signed", LX_FSM_BOOL, "false", "negative when the turn is clockwise")},
    0, 0, LX_FSM_ONCE, v2_angle, v2_angle, NULL};

static void v2_lerp(LxFsmCtx *c) {
    LxVec3 r = lx_lerp3(lx_fsm_vec3(c, 0), lx_fsm_vec3(c, 1), lx_saturate(lx_fsm_float(c, 2)));
    r.z = 0;
    lx_fsm_set_vec3(c, 3, r);
}
static const LxFsmActionDesc A_V2_LERP = {"Vector2 Lerp", "Vector2", "Blends between two Vector2s.", "lerp {0}..{1} at {2} -> {3}",
    {P("From", LX_FSM_VEC2, "0 0"), P("To", LX_FSM_VEC2, "0 0"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, v2_lerp, v2_lerp, NULL};

static void v2_move_towards(LxFsmCtx *c) {
    float step = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) step *= c->dt;
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    a.z = b.z = 0;
    LxVec3 r = fsm_v_move_towards(a, b, step);
    lx_fsm_set_vec3(c, 4, r);
}
static const LxFsmActionDesc A_V2_MOVE_TOWARDS = {"Vector2 Move Towards", "Vector2", "Steps a Vector2 towards a target at a fixed speed.", "{0} -> {1} by {2}",
    {P("Current", LX_FSM_VEC2, "0 0"), P("Target", LX_FSM_VEC2, "0 0"), P("Max Distance", LX_FSM_FLOAT, "1"),
     P("Per Second", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_EVERY_FRAME, v2_move_towards, v2_move_towards, NULL};

static void v2_from_angle(LxFsmCtx *c) {
    float a = lx_fsm_float(c, 0) * LX_DEG2RAD, len = lx_fsm_float(c, 1);
    lx_fsm_set_vec3(c, 2, lx_v3(cosf(a) * len, sinf(a) * len, 0));
}
static const LxFsmActionDesc A_V2_FROM_ANGLE = {"Vector2 From Angle", "Vector2", "Makes a Vector2 pointing at an angle, of a given length.", "angle {0} -> {2}",
    {P("Angle", LX_FSM_FLOAT, "0"), P("Length", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, v2_from_angle, v2_from_angle, NULL};

static void v2_perpendicular(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    lx_fsm_set_vec3(c, 1, lx_v3(-v.y, v.x, 0));
}
static const LxFsmActionDesc A_V2_PERP = {"Vector2 Perpendicular", "Vector2", "Turns a Vector2 90 degrees anticlockwise.", "perp {0} -> {1}",
    {P("Vector2", LX_FSM_VEC2, "1 0"), POUT("Store Result", LX_FSM_VEC2)}, 0, 0, LX_FSM_ONCE, v2_perpendicular, v2_perpendicular, NULL};

static void v2_snap_angle(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    float step = lx_fsm_float(c, 1);
    float len = sqrtf(v.x * v.x + v.y * v.y);
    float a = atan2f(v.y, v.x) * LX_RAD2DEG;
    a = fsm_snap(a, step) * LX_DEG2RAD;
    lx_fsm_set_vec3(c, 2, lx_v3(cosf(a) * len, sinf(a) * len, 0));
}
static const LxFsmActionDesc A_V2_SNAP_ANGLE = {"Vector2 Snap To Angle", "Vector2", "Rounds a direction to the nearest multiple of an angle: eight-way movement, for example.",
    "snap {0} to {1} deg -> {2}",
    {P("Vector2", LX_FSM_VEC2, "1 0"), P("Angle Step", LX_FSM_FLOAT, "45"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, v2_snap_angle, v2_snap_angle, NULL};

/* ==========================================================================================
 *  Quaternion
 * ========================================================================================== */
static void q_set(LxFsmCtx *c) { lx_fsm_set_quat(c, 1, lx_fsm_quat(c, 0)); }
static const LxFsmActionDesc A_Q_SET = {"Quaternion Set Value", "Quaternion", "Stores a rotation in a Quaternion variable.", "{1} = {0}",
    {P("Value", LX_FSM_QUAT, "0 0 0"), POUT("Store Result", LX_FSM_QUAT)}, 0, 0, LX_FSM_ONCE, q_set, q_set, NULL};

static void q_identity(LxFsmCtx *c) { lx_fsm_set_quat(c, 0, lx_quat_identity()); }
static const LxFsmActionDesc A_Q_IDENTITY = {"Quaternion Identity", "Quaternion", "The rotation that turns nothing at all.", "identity -> {0}",
    {POUT("Store Result", LX_FSM_QUAT)}, 0, 0, LX_FSM_ONCE, q_identity, q_identity, NULL};

static void q_euler(LxFsmCtx *c) { lx_fsm_set_quat(c, 1, lx_quat_euler_deg(lx_fsm_vec3(c, 0))); }
static const LxFsmActionDesc A_Q_EULER = {"Quaternion From Euler", "Quaternion", "Makes a rotation from angles in degrees around X, Y and Z.", "euler {0} -> {1}",
    {P("Euler Angles", LX_FSM_VEC3, "0 0 0"), POUT("Store Result", LX_FSM_QUAT)}, 0, 0, LX_FSM_ONCE, q_euler, q_euler, NULL};

static void q_to_euler(LxFsmCtx *c) {
    LxVec3 e = lx_quat_to_euler_deg(lx_fsm_quat(c, 0));
    lx_fsm_set_vec3(c, 1, e);
    lx_fsm_set_float(c, 2, e.x);
    lx_fsm_set_float(c, 3, e.y);
    lx_fsm_set_float(c, 4, e.z);
}
static const LxFsmActionDesc A_Q_TO_EULER = {"Quaternion Get Euler", "Quaternion", "Reads a rotation back as angles in degrees.", "{0} -> {1}",
    {P("Quaternion", LX_FSM_QUAT, "0 0 0"), POPT("Store Euler", LX_FSM_VEC3), POPT("Store X", LX_FSM_FLOAT),
     POPT("Store Y", LX_FSM_FLOAT), POPT("Store Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, q_to_euler, q_to_euler, NULL};

static void q_angle_axis(LxFsmCtx *c) {
    lx_fsm_set_quat(c, 2, lx_quat_axis_angle(lx_fsm_vec3(c, 1), lx_fsm_float(c, 0) * LX_DEG2RAD));
}
static const LxFsmActionDesc A_Q_ANGLE_AXIS = {"Quaternion Angle Axis", "Quaternion", "A rotation of so many degrees around an axis.", "{0} deg about {1} -> {2}",
    {P("Angle", LX_FSM_FLOAT, "90"), P("Axis", LX_FSM_VEC3, "0 1 0"), POUT("Store Result", LX_FSM_QUAT)},
    0, 0, LX_FSM_ONCE, q_angle_axis, q_angle_axis, NULL};

static void q_look(LxFsmCtx *c) {
    LxVec3 f = lx_norm3(lx_fsm_vec3(c, 0));
    if (lx_len3sq(f) < 1e-8f) f = lx_v3(0, 0, 1);
    LxVec3 up = lx_fsm_is_none(c, 1) ? lx_v3(0, 1, 0) : lx_norm3(lx_fsm_vec3(c, 1));
    lx_fsm_set_quat(c, 2, lx_quat_look(f, up));
}
static const LxFsmActionDesc A_Q_LOOK = {"Quaternion Look Rotation", "Quaternion", "The rotation that points forward along a direction.", "look along {0} -> {2}",
    {P("Forward", LX_FSM_VEC3, "0 0 1"), PIN_OPT("Up", LX_FSM_VEC3), POUT("Store Result", LX_FSM_QUAT)},
    0, 0, LX_FSM_ONCE, q_look, q_look, NULL};

static void q_from_to(LxFsmCtx *c) {
    LxVec3 a = lx_norm3(lx_fsm_vec3(c, 0)), b = lx_norm3(lx_fsm_vec3(c, 1));
    float d = lx_clampf(lx_dot3(a, b), -1, 1);
    LxQuat q;
    if (d > 0.99999f) {
        q = lx_quat_identity();
    } else if (d < -0.99999f) {
        LxVec3 t = fabsf(a.x) > 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0);
        q = lx_quat_axis_angle(lx_norm3(lx_cross3(t, a)), LX_PI);
    } else {
        q = lx_quat_axis_angle(lx_cross3(a, b), acosf(d));
    }
    lx_fsm_set_quat(c, 2, q);
}
static const LxFsmActionDesc A_Q_FROM_TO = {"Quaternion From To Rotation", "Quaternion", "The rotation that turns one direction into another.", "{0} -> {1} = {2}",
    {P("From Direction", LX_FSM_VEC3, "0 0 1"), P("To Direction", LX_FSM_VEC3, "1 0 0"), POUT("Store Result", LX_FSM_QUAT)},
    0, 0, LX_FSM_ONCE, q_from_to, q_from_to, NULL};

static void q_multiply(LxFsmCtx *c) {
    lx_fsm_set_quat(c, 2, lx_quat_norm(lx_quat_mul(lx_fsm_quat(c, 0), lx_fsm_quat(c, 1))));
}
static const LxFsmActionDesc A_Q_MULTIPLY = {"Quaternion Multiply", "Quaternion", "Combines two rotations: the first, then the second.", "{0} * {1} -> {2}",
    {P("Quaternion 1", LX_FSM_QUAT, "0 0 0"), P("Quaternion 2", LX_FSM_QUAT, "0 0 0"), POUT("Store Result", LX_FSM_QUAT)},
    0, 0, LX_FSM_ONCE, q_multiply, q_multiply, NULL};

static void q_inverse(LxFsmCtx *c) { lx_fsm_set_quat(c, 1, lx_quat_conj(lx_quat_norm(lx_fsm_quat(c, 0)))); }
static const LxFsmActionDesc A_Q_INVERSE = {"Quaternion Inverse", "Quaternion", "The rotation that undoes another one.", "inverse {0} -> {1}",
    {P("Quaternion", LX_FSM_QUAT, "0 0 0"), POUT("Store Result", LX_FSM_QUAT)}, 0, 0, LX_FSM_ONCE, q_inverse, q_inverse, NULL};

static void q_rotate_vec(LxFsmCtx *c) {
    lx_fsm_set_vec3(c, 2, lx_quat_rotate(lx_quat_norm(lx_fsm_quat(c, 0)), lx_fsm_vec3(c, 1)));
}
static const LxFsmActionDesc A_Q_ROTATE_VEC = {"Quaternion Rotate Vector", "Quaternion", "Turns a Vector3 by a rotation.", "{0} * {1} -> {2}",
    {P("Quaternion", LX_FSM_QUAT, "0 0 0"), P("Vector3", LX_FSM_VEC3, "0 0 1"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, q_rotate_vec, q_rotate_vec, NULL};

static float fsm_q_dot(LxQuat a, LxQuat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
static LxQuat fsm_q_slerp(LxQuat a, LxQuat b, float t) {
    a = lx_quat_norm(a); b = lx_quat_norm(b);
    float d = fsm_q_dot(a, b);
    if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }
    if (d > 0.9995f) {
        LxQuat r = {lx_lerpf(a.x, b.x, t), lx_lerpf(a.y, b.y, t), lx_lerpf(a.z, b.z, t), lx_lerpf(a.w, b.w, t)};
        return lx_quat_norm(r);
    }
    float theta = acosf(lx_clampf(d, -1, 1)), s = sinf(theta);
    float wa = sinf((1 - t) * theta) / s, wb = sinf(t * theta) / s;
    LxQuat r = {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    return lx_quat_norm(r);
}

static void q_slerp(LxFsmCtx *c) {
    lx_fsm_set_quat(c, 3, fsm_q_slerp(lx_fsm_quat(c, 0), lx_fsm_quat(c, 1), lx_saturate(lx_fsm_float(c, 2))));
}
static const LxFsmActionDesc A_Q_SLERP = {"Quaternion Slerp", "Quaternion", "Blends between two rotations along the shortest arc: the usual way to turn smoothly.",
    "slerp {0}..{1} at {2} -> {3}",
    {P("From", LX_FSM_QUAT, "0 0 0"), P("To", LX_FSM_QUAT, "0 0 0"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_QUAT)},
    0, 0, LX_FSM_ONCE, q_slerp, q_slerp, NULL};

static void q_lerp(LxFsmCtx *c) {
    LxQuat a = lx_quat_norm(lx_fsm_quat(c, 0)), b = lx_quat_norm(lx_fsm_quat(c, 1));
    float t = lx_saturate(lx_fsm_float(c, 2));
    if (fsm_q_dot(a, b) < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    LxQuat r = {lx_lerpf(a.x, b.x, t), lx_lerpf(a.y, b.y, t), lx_lerpf(a.z, b.z, t), lx_lerpf(a.w, b.w, t)};
    lx_fsm_set_quat(c, 3, lx_quat_norm(r));
}
static const LxFsmActionDesc A_Q_LERP = {"Quaternion Lerp", "Quaternion", "Blends between two rotations the cheap way (fine for small turns).",
    "lerp {0}..{1} at {2} -> {3}",
    {P("From", LX_FSM_QUAT, "0 0 0"), P("To", LX_FSM_QUAT, "0 0 0"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_QUAT)},
    0, 0, LX_FSM_ONCE, q_lerp, q_lerp, NULL};

static void q_angle(LxFsmCtx *c) {
    float d = fabsf(fsm_q_dot(lx_quat_norm(lx_fsm_quat(c, 0)), lx_quat_norm(lx_fsm_quat(c, 1))));
    lx_fsm_set_float(c, 2, 2.0f * acosf(lx_clampf(d, -1, 1)) * LX_RAD2DEG);
}
static const LxFsmActionDesc A_Q_ANGLE = {"Quaternion Angle", "Quaternion", "The angle in degrees between two rotations.", "angle({0}, {1}) -> {2}",
    {P("Quaternion 1", LX_FSM_QUAT, "0 0 0"), P("Quaternion 2", LX_FSM_QUAT, "0 0 0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, q_angle, q_angle, NULL};

static void q_rotate_towards(LxFsmCtx *c) {
    LxQuat a = lx_quat_norm(lx_fsm_quat(c, 0)), b = lx_quat_norm(lx_fsm_quat(c, 1));
    float step = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) step *= c->dt;
    float total = 2.0f * acosf(lx_clampf(fabsf(fsm_q_dot(a, b)), -1, 1)) * LX_RAD2DEG;
    LxQuat r = (total <= step || total < 1e-5f) ? b : fsm_q_slerp(a, b, step / total);
    lx_fsm_set_quat(c, 4, r);
    if (total <= step) fsm_send(c, 5);
}
static const LxFsmActionDesc A_Q_ROTATE_TOWARDS = {"Quaternion Rotate Towards", "Quaternion", "Turns a rotation towards a target at so many degrees per second, never overshooting.",
    "{0} -> {1} by {2} deg",
    {P("Current", LX_FSM_QUAT, "0 0 0"), P("Target", LX_FSM_QUAT, "0 0 0"), P("Max Degrees", LX_FSM_FLOAT, "180"),
     P("Per Second", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_QUAT), PEV("Finish Event")},
    0, 0, LX_FSM_EVERY_FRAME, q_rotate_towards, q_rotate_towards, NULL};

static void q_dot(LxFsmCtx *c) {
    lx_fsm_set_float(c, 2, fsm_q_dot(lx_quat_norm(lx_fsm_quat(c, 0)), lx_quat_norm(lx_fsm_quat(c, 1))));
}
static const LxFsmActionDesc A_Q_DOT = {"Quaternion Dot", "Quaternion", "How closely two rotations agree: 1 identical, 0 a quarter turn apart.", "dot({0}, {1}) -> {2}",
    {P("Quaternion 1", LX_FSM_QUAT, "0 0 0"), P("Quaternion 2", LX_FSM_QUAT, "0 0 0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, q_dot, q_dot, NULL};

static void q_equals(LxFsmCtx *c) {
    float d = fabsf(fsm_q_dot(lx_quat_norm(lx_fsm_quat(c, 0)), lx_quat_norm(lx_fsm_quat(c, 1))));
    bool eq = (1.0f - d) <= lx_fsm_float(c, 2);
    lx_fsm_set_bool(c, 3, eq);
    fsm_send(c, eq ? 4 : 5);
}
static const LxFsmActionDesc A_Q_EQUALS = {"Quaternion Equals", "Quaternion", "Tests whether two rotations are the same, within a tolerance.", "{0} == {1}?",
    {P("Quaternion 1", LX_FSM_QUAT, "0 0 0"), P("Quaternion 2", LX_FSM_QUAT, "0 0 0"), P("Tolerance", LX_FSM_FLOAT, "0.0001"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, q_equals, q_equals, NULL};

static void q_get_axes(LxFsmCtx *c) {
    LxQuat q = lx_quat_norm(lx_fsm_quat(c, 0));
    lx_fsm_set_vec3(c, 1, lx_quat_rotate(q, lx_v3(0, 0, 1)));
    lx_fsm_set_vec3(c, 2, lx_quat_rotate(q, lx_v3(1, 0, 0)));
    lx_fsm_set_vec3(c, 3, lx_quat_rotate(q, lx_v3(0, 1, 0)));
}
static const LxFsmActionDesc A_Q_GET_AXES = {"Quaternion Get Axes", "Quaternion", "The forward, right and up directions of a rotation.", "axes of {0}",
    {P("Quaternion", LX_FSM_QUAT, "0 0 0"), POPT("Store Forward", LX_FSM_VEC3), POPT("Store Right", LX_FSM_VEC3), POPT("Store Up", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, q_get_axes, q_get_axes, NULL};

/* ==========================================================================================
 *  Rect
 * ========================================================================================== */
/* A Rect lives in the LxFsmValue's four-component field as (x, y, width, height) — the same place a
 * Color keeps its channels — so these go through lx_fsm_param_value rather than the Vector3 accessors,
 * which would lose the height. */
static void fsm_rect_get(LxFsmCtx *c, int p, float *x, float *y, float *w, float *h) {
    LxFsmValue *v = lx_fsm_param_value(c, p);
    if (!v) { *x = *y = *w = *h = 0; return; }
    *x = v->c.x; *y = v->c.y; *w = v->c.z; *h = v->c.w;
}
static void fsm_rect_set(LxFsmCtx *c, int p, float x, float y, float w, float h) {
    LxFsmValue *v = lx_fsm_param_value(c, p);
    if (!v) return;
    v->type = LX_FSM_RECT;
    v->c = lx_v4(x, y, w, h);
    v->v = lx_v3(x, y, 0);
}

static void rc_set(LxFsmCtx *c) {
    fsm_rect_set(c, 4, lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3));
}
static const LxFsmActionDesc A_RECT_SET = {"Rect Set Value", "Rect", "Builds a rectangle from its position and size.", "rect({0}, {1}, {2}, {3}) -> {4}",
    {P("X", LX_FSM_FLOAT, "0"), P("Y", LX_FSM_FLOAT, "0"), P("Width", LX_FSM_FLOAT, "1"), P("Height", LX_FSM_FLOAT, "1"),
     POUT("Store Result", LX_FSM_RECT)},
    0, 0, LX_FSM_ONCE, rc_set, rc_set, NULL};

static void rc_get(LxFsmCtx *c) {
    float x, y, w, h;
    fsm_rect_get(c, 0, &x, &y, &w, &h);
    lx_fsm_set_float(c, 1, x);
    lx_fsm_set_float(c, 2, y);
    lx_fsm_set_float(c, 3, w);
    lx_fsm_set_float(c, 4, h);
    lx_fsm_set_vec3(c, 5, lx_v3(x + w * 0.5f, y + h * 0.5f, 0));
}
static const LxFsmActionDesc A_RECT_GET = {"Rect Get Value", "Rect", "Reads a rectangle's position, size and centre.", "{0} -> x, y, w, h",
    {P("Rect", LX_FSM_RECT, "0 0 1 1"), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT),
     POPT("Store Width", LX_FSM_FLOAT), POPT("Store Height", LX_FSM_FLOAT), POPT("Store Center", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, rc_get, rc_get, NULL};

static void rc_contains(LxFsmCtx *c) {
    float x, y, w, h;
    fsm_rect_get(c, 0, &x, &y, &w, &h);
    LxVec3 p = lx_fsm_vec3(c, 1);
    bool in = p.x >= x && p.x <= x + w && p.y >= y && p.y <= y + h;
    lx_fsm_set_bool(c, 2, in);
    fsm_send(c, in ? 3 : 4);
}
static const LxFsmActionDesc A_RECT_CONTAINS = {"Rect Contains", "Rect", "True when a point is inside a rectangle.", "{1} in {0}?",
    {P("Rect", LX_FSM_RECT, "0 0 1 1"), P("Point", LX_FSM_VEC2, "0 0"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Inside Event"), PEV("Outside Event")},
    0, 0, LX_FSM_ONCE, rc_contains, rc_contains, NULL};

static void rc_overlaps(LxFsmCtx *c) {
    float ax, ay, aw, ah, bx, by, bw, bh;
    fsm_rect_get(c, 0, &ax, &ay, &aw, &ah);
    fsm_rect_get(c, 1, &bx, &by, &bw, &bh);
    bool hit = ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
    lx_fsm_set_bool(c, 2, hit);
    fsm_send(c, hit ? 3 : 4);
}
static const LxFsmActionDesc A_RECT_OVERLAPS = {"Rect Overlaps", "Rect", "True when two rectangles overlap.", "{0} overlaps {1}?",
    {P("Rect 1", LX_FSM_RECT, "0 0 1 1"), P("Rect 2", LX_FSM_RECT, "0 0 1 1"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Overlap Event"), PEV("No Overlap Event")},
    0, 0, LX_FSM_ONCE, rc_overlaps, rc_overlaps, NULL};

static void rc_point_to_normalized(LxFsmCtx *c) {
    float x, y, w, h;
    fsm_rect_get(c, 0, &x, &y, &w, &h);
    LxVec3 p = lx_fsm_vec3(c, 1);
    lx_fsm_set_vec3(c, 2, lx_v3(w != 0 ? (p.x - x) / w : 0, h != 0 ? (p.y - y) / h : 0, 0));
}
static const LxFsmActionDesc A_RECT_TO_NORM = {"Rect Point To Normalized", "Rect", "Where a point sits inside a rectangle, as 0..1 on each axis.",
    "{1} in {0} -> {2}",
    {P("Rect", LX_FSM_RECT, "0 0 1 1"), P("Point", LX_FSM_VEC2, "0 0"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, rc_point_to_normalized, rc_point_to_normalized, NULL};

static void rc_normalized_to_point(LxFsmCtx *c) {
    float x, y, w, h;
    fsm_rect_get(c, 0, &x, &y, &w, &h);
    LxVec3 n = lx_fsm_vec3(c, 1);
    lx_fsm_set_vec3(c, 2, lx_v3(x + n.x * w, y + n.y * h, 0));
}
static const LxFsmActionDesc A_RECT_FROM_NORM = {"Rect Normalized To Point", "Rect", "Turns a 0..1 position inside a rectangle into a real point.",
    "{1} of {0} -> {2}",
    {P("Rect", LX_FSM_RECT, "0 0 1 1"), P("Normalized", LX_FSM_VEC2, "0.5 0.5"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, rc_normalized_to_point, rc_normalized_to_point, NULL};

static void rc_random_point(LxFsmCtx *c) {
    float x, y, w, h;
    fsm_rect_get(c, 0, &x, &y, &w, &h);
    lx_fsm_set_vec3(c, 1, lx_v3(lx_random_range(x, x + w), lx_random_range(y, y + h), 0));
}
static const LxFsmActionDesc A_RECT_RANDOM = {"Rect Random Point", "Rect", "A random point inside a rectangle.", "random in {0} -> {1}",
    {P("Rect", LX_FSM_RECT, "0 0 1 1"), POUT("Store Result", LX_FSM_VEC2)}, 0, 0, LX_FSM_ONCE, rc_random_point, rc_random_point, NULL};

/* ==========================================================================================
 *  Bounds — a centre and a size, both Vector3
 * ========================================================================================== */
static void bd_contains(LxFsmCtx *c) {
    LxVec3 cen = lx_fsm_vec3(c, 0), half = lx_scale3(lx_fsm_vec3(c, 1), 0.5f), p = lx_fsm_vec3(c, 2);
    LxVec3 d = lx_sub3(p, cen);
    bool in = fabsf(d.x) <= half.x && fabsf(d.y) <= half.y && fabsf(d.z) <= half.z;
    lx_fsm_set_bool(c, 3, in);
    fsm_send(c, in ? 4 : 5);
}
static const LxFsmActionDesc A_BOUNDS_CONTAINS = {"Bounds Contains", "Bounds", "True when a point is inside a box.", "{2} in box?",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "1 1 1"), P("Point", LX_FSM_VEC3, "0 0 0"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Inside Event"), PEV("Outside Event")},
    0, 0, LX_FSM_ONCE, bd_contains, bd_contains, NULL};

static void bd_intersects(LxFsmCtx *c) {
    LxVec3 c1 = lx_fsm_vec3(c, 0), h1 = lx_scale3(lx_fsm_vec3(c, 1), 0.5f);
    LxVec3 c2 = lx_fsm_vec3(c, 2), h2 = lx_scale3(lx_fsm_vec3(c, 3), 0.5f);
    bool hit = fabsf(c1.x - c2.x) <= h1.x + h2.x && fabsf(c1.y - c2.y) <= h1.y + h2.y && fabsf(c1.z - c2.z) <= h1.z + h2.z;
    lx_fsm_set_bool(c, 4, hit);
    fsm_send(c, hit ? 5 : 6);
}
static const LxFsmActionDesc A_BOUNDS_INTERSECTS = {"Bounds Intersects", "Bounds", "True when two boxes overlap.", "boxes overlap?",
    {P("Center 1", LX_FSM_VEC3, "0 0 0"), P("Size 1", LX_FSM_VEC3, "1 1 1"), P("Center 2", LX_FSM_VEC3, "0 0 0"), P("Size 2", LX_FSM_VEC3, "1 1 1"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Overlap Event"), PEV("No Overlap Event")},
    0, 0, LX_FSM_ONCE, bd_intersects, bd_intersects, NULL};

static void bd_closest(LxFsmCtx *c) {
    LxVec3 cen = lx_fsm_vec3(c, 0), half = lx_scale3(lx_fsm_vec3(c, 1), 0.5f), p = lx_fsm_vec3(c, 2);
    LxVec3 r = lx_v3(lx_clampf(p.x, cen.x - half.x, cen.x + half.x),
                     lx_clampf(p.y, cen.y - half.y, cen.y + half.y),
                     lx_clampf(p.z, cen.z - half.z, cen.z + half.z));
    lx_fsm_set_vec3(c, 3, r);
    lx_fsm_set_float(c, 4, lx_len3(lx_sub3(p, r)));
}
static const LxFsmActionDesc A_BOUNDS_CLOSEST = {"Bounds Closest Point", "Bounds", "The point on (or inside) a box nearest a position, and how far away it is.",
    "closest in box to {2} -> {3}",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "1 1 1"), P("Point", LX_FSM_VEC3, "0 0 0"),
     POUT("Store Point", LX_FSM_VEC3), POPT("Store Distance", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, bd_closest, bd_closest, NULL};

static void bd_minmax(LxFsmCtx *c) {
    LxVec3 cen = lx_fsm_vec3(c, 0), half = lx_scale3(lx_fsm_vec3(c, 1), 0.5f);
    lx_fsm_set_vec3(c, 2, lx_sub3(cen, half));
    lx_fsm_set_vec3(c, 3, lx_add3(cen, half));
    lx_fsm_set_vec3(c, 4, half);
}
static const LxFsmActionDesc A_BOUNDS_MINMAX = {"Bounds Get Min Max", "Bounds", "The lowest and highest corners of a box, and half its size.", "box corners",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "1 1 1"), POPT("Store Min", LX_FSM_VEC3),
     POPT("Store Max", LX_FSM_VEC3), POPT("Store Extents", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, bd_minmax, bd_minmax, NULL};

static void bd_from_minmax(LxFsmCtx *c) {
    LxVec3 lo = lx_fsm_vec3(c, 0), hi = lx_fsm_vec3(c, 1);
    lx_fsm_set_vec3(c, 2, lx_scale3(lx_add3(lo, hi), 0.5f));
    lx_fsm_set_vec3(c, 3, lx_sub3(hi, lo));
}
static const LxFsmActionDesc A_BOUNDS_FROM_MINMAX = {"Bounds From Min Max", "Bounds", "Builds a box from its lowest and highest corners.", "box from {0}..{1}",
    {P("Min", LX_FSM_VEC3, "0 0 0"), P("Max", LX_FSM_VEC3, "1 1 1"), POUT("Store Center", LX_FSM_VEC3), POUT("Store Size", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, bd_from_minmax, bd_from_minmax, NULL};

static void bd_encapsulate(LxFsmCtx *c) {
    LxVec3 cen = lx_fsm_vec3(c, 0), half = lx_scale3(lx_fsm_vec3(c, 1), 0.5f), p = lx_fsm_vec3(c, 2);
    LxVec3 lo = lx_min3(lx_sub3(cen, half), p), hi = lx_max3(lx_add3(cen, half), p);
    lx_fsm_set_vec3(c, 0, lx_scale3(lx_add3(lo, hi), 0.5f));
    lx_fsm_set_vec3(c, 1, lx_sub3(hi, lo));
}
static const LxFsmActionDesc A_BOUNDS_ENCAPSULATE = {"Bounds Encapsulate", "Bounds", "Grows a box so it also contains a point.", "grow box to hold {2}",
    {POUT("Center", LX_FSM_VEC3), POUT("Size", LX_FSM_VEC3), P("Point", LX_FSM_VEC3, "0 0 0")},
    0, 0, LX_FSM_ONCE, bd_encapsulate, bd_encapsulate, NULL};

static void bd_expand(LxFsmCtx *c) {
    lx_fsm_set_vec3(c, 1, lx_add3(lx_fsm_vec3(c, 1), lx_v3s(lx_fsm_float(c, 2) * 2.0f)));
}
static const LxFsmActionDesc A_BOUNDS_EXPAND = {"Bounds Expand", "Bounds", "Pushes a box's walls outwards by the same amount on every side.", "expand box by {2}",
    {P("Center", LX_FSM_VEC3, "0 0 0"), POUT("Size", LX_FSM_VEC3), P("Amount", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, bd_expand, bd_expand, NULL};

static void bd_random(LxFsmCtx *c) {
    LxVec3 cen = lx_fsm_vec3(c, 0), half = lx_scale3(lx_fsm_vec3(c, 1), 0.5f);
    lx_fsm_set_vec3(c, 2, lx_v3(lx_random_range(cen.x - half.x, cen.x + half.x),
                                lx_random_range(cen.y - half.y, cen.y + half.y),
                                lx_random_range(cen.z - half.z, cen.z + half.z)));
}
static const LxFsmActionDesc A_BOUNDS_RANDOM = {"Bounds Random Point", "Bounds", "A random point inside a box.", "random in box -> {2}",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "1 1 1"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, bd_random, bd_random, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_vector(void) {
    static const LxFsmActionDesc *all[] = {
        &A_V3_SET, &A_V3_SET_XYZ, &A_V3_GET_XYZ, &A_V3_GET_AXIS, &A_V3_SET_AXIS, &A_V3_OPERATOR,
        &A_V3_ADD, &A_V3_SUBTRACT, &A_V3_SCALE, &A_V3_NEGATE, &A_V3_NORMALIZE, &A_V3_MAGNITUDE,
        &A_V3_CLAMP_MAG, &A_V3_DISTANCE, &A_V3_DOT, &A_V3_CROSS, &A_V3_ANGLE, &A_V3_SIGNED_ANGLE,
        &A_V3_DIRECTION, &A_V3_LERP, &A_V3_SLERP, &A_V3_MOVE_TOWARDS, &A_V3_SMOOTH_DAMP,
        &A_V3_PROJECT, &A_V3_PROJECT_PLANE, &A_V3_REFLECT, &A_V3_ROTATE, &A_V3_SNAP_GRID,
        &A_V3_EQUALS, &A_V3_SELECT, &A_V3_CONSTANT, &A_V3_LIST_SUM,

        &A_V2_SET, &A_V2_SET_XY, &A_V2_GET_XY, &A_V2_OPERATOR, &A_V2_SCALE, &A_V2_NORMALIZE,
        &A_V2_MAGNITUDE, &A_V2_DISTANCE, &A_V2_DOT, &A_V2_ANGLE, &A_V2_LERP, &A_V2_MOVE_TOWARDS,
        &A_V2_FROM_ANGLE, &A_V2_PERP, &A_V2_SNAP_ANGLE,

        &A_Q_SET, &A_Q_IDENTITY, &A_Q_EULER, &A_Q_TO_EULER, &A_Q_ANGLE_AXIS, &A_Q_LOOK, &A_Q_FROM_TO,
        &A_Q_MULTIPLY, &A_Q_INVERSE, &A_Q_ROTATE_VEC, &A_Q_SLERP, &A_Q_LERP, &A_Q_ANGLE,
        &A_Q_ROTATE_TOWARDS, &A_Q_DOT, &A_Q_EQUALS, &A_Q_GET_AXES,

        &A_RECT_SET, &A_RECT_GET, &A_RECT_CONTAINS, &A_RECT_OVERLAPS, &A_RECT_TO_NORM,
        &A_RECT_FROM_NORM, &A_RECT_RANDOM,

        &A_BOUNDS_CONTAINS, &A_BOUNDS_INTERSECTS, &A_BOUNDS_CLOSEST, &A_BOUNDS_MINMAX,
        &A_BOUNDS_FROM_MINMAX, &A_BOUNDS_ENCAPSULATE, &A_BOUNDS_EXPAND, &A_BOUNDS_RANDOM,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
