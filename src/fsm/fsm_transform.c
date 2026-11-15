/* Where things are: the Transform, Tween and Animate Variables categories.
 *
 * Every action takes a Space setting where it matters: Self is the object's own space (relative to its
 * parent, or along its own facing for directions), World is the scene's. One action covers both spaces
 * and the per-axis cases, rather than a separate action for each combination. */
#include "fsm_actions.h"

static LxVec3 fsm_pos(LxScene *s, LxEntity e, bool world) {
    return world ? lx_entity_world_position(s, e) : lx_entity_position(s, e);
}
static void fsm_set_pos(LxScene *s, LxEntity e, LxVec3 p, bool world) {
    if (world) lx_entity_set_world_pose(s, e, p, lx_entity_world_rotation(s, e));
    else lx_entity_set_position(s, e, p);
}

/* ==========================================================================================
 *  position
 * ========================================================================================== */
static void tr_get_position(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 p = fsm_pos(c->scene, e, lx_fsm_int(c, 1) == 1);
    lx_fsm_set_vec3(c, 2, p);
    lx_fsm_set_float(c, 3, p.x);
    lx_fsm_set_float(c, 4, p.y);
    lx_fsm_set_float(c, 5, p.z);
}
static const LxFsmActionDesc A_TR_GET_POS = {"Get Position", "Transform", "Where an object is, as a Vector3 and as separate X, Y and Z.",
    "position of {0} -> {2}",
    {POBJ("Object"), PENUMT("Space", FSM_SPACE, "1", "Self is relative to the parent; World is the scene"),
     POPT("Store Position", LX_FSM_VEC3), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT), POPT("Store Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, tr_get_position, tr_get_position, NULL};

static void tr_set_position(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    bool world = lx_fsm_int(c, 5) == 1;
    LxVec3 p = lx_fsm_is_none(c, 1) ? fsm_pos(c->scene, e, world) : lx_fsm_vec3(c, 1);
    if (!lx_fsm_is_none(c, 2)) p.x = lx_fsm_float(c, 2);
    if (!lx_fsm_is_none(c, 3)) p.y = lx_fsm_float(c, 3);
    if (!lx_fsm_is_none(c, 4)) p.z = lx_fsm_float(c, 4);
    fsm_set_pos(c->scene, e, p, world);
}
static const LxFsmActionDesc A_TR_SET_POS = {"Set Position", "Transform", "Moves an object to a position. Set only X, Y or Z to change one axis and leave the rest.",
    "{0}.position = {1}",
    {POBJ("Object"), PIN_OPT("Position", LX_FSM_VEC3), PIN_OPT("X", LX_FSM_FLOAT), PIN_OPT("Y", LX_FSM_FLOAT), PIN_OPT("Z", LX_FSM_FLOAT),
     PENUM("Space", FSM_SPACE, "1")},
    0, 0, LX_FSM_ONCE, tr_set_position, tr_set_position, NULL};

static void tr_translate(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 d = lx_fsm_vec3(c, 1);
    if (lx_fsm_bool(c, 3)) d = lx_scale3(d, c->dt);
    if (lx_fsm_int(c, 2) == 0) d = lx_entity_transform_direction(c->scene, e, d);   /* along its own facing */
    lx_entity_set_position(c->scene, e, lx_add3(lx_entity_position(c->scene, e), d));
}
static const LxFsmActionDesc A_TR_TRANSLATE = {"Translate", "Transform", "Nudges an object along a direction. Self moves it the way it is facing.",
    "move {0} by {1}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "0 0 1"), PENUM("Space", FSM_SPACE, "0"),
     PT("Per Second", LX_FSM_BOOL, "true", "treat the offset as a speed, so it is frame rate independent")},
    0, 0, LX_FSM_EVERY_FRAME, tr_translate, tr_translate, NULL};

static void tr_clamp_position(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 p = lx_entity_position(c->scene, e), lo = lx_fsm_vec3(c, 1), hi = lx_fsm_vec3(c, 2);
    LxVec3 r = lx_v3(lx_clampf(p.x, lo.x, hi.x), lx_clampf(p.y, lo.y, hi.y), lx_clampf(p.z, lo.z, hi.z));
    bool hit = lx_len3sq(lx_sub3(r, p)) > 1e-10f;
    if (hit) lx_entity_set_position(c->scene, e, r);
    lx_fsm_set_bool(c, 3, hit);
    if (hit) fsm_send(c, 4);
}
static const LxFsmActionDesc A_TR_CLAMP_POS = {"Clamp Position", "Transform", "Keeps an object inside a box: play areas, camera limits.",
    "clamp {0} to {1}..{2}",
    {POBJ("Object"), P("Min", LX_FSM_VEC3, "-10 -10 -10"), P("Max", LX_FSM_VEC3, "10 10 10"),
     POPT("Store Was Clamped", LX_FSM_BOOL), PEV("Hit Limit Event")},
    0, 0, LX_FSM_EVERY_FRAME, tr_clamp_position, tr_clamp_position, NULL};

static void tr_snap_grid(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 p = lx_entity_position(c->scene, e), g = lx_fsm_vec3(c, 1);
    LxVec3 r = lx_v3(g.x > 0 ? floorf(p.x / g.x + 0.5f) * g.x : p.x,
                     g.y > 0 ? floorf(p.y / g.y + 0.5f) * g.y : p.y,
                     g.z > 0 ? floorf(p.z / g.z + 0.5f) * g.z : p.z);
    lx_entity_set_position(c->scene, e, r);
}
static const LxFsmActionDesc A_TR_SNAP_GRID = {"Snap To Grid", "Transform", "Rounds an object's position onto a grid: tile placement, building games.",
    "snap {0} to {1}",
    {POBJ("Object"), PT("Grid Size", LX_FSM_VEC3, "1 0 1", "0 on an axis leaves that axis alone")},
    0, 0, LX_FSM_ONCE, tr_snap_grid, tr_snap_grid, NULL};

/* ==========================================================================================
 *  rotation
 * ========================================================================================== */
static void tr_get_rotation(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    bool world = lx_fsm_int(c, 1) == 1;
    LxQuat q = world ? lx_entity_world_rotation(c->scene, e) : lx_entity_rotation(c->scene, e);
    LxVec3 euler = world ? lx_quat_to_euler_deg(q) : lx_entity_euler(c->scene, e);
    lx_fsm_set_quat(c, 2, q);
    lx_fsm_set_vec3(c, 3, euler);
    lx_fsm_set_float(c, 4, euler.x);
    lx_fsm_set_float(c, 5, euler.y);
    lx_fsm_set_float(c, 6, euler.z);
}
static const LxFsmActionDesc A_TR_GET_ROT = {"Get Rotation", "Transform", "How an object is turned, as a rotation and as angles in degrees.",
    "rotation of {0}",
    {POBJ("Object"), PENUM("Space", FSM_SPACE, "1"), POPT("Store Rotation", LX_FSM_QUAT), POPT("Store Euler", LX_FSM_VEC3),
     POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT), POPT("Store Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, tr_get_rotation, tr_get_rotation, NULL};

static void tr_set_rotation(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    bool world = lx_fsm_int(c, 6) == 1;
    LxVec3 euler;
    if (!lx_fsm_is_none(c, 1)) euler = lx_quat_to_euler_deg(lx_fsm_quat(c, 1));
    else if (!lx_fsm_is_none(c, 2)) euler = lx_fsm_vec3(c, 2);
    else euler = world ? lx_quat_to_euler_deg(lx_entity_world_rotation(c->scene, e)) : lx_entity_euler(c->scene, e);
    if (!lx_fsm_is_none(c, 3)) euler.x = lx_fsm_float(c, 3);
    if (!lx_fsm_is_none(c, 4)) euler.y = lx_fsm_float(c, 4);
    if (!lx_fsm_is_none(c, 5)) euler.z = lx_fsm_float(c, 5);
    if (world) lx_entity_set_world_pose(c->scene, e, lx_entity_world_position(c->scene, e), lx_quat_euler_deg(euler));
    else lx_entity_set_euler(c->scene, e, euler);
}
static const LxFsmActionDesc A_TR_SET_ROT = {"Set Rotation", "Transform", "Turns an object to face a given way. Set only one axis to change just that one.",
    "{0}.rotation = {2}",
    {POBJ("Object"), PIN_OPT("Rotation", LX_FSM_QUAT), PIN_OPT("Euler Angles", LX_FSM_VEC3),
     PIN_OPT("X", LX_FSM_FLOAT), PIN_OPT("Y", LX_FSM_FLOAT), PIN_OPT("Z", LX_FSM_FLOAT), PENUM("Space", FSM_SPACE, "1")},
    0, 0, LX_FSM_ONCE, tr_set_rotation, tr_set_rotation, NULL};

static void tr_rotate(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 d = lx_fsm_vec3(c, 1);
    if (lx_fsm_bool(c, 3)) d = lx_scale3(d, c->dt);
    LxQuat delta = lx_quat_euler_deg(d);
    LxQuat cur = lx_entity_rotation(c->scene, e);
    /* Self turns about its own axes, World about the scene's */
    lx_entity_set_rotation(c->scene, e, lx_quat_norm(lx_fsm_int(c, 2) == 0 ? lx_quat_mul(cur, delta) : lx_quat_mul(delta, cur)));
}
static const LxFsmActionDesc A_TR_ROTATE = {"Rotate", "Transform", "Spins an object by so many degrees per second: turrets, pickups, propellers.",
    "rotate {0} by {1}",
    {POBJ("Object"), P("Degrees", LX_FSM_VEC3, "0 90 0"), PENUM("Space", FSM_SPACE, "0"), P("Per Second", LX_FSM_BOOL, "true")},
    0, 0, LX_FSM_EVERY_FRAME, tr_rotate, tr_rotate, NULL};

static void tr_rotate_around(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxEntity pivot_obj = fsm_obj(c, 1);
    LxVec3 pivot = pivot_obj ? lx_entity_world_position(c->scene, pivot_obj) : lx_fsm_vec3(c, 2);
    float deg = lx_fsm_float(c, 4);
    if (lx_fsm_bool(c, 5)) deg *= c->dt;
    LxQuat q = lx_quat_axis_angle(lx_fsm_vec3(c, 3), deg * LX_DEG2RAD);
    LxVec3 p = lx_entity_world_position(c->scene, e);
    LxVec3 moved = lx_add3(pivot, lx_quat_rotate(q, lx_sub3(p, pivot)));
    LxQuat rot = lx_fsm_bool(c, 6) ? lx_quat_norm(lx_quat_mul(q, lx_entity_world_rotation(c->scene, e)))
                                   : lx_entity_world_rotation(c->scene, e);
    lx_entity_set_world_pose(c->scene, e, moved, rot);
}
static const LxFsmActionDesc A_TR_ROTATE_AROUND = {"Rotate Around", "Transform", "Swings an object around a point: orbiting cameras, moons, circling enemies.",
    "{0} orbits {1}",
    {POBJ("Object"), POBJNT("Around Object", "the point to orbit; None uses the position below"),
     P("Around Position", LX_FSM_VEC3, "0 0 0"), P("Axis", LX_FSM_VEC3, "0 1 0"), P("Degrees", LX_FSM_FLOAT, "45"),
     P("Per Second", LX_FSM_BOOL, "true"), PT("Turn With It", LX_FSM_BOOL, "false", "also spin the object itself")},
    0, 0, LX_FSM_EVERY_FRAME, tr_rotate_around, tr_rotate_around, NULL};

static void tr_look_at(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxEntity t = fsm_obj(c, 1);
    LxVec3 target = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 2);
    target = lx_add3(target, lx_fsm_vec3(c, 3));
    LxVec3 from = lx_entity_world_position(c->scene, e);
    LxVec3 dir = lx_sub3(target, from);
    if (lx_fsm_bool(c, 4)) dir.y = 0;
    if (lx_len3sq(dir) < 1e-10f) return;
    float speed = lx_fsm_float(c, 5);
    float left = lx_entity_turn_towards(c->scene, e, dir, lx_v3(0, 1, 0), lx_fsm_bool(c, 4), speed, c->dt);
    lx_fsm_set_float(c, 6, left);
    if (left <= lx_fsm_float(c, 7)) fsm_send(c, 8);
}
static const LxFsmActionDesc A_TR_LOOK_AT = {"Look At", "Transform", "Turns an object to face another, instantly or at a turn speed.",
    "{0} looks at {1}",
    {POBJ("Object"), POBJNT("Target", "None uses the position below"), P("Target Position", LX_FSM_VEC3, "0 0 0"),
     PT("Offset", LX_FSM_VEC3, "0 0 0", "added to the target, e.g. 0 1.6 0 to look at the head"),
     PT("Keep Upright", LX_FSM_BOOL, "true", "turn left and right only, never tip over"),
     PT("Turn Speed", LX_FSM_FLOAT, "0", "degrees per second; 0 = snap straight round"),
     POPT("Store Angle Left", LX_FSM_FLOAT), PT("Facing Tolerance", LX_FSM_FLOAT, "1", "degrees that still count as facing it"),
     PEV("Facing Event")},
    0, 0, LX_FSM_EVERY_FRAME, tr_look_at, tr_look_at, NULL};

static void tr_align(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 dir = lx_fsm_vec3(c, 1);
    if (lx_len3sq(dir) < 1e-10f) return;
    lx_entity_turn_towards(c->scene, e, dir, lx_fsm_is_none(c, 2) ? lx_v3(0, 1, 0) : lx_fsm_vec3(c, 2),
                           lx_fsm_bool(c, 3), lx_fsm_float(c, 4), c->dt);
}
static const LxFsmActionDesc A_TR_ALIGN = {"Align To Direction", "Transform", "Points an object's forward along a direction: facing the way it is moving, lying along a slope.",
    "{0} aligns to {1}",
    {POBJ("Object"), P("Direction", LX_FSM_VEC3, "0 0 1"), PIN_OPT("Up", LX_FSM_VEC3),
     P("Keep Upright", LX_FSM_BOOL, "false"), PT("Turn Speed", LX_FSM_FLOAT, "0", "degrees per second; 0 = at once")},
    0, 0, LX_FSM_EVERY_FRAME, tr_align, tr_align, NULL};

static void tr_face_camera(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_entity_face_camera(c->scene, e, lx_fsm_bool(c, 1));
}
static const LxFsmActionDesc A_TR_FACE_CAMERA = {"Face Camera", "Transform", "Keeps an object turned towards the camera: sprites, name tags, health bars.",
    "{0} faces the camera",
    {POBJ("Object"), PT("Upright", LX_FSM_BOOL, "true", "turn left and right only")},
    0, 0, LX_FSM_LATE, tr_face_camera, tr_face_camera, NULL};

/* ==========================================================================================
 *  scale
 * ========================================================================================== */
static void tr_get_scale(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 s = lx_entity_scale(c->scene, e);
    lx_fsm_set_vec3(c, 1, s);
    lx_fsm_set_float(c, 2, s.x);
    lx_fsm_set_float(c, 3, s.y);
    lx_fsm_set_float(c, 4, s.z);
}
static const LxFsmActionDesc A_TR_GET_SCALE = {"Get Scale", "Transform", "How big an object is.", "scale of {0} -> {1}",
    {POBJ("Object"), POPT("Store Scale", LX_FSM_VEC3), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT), POPT("Store Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, tr_get_scale, tr_get_scale, NULL};

static void tr_set_scale(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 s = lx_fsm_is_none(c, 1) ? lx_entity_scale(c->scene, e) : lx_fsm_vec3(c, 1);
    if (!lx_fsm_is_none(c, 2)) s = lx_v3s(lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) s.x = lx_fsm_float(c, 3);
    if (!lx_fsm_is_none(c, 4)) s.y = lx_fsm_float(c, 4);
    if (!lx_fsm_is_none(c, 5)) s.z = lx_fsm_float(c, 5);
    lx_entity_set_scale(c->scene, e, s);
}
static const LxFsmActionDesc A_TR_SET_SCALE = {"Set Scale", "Transform", "Resizes an object. Uniform sets all three axes at once.", "{0}.scale = {1}",
    {POBJ("Object"), PIN_OPT("Scale", LX_FSM_VEC3), PT("Uniform", LX_FSM_FLOAT, "", "one number for all three axes"),
     PIN_OPT("X", LX_FSM_FLOAT), PIN_OPT("Y", LX_FSM_FLOAT), PIN_OPT("Z", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, tr_set_scale, tr_set_scale, NULL};

/* ==========================================================================================
 *  directions and spaces
 * ========================================================================================== */
static void tr_get_axes(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    lx_fsm_set_vec3(c, 1, lx_entity_forward(c->scene, e));
    lx_fsm_set_vec3(c, 2, lx_entity_right(c->scene, e));
    LxVec3 up = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), lx_v3(0, 1, 0));
    lx_fsm_set_vec3(c, 3, up);
}
static const LxFsmActionDesc A_TR_GET_AXES = {"Get Direction Vectors", "Transform", "Which way an object is facing: its forward, right and up.",
    "axes of {0}",
    {POBJ("Object"), POPT("Store Forward", LX_FSM_VEC3), POPT("Store Right", LX_FSM_VEC3), POPT("Store Up", LX_FSM_VEC3)},
    0, 0, LX_FSM_EVERY_FRAME, tr_get_axes, tr_get_axes, NULL};

static void tr_transform_point(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxVec3 v = lx_fsm_vec3(c, 1);
    bool to_world = lx_fsm_int(c, 2) == 0;
    bool is_dir = lx_fsm_bool(c, 3);
    LxVec3 r;
    if (to_world) r = is_dir ? lx_entity_transform_direction(c->scene, e, v) : lx_entity_transform_point(c->scene, e, v);
    else r = is_dir ? lx_entity_inverse_transform_direction(c->scene, e, v) : lx_entity_inverse_transform_point(c->scene, e, v);
    lx_fsm_set_vec3(c, 4, r);
}
static const LxFsmActionDesc A_TR_TRANSFORM_POINT = {"Transform Point", "Transform", "Converts a point or a direction between an object's own space and the world.",
    "{1} {2} of {0} -> {4}",
    {POBJ("Object"), P("Point", LX_FSM_VEC3, "0 0 1"), PENUM("Convert", "Local To World|World To Local", "0"),
     PT("Is Direction", LX_FSM_BOOL, "false", "directions ignore the object's position"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, tr_transform_point, tr_transform_point, NULL};

static void tr_distance(LxFsmCtx *c) {
    LxEntity a = fsm_obj(c, 0), b = fsm_obj(c, 1);
    if (!a) return;
    LxVec3 pa = lx_entity_world_position(c->scene, a);
    LxVec3 pb = b ? lx_entity_world_position(c->scene, b) : lx_fsm_vec3(c, 2);
    LxVec3 d = lx_sub3(pb, pa);
    if (lx_fsm_bool(c, 3)) d.y = 0;
    lx_fsm_set_float(c, 4, lx_len3(d));
    lx_fsm_set_vec3(c, 5, lx_norm3(d));
}
static const LxFsmActionDesc A_TR_DISTANCE = {"Get Distance", "Transform", "How far apart two objects are, and the direction from one to the other.",
    "{0} to {1} -> {4}",
    {POBJ("Object"), POBJN("Target"), P("Target Position", LX_FSM_VEC3, "0 0 0"),
     PT("Ignore Height", LX_FSM_BOOL, "false", "measure along the ground"),
     POPT("Store Distance", LX_FSM_FLOAT), POPT("Store Direction", LX_FSM_VEC3)},
    0, 0, LX_FSM_EVERY_FRAME, tr_distance, tr_distance, NULL};

/* ==========================================================================================
 *  smooth motion
 * ========================================================================================== */
static void tr_move_towards(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxEntity t = fsm_obj(c, 1);
    LxVec3 target = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 2);
    LxVec3 p = lx_entity_world_position(c->scene, e);
    LxVec3 d = lx_sub3(target, p);
    if (lx_fsm_bool(c, 4)) d.y = 0;
    float dist = lx_len3(d);
    float stop = lx_fsm_float(c, 5);
    lx_fsm_set_float(c, 6, dist);
    if (dist <= stop) { fsm_send(c, 7); return; }
    float step = lx_fsm_float(c, 3) * c->dt;
    if (step > dist - stop) step = dist - stop;
    lx_entity_set_world_pose(c->scene, e, lx_madd3(p, lx_norm3(d), step), lx_entity_world_rotation(c->scene, e));
}
static const LxFsmActionDesc A_TR_MOVE_TOWARDS = {"Move Towards", "Transform", "Slides an object towards another at a steady speed and stops when it arrives.",
    "{0} -> {1} at {3}",
    {POBJ("Object"), POBJNT("Target", "None uses the position below"), P("Target Position", LX_FSM_VEC3, "0 0 0"),
     P("Speed", LX_FSM_FLOAT, "4"), PT("Ignore Height", LX_FSM_BOOL, "false", "do not move up or down"),
     PT("Stop Distance", LX_FSM_FLOAT, "0.1", "how close counts as arrived"),
     POPT("Store Distance", LX_FSM_FLOAT), PEV("Arrived Event")},
    0, 0, LX_FSM_EVERY_FRAME, tr_move_towards, tr_move_towards, NULL};

typedef struct { LxVec3 vel; } FsmSmooth;
static float fsm_sd(float cur, float target, float *vel, float st, float dt) {
    st = lx_maxf(0.0001f, st);
    float omega = 2.0f / st, x = omega * dt;
    float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = cur - target, t = cur - change;
    float temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * e;
    return t + (change + temp) * e;
}
static void tr_smooth_follow(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    FsmSmooth *s = c->state;
    LxEntity t = fsm_obj(c, 1);
    LxVec3 target = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 2);
    target = lx_add3(target, lx_fsm_vec3(c, 3));
    LxVec3 p = lx_entity_world_position(c->scene, e);
    float st = lx_fsm_float(c, 4);
    LxVec3 r = lx_v3(fsm_sd(p.x, target.x, &s->vel.x, st, c->dt),
                     fsm_sd(p.y, target.y, &s->vel.y, st, c->dt),
                     fsm_sd(p.z, target.z, &s->vel.z, st, c->dt));
    lx_entity_set_world_pose(c->scene, e, r, lx_entity_world_rotation(c->scene, e));
}
static const LxFsmActionDesc A_TR_SMOOTH_FOLLOW = {"Smooth Follow", "Transform", "Eases an object towards a target like a spring: the usual way to make a camera follow a player.",
    "{0} follows {1}",
    {POBJ("Object"), POBJNT("Target", "None uses the position below"), P("Target Position", LX_FSM_VEC3, "0 0 0"),
     PT("Offset", LX_FSM_VEC3, "0 0 0", "where to sit relative to the target"),
     PT("Smooth Time", LX_FSM_FLOAT, "0.3", "roughly how long it takes to catch up")},
    sizeof(FsmSmooth), 0, LX_FSM_LATE, tr_smooth_follow, tr_smooth_follow, NULL};

typedef struct { int tween; bool done; } FsmTween;
static void tr_move_to_enter(LxFsmCtx *c) {
    FsmTween *w = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) { lx_fsm_finish(c); return; }
    LxEntity t = fsm_obj(c, 1);
    LxVec3 target = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 2);
    if (!t && lx_fsm_bool(c, 6)) target = lx_add3(lx_entity_world_position(c->scene, e), target);   /* an offset, not a place */
    w->tween = lx_tween_position(c->scene, e, target, lx_maxf(0.0001f, lx_fsm_float(c, 3)), fsm_ease(lx_fsm_int(c, 4)));
}
static void tr_move_to_update(LxFsmCtx *c) {
    FsmTween *w = c->state;
    if (w->done) return;
    if (lx_fsm_state_time(c) >= lx_fsm_float(c, 3)) { w->done = true; lx_fsm_finish(c); fsm_send(c, 5); }
}
static void tr_move_to_exit(LxFsmCtx *c) {
    FsmTween *w = c->state;
    if (w->tween && !w->done) lx_tween_cancel(c->scene, w->tween);
}
static const LxFsmActionDesc A_TR_MOVE_TO = {"Move To Over Time", "Tween", "Slides an object to a position over a set time, with easing. Leaving the state stops it.",
    "{0} -> {1} in {3}s",
    {POBJ("Object"), POBJNT("Target", "None uses the position below"), P("Target Position", LX_FSM_VEC3, "0 0 0"),
     P("Duration", LX_FSM_FLOAT, "1"), PENUM("Ease", FSM_EASES, "3"), PEV("Finish Event"),
     PT("Relative", LX_FSM_BOOL, "false", "Target Position is an offset from where it is now, not a place to go")},
    sizeof(FsmTween), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, tr_move_to_enter, tr_move_to_update, tr_move_to_exit};

static void tr_scale_to_enter(LxFsmCtx *c) {
    FsmTween *w = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) { lx_fsm_finish(c); return; }
    w->tween = lx_tween_scale(c->scene, e, lx_fsm_vec3(c, 1), lx_maxf(0.0001f, lx_fsm_float(c, 2)), fsm_ease(lx_fsm_int(c, 3)));
}
static void tr_scale_to_update(LxFsmCtx *c) {
    FsmTween *w = c->state;
    if (w->done) return;
    if (lx_fsm_state_time(c) >= lx_fsm_float(c, 2)) { w->done = true; lx_fsm_finish(c); fsm_send(c, 4); }
}
static void tr_tween_exit(LxFsmCtx *c) {
    FsmTween *w = c->state;
    if (w->tween && !w->done) lx_tween_cancel(c->scene, w->tween);
}
static const LxFsmActionDesc A_TR_SCALE_TO = {"Scale To Over Time", "Tween", "Grows or shrinks an object over a set time: pickups popping in, doors opening.",
    "{0} scale -> {1} in {2}s",
    {POBJ("Object"), P("Target Scale", LX_FSM_VEC3, "1 1 1"), P("Duration", LX_FSM_FLOAT, "0.3"),
     PENUM("Ease", FSM_EASES, "4"), PEV("Finish Event")},
    sizeof(FsmTween), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, tr_scale_to_enter, tr_scale_to_update, tr_tween_exit};

static void tr_rotate_to_enter(LxFsmCtx *c) {
    FsmTween *w = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) { lx_fsm_finish(c); return; }
    w->tween = lx_tween_euler(c->scene, e, lx_fsm_vec3(c, 1), lx_maxf(0.0001f, lx_fsm_float(c, 2)), fsm_ease(lx_fsm_int(c, 3)));
}
static void tr_rotate_to_update(LxFsmCtx *c) {
    FsmTween *w = c->state;
    if (w->done) return;
    if (lx_fsm_state_time(c) >= lx_fsm_float(c, 2)) { w->done = true; lx_fsm_finish(c); fsm_send(c, 4); }
}
static const LxFsmActionDesc A_TR_ROTATE_TO = {"Rotate To Over Time", "Tween", "Turns an object to an angle over a set time: swinging doors, turning platforms.",
    "{0} rotation -> {1} in {2}s",
    {POBJ("Object"), P("Target Euler", LX_FSM_VEC3, "0 90 0"), P("Duration", LX_FSM_FLOAT, "1"),
     PENUM("Ease", FSM_EASES, "3"), PEV("Finish Event")},
    sizeof(FsmTween), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, tr_rotate_to_enter, tr_rotate_to_update, tr_tween_exit};

