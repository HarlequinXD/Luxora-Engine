/* Gameplay: the AI, Targeting, Movement, Combat and Interaction categories.
 *
 * These are the actions a game is actually built out of — seeing, patrolling, chasing, shooting,
 * taking damage, spawning waves — written on top of Luxora's game kit, so a C behaviour can do
 * exactly the same things a graph can. */
#include "fsm_actions.h"

/* ==========================================================================================
 *  senses
 * ========================================================================================== */
static void ai_line_of_sight(LxFsmCtx *c) {
    LxEntity me = fsm_obj(c, 0), target = fsm_obj(c, 1);
    if (!me || !target) { fsm_send(c, 8); return; }
    bool seen = lx_line_of_sight(c->scene, me, target, lx_fsm_float(c, 2), lx_fsm_float(c, 3),
                                 lx_fsm_float(c, 4), lx_fsm_float(c, 5));
    float dist = lx_len3(lx_sub3(lx_entity_world_position(c->scene, target), lx_entity_world_position(c->scene, me)));
    lx_fsm_set_bool(c, 6, seen);
    lx_fsm_set_float(c, 7, dist);
    fsm_send(c, seen ? 9 : 8);
}
static const LxFsmActionDesc A_AI_LOS = {"Can See Target", "AI", "Is the target in range, inside the view cone, and not behind anything? The whole of \"can this enemy see the player\" in one action.",
    "{0} sees {1}?",
    {POBJ("Object"), POBJ("Target"), P("Range", LX_FSM_FLOAT, "15"),
     PT("View Angle", LX_FSM_FLOAT, "90", "the full width of the cone in degrees; 360 sees all round"),
     PT("Eye Height", LX_FSM_FLOAT, "1.6", "how far above its origin this object looks from"),
     PT("Target Height", LX_FSM_FLOAT, "1.0", "how far above its origin the target is aimed at"),
     POPT("Store Can See", LX_FSM_BOOL), POPT("Store Distance", LX_FSM_FLOAT),
     PEV("Cannot See Event"), PEV("Can See Event")},
    0, 0, LX_FSM_EVERY_FRAME, ai_line_of_sight, ai_line_of_sight, NULL};

/* ==========================================================================================
 *  moving about
 * ========================================================================================== */
typedef struct { float vy; } FsmWalk;
static void ai_walk_to(LxFsmCtx *c) {
    FsmWalk *w = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) { lx_fsm_finish(c); return; }
    LxEntity t = fsm_obj(c, 1);
    LxVec3 point = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 2);
    bool there = lx_walk_to(c->scene, e, point, lx_fsm_float(c, 3), lx_fsm_float(c, 4), lx_fsm_float(c, 5), &w->vy, c->dt);
    float dist = lx_len3(lx_sub3(point, lx_entity_world_position(c->scene, e)));
    lx_fsm_set_float(c, 6, dist);
    if (there) { lx_fsm_finish(c); fsm_send(c, 7); }
}
static const LxFsmActionDesc A_AI_WALK_TO = {"Walk To", "AI", "Walks an object to a point, turning to face the way it goes. With a Character Controller it falls and slides along walls; without one it simply moves.",
    "{0} walks to {1}",
    {POBJ("Object"), POBJNT("Target", "None uses the position below"), P("Target Position", LX_FSM_VEC3, "0 0 0"),
     P("Speed", LX_FSM_FLOAT, "3"), PT("Stop Distance", LX_FSM_FLOAT, "0.5", "how close counts as arrived"),
     PT("Turn Speed", LX_FSM_FLOAT, "360", "degrees per second; 0 = snap round at once"),
     POPT("Store Distance", LX_FSM_FLOAT), PEV("Arrived Event")},
    sizeof(FsmWalk), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, ai_walk_to, ai_walk_to, NULL};

