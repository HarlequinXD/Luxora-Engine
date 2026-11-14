/* Built-in State Graph actions, part three: character controllers and cameras for the game templates -
 * first person mouse look, walking/running/jumping with gravity, and a third person orbit camera. */
#include "../lx_internal.h"
#include <stdio.h>

#define P(n, t, d) {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip) {n, t, d, 0, NULL, tip}
#define POPT(n, t) {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n) {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define POBJ(n) {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJN(n) {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}

static void ev(LxFsmCtx *c, int p) { lx_fsm_send(c, lx_fsm_event(c, p)); }
static LxEntity obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }

/* look input from the Input Manager: mouse movement is per frame, sticks and touch are rates */
static LxVec2 look_input(LxFsmCtx *c, int px, int py, float dt) {
    char ax[LX_NAME_MAX], ay[LX_NAME_MAX];
    lx_strcpy(ax, sizeof ax, lx_fsm_string(c, px));
    lx_strcpy(ay, sizeof ay, lx_fsm_string(c, py));
    LxVec2 v = lx_v2(lx_axis(ax), lx_axis(ay));
    if (lx_input_last_device() != LX_DEVICE_KEYBOARD_MOUSE) { v.x *= dt * 60.0f; v.y *= dt * 60.0f; }
    if (!isfinite(v.x) || !isfinite(v.y)) v = lx_v2(0, 0);
    return v;
}

/* ======================================================================
 *  First Person Look
 * ====================================================================== */
typedef struct FpLook { float yaw, pitch; bool have; } FpLook;
static void fp_look_update(LxFsmCtx *c) {
    FpLook *s = c->state;
    LxEntity body = obj(c, 0), head = obj(c, 1);
    if (!body) return;
    if (!s->have) {
        s->yaw = lx_entity_euler(c->scene, body).y;
        s->pitch = head ? lx_entity_euler(c->scene, head).x : 0;
        s->have = true;
    }
    LxVec2 in = look_input(c, 4, 5, c->dt);
    float sens = lx_fsm_float(c, 2) * 1.5f;
    if (lx_fsm_bool(c, 3)) in.y = -in.y;
    s->yaw = fmodf(s->yaw - in.x * sens, 360.0f);
    float lo = lx_fsm_float(c, 6), hi = lx_fsm_float(c, 7);
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    s->pitch = lx_clampf(s->pitch + in.y * sens, lx_maxf(lo, -89.5f), lx_minf(hi, 89.5f));
    LxVec3 be = lx_entity_euler(c->scene, body);
    lx_entity_set_euler(c->scene, body, lx_v3(be.x, s->yaw, be.z));
    if (head && head != body) {
        LxVec3 he = lx_entity_euler(c->scene, head);
        lx_entity_set_euler(c->scene, head, lx_v3(s->pitch, he.y, he.z));
    } else if (head == body) lx_entity_set_euler(c->scene, body, lx_v3(s->pitch, s->yaw, be.z));
}
static const LxFsmActionDesc A_FP_LOOK = {"First Person Look", "Character",
    "Mouse / right stick look: turns the body left and right and tilts the camera (a child of the body) up and down. "
    "Use Set Mouse to lock the mouse.",
    "Look: turn {0}, tilt {1}",
    {POBJ("Body"), PT("Camera", LX_FSM_OBJECT, "None", "the head camera, a child of the body"), P("Sensitivity", LX_FSM_FLOAT, "1"), P("Invert Y", LX_FSM_BOOL, "false"),
     P("Look X Axis", LX_FSM_STRING, "LookX"), P("Look Y Axis", LX_FSM_STRING, "LookY"), P("Min Pitch", LX_FSM_FLOAT, "-85"), P("Max Pitch", LX_FSM_FLOAT, "85")},
    sizeof(FpLook), 0, LX_FSM_EVERY_FRAME, NULL, fp_look_update, NULL};

/* ======================================================================
 *  Character Walk: move, run, jump and fall with a Character Controller
 * ====================================================================== */
