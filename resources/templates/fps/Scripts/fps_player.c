/* FPS Player: first person movement and the player's health. Put it on the Player (with a Character
 * Controller); the camera is a child named "Main Camera".
 *
 *   Mouse / right stick look        Sprint (Shift) uses stamina        Crouch (C / Ctrl) - lower and slower
 *   Jump (Space) with coyote time (a moment after walking off a ledge) and a jump buffer (pressed just before landing)
 *   Head bob, a dip when landing, camera kick from the gun, a wider view when sprinting, zoom when aiming
 *   Interact (E): uses whatever "Interactable" object the crosshair is on (doors, health and ammo)
 *   "Damage" messages hurt; at 0 health the game is lost and the camera drops. */
#include "game.h"

#define P_WALK 0
#define P_SPRINT 1
#define P_CROUCH 2
#define P_JUMP 3
#define P_SENS 4
#define P_GRAVITY 5
#define P_BOB 6
#define P_FOV 7

#define EYE_STAND 1.62f
#define EYE_CROUCH 1.0f
#define HEIGHT_STAND 1.8f
#define HEIGHT_CROUCH 1.15f

typedef struct Player {
    bool started;
    LxEntity cam;
    float yaw, pitch, kick;         /* kick: recoil still to be recovered */
    LxVec3 vel;                     /* horizontal velocity */
    float vy, coyote, buffer;
    bool grounded, crouched;
    float crouch;                   /* 0 standing .. 1 crouched */
    float bob_t, bob_amount, land;  /* head bob phase and strength, landing dip */
    float exhausted;                /* can't sprint again until stamina is back up a bit */
    float fov;
    float dead_t;
} Player;

static void player_start(LxBehaviourCtx *ctx) {
    Player *p = ctx->state;
    LxScene *s = ctx->scene;
    p->started = true;
    p->cam = lx_entity_find_child(s, ctx->self, "Main Camera");
    p->yaw = lx_entity_euler(s, ctx->self).y;
    p->fov = ctx->params[P_FOV] > 0 ? ctx->params[P_FOV] : 72;
}

/* mouse movement arrives per frame; sticks and touch are rates, so they're scaled by time */
static LxVec2 look_input(float dt) {
    LxVec2 v = lx_v2(lx_axis("LookX"), lx_axis("LookY"));
    if (lx_input_last_device() != LX_DEVICE_KEYBOARD_MOUSE) { v.x *= dt * 60; v.y *= dt * 60; }
    if (!isfinite(v.x) || !isfinite(v.y)) v = lx_v2(0, 0);
    return v;
}

/* room to stand up: nothing above the head */
static bool can_stand(LxBehaviourCtx *ctx) {
    LxVec3 head = lx_add3(lx_entity_world_position(ctx->scene, ctx->self), lx_v3(0, HEIGHT_CROUCH - 0.05f, 0));
    LxRaycastHit hit;
    return !lx_physics_raycast_ignore(ctx->scene, head, lx_v3(0, 1, 0), HEIGHT_STAND - HEIGHT_CROUCH + 0.1f, LX_ALL_LAYERS, ctx->self, &hit);
}

/* what the crosshair is on, if it can be used */
static void interact(LxBehaviourCtx *ctx, Player *p) {
    if (!p->cam) return;
    LxScene *s = ctx->scene;
    LxVec3 from = lx_entity_world_position(s, p->cam);
    LxVec3 dir = lx_quat_rotate(lx_entity_world_rotation(s, p->cam), lx_v3(0, 0, 1));
    LxRaycastHit hit;
    LxEntity target = 0;
    if (lx_physics_raycast_ignore(s, from, dir, 2.6f, LX_ALL_LAYERS, ctx->self, &hit)) target = tagged_up(s, hit.entity, "Interactable");
    if (!target) {   /* pickups have no collider: the closest one near the line of sight */
        float best = 0.97f;
        int n = lx_scene_entity_count(s);
        for (int i = 0; i < n; i++) {
            LxEntity e = lx_scene_entity_at(s, i);
            if (!e || !lx_entity_has_tag(s, e, "Interactable")) continue;
            LxVec3 to = lx_sub3(lx_entity_world_position(s, e), from);
            float d = lx_len3(to);
            if (d > 2.4f || d < 1e-3f) continue;
            float dot = lx_dot3(lx_scale3(to, 1 / d), dir);
            if (dot > best) { best = dot; target = e; }
        }
    }
    if (!target) return;
    lx_send_message(s, target, "Hover", 0, ctx->self);
    if (lx_action_pressed("Interact")) lx_send_message(s, target, "Interact", 0, ctx->self);
}