/* ==========================================================================================
 *  shaking and wobbling
 * ========================================================================================== */
typedef struct { LxVec3 home; bool started; float t; } FsmShake;
static void tr_shake(LxFsmCtx *c) {
    FsmShake *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    if (!s->started) { s->started = true; s->home = lx_entity_position(c->scene, e); }
    float len = lx_fsm_float(c, 2);
    s->t += c->dt;
    float fade = (len > 0) ? lx_saturate(1.0f - s->t / len) : 1.0f;
    float amount = lx_fsm_float(c, 1) * (lx_fsm_bool(c, 3) ? fade : 1.0f);
    LxVec3 jitter = lx_scale3(lx_random_in_sphere(1.0f), amount);
    if (lx_fsm_bool(c, 4)) jitter.y = 0;
    lx_entity_set_position(c->scene, e, lx_add3(s->home, jitter));
    if (len > 0 && s->t >= len) {
        lx_entity_set_position(c->scene, e, s->home);
        lx_fsm_finish(c);
        fsm_send(c, 5);
    }
}
static void tr_shake_exit(LxFsmCtx *c) {
    FsmShake *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (s->started && e) lx_entity_set_position(c->scene, e, s->home);
}
static const LxFsmActionDesc A_TR_SHAKE = {"Shake Position", "Transform", "Jitters an object about its resting place and puts it back afterwards: screen shake, impacts.",
    "shake {0} by {1} for {2}s",
    {POBJ("Object"), P("Strength", LX_FSM_FLOAT, "0.2"), PT("Duration", LX_FSM_FLOAT, "0.3", "0 = until the state ends"),
     PT("Fade Out", LX_FSM_BOOL, "true", "shake less as it goes on"), PT("Flat", LX_FSM_BOOL, "false", "do not shake up and down"),
     PEV("Finish Event")},
    sizeof(FsmShake), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, tr_shake, tr_shake, tr_shake_exit};

