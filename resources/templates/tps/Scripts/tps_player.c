/* TPS Player: third person movement, sword combat and the player's health. Put it on the Player (with a
 * Character Controller), with a child "Body" (the model) and a child "Sword" (a pivot at the hand, the blade
 * under it). The camera is the "Main Camera" with the TPS Camera script.
 *
 *   Moves where the camera looks and turns to face where it goes; sticks walk slower when pushed gently
 *   Sprint (Shift) uses stamina; Jump (Space): coyote time, a jump buffer, and short hops when let go early
 *   Crouch (C / Ctrl / B): a dodge roll - you can't be hurt in the first part of it
 *   Fire (left mouse): a three-hit combo, press again during a swing to chain; the third hit is the strongest
 *   Lock On (Tab / middle mouse / right stick): face and circle the nearest enemy in view; again to let go
 *   Interact (E): uses the nearest "Interactable" thing in front of you
 *   "Damage" messages hurt; at 0 health the game is lost. */
#include "game.h"

#define P_RUN 0
#define P_SPRINT 1
#define P_JUMP 2
#define P_TURN 3
#define P_DAMAGE 4
#define P_REACH 5
#define P_DODGE 6
#define P_GRAVITY 7

typedef struct Combo { float length, hit_at, power; LxVec3 from, to; } Combo;
static const Combo COMBO[3] = {
    {0.42f, 0.16f, 1.0f, {-10, -75, 0}, {-15, 85, 0}},     /* right to left */
    {0.42f, 0.16f, 1.0f, {-10, 85, 0}, {-15, -80, 0}},     /* back again */
    {0.62f, 0.27f, 1.9f, {-95, 0, 0}, {50, 0, 0}},         /* overhead: up (-X turn) to down */
};
static const LxVec3 SWORD_REST = {25, 10, 0};   /* pointing ahead and a little down */

typedef struct Player {
    bool started;
    LxEntity body, sword, cam;
    float yaw;
    LxVec3 vel;
    float vy, coyote, buffer;
    bool grounded;
    int step;                   /* combo step 1..3, 0 = not attacking */
    float attack_t;
    bool queued, hit_done;
    float roll_t;               /* > 0 while rolling */
    LxVec3 roll_dir;
    float exhausted, dead_t;
} Player;

static void player_start(LxBehaviourCtx *ctx) {
    Player *p = ctx->state;
    LxScene *s = ctx->scene;
    p->started = true;
    p->body = lx_entity_find_child(s, ctx->self, "Body");
    p->sword = lx_entity_find_child(s, ctx->self, "Sword");
    p->cam = lx_entity_find(s, "Main Camera");
    p->yaw = lx_entity_euler(s, ctx->self).y;
    if (p->sword) lx_entity_set_euler(s, p->sword, SWORD_REST);
}

static bool target_ok(LxScene *s, LxEntity e) { return e && lx_entity_valid(s, e) && lx_entity_has_tag(s, e, "Enemy"); }

/* ---------------- lock-on ---------------- */
static LxEntity pick_target(LxBehaviourCtx *ctx, Player *p) {
    LxScene *s = ctx->scene;
    LxVec3 me = lx_entity_world_position(s, ctx->self);
    LxVec3 look = p->cam ? flat(lx_quat_rotate(lx_entity_world_rotation(s, p->cam), lx_v3(0, 0, 1))) : forward_of_yaw(p->yaw);
    if (lx_len3sq(look) > 1e-6f) look = lx_norm3(look);
    LxEntity best = 0;
    float best_score = 1e9f;
    int n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (!target_ok(s, e)) continue;
        LxVec3 to = flat(lx_sub3(lx_entity_world_position(s, e), me));
        float d = lx_len3(to);
        if (d > 18 || d < 1e-3f) continue;
        float facing = lx_dot3(lx_scale3(to, 1 / d), look);
        if (facing < 0.2f) continue;
        float score = d * (2.2f - facing);   /* near and in front */
        if (score < best_score) { best_score = score; best = e; }
    }
    return best;
}

/* ---------------- the sword ---------------- */
static LxEntity nearest_enemy(LxScene *s, LxVec3 me, float range) {
    LxEntity best = 0;
    float bd = range;
    int n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (!target_ok(s, e)) continue;
        float d = lx_len3(flat(lx_sub3(lx_entity_world_position(s, e), me)));
        if (d < bd) { bd = d; best = e; }
    }
    return best;
}

