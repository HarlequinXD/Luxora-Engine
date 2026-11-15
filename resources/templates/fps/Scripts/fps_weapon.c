/* FPS Weapon: a hitscan rifle. Put it on the gun model, a child of the camera; tag it "Weapon".
 *
 *   Fire (left mouse / right trigger): automatic or single shots, a bullet per shot from the clip
 *   Aim (right mouse / left trigger): zoom, steadier aim, the gun comes to the middle
 *   Reload (R): bullets move from the reserve into the clip (automatic when you fire an empty clip)
 *   Spread grows while firing and moving, shrinks when still or aiming; each shot kicks the camera up.
 *   Hits send "Damage" to what they hit (twice as much on the head) and leave a spark (Prefabs/Impact). */
#include "game.h"

#define P_DAMAGE 0
#define P_RATE 1
#define P_RELOAD 2
#define P_RANGE 3
#define P_SPREAD 4
#define P_RECOIL 5
#define P_AUTO 6

typedef struct Weapon {
    bool started;
    LxEntity cam, player, light;
    LxVec3 hip;                 /* where the gun rests */
    float cooldown, reload_t, heat;   /* heat: spread from firing */
    float aim;                  /* 0 hip .. 1 aimed */
    float kick;                 /* the gun's own jolt back */
    LxVec2 sway;
    float bob_t;
    bool reloading;
} Weapon;

static const LxVec3 AIM_POS = {0.0f, -0.165f, -0.36f};

static void weapon_start(LxBehaviourCtx *ctx) {
    Weapon *w = ctx->state;
    LxScene *s = ctx->scene;
    w->started = true;
    w->cam = lx_entity_parent(s, ctx->self);
    w->player = root_of(s, ctx->self);
    w->light = w->cam ? lx_entity_find_child(s, w->cam, "Muzzle Light") : 0;
    w->hip = lx_entity_position(s, ctx->self);
}

static void start_reload(LxBehaviourCtx *ctx, Weapon *w) {
    (void)ctx;
    if (w->reloading || g_game.reserve <= 0 || g_game.ammo >= g_game.clip_size) return;
    w->reloading = true;
    w->reload_t = 0;
}

static void finish_reload(Weapon *w) {
    int take = g_game.clip_size - g_game.ammo;
    if (take > g_game.reserve) take = g_game.reserve;
    g_game.ammo += take;
    g_game.reserve -= take;
    w->reloading = false;
}

static void shoot(LxBehaviourCtx *ctx, Weapon *w) {
    LxScene *s = ctx->scene;
    g_game.ammo--;
    w->cooldown = 1.0f / fmaxf(ctx->params[P_RATE], 0.5f);
    /* aim: the camera's forward, pushed off by the current spread */
    LxQuat q = lx_entity_world_rotation(s, w->cam);
    float spread = g_game.spread;
    float a = randf(0, 6.2832f), r = randf(0, 1) * spread * (3.14159f / 180);
    LxVec3 dir = lx_quat_rotate(q, lx_norm3(lx_v3(cosf(a) * r, sinf(a) * r, -1)));
    LxVec3 from = lx_entity_world_position(s, w->cam);
    LxRaycastHit hit;
    if (lx_physics_raycast_ignore(s, from, dir, ctx->params[P_RANGE], LX_ALL_LAYERS, w->player, &hit)) {
        lx_prefab_instantiate(s, "Prefabs/Impact.lxprefab", lx_madd3(hit.point, hit.normal, 0.03f), 0);
        LxEntity enemy = tagged_up(s, hit.entity, "Enemy");
        if (enemy) {
            float damage = ctx->params[P_DAMAGE];
            bool head = hit.point.y - lx_entity_world_position(s, enemy).y > 1.45f;
            if (head) damage *= 2;
            lx_send_message(s, enemy, "Damage", damage, w->player);
            g_game.hit_marker = head ? 1.0f : 0.75f;
        }
    }
    /* feel: camera kick, gun jolt, flash, more spread */
    g_game.recoil += ctx->params[P_RECOIL] * (g_game.aiming ? 0.5f : 1.0f) * randf(0.8f, 1.2f);
    w->kick = 1;
    w->heat = fminf(w->heat + 0.35f, 2.5f);
    game_shake(0.08f);
    g_game.noise_at = from;   /* enemies hear it */
    g_game.noise_time = 0;
    if (w->light) lx_get_light(s, w->light)->intensity = 45;
}