static void ai_patrol(LxFsmCtx *c) {
    LxPatrol *p = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    int flags = lx_patrol_update(c->scene, e, fsm_obj(c, 1), p, lx_fsm_float(c, 2), lx_fsm_float(c, 3),
                                 (LxPatrolMode)lx_fsm_int(c, 4), c->dt);
    lx_fsm_set_int(c, 5, p->index);
    if (flags & LX_PATROL_REACHED) fsm_send(c, 6);
    if (flags & LX_PATROL_FINISHED) { lx_fsm_finish(c); fsm_send(c, 7); }
}
static const LxFsmActionDesc A_AI_PATROL = {"Patrol", "AI", "Walks an object round a set of waypoints — the children of the waypoints object — pausing at each one.",
    "{0} patrols {1}",
    {POBJ("Object"), POBJT("Waypoints", "an object whose children are the points to visit"),
     P("Speed", LX_FSM_FLOAT, "2"), PT("Wait Seconds", LX_FSM_FLOAT, "1", "how long to pause at each point"),
     PENUMT("Mode", "Loop|Ping Pong|Once", "0", "Ping Pong walks back along the route; Once stops at the end"),
     POPT("Store Waypoint Index", LX_FSM_INT), PEV("Reached Point Event"), PEV("Finished Event")},
    sizeof(LxPatrol), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, ai_patrol, ai_patrol, NULL};

static void ai_wander(LxFsmCtx *c) {
    LxWander *w = c->state;
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    bool arrived = lx_wander_update(c->scene, e, w, lx_fsm_float(c, 1), lx_fsm_float(c, 2),
                                    lx_fsm_float(c, 3), lx_fsm_float(c, 4), c->dt);
    lx_fsm_set_bool(c, 5, w->moving);
    if (arrived) fsm_send(c, 6);
}
static const LxFsmActionDesc A_AI_WANDER = {"Wander", "AI", "Drifts an object around its starting place, pausing now and then: idle animals, ambient crowds.",
    "{0} wanders",
    {POBJ("Object"), PT("Radius", LX_FSM_FLOAT, "8", "how far from where it started it will stray"),
     P("Speed", LX_FSM_FLOAT, "1.5"), P("Min Wait", LX_FSM_FLOAT, "1"), P("Max Wait", LX_FSM_FLOAT, "4"),
     POPT("Store Is Moving", LX_FSM_BOOL), PEV("Arrived Event")},
    sizeof(LxWander), 0, LX_FSM_EVERY_FRAME, ai_wander, ai_wander, NULL};

typedef struct { float vy; } FsmFlee;
static void ai_flee(LxFsmCtx *c) {
    FsmFlee *f = c->state;
    LxEntity e = fsm_obj(c, 0), from = fsm_obj(c, 1);
    if (!e || !from) return;
    LxVec3 me = lx_entity_world_position(c->scene, e);
    LxVec3 away = lx_sub3(me, lx_entity_world_position(c->scene, from));
    away.y = 0;
    float dist = lx_len3(away);
    lx_fsm_set_float(c, 4, dist);
    if (dist >= lx_fsm_float(c, 3)) { fsm_send(c, 5); return; }
    if (dist < 1e-4f) away = lx_v3(1, 0, 0);
    LxVec3 goal = lx_madd3(me, lx_norm3(away), lx_fsm_float(c, 3));
    lx_walk_to(c->scene, e, goal, lx_fsm_float(c, 2), 0.1f, 540.0f, &f->vy, c->dt);
}
static const LxFsmActionDesc A_AI_FLEE = {"Flee From", "AI", "Runs an object directly away from another until it is far enough off.",
    "{0} flees {1}",
    {POBJ("Object"), POBJ("Flee From"), P("Speed", LX_FSM_FLOAT, "4"),
     PT("Safe Distance", LX_FSM_FLOAT, "15", "stop running once this far away"),
     POPT("Store Distance", LX_FSM_FLOAT), PEV("Safe Event")},
    sizeof(FsmFlee), 0, LX_FSM_EVERY_FRAME, ai_flee, ai_flee, NULL};

/* ==========================================================================================
 *  combat
 * ========================================================================================== */