typedef struct Walk { float vy; bool grounded, have; float coyote; } Walk;
static void walk_update(LxFsmCtx *c) {
    Walk *s = c->state;
    LxEntity e = obj(c, 0);
    if (!e || !lx_get_character(c->scene, e) || c->dt <= 0) return;
    float dt = lx_minf(c->dt, 0.1f);
    char ax[LX_NAME_MAX], ay[LX_NAME_MAX], run[LX_NAME_MAX], jump[LX_NAME_MAX];
    lx_strcpy(ax, sizeof ax, lx_fsm_string(c, 1));
    lx_strcpy(ay, sizeof ay, lx_fsm_string(c, 2));
    lx_strcpy(run, sizeof run, lx_fsm_string(c, 5));
    lx_strcpy(jump, sizeof jump, lx_fsm_string(c, 7));
    LxVec2 in = lx_axis2(ax, ay);
    if (!isfinite(in.x) || !isfinite(in.y)) in = lx_v2(0, 0);
    float len = sqrtf(in.x * in.x + in.y * in.y);
    if (len > 1) { in.x /= len; in.y /= len; }
    /* directions on the ground, from the Relative To object (camera or body); none = the world */
    LxEntity rel = obj(c, 3);
    LxVec3 fwd = lx_v3(0, 0, -1), right = lx_v3(1, 0, 0);
    if (rel) {
        LxQuat q = lx_entity_world_rotation(c->scene, rel);
        fwd = lx_quat_rotate(q, lx_v3(0, 0, -1));
        fwd.y = 0;
        if (lx_len3sq(fwd) < 1e-6f) fwd = lx_quat_rotate(q, lx_v3(0, 1, 0)), fwd.y = 0, fwd = lx_neg3(fwd);   /* looking straight down */
        fwd = lx_len3sq(fwd) > 1e-8f ? lx_norm3(fwd) : lx_v3(0, 0, -1);
        right = lx_v3(-fwd.z, 0, fwd.x);
    }
    float speed = lx_maxf(0, lx_fsm_float(c, 4)) * (run[0] && lx_action_down(run) ? lx_maxf(0, lx_fsm_float(c, 6)) : 1.0f);
    LxVec3 move = lx_scale3(lx_add3(lx_scale3(fwd, in.y), lx_scale3(right, in.x)), speed);
    /* gravity and jumping (a short grace time after walking off a ledge) */
    float g = lx_maxf(0, lx_fsm_float(c, 9));
    bool was = s->have ? s->grounded : lx_character_grounded(c->scene, e);
    s->have = true;
    if (was) { s->coyote = 0.12f; if (s->vy < 0) s->vy = -2.0f; }
    else s->coyote -= dt;
    if (jump[0] && lx_action_pressed(jump) && s->coyote > 0) {
        s->vy = sqrtf(2 * g * lx_maxf(0, lx_fsm_float(c, 8)));
        s->coyote = 0;
        ev(c, 12);
    }
    s->vy = lx_maxf(s->vy - g * dt, -60.0f);
    LxVec3 d = lx_v3(move.x * dt, s->vy * dt, move.z * dt);
    lx_character_move(c->scene, e, d);
    bool now = lx_character_grounded(c->scene, e);
    LxCharacterController *cc = lx_get_character(c->scene, e);
    if (cc && s->vy > 0 && cc->velocity.y < s->vy * 0.5f && !now) s->vy = 0;   /* bumped a ceiling */
    if (now && !s->grounded && s->vy <= 0) ev(c, 13);
    s->grounded = now;
    /* face the way it walks (third person) */
    if (lx_fsm_bool(c, 10) && lx_len3sq(move) > 1e-4f) {
        LxQuat want = lx_quat_look(lx_norm3(move), lx_v3(0, 1, 0)), cur = lx_entity_world_rotation(c->scene, e);
        float k = lx_clampf(1 - expf(-lx_maxf(0, lx_fsm_float(c, 11)) * dt), 0, 1);
        if (cur.x * want.x + cur.y * want.y + cur.z * want.z + cur.w * want.w < 0) { want.x = -want.x; want.y = -want.y; want.z = -want.z; want.w = -want.w; }
        LxQuat r = {cur.x + (want.x - cur.x) * k, cur.y + (want.y - cur.y) * k, cur.z + (want.z - cur.z) * k, cur.w + (want.w - cur.w) * k};
        if (r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w > 1e-10f) lx_entity_set_world_pose(c->scene, e, lx_entity_world_position(c->scene, e), lx_quat_norm(r));
    }
    lx_fsm_set_float(c, 14, sqrtf(move.x * move.x + move.z * move.z));
    lx_fsm_set_bool(c, 15, now);
}
static const LxFsmActionDesc A_CHAR_WALK = {"Character Walk", "Character",
    "Walks, runs, jumps and falls with a Character Controller, steered by the Input Manager. Moves relative to where the "
    "Relative To object faces (the camera for third person, the body for first person).",
    "Walk {0} at {4} m/s, jump {8} m",
    {POBJ("Object"), P("Move X Axis", LX_FSM_STRING, "MoveX"), P("Move Y Axis", LX_FSM_STRING, "MoveY"), PT("Relative To", LX_FSM_OBJECT, "Owner", "None = world directions"),
     P("Walk Speed", LX_FSM_FLOAT, "4.5"), PT("Run Action", LX_FSM_STRING, "Sprint", "an Input Manager action; empty = no running"), P("Run Multiplier", LX_FSM_FLOAT, "1.7"),
     PT("Jump Action", LX_FSM_STRING, "Jump", "empty = no jumping"), P("Jump Height", LX_FSM_FLOAT, "1.2"), P("Gravity", LX_FSM_FLOAT, "20"),
     PT("Turn To Move", LX_FSM_BOOL, "false", "turn the object to face where it walks (third person)"), P("Turn Speed", LX_FSM_FLOAT, "12"),
     PEV("Jumped"), PEV("Landed"), POPT("Store Speed", LX_FSM_FLOAT), POPT("Store Grounded", LX_FSM_BOOL)},
    sizeof(Walk), 0, LX_FSM_EVERY_FRAME, NULL, walk_update, NULL};