static void weapon_update(LxBehaviourCtx *ctx, float dt) {
    Weapon *w = ctx->state;
    LxScene *s = ctx->scene;
    if (!w->started) weapon_start(ctx);
    if (dt <= 0 || !w->cam) return;
    bool playing = game_playing();
    w->cooldown -= dt;
    if (w->light) { LxLight *l = lx_get_light(s, w->light); l->intensity = approach(l->intensity, 0, dt * 900); }

    /* aiming */
    bool aim = playing && lx_action_down("Aim") && !w->reloading && !g_game.sprinting;
    g_game.aiming = aim;
    w->aim += ((aim ? 1.0f : 0.0f) - w->aim) * smooth(14, dt);

    /* reloading */
    if (playing && lx_action_pressed("Reload")) start_reload(ctx, w);
    float reload_len = fmaxf(ctx->params[P_RELOAD], 0.2f);
    if (w->reloading) {
        w->reload_t += dt;
        if (w->reload_t >= reload_len) finish_reload(w);
    }
    g_game.reloading = w->reloading;
    g_game.reload_progress = w->reloading ? w->reload_t / reload_len : 0;

    /* firing */
    bool trigger = playing && (ctx->params[P_AUTO] > 0 ? lx_action_down("Fire") : lx_action_pressed("Fire"));
    if (trigger && !w->reloading && !g_game.sprinting && w->cooldown <= 0) {
        if (g_game.ammo > 0) shoot(ctx, w);
        else if (lx_action_pressed("Fire")) {
            if (g_game.reserve > 0) start_reload(ctx, w);
            else game_message("Out of ammo");
            w->cooldown = 0.25f;
        }
    }

    /* spread: base, plus moving, plus firing heat; aiming tightens it */
    LxCharacterController *cc = w->player ? lx_get_character(s, w->player) : NULL;
    float speed = cc ? lx_len3(flat(cc->velocity)) : 0;
    bool airborne = cc && !cc->grounded;
    w->heat = approach(w->heat, 0, dt * 3.5f);
    float target = ctx->params[P_SPREAD] * (1 + speed * 0.18f + w->heat + (airborne ? 1.5f : 0)) * (1 - 0.75f * w->aim);
    g_game.spread += (target - g_game.spread) * smooth(16, dt);

    /* the gun's pose: hip or aimed, sway from looking, bob from walking, jolt from firing, dip while reloading */
    LxVec2 look = lx_v2(lx_axis("LookX"), lx_axis("LookY"));
    if (!isfinite(look.x) || !isfinite(look.y)) look = lx_v2(0, 0);
    w->sway.x += (clampf(-look.x * 0.004f, -0.05f, 0.05f) - w->sway.x) * smooth(8, dt);
    w->sway.y += (clampf(-look.y * 0.004f, -0.05f, 0.05f) - w->sway.y) * smooth(8, dt);
    w->bob_t += dt * (4 + speed * 1.4f);
    float bob = clampf(speed / 6, 0, 1) * (1 - 0.85f * w->aim) * (airborne ? 0 : 1);
    w->kick = approach(w->kick, 0, dt * 9);
    LxVec3 pos = lx_lerp3(w->hip, AIM_POS, w->aim);
    pos = lx_add3(pos, lx_v3(w->sway.x + cosf(w->bob_t) * 0.012f * bob, w->sway.y - fabsf(sinf(w->bob_t)) * 0.016f * bob, w->kick * 0.06f));
    float dip = w->reloading ? sinf(3.14159f * clampf(w->reload_t / reload_len, 0, 1)) : 0;
    if (g_game.sprinting) dip = fmaxf(dip, 0.5f);
    pos.y -= dip * 0.12f;
    lx_entity_set_position(s, ctx->self, pos);
    lx_entity_set_euler(s, ctx->self, lx_v3(w->kick * 6 - dip * 30, w->sway.x * 120 + dip * 18, dip * 20));
}

LX_SCRIPT_REGISTER(fps_weapon) {
    LxBehaviourDesc d = {0};
    d.name = "FPS Weapon";
    d.state_size = sizeof(Weapon);
    d.param_names[P_DAMAGE] = "Damage";
    d.param_names[P_RATE] = "Shots Per Second";
    d.param_names[P_RELOAD] = "Reload Seconds";
    d.param_names[P_RANGE] = "Range";
    d.param_names[P_SPREAD] = "Spread (degrees)";
    d.param_names[P_RECOIL] = "Recoil (degrees)";
    d.param_names[P_AUTO] = "Automatic (0 / 1)";
    float defaults[7] = {25, 8, 1.3f, 120, 0.9f, 1.4f, 1};
    for (int i = 0; i < 7; i++) d.param_defaults[i] = defaults[i];
    d.on_start = weapon_start;
    d.on_update = weapon_update;
    lx_register_behaviour(&d);
}