static void player_update(LxBehaviourCtx *ctx, float dt) {
    Player *p = ctx->state;
    LxScene *s = ctx->scene;
    if (!p->started) player_start(ctx);
    if (dt <= 0) return;
    dt = fminf(dt, 0.1f);
    LxCharacterController *cc = lx_get_character(s, ctx->self);
    if (!cc) return;
    bool playing = game_playing();

    /* ---- dead: drop to the floor ---- */
    if (g_game.phase == PHASE_LOST) {
        p->dead_t += dt;
        if (p->cam) {
            float k = clampf(p->dead_t / 0.8f, 0, 1);
            lx_entity_set_position(s, p->cam, lx_v3(0, EYE_STAND - 1.2f * k * k, 0));
            lx_entity_set_euler(s, p->cam, lx_v3(-p->pitch * (1 - k) + 15 * k, 0, 70 * k));
        }
        return;
    }

    /* ---- look ---- */
    if (playing) {
        LxVec2 in = look_input(dt);
        float sens = ctx->params[P_SENS] * 1.5f * (g_game.aiming ? 0.55f : 1.0f);
        p->yaw = fmodf(p->yaw - in.x * sens, 360.0f);
        p->pitch = clampf(p->pitch + in.y * sens, -86, 86);
    }
    /* gun kick: jump up at once, settle most of the way back */
    if (g_game.recoil > 0) { p->pitch = clampf(p->pitch + g_game.recoil, -86, 86); p->kick += g_game.recoil * 0.7f; g_game.recoil = 0; }
    float back = p->kick * smooth(9, dt);
    p->kick -= back;
    p->pitch -= back;
    lx_entity_set_euler(s, ctx->self, lx_v3(0, p->yaw, 0));

    /* ---- move ---- */
    LxVec2 move = playing ? lx_axis2("MoveX", "MoveY") : lx_v2(0, 0);
    float amount = fminf(1, sqrtf(move.x * move.x + move.y * move.y));
    bool wants_crouch = playing && lx_action_down("Crouch");
    if (wants_crouch) p->crouched = true;
    else if (p->crouched && can_stand(ctx)) p->crouched = false;
    p->crouch = approach(p->crouch, p->crouched ? 1 : 0, dt * 7);
    cc->height = HEIGHT_STAND + (HEIGHT_CROUCH - HEIGHT_STAND) * p->crouch;

    /* sprint: forward only, uses stamina; let it run out and you have to catch your breath */
    bool sprint = playing && lx_action_down("Sprint") && move.y > 0.3f && !p->crouched && !g_game.aiming && p->exhausted <= 0;
    if (sprint && amount > 0.1f) {
        g_game.stamina -= dt * 0.22f;
        if (g_game.stamina <= 0) { g_game.stamina = 0; p->exhausted = 1.2f; }
    } else {
        p->exhausted -= dt;
        g_game.stamina = fminf(1, g_game.stamina + dt * (p->exhausted > 0 ? 0.12f : 0.3f));
    }
    g_game.sprinting = sprint && amount > 0.1f;
    float speed = p->crouched ? ctx->params[P_CROUCH] : g_game.sprinting ? ctx->params[P_SPRINT] : ctx->params[P_WALK];
    if (g_game.aiming) speed *= 0.6f;
    LxVec3 fwd = forward_of_yaw(p->yaw), right = right_of(fwd);
    LxVec3 want = lx_scale3(lx_add3(lx_scale3(fwd, move.y), lx_scale3(right, move.x)), speed);
    float accel = p->grounded ? 14.0f : 3.0f;   /* snappy on the ground, a little air control */
    p->vel = lx_lerp3(p->vel, want, smooth(accel, dt));

    /* jump: buffered presses and coyote time */
    float g = ctx->params[P_GRAVITY];
    if (p->grounded) p->coyote = 0.12f; else p->coyote -= dt;
    if (playing && lx_action_pressed("Jump")) p->buffer = 0.15f; else p->buffer -= dt;
    if (p->buffer > 0 && p->coyote > 0 && !p->crouched) {
        p->vy = sqrtf(2 * g * ctx->params[P_JUMP]);
        p->buffer = p->coyote = 0;
    }
    if (p->grounded && p->vy < 0) p->vy = -2;
    p->vy = fmaxf(p->vy - g * dt, -50);
    float fall = p->vy;
    lx_character_move(s, ctx->self, lx_v3(p->vel.x * dt, p->vy * dt, p->vel.z * dt));
    bool now = lx_character_grounded(s, ctx->self);
    if (p->vy > 0 && !now && cc->velocity.y < p->vy * 0.5f) p->vy = 0;   /* bumped the ceiling */
    if (now && !p->grounded && fall < -4) p->land = fminf(1, -fall / 14);   /* landing dip, harder from higher */
    p->grounded = now;

    /* ---- camera: eye height, bob, dip, shake, field of view ---- */
    if (p->cam) {
        float flat_speed = lx_len3(flat(cc->velocity));
        p->bob_amount += ((p->grounded ? clampf(flat_speed / 6, 0, 1) : 0) - p->bob_amount) * smooth(8, dt);
        p->bob_t += dt * (6 + flat_speed * 0.9f);
        p->land = approach(p->land, 0, dt * 2.5f);
        float bob = ctx->params[P_BOB] * p->bob_amount * (g_game.aiming ? 0.2f : 1.0f);
        float eye = EYE_STAND + (EYE_CROUCH - EYE_STAND) * p->crouch;
        LxVec3 off = lx_v3(-cosf(p->bob_t * 0.5f) * 0.035f * bob, fabsf(sinf(p->bob_t * 0.5f)) * 0.05f * bob - sinf(p->land * 3.1416f) * 0.18f, 0);
        float sh = g_game.shake;
        LxVec3 jitter = lx_v3(randf(-1, 1) * sh * 1.6f, randf(-1, 1) * sh * 1.6f, randf(-1, 1) * sh * 2.5f);
        lx_entity_set_position(s, p->cam, lx_v3(off.x, eye + off.y, 0));
        lx_entity_set_euler(s, p->cam, lx_v3(-p->pitch + jitter.x, jitter.y, jitter.z + cosf(p->bob_t * 0.5f) * 0.6f * bob));   /* + pitch = up: -X turn */
        LxCamera *cam = lx_get_camera(s, p->cam);
        float base = ctx->params[P_FOV] > 0 ? ctx->params[P_FOV] : 72;
        float target = g_game.aiming ? base * 0.68f : g_game.sprinting ? base + 8 : base;
        p->fov += (target - p->fov) * smooth(10, dt);
        if (cam) cam->fov_deg = p->fov;
    }
    if (playing) interact(ctx, p);
}