/* ======================================================================
 *  Third Person Camera
 * ====================================================================== */
typedef struct TpCam { float yaw, pitch, dist; bool have; LxVec3 pivot; } TpCam;
static void tp_cam_update(LxFsmCtx *c) {
    TpCam *s = c->state;
    LxEntity cam = obj(c, 0), target = obj(c, 1);
    if (!cam || !target || cam == target) return;
    float dt = lx_minf(c->dt, 0.1f);
    LxVec3 pivot = lx_add3(lx_entity_world_position(c->scene, target), lx_v3(0, lx_fsm_float(c, 3), 0));
    float want_dist = lx_clampf(lx_fsm_float(c, 2), 0.5f, 100.0f);
    if (!s->have) {
        LxVec3 off = lx_sub3(lx_entity_world_position(c->scene, cam), pivot);
        s->yaw = lx_len3sq(off) > 1e-4f ? atan2f(off.x, off.z) * LX_RAD2DEG : lx_entity_euler(c->scene, target).y;
        s->pitch = 15;
        s->dist = want_dist;
        s->pivot = pivot;
        s->have = true;
    }
    LxVec2 in = look_input(c, 5, 6, dt);
    float sens = lx_fsm_float(c, 4) * 1.5f;
    s->yaw = fmodf(s->yaw - in.x * sens, 360.0f);
    float lo = lx_fsm_float(c, 7), hi = lx_fsm_float(c, 8);
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    s->pitch = lx_clampf(s->pitch - in.y * sens, lx_maxf(lo, -89), lx_minf(hi, 89));
    /* the pivot follows smoothly so the camera doesn't jitter with the character's steps */
    float k = lx_clampf(1 - expf(-lx_maxf(0, lx_fsm_float(c, 9)) * dt), 0, 1);
    if (lx_fsm_float(c, 9) <= 0) k = 1;
    s->pivot = lx_lerp3(s->pivot, pivot, k);
    LxQuat q = lx_quat_euler_deg(lx_v3(-s->pitch, s->yaw, 0));
    LxVec3 back = lx_quat_rotate(q, lx_v3(0, 0, 1));
    /* don't go through walls: pull in to what's between the character and the camera */
    float dist = want_dist;
    if (lx_fsm_bool(c, 10)) {
        LxRaycastHit hit;
        if (lx_physics_raycast_ignore(c->scene, s->pivot, back, want_dist + 0.3f, LX_ALL_LAYERS, target, &hit) && hit.entity != cam)
            dist = lx_maxf(0.4f, hit.distance - 0.3f);
    }
    s->dist = dist < s->dist ? dist : s->dist + (dist - s->dist) * lx_clampf(dt * 4, 0, 1);   /* in at once, out gently */
    LxVec3 pos = lx_madd3(s->pivot, back, s->dist);
    lx_entity_set_world_pose(c->scene, cam, pos, lx_quat_look(lx_sub3(s->pivot, pos), lx_v3(0, 1, 0)));
}
static const LxFsmActionDesc A_TP_CAM = {"Third Person Camera", "Camera",
    "Orbits a camera around a character with the mouse or right stick, follows it smoothly and doesn't go through walls.",
    "{0} orbits {1} at {2} m",
    {POBJ("Camera"), P("Target", LX_FSM_OBJECT, "tag:Player"), P("Distance", LX_FSM_FLOAT, "5"), PT("Height", LX_FSM_FLOAT, "1.6", "the point it looks at, above the target's feet"),
     P("Sensitivity", LX_FSM_FLOAT, "1"), P("Look X Axis", LX_FSM_STRING, "LookX"), P("Look Y Axis", LX_FSM_STRING, "LookY"), P("Min Pitch", LX_FSM_FLOAT, "-30"),
     P("Max Pitch", LX_FSM_FLOAT, "70"), PT("Follow Smoothing", LX_FSM_FLOAT, "12", "0 = stick exactly"), P("Avoid Walls", LX_FSM_BOOL, "true")},
    sizeof(TpCam), 0, LX_FSM_LATE, NULL, tp_cam_update, NULL};

void lx_fsm_register_controller_actions(void) {
    static const LxFsmActionDesc *all[] = {&A_FP_LOOK, &A_CHAR_WALK, &A_TP_CAM};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