static void strike(LxBehaviourCtx *ctx, Player *p) {
    LxScene *s = ctx->scene;
    LxVec3 me = lx_entity_world_position(s, ctx->self), fwd = forward_of_yaw(p->yaw);
    float reach = ctx->params[P_REACH], damage = ctx->params[P_DAMAGE] * COMBO[p->step - 1].power;
    int hits = 0, n = lx_scene_entity_count(s);
    LxEntity hit_list[32];
    for (int i = 0; i < n && hits < 32; i++) {   /* collected first: a hit can end an enemy */
        LxEntity e = lx_scene_entity_at(s, i);
        if (!target_ok(s, e)) continue;
        LxVec3 to = flat(lx_sub3(lx_entity_world_position(s, e), me));
        float d = lx_len3(to);
        if (d > reach + 0.4f) continue;
        if (d > 0.6f && lx_dot3(lx_scale3(to, 1 / d), fwd) < 0.3f) continue;   /* about 70 degrees either side */
        hit_list[hits++] = e;
    }
    for (int i = 0; i < hits; i++) lx_send_message(s, hit_list[i], "Damage", damage, ctx->self);
    if (hits) {
        g_game.hit_marker = 1;
        game_shake(p->step == 3 ? 0.3f : 0.15f);
    }
}

static void sword_pose(LxBehaviourCtx *ctx, Player *p, float dt) {
    if (!p->sword) return;
    LxScene *s = ctx->scene;
    LxVec3 want = SWORD_REST;
    if (p->step) {
        const Combo *c = &COMBO[p->step - 1];
        float t = p->attack_t, a = c->hit_at - 0.09f, b = c->hit_at + 0.05f;
        if (t < a) want = lx_lerp3(SWORD_REST, c->from, lx_ease(LX_EASE_OUT, clampf(t / fmaxf(a, 0.01f), 0, 1)));
        else if (t < b) want = lx_lerp3(c->from, c->to, lx_ease(LX_EASE_IN_OUT, (t - a) / (b - a)));
        else want = lx_lerp3(c->to, SWORD_REST, lx_ease(LX_EASE_IN_OUT, clampf((t - b) / (c->length - b), 0, 1)));
        lx_entity_set_euler(s, p->sword, want);
        return;
    }
    LxVec3 cur = lx_entity_euler(s, p->sword);
    lx_entity_set_euler(s, p->sword, lx_lerp3(cur, want, smooth(12, dt)));
}

/* ---------------- interacting ---------------- */
static void interact(LxBehaviourCtx *ctx, Player *p) {
    LxScene *s = ctx->scene;
    LxVec3 me = lx_add3(lx_entity_world_position(s, ctx->self), lx_v3(0, 1, 0)), fwd = forward_of_yaw(p->yaw);
    LxEntity best = 0;
    float bd = 2.4f;
    int n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (!e || !lx_entity_has_tag(s, e, "Interactable")) continue;
        LxVec3 to = lx_sub3(lx_entity_world_position(s, e), me);
        float d = lx_len3(to);
        if (d > bd || (d > 0.5f && lx_dot3(flat(to), fwd) < 0.15f * d)) continue;
        bd = d;
        best = e;
    }
    if (!best) return;
    lx_send_message(s, best, "Hover", 0, ctx->self);
    if (lx_action_pressed("Interact")) lx_send_message(s, best, "Interact", 0, ctx->self);
}