static void cb_shoot(LxFsmCtx *c) {
    LxEntity shooter = fsm_obj(c, 0);
    if (!shooter) return;
    LxVec3 from = lx_entity_transform_point(c->scene, shooter, lx_fsm_vec3(c, 1));
    LxVec3 dir = lx_entity_transform_direction(c->scene, shooter, lx_fsm_vec3(c, 2));
    char msg[LX_NAME_MAX];
    lx_strcpy(msg, sizeof msg, lx_fsm_string(c, 6));
    LxRaycastHit hit = {0};
    bool got = lx_shoot_ray(c->scene, from, lx_norm3(dir), lx_fsm_float(c, 3), lx_fsm_float(c, 4),
                            lx_fsm_float(c, 5), msg, lx_fsm_asset(c, 7), shooter, &hit);
    lx_fsm_set_bool(c, 8, got);
    lx_fsm_set_object(c, 9, got ? hit.entity : 0);
    lx_fsm_set_vec3(c, 10, got ? hit.point : lx_madd3(from, lx_norm3(dir), lx_fsm_float(c, 3)));
    fsm_send(c, got ? 11 : 12);
}
static const LxFsmActionDesc A_CB_SHOOT = {"Shoot Ray", "Combat", "An instant-hit shot: fires a ray, sends a damage message to whatever it hits, and spawns an impact effect. Hitscan weapons in one action.",
    "{0} shoots",
    {POBJT("Shooter", "the ray starts from this object and never hits it"),
     PT("Muzzle Offset", LX_FSM_VEC3, "0 1.5 0.5", "where on the shooter the shot comes from, in its own space"),
     PT("Direction", LX_FSM_VEC3, "0 0 1", "in the shooter's own space, so 0 0 1 is straight ahead"),
     P("Range", LX_FSM_FLOAT, "100"), PT("Spread", LX_FSM_FLOAT, "0", "degrees of random scatter; 0 is perfectly accurate"),
     P("Damage", LX_FSM_FLOAT, "10"), PT("Damage Message", LX_FSM_STRING, "Damage", "sent to whatever is hit"),
     PASSETT("Impact Prefab", "prefab", "spawned where the shot lands"),
     POPT("Store Did Hit", LX_FSM_BOOL), POPT("Store Hit Object", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3),
     PEV("Hit Event"), PEV("Miss Event")},
    0, 0, LX_FSM_ONCE, cb_shoot, cb_shoot, NULL};

static void cb_fire_projectile(LxFsmCtx *c) {
    if (!lx_fsm_spawn_allowed(c->scene)) return;
    LxEntity from = fsm_obj(c, 1);
    if (!from) return;
    LxEntity e = lx_fire_projectile(c->scene, lx_fsm_asset(c, 0), from, lx_fsm_vec3(c, 2), lx_fsm_float(c, 3));
    lx_fsm_set_object(c, 4, e);
    fsm_send(c, e ? 5 : 6);
}
static const LxFsmActionDesc A_CB_FIRE = {"Fire Projectile", "Combat", "Spawns a projectile prefab pointing where the firing object faces. Put Projectile Update on the prefab to make it fly.",
    "{1} fires {0}",
    {PASSET("Projectile Prefab", "prefab"), POBJT("From Object", "the projectile starts here, facing the same way"),
     PT("Muzzle Offset", LX_FSM_VEC3, "0 1.5 0.5", "in the firing object's own space"),
     PT("Spread", LX_FSM_FLOAT, "0", "degrees of random scatter"),
     POPT("Store Projectile", LX_FSM_OBJECT), PEV("Fired Event"), PEV("Failed Event")},
    0, 0, LX_FSM_ONCE, cb_fire_projectile, cb_fire_projectile, NULL};

static void cb_projectile_update(LxFsmCtx *c) {
    LxProjectile *p = c->state;
    LxRaycastHit hit = {0};
    char msg[LX_NAME_MAX], ignore[LX_NAME_MAX];
    lx_strcpy(msg, sizeof msg, lx_fsm_string(c, 4));
    lx_strcpy(ignore, sizeof ignore, lx_fsm_string(c, 5));
    int r = lx_projectile_update(c->scene, c->self, p, lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2),
                                 lx_fsm_float(c, 3), msg, ignore, lx_fsm_asset(c, 6), c->dt, &hit);
    if (r == 1) {
        lx_fsm_set_object(c, 7, hit.entity);
        lx_fsm_set_vec3(c, 8, hit.point);
        lx_fsm_finish(c);
        fsm_send(c, 9);
    } else if (r == 2) {
        lx_fsm_finish(c);
        fsm_send(c, 10);
    }
}
static const LxFsmActionDesc A_CB_PROJECTILE = {"Projectile Update", "Combat", "Put this on the projectile itself: it flies forward, drops with gravity, damages what it hits and removes itself.",
    "projectile at {0} m/s",
    {P("Speed", LX_FSM_FLOAT, "40"), PT("Gravity", LX_FSM_FLOAT, "0", "how fast it drops; 0 flies dead straight"),
     PT("Lifetime", LX_FSM_FLOAT, "5", "seconds before it gives up and vanishes"), P("Damage", LX_FSM_FLOAT, "25"),
     PT("Damage Message", LX_FSM_STRING, "Damage", "sent to whatever is hit"),
     PT("Ignore Tag", LX_FSM_STRING, "", "never hit things carrying this tag, e.g. the shooter's own team"),
     PASSETT("Impact Prefab", "prefab", "spawned where it lands"),
     POPT("Store Hit Object", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3),
     PEV("Hit Event"), PEV("Timed Out Event")},
    sizeof(LxProjectile), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, cb_projectile_update, NULL};

