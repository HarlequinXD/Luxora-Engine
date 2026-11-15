/* Pickup: spins and bobs until it's taken. Walk-over pickups (coins) are taken by touching them; the others
 * (health, ammo) need the Interact button while looking at them - tag those "Interactable".
 *
 * Params: Kind (0 health, 1 ammo, 2 coin), Amount, Walk Over (1 = touch to take), Radius. */
#include "game.h"

enum { KIND_HEALTH, KIND_AMMO, KIND_COIN };

typedef struct Pickup {
    bool started, taken;
    float t, base_y, gone;
} Pickup;

static void pickup_start(LxBehaviourCtx *ctx) {
    Pickup *p = ctx->state;
    p->started = true;
    p->base_y = lx_entity_position(ctx->scene, ctx->self).y;
    p->t = randf(0, 6);
}

static const char *what(int kind) { return kind == KIND_HEALTH ? "Health" : kind == KIND_AMMO ? "Ammo" : "Coin"; }

/* true when it was used up */
static bool take(LxBehaviourCtx *ctx) {
    int kind = (int)ctx->params[0], amount = (int)ctx->params[1];
    char text[64];
    switch (kind) {
    case KIND_HEALTH:
        if (g_game.health >= g_game.max_health) { game_message("Health is full"); return false; }
        g_game.health = fminf(g_game.max_health, g_game.health + (float)amount);
        snprintf(text, sizeof text, "+%d health", amount);
        break;
    case KIND_AMMO:
        g_game.reserve += amount;
        snprintf(text, sizeof text, "+%d ammo", amount);
        break;
    default:
        g_game.score += amount;
        snprintf(text, sizeof text, "+%d", amount);
        break;
    }
    game_message(text);
    return true;
}

static void collect(LxBehaviourCtx *ctx, Pickup *p) {
    if (p->taken || !game_playing() || !take(ctx)) return;
    p->taken = true;
    lx_entity_set_tag(ctx->scene, ctx->self, "Taken");   /* a coin stops counting the moment it's taken */
    lx_tween_scale(ctx->scene, ctx->self, lx_v3(0.01f, 0.01f, 0.01f), 0.18f, LX_EASE_BACK_OUT);
}

static void pickup_update(LxBehaviourCtx *ctx, float dt) {
    Pickup *p = ctx->state;
    LxScene *s = ctx->scene;
    if (!p->started) pickup_start(ctx);
    if (p->taken) {
        p->gone += dt;
        if (p->gone > 0.2f) lx_entity_destroy(s, ctx->self);
        return;
    }
    /* spin and bob */
    p->t += dt;
    LxVec3 pos = lx_entity_position(s, ctx->self), e = lx_entity_euler(s, ctx->self);
    lx_entity_set_position(s, ctx->self, lx_v3(pos.x, p->base_y + sinf(p->t * 2.4f) * 0.12f, pos.z));
    lx_entity_set_euler(s, ctx->self, lx_v3(e.x, e.y + 110 * dt, e.z));
    /* walk-over pickups */
    if (ctx->params[2] > 0) {
        LxEntity player = lx_entity_find_with_tag(s, "Player");
        if (!player) return;
        LxVec3 d = lx_sub3(lx_add3(lx_entity_world_position(s, player), lx_v3(0, 0.9f, 0)), lx_entity_world_position(s, ctx->self));
        float r = ctx->params[3] > 0 ? ctx->params[3] : 1.2f;
        if (lx_len3sq(flat(d)) < r * r && fabsf(d.y) < 1.6f) collect(ctx, p);
    }
}

static void pickup_message(LxBehaviourCtx *ctx, const char *message, float value, LxEntity sender) {
    Pickup *p = ctx->state;
    (void)value; (void)sender;
    if (p->taken) return;
    if (!strcmp(message, "Hover")) snprintf(g_game.prompt, sizeof g_game.prompt, "Pick up %s (+%d)", what((int)ctx->params[0]), (int)ctx->params[1]);
    else if (!strcmp(message, "Interact")) collect(ctx, p);
}

LX_SCRIPT_REGISTER(pickup) {
    LxBehaviourDesc d = {0};
    d.name = "Pickup";
    d.state_size = sizeof(Pickup);
    d.param_names[0] = "Kind (0 health, 1 ammo, 2 coin)";
    d.param_names[1] = "Amount";
    d.param_names[2] = "Walk Over (0 / 1)";
    d.param_names[3] = "Radius";
    d.param_defaults[0] = 0;
    d.param_defaults[1] = 25;
    d.param_defaults[2] = 0;
    d.param_defaults[3] = 1.2f;
    d.on_start = pickup_start;
    d.on_update = pickup_update;
    d.on_message = pickup_message;
    lx_register_behaviour(&d);
}