static void player_message(LxBehaviourCtx *ctx, const char *message, float value, LxEntity sender) {
    (void)ctx; (void)sender;
    if (strcmp(message, "Damage") || !game_playing() || value <= 0) return;
    g_game.health = fmaxf(0, g_game.health - value);
    g_game.hurt = 1;
    game_shake(0.35f);
}

LX_SCRIPT_REGISTER(fps_player) {
    LxBehaviourDesc d = {0};
    d.name = "FPS Player";
    d.state_size = sizeof(Player);
    d.param_names[P_WALK] = "Walk Speed";
    d.param_names[P_SPRINT] = "Sprint Speed";
    d.param_names[P_CROUCH] = "Crouch Speed";
    d.param_names[P_JUMP] = "Jump Height";
    d.param_names[P_SENS] = "Look Sensitivity";
    d.param_names[P_GRAVITY] = "Gravity";
    d.param_names[P_BOB] = "Head Bob";
    d.param_names[P_FOV] = "Field Of View";
    float defaults[8] = {4.6f, 7.4f, 2.2f, 1.2f, 1, 22, 1, 72};
    for (int i = 0; i < 8; i++) d.param_defaults[i] = defaults[i];
    d.on_start = player_start;
    d.on_update = player_update;
    d.on_message = player_message;
    lx_register_behaviour(&d);
}
