/* TPS Camera: orbits the player with the mouse / right stick, a little over the shoulder, and never goes
 * through walls. While locked on it swings round behind the player to keep the target in view. It widens
 * when sprinting and shakes with hits. Put it on the Main Camera. */
#include "game.h"

#define P_DIST 0
#define P_HEIGHT 1
#define P_SENS 2
#define P_FOLLOW 3
#define P_SHOULDER 4
#define P_MIN_PITCH 5
#define P_MAX_PITCH 6
#define P_FOV 7

typedef struct Cam {
    bool started;
    LxEntity player;
    float yaw, pitch, dist, fov;
    LxVec3 pivot;
} Cam;

static void cam_start(LxBehaviourCtx *ctx) {
    Cam *c = ctx->state;
    LxScene *s = ctx->scene;
    c->started = true;
    c->player = lx_entity_find_with_tag(s, "Player");
    c->pitch = 14;
    c->dist = ctx->params[P_DIST];
    c->fov = ctx->params[P_FOV];
    if (c->player) {
        c->pivot = lx_add3(lx_entity_world_position(s, c->player), lx_v3(0, ctx->params[P_HEIGHT], 0));
        c->yaw = lx_entity_euler(s, c->player).y;   /* start behind the player */
    }
}

static void cam_late(LxBehaviourCtx *ctx, float dt) {
    Cam *c = ctx->state;
    LxScene *s = ctx->scene;
    if (!c->started) cam_start(ctx);
    if (!c->player || !lx_entity_valid(s, c->player)) { c->player = lx_entity_find_with_tag(s, "Player"); return; }
    if (dt <= 0) return;   /* paused: stay where it is */
    dt = fminf(dt, 0.1f);
    LxVec3 target_pivot = lx_add3(lx_entity_world_position(s, c->player), lx_v3(0, ctx->params[P_HEIGHT], 0));

    /* look around */
    if (game_playing()) {
        LxVec2 in = lx_v2(lx_axis("LookX"), lx_axis("LookY"));
        if (lx_input_last_device() != LX_DEVICE_KEYBOARD_MOUSE) { in.x *= dt * 60; in.y *= dt * 60; }
        if (!isfinite(in.x) || !isfinite(in.y)) in = lx_v2(0, 0);
        float sens = ctx->params[P_SENS] * 1.5f;
        c->yaw = fmodf(c->yaw - in.x * sens, 360.0f);
        c->pitch = clampf(c->pitch - in.y * sens, ctx->params[P_MIN_PITCH], ctx->params[P_MAX_PITCH]);
    }
    /* locked on: behind the player, looking past them at the target */
    LxEntity lock = g_game.lock_target;
    if (lock && lx_entity_valid(s, lock)) {
        LxVec3 to = flat(lx_sub3(lx_entity_world_position(s, lock), target_pivot));
        if (lx_len3sq(to) > 0.01f) c->yaw += angle_delta(c->yaw, yaw_of(to)) * smooth(6, dt);
        c->pitch += (12 - c->pitch) * smooth(3, dt);
    }
    c->pivot = lx_lerp3(c->pivot, target_pivot, ctx->params[P_FOLLOW] > 0 ? smooth(ctx->params[P_FOLLOW], dt) : 1);

    /* place it, pulled in by anything between the player and the camera */
    LxQuat q = lx_quat_euler_deg(lx_v3(c->pitch, c->yaw, 0));   /* + pitch: looking down */
    LxVec3 back = lx_quat_rotate(q, lx_v3(0, 0, -1)), side = lx_quat_rotate(q, lx_v3(-1, 0, 0));   /* forward +Z, right -X */
    LxVec3 pivot = lx_madd3(c->pivot, side, ctx->params[P_SHOULDER]);
    float want = ctx->params[P_DIST];
    LxRaycastHit hit;
    if (lx_physics_raycast_ignore(s, pivot, back, want + 0.3f, LX_ALL_LAYERS, c->player, &hit) && tagged_up(s, hit.entity, "Enemy") == 0)
        want = fmaxf(0.5f, hit.distance - 0.3f);
    c->dist = want < c->dist ? want : c->dist + (want - c->dist) * smooth(3, dt);   /* in at once, out gently */
    LxVec3 pos = lx_madd3(pivot, back, c->dist);
    LxQuat rot = lx_quat_look(lx_sub3(pivot, pos), lx_v3(0, 1, 0));
    float sh = g_game.shake;
    if (sh > 0) rot = lx_quat_mul(rot, lx_quat_euler_deg(lx_v3(randf(-1, 1) * sh * 2, randf(-1, 1) * sh * 2, randf(-1, 1) * sh * 3)));
    lx_entity_set_world_pose(s, ctx->self, pos, rot);
    /* a wider view at full speed */
    float fov = ctx->params[P_FOV] + (g_game.sprinting ? 7 : 0);
    c->fov += (fov - c->fov) * smooth(5, dt);
    LxCamera *cam = lx_get_camera(s, ctx->self);
    if (cam) cam->fov_deg = c->fov;
}

LX_SCRIPT_REGISTER(tps_camera) {
    LxBehaviourDesc d = {0};
    d.name = "TPS Camera";
    d.state_size = sizeof(Cam);
    d.param_names[P_DIST] = "Distance";
    d.param_names[P_HEIGHT] = "Height";
    d.param_names[P_SENS] = "Sensitivity";
    d.param_names[P_FOLLOW] = "Follow Smoothing";
    d.param_names[P_SHOULDER] = "Shoulder Offset";
    d.param_names[P_MIN_PITCH] = "Min Pitch";
    d.param_names[P_MAX_PITCH] = "Max Pitch";
    d.param_names[P_FOV] = "Field Of View";
    float defaults[8] = {5, 1.6f, 1, 14, 0.55f, -35, 70, 60};
    for (int i = 0; i < 8; i++) d.param_defaults[i] = defaults[i];
    d.on_start = cam_start;
    d.on_late_update = cam_late;
    lx_register_behaviour(&d);
}
