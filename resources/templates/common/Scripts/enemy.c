/* Enemy: wanders near home, spots the player (in range, in front, nothing in the way), chases and attacks.
 * Melee enemies lunge when close; ranged ones keep their distance and shoot (a wind-up you can see, then a
 * hitscan shot that can miss). "Damage" messages hurt it: it flashes, gets knocked back, turns on whoever hit
 * it, and at 0 health adds its score and shrinks away.
 *
 * Needs a Character Controller (to walk) and a collider (what bullets and swords hit). A child named "Body"
 * flashes red when hit; a child of the body named "Eye" glows while a ranged enemy aims. */
#include "game.h"

enum { IDLE, WANDER, CHASE, WINDUP, RECOVER, DEAD };

typedef struct Enemy {
    int state;
    float t;                    /* seconds in this state */
    bool started;
    LxVec3 home, wander_to;
    float yaw, vy;
    float cooldown;             /* until the next attack */
    float flash;                /* red after a hit */
    float lost;                 /* seconds without seeing the player */
    LxVec3 knock;               /* knock-back velocity */
    LxEntity body, eye, muzzle;
    char body_material[LX_REF_MAX];
    LxVec3 body_scale, eye_scale;
} Enemy;

#define P_HEALTH 0
#define P_SPEED 1
#define P_SIGHT 2
#define P_REACH 3
#define P_DAMAGE 4
#define P_SCORE 5
#define P_RANGED 6
#define P_RATE 7

static void go(Enemy *e, int state) { e->state = state; e->t = 0; }

static LxEntity child(LxScene *s, LxEntity parent, const char *name) { return lx_entity_find_child(s, parent, name); }

static void enemy_start(LxBehaviourCtx *ctx) {
    Enemy *e = ctx->state;
    LxScene *s = ctx->scene;
    e->started = true;
    e->home = lx_entity_world_position(s, ctx->self);
    e->yaw = lx_entity_euler(s, ctx->self).y;
    e->body = child(s, ctx->self, "Body");
    e->eye = child(s, ctx->self, "Eye");
    if (e->body) {
        LxMeshRenderer *m = lx_get_mesh(s, e->body);
        if (m) snprintf(e->body_material, sizeof e->body_material, "%s", m->material);
        e->body_scale = lx_entity_scale(s, e->body);
    }
    if (e->eye) e->eye_scale = lx_entity_scale(s, e->eye);
    if (ctx->params[P_RANGED] > 0) {   /* a light for the muzzle flash */
        e->muzzle = lx_entity_create(s, "Muzzle Flash");
        lx_entity_set_parent(s, e->muzzle, ctx->self);
        lx_entity_set_position(s, e->muzzle, lx_v3(-0.35f, 1.3f, 0.7f));   /* front right (+Z ahead, -X right) */
        LxLight *l = lx_add_light(s, e->muzzle, LX_LIGHT_POINT);
        l->color = lx_v3(1, 0.45f, 0.2f);
        l->intensity = 0;
        l->range = 6;
        l->cast_shadows = false;
    }
    e->cooldown = randf(0.3f, 1.0f);
    go(e, IDLE);
    e->t = -randf(0, 1.5f);   /* don't all move at once */
}

static LxEntity player_of(LxScene *s) { return lx_entity_find_with_tag(s, "Player"); }

/* can it see the player: close enough, in front (or already hunting), and nothing in the way */
static bool sees(LxBehaviourCtx *ctx, Enemy *e, LxEntity player, float range, bool any_direction) {
    if (!player) return false;
    LxScene *s = ctx->scene;
    LxVec3 me = lx_add3(lx_entity_world_position(s, ctx->self), lx_v3(0, 1.5f, 0));
    LxVec3 him = lx_add3(lx_entity_world_position(s, player), lx_v3(0, 1.2f, 0));
    LxVec3 to = lx_sub3(him, me);
    float d = lx_len3(to);
    if (d > range || d < 1e-3f) return d < 1e-3f;
    if (!any_direction && lx_dot3(lx_norm3(flat(to)), forward_of_yaw(e->yaw)) < -0.1f) return false;   /* about 100 degrees either side */
    LxRaycastHit hit;
    if (!lx_physics_raycast_ignore(s, me, lx_scale3(to, 1.0f / d), d, LX_ALL_LAYERS, ctx->self, &hit)) return true;
    return root_of(s, hit.entity) == player;
}

/* noticing the player without seeing them: footsteps right behind it, or a gunshot within earshot */
static bool hears(LxBehaviourCtx *ctx, LxEntity player, float dist) {
    if (!player) return false;
    if (dist < 2.5f) return true;
    if (g_game.noise_time > 0.2f) return false;
    LxVec3 me = lx_entity_world_position(ctx->scene, ctx->self);
    return lx_len3(lx_sub3(g_game.noise_at, me)) < 25;
}

