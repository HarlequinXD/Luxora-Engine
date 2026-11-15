/* Door: slides open sideways when the player uses it, and closes again by itself after a while (or on the
 * next use). Give it a collider and tag it "Interactable".
 *
 * Params: Slide Distance (metres, along the door's own X), Seconds To Open, Close After (seconds, 0 = stay open). */
#include "game.h"

typedef struct Door {
    bool started, open;
    LxVec3 closed_pos;
    float open_time;
} Door;

static void door_start(LxBehaviourCtx *ctx) {
    Door *d = ctx->state;
    d->started = true;
    d->closed_pos = lx_entity_position(ctx->scene, ctx->self);
}

static void door_set(LxBehaviourCtx *ctx, Door *d, bool open) {
    LxScene *s = ctx->scene;
    d->open = open;
    d->open_time = 0;
    LxVec3 side = lx_quat_rotate(lx_entity_rotation(s, ctx->self), lx_v3(1, 0, 0));
    LxVec3 to = open ? lx_madd3(d->closed_pos, side, ctx->params[0]) : d->closed_pos;
    lx_tween_position(s, ctx->self, to, ctx->params[1] > 0 ? ctx->params[1] : 0.6f, LX_EASE_IN_OUT);
}

static void door_update(LxBehaviourCtx *ctx, float dt) {
    Door *d = ctx->state;
    if (!d->started) door_start(ctx);
    if (!d->open || ctx->params[2] <= 0) return;
    d->open_time += dt;
    if (d->open_time < ctx->params[2]) return;
    /* close only when nobody stands in the doorway */
    LxEntity player = lx_entity_find_with_tag(ctx->scene, "Player");
    if (player && lx_len3(flat(lx_sub3(lx_entity_world_position(ctx->scene, player), d->closed_pos))) < 1.6f) return;
    door_set(ctx, d, false);
}

static void door_message(LxBehaviourCtx *ctx, const char *message, float value, LxEntity sender) {
    Door *d = ctx->state;
    (void)value; (void)sender;
    if (!d->started) door_start(ctx);
    if (!strcmp(message, "Hover")) snprintf(g_game.prompt, sizeof g_game.prompt, "%s Door", d->open ? "Close" : "Open");
    else if (!strcmp(message, "Interact")) door_set(ctx, d, !d->open);
}

LX_SCRIPT_REGISTER(door) {
    LxBehaviourDesc d = {0};
    d.name = "Door";
    d.state_size = sizeof(Door);
    d.param_names[0] = "Slide Distance";
    d.param_names[1] = "Seconds To Open";
    d.param_names[2] = "Close After";
    d.param_defaults[0] = 2.2f;
    d.param_defaults[1] = 0.6f;
    d.param_defaults[2] = 6;
    d.on_start = door_start;
    d.on_update = door_update;
    d.on_message = door_message;
    lx_register_behaviour(&d);
}