/* ==========================================================================================
 *  Animate Variables — a value driven by a repeating wave
 * ========================================================================================== */
static void av_wave(LxFsmCtx *c) {
    float t = lx_fsm_state_time(c) * lx_maxf(0.0001f, lx_fsm_float(c, 1)) + lx_fsm_float(c, 2);
    float phase = t - floorf(t);          /* 0..1 through one cycle */
    float w;
    switch (lx_fsm_int(c, 0)) {
    case 0: w = sinf(phase * 2.0f * LX_PI) * 0.5f + 0.5f; break;                /* Sine */
    case 1: w = phase; break;                                                    /* Sawtooth */
    case 2: w = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f; break;        /* Triangle */
    case 3: w = phase < 0.5f ? 1.0f : 0.0f; break;                               /* Square */
    case 4: w = 1.0f - fabsf(cosf(phase * LX_PI)); break;                        /* Bounce */
    case 5: {                                                                     /* Pulse: a short spike */
        float k = lx_saturate(phase * 4.0f);
        w = k < 1.0f ? 1.0f - k : 0.0f;
        break;
    }
    default: {                                                                    /* Heartbeat: two quick beats */
        float a = expf(-powf((phase - 0.10f) * 14.0f, 2.0f));
        float b = expf(-powf((phase - 0.28f) * 14.0f, 2.0f)) * 0.6f;
        w = lx_saturate(a + b);
        break;
    }
    }
    lx_fsm_set_float(c, 5, lx_lerpf(lx_fsm_float(c, 3), lx_fsm_float(c, 4), w));
    lx_fsm_set_float(c, 6, w);
}
static const LxFsmActionDesc A_AV_WAVE = {"Animate Float", "Animate Variables", "Drives a number with a repeating wave: sine, sawtooth, triangle, square, bounce, pulse or heartbeat.",
    "{0} wave -> {5}",
    {PENUM("Wave", "Sine|Sawtooth|Triangle|Square|Bounce|Pulse|Heartbeat", "0"),
     PT("Cycles Per Second", LX_FSM_FLOAT, "1", "how fast the wave repeats"),
     PT("Phase", LX_FSM_FLOAT, "0", "shifts where in the wave it starts"),
     P("Output Min", LX_FSM_FLOAT, "0"), P("Output Max", LX_FSM_FLOAT, "1"),
     POUT("Store Result", LX_FSM_FLOAT), POPT("Store Wave 0 1", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, av_wave, av_wave, NULL};

typedef struct { float value, vel; bool started; } FsmSpringF;
static void av_spring(LxFsmCtx *c) {
    FsmSpringF *s = c->state;
    if (!s->started) { s->started = true; s->value = lx_fsm_float(c, 3); }
    float target = lx_fsm_float(c, 0);
    float stiffness = lx_maxf(0.0001f, lx_fsm_float(c, 1));
    float damping = lx_maxf(0, lx_fsm_float(c, 2));
    float accel = (target - s->value) * stiffness - s->vel * damping;
    s->vel += accel * c->dt;
    s->value += s->vel * c->dt;
    lx_fsm_set_float(c, 4, s->value);
    lx_fsm_set_float(c, 5, s->vel);
}
static const LxFsmActionDesc A_AV_SPRING = {"Animate Float Spring", "Animate Variables", "Pulls a number towards a target on a spring, so it overshoots and wobbles into place.",
    "spring -> {0}",
    {P("Target", LX_FSM_FLOAT, "1"), PT("Stiffness", LX_FSM_FLOAT, "80", "how hard the spring pulls"),
     PT("Damping", LX_FSM_FLOAT, "8", "how quickly the wobble dies away; higher settles sooner"),
     PT("Start Value", LX_FSM_FLOAT, "0", "where it begins when the state is entered"),
     POUT("Store Result", LX_FSM_FLOAT), POPT("Store Velocity", LX_FSM_FLOAT)},
    sizeof(FsmSpringF), 0, LX_FSM_EVERY_FRAME, av_spring, av_spring, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_transform(void) {
    static const LxFsmActionDesc *all[] = {
        &A_TR_GET_POS, &A_TR_SET_POS, &A_TR_TRANSLATE, &A_TR_CLAMP_POS, &A_TR_SNAP_GRID,
        &A_TR_GET_ROT, &A_TR_SET_ROT, &A_TR_ROTATE, &A_TR_ROTATE_AROUND, &A_TR_LOOK_AT, &A_TR_ALIGN, &A_TR_FACE_CAMERA,
        &A_TR_GET_SCALE, &A_TR_SET_SCALE,
        &A_TR_GET_AXES, &A_TR_TRANSFORM_POINT, &A_TR_DISTANCE,
        &A_TR_MOVE_TOWARDS, &A_TR_SMOOTH_FOLLOW, &A_TR_MOVE_TO, &A_TR_SCALE_TO, &A_TR_ROTATE_TO,
        &A_TR_SHAKE, &A_AV_WAVE, &A_AV_SPRING,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