/* walk (with gravity) in a flat direction, turning to face it */
static void walk(LxBehaviourCtx *ctx, Enemy *e, LxVec3 dir, float speed, float dt, bool face) {
    LxScene *s = ctx->scene;
    dir = flat(dir);
    float len = lx_len3(dir);
    LxVec3 v = len > 1e-4f ? lx_scale3(dir, speed / len) : lx_v3(0, 0, 0);
    if (face && len > 1e-4f) e->yaw += angle_delta(e->yaw, yaw_of(dir)) * smooth(10, dt);
    /* keep a little apart from other enemies */
    int n = lx_scene_entity_count(s);
    LxVec3 me = lx_entity_world_position(s, ctx->self);
    for (int i = 0; i < n; i++) {
        LxEntity o = lx_scene_entity_at(s, i);
        if (!o || o == ctx->self || !lx_entity_has_tag(s, o, "Enemy")) continue;
        LxVec3 away = flat(lx_sub3(me, lx_entity_world_position(s, o)));
        float d = lx_len3(away);
        if (d > 1e-3f && d < 1.3f) v = lx_add3(v, lx_scale3(away, (1.3f - d) * 3.0f / d));
    }
    if (lx_character_grounded(s, ctx->self) && e->vy < 0) e->vy = -2;
    e->vy = fmaxf(e->vy - 20 * dt, -40);
    LxVec3 move = lx_add3(v, e->knock);
    lx_character_move(s, ctx->self, lx_v3(move.x * dt, e->vy * dt, move.z * dt));
    e->knock = lx_scale3(e->knock, 1 - smooth(8, dt));
    lx_entity_set_euler(s, ctx->self, lx_v3(0, e->yaw, 0));
}

static void attack_hits(LxBehaviourCtx *ctx, Enemy *e, LxEntity player) {
    LxScene *s = ctx->scene;
    float damage = ctx->params[P_DAMAGE];
    if (ctx->params[P_RANGED] > 0) {
        if (e->muzzle) lx_get_light(s, e->muzzle)->intensity = 30;
        if (!sees(ctx, e, player, ctx->params[P_SIGHT] * 1.5f, true)) return;
        float d = lx_len3(lx_sub3(lx_entity_world_position(s, player), lx_entity_world_position(s, ctx->self)));
        float chance = clampf(1.0f - d / (ctx->params[P_SIGHT] * 1.6f), 0.25f, 0.85f);   /* closer = more likely to hit */
        if (lx_random_chance(chance)) lx_send_message(s, player, "Damage", damage, ctx->self);
    } else {
        float d = lx_len3(flat(lx_sub3(lx_entity_world_position(s, player), lx_entity_world_position(s, ctx->self))));
        if (d <= ctx->params[P_REACH] * 1.35f) lx_send_message(s, player, "Damage", damage, ctx->self);
    }
}