static void cb_area_damage(LxFsmCtx *c) {
    LxEntity at = fsm_obj(c, 0);
    LxVec3 center = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 1);
    char msg[LX_NAME_MAX], tag[LX_NAME_MAX];
    lx_strcpy(msg, sizeof msg, lx_fsm_string(c, 5));
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 6));
    int n = lx_area_damage(c->scene, center, lx_fsm_float(c, 2), lx_fsm_float(c, 3), lx_fsm_bool(c, 4),
                           msg, tag[0] ? tag : NULL, c->self);
    if (lx_fsm_float(c, 7) > 0) lx_physics_explosion(c->scene, center, lx_fsm_float(c, 2), lx_fsm_float(c, 7));
    lx_fsm_set_int(c, 8, n);
    fsm_send(c, n > 0 ? 9 : 10);
}
static const LxFsmActionDesc A_CB_AREA_DAMAGE = {"Area Damage", "Combat", "Damages everything within a radius, optionally weaker further out, and can shove physics bodies away too. Grenades and explosions.",
    "damage r={2} at {0}",
    {POBJT("At Object", "the centre of the blast"), P("Position", LX_FSM_VEC3, "0 0 0"),
     P("Radius", LX_FSM_FLOAT, "5"), P("Damage", LX_FSM_FLOAT, "50"),
     PT("Falloff", LX_FSM_BOOL, "true", "less damage further from the centre"),
     PT("Damage Message", LX_FSM_STRING, "Damage", "sent to everything caught in it"),
     PT("Tag Filter", LX_FSM_STRING, "", "only damage things carrying this tag; empty hits everything"),
     PT("Physics Impulse", LX_FSM_FLOAT, "8", "how hard it throws physics bodies; 0 = not at all"),
     POPT("Store Hit Count", LX_FSM_INT), PEV("Hit Something Event"), PEV("Hit Nothing Event")},
    0, 0, LX_FSM_ONCE, cb_area_damage, cb_area_damage, NULL};

static void cb_apply_damage(LxFsmCtx *c) {
    float hp = lx_fsm_float(c, 0) - lx_maxf(0, lx_fsm_float(c, 1));
    float max = lx_fsm_float(c, 2);
    if (max > 0) hp = lx_minf(hp, max);
    lx_fsm_set_float(c, 0, hp);
    lx_fsm_set_float(c, 3, max > 0 ? lx_saturate(hp / max) : 0.0f);
    if (hp <= 0) fsm_send(c, 4);
    else fsm_send(c, 5);
}
static const LxFsmActionDesc A_CB_APPLY_DAMAGE = {"Apply Damage", "Combat", "Takes health off a variable, reports it as 0..1 for a health bar, and sends an event when it runs out.",
    "{0} -= {1}",
    {POUTT("Health Variable", LX_FSM_FLOAT, "the Float holding current health"), P("Damage", LX_FSM_FLOAT, "10"),
     PT("Max Health", LX_FSM_FLOAT, "100", "used for the 0..1 output and as a cap when healing"),
     POPTT("Store Health 0 1", LX_FSM_FLOAT, "feed this straight into a health bar"),
     PEV("Died Event"), PEV("Survived Event")},
    0, 0, LX_FSM_ONCE, cb_apply_damage, cb_apply_damage, NULL};

/* ==========================================================================================
 *  spawning
 * ========================================================================================== */