static void player_update(LxBehaviourCtx *ctx, float dt) {
    Player *p = ctx->state;
    LxScene *s = ctx->scene;
    if (!p->started) player_start(ctx);
    if (dt <= 0) return;
    dt = fminf(dt, 0.1f);
    if (!lx_get_character(s, ctx->self)) return;
    bool playing = game_playing();
    float g = ctx->params[P_GRAVITY];

    if (g_game.phase == PHASE_LOST) {   /* fall over */
        p->dead_t += dt;
        if (p->body) lx_entity_set_euler(s, p->body, lx_v3(-85 * lx_ease(LX_EASE_BOUNCE_OUT, clampf(p->dead_t / 0.7f, 0, 1)), 0, 0));
        g_game.lock_target = 0;
        return;
    }

    /* ---- input, relative to the camera ---- */
    LxVec2 in = playing ? lx_axis2("MoveX", "MoveY") : lx_v2(0, 0);
    float amount = fminf(1, sqrtf(in.x * in.x + in.y * in.y));
    LxVec3 cf = lx_v3(0, 0, 1);
    if (p->cam) {
        cf = flat(lx_quat_rotate(lx_entity_world_rotation(s, p->cam), lx_v3(0, 0, 1)));
        cf = lx_len3sq(cf) > 1e-6f ? lx_norm3(cf) : forward_of_yaw(p->yaw);
    }
    LxVec3 cr = right_of(cf);
    LxVec3 dir = lx_add3(lx_scale3(cf, in.y), lx_scale3(cr, in.x));

    /* ---- lock-on ---- */
    if (playing && lx_action_pressed("Lock On")) g_game.lock_target = g_game.lock_target ? 0 : pick_target(ctx, p);
    LxEntity lock = g_game.lock_target;
    LxVec3 me = lx_entity_world_position(s, ctx->self);
    if (lock && (!target_ok(s, lock) || lx_len3(lx_sub3(lx_entity_world_position(s, lock), me)) > 24)) lock = g_game.lock_target = 0;

    /* ---- dodge roll ---- */
    if (playing && p->roll_t <= 0 && p->grounded && lx_action_pressed("Crouch") && g_game.stamina >= 0.2f) {
        p->roll_t = 0.001f;
        p->roll_dir = amount > 0.1f ? lx_norm3(dir) : lx_neg3(forward_of_yaw(p->yaw));
        p->yaw = yaw_of(p->roll_dir) + (amount > 0.1f ? 0 : 180);
        g_game.stamina -= 0.2f;
        p->step = 0;   /* a roll cancels an attack */
    }

    /* ---- attack ---- */
    if (playing && lx_action_pressed("Fire") && p->roll_t <= 0) {
        if (!p->step) {
            p->step = 1; p->attack_t = 0; p->hit_done = false; p->queued = false;
            LxEntity aim = lock ? lock : nearest_enemy(s, me, 3.2f);   /* a little help aiming */
            if (aim) p->yaw = yaw_of(flat(lx_sub3(lx_entity_world_position(s, aim), me)));
        } else p->queued = true;   /* the next swing follows this one */
    }
    if (p->step) {
        const Combo *c = &COMBO[p->step - 1];
        p->attack_t += dt;
        if (!p->hit_done && p->attack_t >= c->hit_at) { p->hit_done = true; strike(ctx, p); }
        float chain = c->length * 0.72f;
        if (p->queued && p->step < 3 && p->attack_t >= chain) {
            p->step++; p->attack_t = 0; p->hit_done = false; p->queued = false;
            LxEntity aim = lock ? lock : nearest_enemy(s, me, 3.2f);
            if (aim) p->yaw = yaw_of(flat(lx_sub3(lx_entity_world_position(s, aim), me)));
        } else if (p->attack_t >= c->length) p->step = 0;
    }

    /* ---- speed ---- */
    bool sprint = playing && lx_action_down("Sprint") && amount > 0.3f && p->exhausted <= 0 && !p->step;
    if (sprint) {
        g_game.stamina -= dt * 0.2f;
        if (g_game.stamina <= 0) { g_game.stamina = 0; p->exhausted = 1.2f; }
    } else if (p->roll_t <= 0) {
        p->exhausted -= dt;
        g_game.stamina = fminf(1, g_game.stamina + dt * (p->exhausted > 0 ? 0.12f : 0.3f));
    }
    g_game.sprinting = sprint;
    float speed = (sprint ? ctx->params[P_SPRINT] : ctx->params[P_RUN]) * amount;
    if (lock && !sprint) speed *= 0.8f;            /* circling a target */
    if (p->step) speed *= 0.25f;                   /* planted while swinging */
    LxVec3 want = amount > 0.01f ? lx_scale3(lx_norm3(dir), speed) : lx_v3(0, 0, 0);
    if (p->step) {   /* a small lunge into each swing */
        const Combo *c = &COMBO[p->step - 1];
        if (p->attack_t > c->hit_at - 0.1f && p->attack_t < c->hit_at + 0.02f) want = lx_add3(want, lx_scale3(forward_of_yaw(p->yaw), 4.5f));
    }
    if (p->roll_t > 0) {   /* the roll: fast, then easing off */
        float len = 0.5f, k = p->roll_t / len;
        want = lx_scale3(p->roll_dir, ctx->params[P_DODGE] / len * 1.6f * (1 - k * k));
        p->roll_t += dt;
        if (p->body) lx_entity_set_euler(s, p->body, lx_v3(-360 * lx_ease(LX_EASE_IN_OUT, clampf(k, 0, 1)), 0, 0));
        if (p->roll_t >= len) { p->roll_t = 0; if (p->body) lx_entity_set_euler(s, p->body, lx_v3(0, 0, 0)); }
    }
    float accel = p->roll_t > 0 ? 30 : p->grounded ? 12 : 3;
    p->vel = lx_lerp3(p->vel, want, smooth(accel, dt));

    /* ---- facing ---- */
    if (p->roll_t <= 0 && !p->step) {
        float face = p->yaw;
        if (lock && !sprint) face = yaw_of(flat(lx_sub3(lx_entity_world_position(s, lock), me)));
        else if (lx_len3sq(flat(p->vel)) > 0.25f) face = yaw_of(p->vel);
        p->yaw += angle_delta(p->yaw, face) * smooth(ctx->params[P_TURN], dt);
    }
    lx_entity_set_euler(s, ctx->self, lx_v3(0, p->yaw, 0));

    /* ---- jump and gravity ---- */
    if (p->grounded) p->coyote = 0.12f; else p->coyote -= dt;
    if (playing && lx_action_pressed("Jump")) p->buffer = 0.15f; else p->buffer -= dt;
    if (p->buffer > 0 && p->coyote > 0 && p->roll_t <= 0 && !p->step) {
        p->vy = sqrtf(2 * g * ctx->params[P_JUMP]);
        p->buffer = p->coyote = 0;
    }
    if (p->vy > 0 && !lx_action_down("Jump")) p->vy -= g * dt * 1.5f;   /* let go early: a short hop */
    if (p->grounded && p->vy < 0) p->vy = -2;
    p->vy = fmaxf(p->vy - g * dt, -50);
    LxCharacterController *cc = lx_get_character(s, ctx->self);
    lx_character_move(s, ctx->self, lx_v3(p->vel.x * dt, p->vy * dt, p->vel.z * dt));
    bool now = lx_character_grounded(s, ctx->self);
    if (p->vy > 0 && !now && cc->velocity.y < p->vy * 0.5f) p->vy = 0;
    p->grounded = now;

    sword_pose(ctx, p, dt);
    if (playing) interact(ctx, p);
}