static void enemy_update(LxBehaviourCtx *ctx, float dt) {
    Enemy *e = ctx->state;
    LxScene *s = ctx->scene;
    if (!e->started) enemy_start(ctx);
    if (dt <= 0) return;
    e->t += dt;
    e->cooldown -= dt;
    LxEntity player = player_of(s);
    LxVec3 me = lx_entity_world_position(s, ctx->self);
    LxVec3 to_player = player ? lx_sub3(lx_entity_world_position(s, player), me) : lx_v3(0, 0, 0);
    float dist = lx_len3(flat(to_player));
    bool ranged = ctx->params[P_RANGED] > 0;
    float sight = ctx->params[P_SIGHT], reach = ctx->params[P_REACH], speed = ctx->params[P_SPEED];
    /* the hit flash and the muzzle light fade */
    if (e->flash > 0) {
        e->flash -= dt;
        if (e->flash <= 0 && e->body) snprintf(lx_get_mesh(s, e->body)->material, LX_REF_MAX, "%s", e->body_material);
    }
    if (e->muzzle) { LxLight *l = lx_get_light(s, e->muzzle); l->intensity = approach(l->intensity, 0, dt * 400); }
    if (e->state != DEAD && g_game.phase == PHASE_LOST) {   /* the player is down: celebrate */
        e->yaw += 140 * dt;
        walk(ctx, e, lx_v3(0, 0, 0), 0, dt, false);
        return;
    }
    if (!game_playing() && e->state != DEAD) { walk(ctx, e, lx_v3(0, 0, 0), 0, dt, false); return; }

    switch (e->state) {
    case IDLE:
        walk(ctx, e, lx_v3(0, 0, 0), 0, dt, false);
        if (sees(ctx, e, player, sight, false) || hears(ctx, player, dist)) { go(e, CHASE); break; }
        if (e->t > 2.5f) {
            float a = randf(0, 6.2832f), r = randf(2, 6);
            e->wander_to = lx_add3(e->home, lx_v3(cosf(a) * r, 0, sinf(a) * r));
            go(e, WANDER);
        }
        break;
    case WANDER: {
        LxVec3 to = flat(lx_sub3(e->wander_to, me));
        walk(ctx, e, to, speed * 0.45f, dt, true);
        if (sees(ctx, e, player, sight, false) || hears(ctx, player, dist)) go(e, CHASE);
        else if (lx_len3(to) < 0.5f || e->t > 6) go(e, IDLE);
        break;
    }
    case CHASE: {
        bool see = sees(ctx, e, player, sight * 1.6f, true);
        e->lost = see || hears(ctx, player, dist) ? 0 : e->lost + dt;
        if (e->lost > 4) { e->home = me; go(e, IDLE); break; }
        float stop = ranged ? sight * 0.6f : reach * 0.85f;
        LxVec3 dir = dist > stop ? to_player : lx_v3(0, 0, 0);
        if (ranged && dist < stop * 0.5f) dir = lx_neg3(to_player);   /* back off a little */
        walk(ctx, e, dir, speed, dt, true);
        if (dist > 1e-3f) e->yaw += angle_delta(e->yaw, yaw_of(to_player)) * smooth(8, dt);
        bool in_reach = ranged ? see && dist < sight : dist <= reach;
        if (in_reach && e->cooldown <= 0) go(e, WINDUP);
        break;
    }
    case WINDUP: {   /* the tell: squash (melee) or a glowing eye (ranged), then the attack */
        float len = ranged ? 0.6f : 0.35f;
        float k = clampf(e->t / len, 0, 1);
        if (e->body && !ranged) lx_entity_set_scale(s, e->body, lx_v3(e->body_scale.x * (1 + 0.25f * k), e->body_scale.y * (1 - 0.2f * k), e->body_scale.z * (1 + 0.25f * k)));
        if (e->eye && ranged) lx_entity_set_scale(s, e->eye, lx_scale3(e->eye_scale, 1 + 0.8f * k));
        if (dist > 1e-3f) e->yaw += angle_delta(e->yaw, yaw_of(to_player)) * smooth(14, dt);
        walk(ctx, e, ranged ? lx_v3(0, 0, 0) : lx_scale3(lx_norm3(flat(to_player)), k > 0.7f ? 1 : 0), ranged ? 0 : speed * 1.6f, dt, false);
        if (e->t >= len) {
            if (player) attack_hits(ctx, e, player);
            e->cooldown = ctx->params[P_RATE] * randf(0.85f, 1.25f);
            go(e, RECOVER);
        }
        break;
    }
    case RECOVER:
        if (e->body) lx_entity_set_scale(s, e->body, lx_lerp3(lx_entity_scale(s, e->body), e->body_scale, smooth(12, dt)));
        if (e->eye) lx_entity_set_scale(s, e->eye, lx_lerp3(lx_entity_scale(s, e->eye), e->eye_scale, smooth(12, dt)));
        walk(ctx, e, lx_v3(0, 0, 0), 0, dt, false);
        if (e->t > 0.35f) go(e, CHASE);
        break;
    case DEAD:
        if (e->t > 0.45f) lx_entity_destroy(s, ctx->self);
        break;
    }
}

static void enemy_message(LxBehaviourCtx *ctx, const char *message, float value, LxEntity sender) {
    Enemy *e = ctx->state;
    LxScene *s = ctx->scene;
    if (strcmp(message, "Damage") || e->state == DEAD) return;
    if (!e->started) enemy_start(ctx);
    ctx->params[P_HEALTH] -= value;
    /* flash, get knocked back from whoever hit it, and go after them */
    if (e->body) {
        snprintf(lx_get_mesh(s, e->body)->material, LX_REF_MAX, "%s", "builtin:red");
        e->flash = 0.12f;
    }
    if (sender) {
        LxVec3 away = flat(lx_sub3(lx_entity_world_position(s, ctx->self), lx_entity_world_position(s, root_of(s, sender))));
        float d = lx_len3(away);
        if (d > 1e-3f) e->knock = lx_add3(e->knock, lx_scale3(away, fminf(value, 60) * 0.12f / d));
    }
    if (ctx->params[P_HEALTH] <= 0) {
        g_game.score += (int)ctx->params[P_SCORE];
        lx_entity_set_tag(s, ctx->self, "Dead");   /* no longer counted, aimed at or locked on to */
        if (g_game.lock_target == ctx->self) g_game.lock_target = 0;
        lx_tween_scale(s, ctx->self, lx_v3(0.01f, 0.01f, 0.01f), 0.4f, LX_EASE_IN);
        go(e, DEAD);
    } else if (e->state == IDLE || e->state == WANDER) go(e, CHASE);
}

LX_SCRIPT_REGISTER(enemy) {
    LxBehaviourDesc d = {0};
    d.name = "Enemy";
    d.state_size = sizeof(Enemy);
    d.param_names[P_HEALTH] = "Health";
    d.param_names[P_SPEED] = "Speed";
    d.param_names[P_SIGHT] = "Sight Range";
    d.param_names[P_REACH] = "Attack Reach";
    d.param_names[P_DAMAGE] = "Attack Damage";
    d.param_names[P_SCORE] = "Score";
    d.param_names[P_RANGED] = "Ranged (0 / 1)";
    d.param_names[P_RATE] = "Seconds Between Attacks";
    float defaults[8] = {60, 3.2f, 14, 1.7f, 10, 100, 0, 1.1f};
    for (int i = 0; i < 8; i++) d.param_defaults[i] = defaults[i];
    d.on_start = enemy_start;
    d.on_update = enemy_update;
    d.on_message = enemy_message;
    lx_register_behaviour(&d);
}