static void sp_spawner(LxFsmCtx *c) {
    LxSpawner *sp = c->state;
    LxEntity e = lx_spawner_update(c->scene, sp, lx_fsm_asset(c, 0), fsm_obj(c, 1), lx_fsm_float(c, 2),
                                   lx_fsm_float(c, 3), lx_fsm_int(c, 4), lx_fsm_int(c, 5), c->dt);
    lx_fsm_set_int(c, 6, sp->alive_count);
    lx_fsm_set_int(c, 7, sp->spawned);
    if (e) {
        lx_fsm_set_object(c, 8, e);
        fsm_send(c, 9);
    }
    int total = lx_fsm_int(c, 5);
    if (total > 0 && sp->spawned >= total && sp->alive_count == 0) { lx_fsm_finish(c); fsm_send(c, 10); }
}
static const LxFsmActionDesc A_SP_SPAWNER = {"Spawner", "AI", "Keeps a stream of spawned objects going: one every so often, never more than this many alive at once. Enemy waves, ambient traffic.",
    "spawn {0} every {3}s",
    {PASSET("Prefab", "prefab"), POBJT("At Object", "spawn here, or at one of its children picked at random"),
     PT("Radius", LX_FSM_FLOAT, "0", "scatter each spawn this far around the point"),
     PT("Interval", LX_FSM_FLOAT, "2", "seconds between spawns"),
     PT("Max Alive", LX_FSM_INT, "5", "hold off while this many are still about"),
     PT("Total", LX_FSM_INT, "0", "stop after this many; 0 = keep going forever"),
     POPT("Store Alive Count", LX_FSM_INT), POPT("Store Spawned Total", LX_FSM_INT),
     POPT("Store Last Spawned", LX_FSM_OBJECT), PEV("Spawned Event"), PEV("Wave Cleared Event")},
    sizeof(LxSpawner), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, sp_spawner, NULL};

/* ==========================================================================================
 *  interaction
 * ========================================================================================== */
static void it_interactable(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0), player = fsm_obj(c, 1);
    if (!e) return;
    char prompt[LX_REF_MAX];
    lx_strcpy(prompt, sizeof prompt, lx_fsm_string(c, 4));
    if (!player) player = lx_entity_find_with_tag(c->scene, "Player");
    if (!player) return;
    bool used = lx_interactable_update(c->scene, e, player, lx_fsm_float(c, 2), lx_fsm_bool(c, 3), prompt);
    float dist = lx_len3(lx_sub3(lx_entity_world_position(c->scene, player), lx_entity_world_position(c->scene, e)));
    lx_fsm_set_float(c, 5, dist);
    lx_fsm_set_bool(c, 6, dist <= lx_fsm_float(c, 2));
    if (used) fsm_send(c, 7);
}
static const LxFsmActionDesc A_IT_INTERACTABLE = {"Interactable", "Interaction", "Shows a prompt when the player is close enough (and facing it), and sends an event when they press Interact. Doors, levers, pickups, NPCs.",
    "{0} interactable",
    {POBJ("Object"), POBJNT("Player", "None finds the object tagged Player"),
     PT("Distance", LX_FSM_FLOAT, "2.5", "how close the player must be"),
     PT("Must Face It", LX_FSM_BOOL, "true", "the player has to be looking at it too"),
     PT("Prompt", LX_FSM_STRING, "Use", "shown near the bottom with the right key for the player's device"),
     POPT("Store Distance", LX_FSM_FLOAT), POPT("Store In Range", LX_FSM_BOOL), PEV("Interacted Event")},
    0, 0, LX_FSM_EVERY_FRAME, it_interactable, it_interactable, NULL};

/* ==========================================================================================
 *  crowds
 * ========================================================================================== */
static void cr_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxCrowdComponent *cr = e ? lx_get_crowd(c->scene, e) : NULL;
    lx_fsm_set_bool(c, 4, cr != NULL);
    if (!cr) return;
    if (!lx_fsm_is_none(c, 1)) cr->agent_speed = lx_maxf(0, lx_fsm_float(c, 1));
    if (!lx_fsm_is_none(c, 2)) cr->area_radius = lx_maxf(0, lx_fsm_float(c, 2));
    lx_fsm_set_int(c, 3, cr->agent_count);
}
static const LxFsmActionDesc A_CR_SET = {"Set Crowd", "AI", "Changes a Crowd component's speed and the area its agents mill about in.",
    "crowd on {0}",
    {POBJ("Object"), PIN_OPT("Agent Speed", LX_FSM_FLOAT), PIN_OPT("Area Radius", LX_FSM_FLOAT),
     POPT("Store Agent Count", LX_FSM_INT), POPT("Store Has Crowd", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, cr_set, cr_set, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_ai(void) {
    static const LxFsmActionDesc *all[] = {
        &A_AI_LOS, &A_AI_WALK_TO, &A_AI_PATROL, &A_AI_WANDER, &A_AI_FLEE,
        &A_CB_SHOOT, &A_CB_FIRE, &A_CB_PROJECTILE, &A_CB_AREA_DAMAGE, &A_CB_APPLY_DAMAGE,
        &A_SP_SPAWNER, &A_IT_INTERACTABLE, &A_CR_SET,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
