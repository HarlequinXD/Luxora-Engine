/* Impact: a bullet hit's spark - flashes, shrinks and removes itself. Used by Prefabs/Impact. */
#include "game.h"

typedef struct Impact { float t; LxVec3 size; bool started; } Impact;

static void impact_update(LxBehaviourCtx *ctx, float dt) {
    Impact *m = ctx->state;
    if (!m->started) { m->started = true; m->size = lx_entity_scale(ctx->scene, ctx->self); }
    m->t += dt;
    float k = 1 - clampf(m->t / 0.22f, 0, 1);
    lx_entity_set_scale(ctx->scene, ctx->self, lx_scale3(m->size, 0.02f + k * k));
    if (m->t >= 0.22f) lx_entity_destroy(ctx->scene, ctx->self);
}

LX_SCRIPT_REGISTER(impact) {
    LxBehaviourDesc d = {0};
    d.name = "Impact";
    d.state_size = sizeof(Impact);
    d.on_update = impact_update;
    lx_register_behaviour(&d);
}