static void player_message(LxBehaviourCtx *ctx, const char *message, float value, LxEntity sender) {
    Player *p = ctx->state;
    (void)sender;
    if (strcmp(message, "Damage") || !game_playing() || value <= 0) return;
    if (p->roll_t > 0 && p->roll_t < 0.35f) { game_message("Dodged!"); return; }   /* the roll's safe moment */
    g_game.health = fmaxf(0, g_game.health - value);
    g_game.hurt = 1;
    game_shake(0.4f);
    (void)ctx;
}

LX_SCRIPT_REGISTER(tps_player) {
    LxBehaviourDesc d = {0};
    d.name = "TPS Player";
    d.state_size = sizeof(Player);
    d.param_names[P_RUN] = "Run Speed";
    d.param_names[P_SPRINT] = "Sprint Speed";
    d.param_names[P_JUMP] = "Jump Height";
    d.param_names[P_TURN] = "Turn Speed";
    d.param_names[P_DAMAGE] = "Sword Damage";
    d.param_names[P_REACH] = "Sword Reach";
    d.param_names[P_DODGE] = "Dodge Distance";
    d.param_names[P_GRAVITY] = "Gravity";
    float defaults[8] = {5.2f, 8.5f, 1.7f, 12, 24, 2.2f, 4, 24};
    for (int i = 0; i < 8; i++) d.param_defaults[i] = defaults[i];
    d.on_start = player_start;
    d.on_update = player_update;
    d.on_message = player_message;
    lx_register_behaviour(&d);
}
