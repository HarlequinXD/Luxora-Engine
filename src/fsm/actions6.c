/* Built-in State Graph actions, part six: the game kit (AI, combat, spawning, inventory, checkpoints,
 * dialogue, menus, markers, music, faded scene changes, day and night...). Each is a thin wrapper over the
 * scripting API's game kit (luxora.h), so scripts can do exactly the same. */
#include "../lx_internal.h"
#include <stdio.h>

#define P(n, t, d) {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip) {n, t, d, 0, NULL, tip}
#define POUT(n, t) {n, t, "", LX_FSM_P_OUT, NULL, NULL}
#define POPT(n, t) {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n) {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PENUM(n, opts, d) {n, LX_FSM_ENUM, d, 0, opts, NULL}
#define PASSET(n, kind) {n, LX_FSM_ASSET, "", 0, kind, NULL}
#define PASSETOPT(n, kind) {n, LX_FSM_ASSET, "", LX_FSM_P_OPTIONAL, kind, NULL}
#define POBJ(n) {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJN(n) {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}
#define OPTF(n) {n, LX_FSM_FLOAT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define OPTS(n) {n, LX_FSM_STRING, "", LX_FSM_P_OPTIONAL, NULL, NULL}

static void ev(LxFsmCtx *c, int p) { lx_fsm_send(c, lx_fsm_event(c, p)); }
static LxEntity obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }
static uint32_t rgba(LxVec4 c) {
    return LX_RGBA((int)(lx_clampf(c.x, 0, 1) * 255), (int)(lx_clampf(c.y, 0, 1) * 255), (int)(lx_clampf(c.z, 0, 1) * 255), (int)(lx_clampf(c.w, 0, 1) * 255));
}
static void str(LxFsmCtx *c, int p, char *out, size_t cap) { lx_strcpy(out, cap, lx_fsm_string(c, p)); }
static void asset(LxFsmCtx *c, int p, char *out, size_t cap) { lx_strcpy(out, cap, lx_fsm_asset(c, p)); }
/* an object, or a point when it's None */
static LxVec3 point_of(LxFsmCtx *c, int po, int pv) {
    LxEntity e = obj(c, po);
    return e ? lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, pv)) : lx_fsm_vec3(c, pv);
}

/* ======================================================================
 *  AI
 * ====================================================================== */
static void line_of_sight(LxFsmCtx *c) {
    bool see = lx_line_of_sight(c->scene, obj(c, 0), obj(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3), lx_fsm_float(c, 4), lx_fsm_float(c, 5));
    lx_fsm_set_bool(c, 8, see);
    ev(c, see ? 6 : 7);
}
static const LxFsmActionDesc A_LOS = {"Line Of Sight", "AI", "Can this object see the target: close enough, inside its view angle, and nothing in the way.",
    "{0} sees {1}? → {6}",
    {POBJ("Object"), P("Target", LX_FSM_OBJECT, "tag:Player"), P("Range", LX_FSM_FLOAT, "20"), PT("View Angle", LX_FSM_FLOAT, "120", "degrees; 360 = all round"),
     PT("Eye Height", LX_FSM_FLOAT, "1.6", "above the object's feet"), PT("Target Height", LX_FSM_FLOAT, "1", "the point aimed at, above the target's feet"),
     PEV("Can See Event"), PEV("Can't See Event"), POPT("Store Can See", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, line_of_sight, line_of_sight, NULL};

static void patrol(LxFsmCtx *c) {
    LxPatrol *p = c->state;
    int f = lx_patrol_update(c->scene, obj(c, 0), obj(c, 1), p, lx_fsm_float(c, 2), lx_fsm_float(c, 3), (LxPatrolMode)lx_fsm_int(c, 4), c->dt);
    lx_fsm_set_int(c, 7, p->index);
    if (f & LX_PATROL_REACHED) ev(c, 5);
    if (f & LX_PATROL_FINISHED) { lx_fsm_finish(c); ev(c, 6); }
}
static const LxFsmActionDesc A_PATROL = {"Patrol", "AI", "Walks from waypoint to waypoint (the children of an object, in order), waiting at each. "
    "With a Character Controller it falls and slides along walls.",
    "{0} patrols {1}",
    {POBJ("Object"), PT("Waypoints", LX_FSM_OBJECT, "name:Waypoints", "an object whose children are the points"), P("Speed", LX_FSM_FLOAT, "2.5"),
     PT("Wait", LX_FSM_FLOAT, "1", "seconds at each point"), PENUM("Mode", "Loop|Ping Pong|Once", "Loop"), PEV("Point Reached Event"), PEV("Finished Event"),
     POPT("Store Waypoint Index", LX_FSM_INT)}, sizeof(LxPatrol), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, patrol, NULL};

static void wander(LxFsmCtx *c) {
    if (lx_wander_update(c->scene, obj(c, 0), c->state, lx_fsm_float(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3), lx_fsm_float(c, 4), c->dt)) ev(c, 5);
}
static const LxFsmActionDesc A_WANDER = {"Wander", "AI", "Strolls to random spots around where it started, resting in between (animals, villagers, idle enemies).",
    "{0} wanders within {1} m",
    {POBJ("Object"), P("Radius", LX_FSM_FLOAT, "6"), P("Speed", LX_FSM_FLOAT, "1.5"), P("Min Wait", LX_FSM_FLOAT, "1"), P("Max Wait", LX_FSM_FLOAT, "3"), PEV("Arrived Event")},
    sizeof(LxWander), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, wander, NULL};

typedef struct { float vy; } Fall;
static void walk_to(LxFsmCtx *c) {
    Fall *f = c->state;
    if (lx_walk_to(c->scene, obj(c, 0), point_of(c, 1, 2), lx_fsm_float(c, 3), lx_fsm_float(c, 4), lx_fsm_float(c, 5), &f->vy, c->dt)) {
        if (lx_fsm_bool(c, 7)) lx_fsm_finish(c);
        ev(c, 6);
    }
}
static const LxFsmActionDesc A_WALK_TO = {"Walk To", "AI", "Walks to an object or point, turning to face the way it goes (falls with a Character Controller).",
    "{0} walks to {1}",
    {POBJ("Object"), POBJN("Target"), PT("Position", LX_FSM_VEC3, "0 0 0", "the point when Target is None; an offset from it otherwise"), P("Speed", LX_FSM_FLOAT, "3"),
     P("Stop Distance", LX_FSM_FLOAT, "0.5"), PT("Turn Speed", LX_FSM_FLOAT, "8", "-1 = don't turn"), PEV("Arrived Event"), PT("Finish On Arrival", LX_FSM_BOOL, "true", "")},
    sizeof(Fall), 0, LX_FSM_EVERY_FRAME, NULL, walk_to, NULL};

static void flee(LxFsmCtx *c) {
    Fall *f = c->state;
    LxEntity me = obj(c, 0), from = obj(c, 1);
    if (!me || !from) return;
    LxVec3 away = lx_sub3(lx_entity_world_position(c->scene, me), lx_entity_world_position(c->scene, from));
    away.y = 0;
    float d = lx_len3(away);
    if (d >= lx_fsm_float(c, 3)) { ev(c, 4); return; }
    if (d < 1e-3f) away = lx_entity_forward(c->scene, me), d = 1;
    lx_walk_to(c->scene, me, lx_madd3(lx_entity_world_position(c->scene, me), away, 3 / d), lx_fsm_float(c, 2), 0.1f, 8, &f->vy, c->dt);
}
static const LxFsmActionDesc A_FLEE = {"Flee From", "AI", "Runs away from something until it's far enough (scared animals, fleeing enemies).", "{0} flees {1}",
    {POBJ("Object"), P("From", LX_FSM_OBJECT, "tag:Player"), P("Speed", LX_FSM_FLOAT, "4"), P("Safe Distance", LX_FSM_FLOAT, "12"), PEV("Safe Event")},
    sizeof(Fall), 0, LX_FSM_EVERY_FRAME, NULL, flee, NULL};

/* ======================================================================
 *  combat
 * ====================================================================== */
static void shoot_ray(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 from = lx_entity_transform_point(c->scene, e, lx_fsm_vec3(c, 1));
    LxVec3 dir = lx_entity_transform_direction(c->scene, e, lx_fsm_vec3(c, 2));
    char msg[64], impact[LX_REF_MAX];
    str(c, 6, msg, sizeof msg);
    asset(c, 7, impact, sizeof impact);
    LxRaycastHit h;
    bool got = lx_shoot_ray(c->scene, from, dir, lx_fsm_float(c, 3), lx_fsm_float(c, 4), lx_fsm_float(c, 5), msg, impact, e, &h);
    lx_fsm_set_object(c, 10, got ? h.entity : 0);
    if (got) lx_fsm_set_vec3(c, 11, h.point);
    ev(c, got ? 8 : 9);
}
static const LxFsmActionDesc A_SHOOT_RAY = {"Shoot Ray", "Combat", "A hitscan shot from an object: whatever it hits gets a message (\"Damage\" with the amount) and an impact prefab appears.",
    "{0} shoots ({5} damage)",
    {POBJ("From Object"), PT("Offset", LX_FSM_VEC3, "0 0 0", "in the object's own space"), PT("Direction", LX_FSM_VEC3, "0 0 1", "in its own space: 0 0 1 = forward"),
     P("Range", LX_FSM_FLOAT, "100"), PT("Spread", LX_FSM_FLOAT, "0", "degrees"), P("Damage", LX_FSM_FLOAT, "10"), P("Message", LX_FSM_STRING, "Damage"),
     PASSETOPT("Impact Prefab", "prefab"), PEV("Hit Event"), PEV("Miss Event"), POPT("Store Hit Object", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, shoot_ray, shoot_ray, NULL};
static void fire_projectile(LxFsmCtx *c) {
    char prefab[LX_REF_MAX];
    asset(c, 0, prefab, sizeof prefab);
    lx_fsm_set_object(c, 4, lx_fire_projectile(c->scene, prefab, obj(c, 1), lx_fsm_vec3(c, 2), lx_fsm_float(c, 3)));
}
static const LxFsmActionDesc A_FIRE_PROJECTILE = {"Fire Projectile", "Combat", "Spawns a projectile prefab facing where the object faces (give the prefab the Projectile action).",
    "{1} fires {0}",
    {PASSET("Prefab", "prefab"), POBJ("From Object"), PT("Offset", LX_FSM_VEC3, "0 1 0.8", "in the object's own space"), PT("Spread", LX_FSM_FLOAT, "0", "degrees"),
     POPT("Store Projectile", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, fire_projectile, fire_projectile, NULL};
static void projectile(LxFsmCtx *c) {
    char msg[64], tag[LX_NAME_MAX], impact[LX_REF_MAX];
    str(c, 4, msg, sizeof msg);
    str(c, 5, tag, sizeof tag);
    asset(c, 6, impact, sizeof impact);
    LxRaycastHit h;
    int r = lx_projectile_update(c->scene, c->self, c->state, lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3), msg, tag, impact, c->dt, &h);
    if (r == 1) { lx_fsm_set_object(c, 8, h.entity); ev(c, 7); }
}
static const LxFsmActionDesc A_PROJECTILE = {"Projectile", "Combat", "On a bullet, arrow or fireball: flies forward, drops with gravity, hits the first thing in its way "
    "(sending it a message) and removes itself, or vanishes after its lifetime.",
    "fly at {0} m/s, {3} damage",
    {P("Speed", LX_FSM_FLOAT, "30"), P("Gravity", LX_FSM_FLOAT, "0"), P("Lifetime", LX_FSM_FLOAT, "3"), P("Damage", LX_FSM_FLOAT, "10"), P("Message", LX_FSM_STRING, "Damage"),
     PT("Ignore Tag", LX_FSM_STRING, "", "flies through objects with this tag (its shooter's: Player)"), PASSETOPT("Impact Prefab", "prefab"), PEV("Hit Event"),
     POPT("Store Hit Object", LX_FSM_OBJECT)}, sizeof(LxProjectile), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, projectile, NULL};
static void area_damage(LxFsmCtx *c) {
    char msg[64], tag[LX_NAME_MAX];
    str(c, 5, msg, sizeof msg);
    str(c, 6, tag, sizeof tag);
    lx_fsm_set_int(c, 7, lx_area_damage(c->scene, point_of(c, 0, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3), lx_fsm_bool(c, 4), msg, tag, obj(c, 0)));
}
static const LxFsmActionDesc A_AREA_DAMAGE = {"Area Damage", "Combat", "Hurts everything within a radius (explosions, shockwaves), less further out with Falloff.",
    "{3} damage within {2} m of {0}",
    {POBJ("Center"), P("Offset", LX_FSM_VEC3, "0 0 0"), P("Radius", LX_FSM_FLOAT, "4"), P("Damage", LX_FSM_FLOAT, "40"), P("Falloff", LX_FSM_BOOL, "true"),
     P("Message", LX_FSM_STRING, "Damage"), PT("Only Tag", LX_FSM_STRING, "", "empty = everything"), POPT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, area_damage, area_damage, NULL};
static void apply_damage(LxFsmCtx *c) {
    float hp = lx_fsm_float(c, 0), amount = lx_fsm_float(c, 1), max = lx_fsm_float(c, 2);
    bool dead = hp <= 0;
    hp -= amount;
    if (max > 0 && hp > max) hp = max;
    if (hp < 0) hp = 0;
    lx_fsm_set_float(c, 0, hp);
    if (dead) return;   /* already dead: no more events */
    if (amount > 0) ev(c, hp <= 0 ? 4 : 3);
    else if (amount < 0) ev(c, 5);
}
static const LxFsmActionDesc A_APPLY_DAMAGE = {"Apply Damage", "Combat", "Takes an amount off a health variable (negative heals, up to Max) and sends Hurt, Died or Healed.",
    "{0} -= {1}",
    {POUT("Health Variable", LX_FSM_FLOAT), PT("Amount", LX_FSM_FLOAT, "10", "Get Event Info's value is the damage sent with a Damage message"), PT("Max Health", LX_FSM_FLOAT, "100", "0 = no limit"),
     PEV("Hurt Event"), PEV("Died Event"), PEV("Healed Event")}, 0, 0, LX_FSM_ONCE, apply_damage, apply_damage, NULL};
static void spawner(LxFsmCtx *c) {
    LxSpawner *sp = c->state;
    char prefab[LX_REF_MAX];
    asset(c, 0, prefab, sizeof prefab);
    int total = lx_fsm_int(c, 5);
    LxEntity e = lx_spawner_update(c->scene, sp, prefab, obj(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3), lx_fsm_int(c, 4), total, c->dt);
    lx_fsm_set_int(c, 9, sp->alive_count);
    if (e) { lx_fsm_set_object(c, 8, e); ev(c, 6); }
    if (total > 0 && sp->spawned >= total && sp->alive_count == 0) { lx_fsm_finish(c); ev(c, 7); }
}
static const LxFsmActionDesc A_SPAWNER = {"Spawner", "Objects", "Keeps spawning a prefab (waves, endless enemies): up to Max Alive at once, Total in all (0 = forever), "
    "at the object or one of its children. All Done is sent when the last one of the Total is gone.",
    "spawn {0} every {3} s",
    {PASSET("Prefab", "prefab"), POBJ("At Object"), PT("Radius", LX_FSM_FLOAT, "0", "scatter around the point"), PT("Interval", LX_FSM_FLOAT, "2", "seconds"),
     P("Max Alive", LX_FSM_INT, "5"), PT("Total", LX_FSM_INT, "0", "0 = forever"), PEV("Spawned Event"), PEV("All Done Event"), POPT("Store Spawned", LX_FSM_OBJECT),
     POPT("Store Alive Count", LX_FSM_INT)}, sizeof(LxSpawner), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, spawner, NULL};

/* ======================================================================
 *  inventory
 * ====================================================================== */
static void inv_add(LxFsmCtx *c) {
    char item[64];
    str(c, 0, item, sizeof item);
    int max = lx_fsm_int(c, 2), have = lx_inventory_count(item), add = lx_fsm_int(c, 1);
    if (max > 0 && have + add > max) { add = max - have; ev(c, 4); }
    lx_fsm_set_int(c, 3, add > 0 ? lx_inventory_add(item, add) : have);
}
static const LxFsmActionDesc A_INV_ADD = {"Inventory Add", "Inventory", "Adds items to the player's inventory (kept through scene changes).", "+{1} {0}",
    {P("Item", LX_FSM_STRING, "Key"), P("Count", LX_FSM_INT, "1"), PT("Max", LX_FSM_INT, "0", "0 = no limit"), POPT("Store New Count", LX_FSM_INT), PEV("Full Event")},
    0, 0, LX_FSM_ONCE, inv_add, inv_add, NULL};
static void inv_remove(LxFsmCtx *c) {
    char item[64];
    str(c, 0, item, sizeof item);
    bool ok = lx_inventory_remove(item, lx_fsm_int(c, 1));
    lx_fsm_set_int(c, 4, lx_inventory_count(item));
    ev(c, ok ? 2 : 3);
}
static const LxFsmActionDesc A_INV_REMOVE = {"Inventory Remove", "Inventory", "Uses up items (a key for a door, potions); Not Enough when there aren't that many.", "-{1} {0}",
    {P("Item", LX_FSM_STRING, "Key"), P("Count", LX_FSM_INT, "1"), PEV("Removed Event"), PEV("Not Enough Event"), POPT("Store New Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, inv_remove, inv_remove, NULL};
static void inv_count(LxFsmCtx *c) {
    char item[64];
    str(c, 0, item, sizeof item);
    int n = lx_inventory_count(item);
    lx_fsm_set_int(c, 2, n);
    ev(c, n >= lx_fsm_int(c, 1) && n > 0 ? 3 : 4);
}
static const LxFsmActionDesc A_INV_COUNT = {"Inventory Has", "Inventory", "How many of an item the player has, and whether it's at least a number.", "has {1} {0}?",
    {P("Item", LX_FSM_STRING, "Key"), P("At Least", LX_FSM_INT, "1"), POPT("Store Count", LX_FSM_INT), PEV("Has Event"), PEV("Hasn't Event")}, 0, 0, LX_FSM_ONCE, inv_count, inv_count, NULL};
static void inv_misc(LxFsmCtx *c) {
    char slot[64];
    str(c, 1, slot, sizeof slot);
    switch (lx_fsm_int(c, 0)) {
    case 0: lx_inventory_save(slot); break;
    case 1: lx_inventory_load(slot); break;
    default: lx_inventory_clear(); break;
    }
    lx_fsm_set_int(c, 2, lx_inventory_size());
}
static const LxFsmActionDesc A_INV_MISC = {"Inventory Save Load", "Inventory", "Saves the inventory, loads it back, or empties it.", "{0} inventory",
    {PENUM("Do", "Save|Load|Clear", "Save"), P("Slot", LX_FSM_STRING, "game"), POPT("Store Item Kinds", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, inv_misc, inv_misc, NULL};

/* ======================================================================
 *  checkpoints, saving
 * ====================================================================== */
static void checkpoint(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (lx_fsm_int(c, 1) == 0) { lx_checkpoint_set(c->scene, e); if (lx_fsm_bool(c, 4)) lx_ui_toast("Checkpoint", 2); ev(c, 2); }
    else ev(c, lx_checkpoint_respawn(c->scene, e) ? 2 : 3);
}
static const LxFsmActionDesc A_CHECKPOINT = {"Checkpoint", "Save Data", "Remembers where the player is (saved), or puts them back there.", "{1} checkpoint for {0}",
    {P("Object", LX_FSM_OBJECT, "tag:Player"), PENUM("Do", "Set|Respawn", "Set"), PEV("Done Event"), PEV("No Checkpoint Event"), PT("Show Message", LX_FSM_BOOL, "true", "Set: a \"Checkpoint\" message")},
    0, 0, LX_FSM_ONCE, checkpoint, checkpoint, NULL};
static void globals_io(LxFsmCtx *c) {
    char slot[64];
    str(c, 1, slot, sizeof slot);
    int n = lx_fsm_int(c, 0) == 0 ? lx_globals_save(slot) : lx_globals_load(slot);
    lx_fsm_set_int(c, 2, n);
    ev(c, n > 0 ? 3 : 4);
}
static const LxFsmActionDesc A_GLOBALS_IO = {"Save Game", "Save Data", "Saves every global variable (score, unlocks, settings...) or loads them back: a whole save game in one action.",
    "{0} the globals ({1})",
    {PENUM("Do", "Save|Load", "Save"), P("Slot", LX_FSM_STRING, "game"), POPT("Store Count", LX_FSM_INT), PEV("Done Event"), PEV("Nothing Saved Event")}, 0, 0, LX_FSM_ONCE, globals_io, globals_io, NULL};

/* ======================================================================
 *  game UI
 * ====================================================================== */
static void show_message(LxFsmCtx *c) { char t[LX_REF_MAX]; str(c, 0, t, sizeof t); lx_ui_toast(t, lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_SHOW_MESSAGE = {"Show Message", "Game UI", "A message near the bottom of the screen for a few seconds (\"Door unlocked\").", "say \"{0}\"",
    {P("Text", LX_FSM_STRING, "Hello"), P("Seconds", LX_FSM_FLOAT, "2.5")}, 0, 0, LX_FSM_ONCE, show_message, NULL, NULL};
static void popup_text(LxFsmCtx *c) {
    char t[64];
    str(c, 2, t, sizeof t);
    lx_ui_popup_text(c->scene, point_of(c, 0, 1), t, rgba(lx_fsm_color(c, 3)), lx_fsm_float(c, 4), lx_fsm_float(c, 5));
}
static const LxFsmActionDesc A_POPUP = {"Popup Text", "Game UI", "Text that rises and fades from a spot in the world (damage numbers, \"+50\").", "\"{2}\" over {0}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "0 2 0"), P("Text", LX_FSM_STRING, "+10"), P("Color", LX_FSM_COLOR, "1 0.9 0.4 1"), P("Size", LX_FSM_FLOAT, "44"), P("Seconds", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, popup_text, NULL, NULL};
static void set_objective(LxFsmCtx *c) { char t[LX_REF_MAX]; str(c, 0, t, sizeof t); lx_ui_set_objective(t); }
static const LxFsmActionDesc A_OBJECTIVE = {"Set Objective", "Game UI", "The current goal at the top left of the screen (empty hides it).", "objective: {0}",
    {P("Text", LX_FSM_STRING, "Find the key")}, 0, 0, LX_FSM_ONCE, set_objective, NULL, NULL};
static void flash(LxFsmCtx *c) { lx_ui_flash(lx_fsm_color(c, 0), lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_FLASH = {"Screen Flash", "Game UI", "The whole screen flashes a colour and fades (getting hit, a pickup, lightning).", "flash {0}",
    {P("Color", LX_FSM_COLOR, "1 0.1 0.05 0.5"), P("Seconds", LX_FSM_FLOAT, "0.3")}, 0, 0, LX_FSM_ONCE, flash, NULL, NULL};
static void bar_over(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    float max = lx_fsm_float(c, 3), v = max > 0 ? lx_fsm_float(c, 2) / max : 0;
    if (!e || (lx_fsm_bool(c, 7) && v >= 0.999f)) return;
    lx_ui_bar_over(c->scene, lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 1)), v, lx_fsm_float(c, 4), lx_fsm_float(c, 5), rgba(lx_fsm_color(c, 6)), LX_RGBA(0, 0, 0, 150));
}
static const LxFsmActionDesc A_BAR_OVER = {"Health Bar Over Object", "Game UI", "A small bar floating over an object (enemy health, progress).", "bar over {0}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "0 2.2 0"), P("Value", LX_FSM_FLOAT, "100"), P("Max", LX_FSM_FLOAT, "100"), P("Width", LX_FSM_FLOAT, "120"),
     P("Height", LX_FSM_FLOAT, "12"), P("Color", LX_FSM_COLOR, "0.9 0.2 0.15 1"), P("Hide When Full", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_EVERY_FRAME, bar_over, bar_over, NULL};
static void marker(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    char t[64];
    str(c, 2, t, sizeof t);
    lx_ui_marker(c->scene, lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 1)), t, rgba(lx_fsm_color(c, 3)), lx_fsm_bool(c, 4), lx_fsm_bool(c, 5));
}
static const LxFsmActionDesc A_MARKER = {"Objective Marker", "Game UI", "A marker on the screen over an object, kept at the edge when it's off screen (quests, waypoints).",
    "marker on {0}",
    {POBJ("Object"), P("Offset", LX_FSM_VEC3, "0 2 0"), OPTS("Label"), P("Color", LX_FSM_COLOR, "1 0.8 0.3 1"), P("Show Off Screen", LX_FSM_BOOL, "true"), P("Show Distance", LX_FSM_BOOL, "true")},
    0, 0, LX_FSM_EVERY_FRAME, marker, marker, NULL};
static void dialogue(LxFsmCtx *c) {
    char who[64], text[LX_REF_MAX];
    str(c, 0, who, sizeof who);
    str(c, 1, text, sizeof text);
    if (lx_ui_dialogue(c->state, who, text, lx_fsm_float(c, 2), lx_delta_unscaled(c->scene))) { lx_fsm_finish(c); ev(c, 3); }
}
static const LxFsmActionDesc A_DIALOGUE = {"Dialogue", "Game UI", "A dialogue box that types out text; Interact / Jump / Enter / a click goes on. Separate pages with |.",
    "{0}: \"{1}\"",
    {P("Speaker", LX_FSM_STRING, "Stranger"), PT("Text", LX_FSM_STRING, "Hello there.|Fine weather.", "pages separated by |"), PT("Letters Per Second", LX_FSM_FLOAT, "45", "0 = all at once"),
     PEV("Finished Event")}, sizeof(LxDialogue), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, dialogue, NULL};
static void choice(LxFsmCtx *c) {
    char title[LX_REF_MAX], opt[4][64];
    const char *opts[4];
    int map[4], n = 0;
    str(c, 0, title, sizeof title);
    for (int i = 0; i < 4; i++) {
        str(c, 1 + i * 2, opt[i], sizeof opt[i]);
        if (opt[i][0]) { opts[n] = opt[i]; map[n++] = i; }
    }
    int pick = lx_ui_choice(c->state, title, opts, n);
    if (pick < 0) return;
    lx_fsm_set_int(c, 9, map[pick]);
    lx_fsm_finish(c);
    ev(c, 2 + map[pick] * 2);
}
static const LxFsmActionDesc A_CHOICE = {"Choice Menu", "Game UI", "Asks a question with up to four answers (keys, stick or mouse); each sends its event.", "{0}",
    {P("Title", LX_FSM_STRING, "What will you do?"), P("Option 1", LX_FSM_STRING, "Yes"), PEV("Event 1"), P("Option 2", LX_FSM_STRING, "No"), PEV("Event 2"),
     OPTS("Option 3"), PEV("Event 3"), OPTS("Option 4"), PEV("Event 4"), POPT("Store Choice", LX_FSM_INT)},
    sizeof(LxChoice), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, choice, NULL};
static void interactable(LxFsmCtx *c) {
    char prompt[64];
    str(c, 4, prompt, sizeof prompt);
    bool in = false;
    LxEntity e = obj(c, 0), p = obj(c, 1);
    if (e && p) {
        LxVec3 d = lx_sub3(lx_entity_world_position(c->scene, e), lx_entity_world_position(c->scene, p));
        in = sqrtf(d.x * d.x + d.z * d.z) <= lx_fsm_float(c, 2);
    }
    lx_fsm_set_bool(c, 6, in);
    if (lx_interactable_update(c->scene, e, p, lx_fsm_float(c, 2), lx_fsm_bool(c, 3), prompt)) ev(c, 5);
}
static const LxFsmActionDesc A_INTERACTABLE = {"Interactable", "Game UI", "Something the player can use: close enough (and facing it), a \"[E] Open\" prompt shows; Interact sends the event.",
    "[E] {4} on {0}",
    {POBJ("Object"), P("Player", LX_FSM_OBJECT, "tag:Player"), P("Distance", LX_FSM_FLOAT, "2.5"), P("Must Face It", LX_FSM_BOOL, "true"), P("Prompt", LX_FSM_STRING, "Open"),
     PEV("Interact Event"), POPT("Store In Range", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, interactable, interactable, NULL};
static void format_time(LxFsmCtx *c) {
    char t[32];
    lx_format_time(t, sizeof t, lx_fsm_float(c, 0), lx_fsm_int(c, 1));
    lx_fsm_set_string(c, 2, t);
}
static const LxFsmActionDesc A_FORMAT_TIME = {"Format Time", "Time", "Seconds as a clock: 1:05, 1:01:05 or 1:05.42 (race timers, countdowns).", "{2} = {0} as time",
    {P("Seconds", LX_FSM_FLOAT, "0"), PENUM("Style", "m:ss|h:mm:ss|m:ss.cc", "m:ss"), POUT("Store String", LX_FSM_STRING)}, 0, 0, LX_FSM_EVERY_FRAME, format_time, format_time, NULL};

/* ======================================================================
 *  audio, scenes, world
 * ====================================================================== */
static void play_music(LxFsmCtx *c) { char clip[LX_REF_MAX]; asset(c, 0, clip, sizeof clip); lx_music_play(c->scene, clip, lx_fsm_float(c, 1), lx_fsm_float(c, 2)); }
static const LxFsmActionDesc A_PLAY_MUSIC = {"Play Music", "Audio", "Plays a looping music track, fading over from the one playing (the same track keeps playing).", "music {0}",
    {PASSET("Clip", "sound"), P("Volume", LX_FSM_FLOAT, "0.8"), PT("Fade", LX_FSM_FLOAT, "1.5", "seconds")}, 0, 0, LX_FSM_ONCE, play_music, NULL, NULL};
static void stop_music(LxFsmCtx *c) { lx_music_stop(lx_fsm_float(c, 0)); }
static const LxFsmActionDesc A_STOP_MUSIC = {"Stop Music", "Audio", "Fades the music out.", "stop the music", {PT("Fade", LX_FSM_FLOAT, "1.5", "seconds")}, 0, 0, LX_FSM_ONCE, stop_music, NULL, NULL};
static void bus_volume(LxFsmCtx *c) {
    LxAudioSettings *a = lx_audio_settings();
    float v = lx_clampf(lx_fsm_float(c, 1), 0, 2);
    int b = lx_fsm_int(c, 0);
    if (b == 0) a->master_volume = v;
    else if (b - 1 < LX_BUS_COUNT) a->bus_volume[b - 1] = v;
}
static const LxFsmActionDesc A_BUS_VOLUME = {"Set Volume", "Audio", "The volume of everything, or of sound effects, music, voices or ambience (options menus).", "{0} volume {1}",
    {PENUM("Bus", "Master|Sound Effects|Music|Voice|Ambient", "Master"), P("Volume", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, bus_volume, bus_volume, NULL};
static void load_faded(LxFsmCtx *c) { char ref[LX_REF_MAX]; asset(c, 0, ref, sizeof ref); lx_load_scene_faded(ref, lx_fsm_float(c, 1), lx_fsm_color(c, 2)); }
static const LxFsmActionDesc A_LOAD_FADED = {"Load Scene Faded", "Scenes", "Fades to a colour, switches scene, and fades back in.", "fade to {0}",
    {PASSET("Scene", "scene"), P("Seconds", LX_FSM_FLOAT, "0.6"), P("Color", LX_FSM_COLOR, "0 0 0 1")}, 0, 0, LX_FSM_ONCE, load_faded, NULL, NULL};
static void day_night(LxFsmCtx *c) {
    float hour = lx_fsm_float(c, 1), len = lx_fsm_float(c, 2);
    if (len > 0) hour = fmodf(hour + c->dt * 24.0f / len, 24.0f);
    lx_fsm_set_float(c, 1, hour);
    lx_day_night_apply(c->scene, obj(c, 0), hour);
    bool night = hour < 6 || hour >= 18;
    lx_fsm_set_bool(c, 5, night);
    int *was = c->state;   /* 1 day, 2 night */
    int now = night ? 2 : 1;
    if (*was && *was != now) ev(c, night ? 4 : 3);
    *was = now;
}
static const LxFsmActionDesc A_DAY_NIGHT = {"Day Night Cycle", "Rendering", "Moves the sun through the day and colours the sky to match; Hour (0..24) keeps counting.",
    "{0} at {1}h",
    {PT("Sun", LX_FSM_OBJECT, "name:Directional Light", "a directional light"), {"Hour", LX_FSM_FLOAT, "12", LX_FSM_P_OUT, NULL, "a Float variable: 0..24, it keeps counting"},
     PT("Seconds Per Day", LX_FSM_FLOAT, "240", "0 = stay at this hour"), PEV("Morning Event"), PEV("Evening Event"), POPT("Store Is Night", LX_FSM_BOOL)},
    sizeof(int), 0, LX_FSM_EVERY_FRAME, day_night, day_night, NULL};
static void set_env(LxFsmCtx *c) {
    LxEnvironment *e = lx_scene_environment(c->scene);
    if (!e) return;
    if (!lx_fsm_is_none(c, 0)) e->fog_density = lx_maxf(0, lx_fsm_float(c, 0));
    if (!lx_fsm_is_none(c, 1)) { LxVec4 f = lx_fsm_color(c, 1); e->fog_color = lx_v3(f.x, f.y, f.z); }
    if (!lx_fsm_is_none(c, 2)) e->ambient_intensity = lx_maxf(0, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) e->exposure = lx_maxf(0, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) { LxVec4 k = lx_fsm_color(c, 4); e->sky_zenith = lx_v3(k.x, k.y, k.z); }
    if (!lx_fsm_is_none(c, 5)) { LxVec4 k = lx_fsm_color(c, 5); e->sky_horizon = lx_v3(k.x, k.y, k.z); }
}
#define OPTC(n) {n, LX_FSM_COLOR, "", LX_FSM_P_OPTIONAL, NULL, NULL}
static const LxFsmActionDesc A_SET_ENV = {"Set Environment", "Rendering", "Changes the fog, ambient light, exposure and sky colours (None = keep): caves, storms, night.",
    "environment",
    {OPTF("Fog Density"), OPTC("Fog Color"), OPTF("Ambient"), OPTF("Exposure"), OPTC("Sky Top"), OPTC("Sky Horizon")}, 0, 0, LX_FSM_ONCE, set_env, set_env, NULL};
static void quality(LxFsmCtx *c) {
    LxGraphicsSettings *g = lx_graphics_settings();
    int q = lx_fsm_int(c, 0);
    lx_graphics_apply_preset(g, q);
    g->quality_preset = q;
}
static const LxFsmActionDesc A_QUALITY = {"Set Graphics Quality", "Application", "Low, Medium, High or Ultra (shadows, lighting, reflections) - for options menus.", "quality {0}",
    {PENUM("Quality", "Low|Medium|High|Ultra", "High")}, 0, 0, LX_FSM_ONCE, quality, NULL, NULL};

typedef struct { float t; bool on; } Slow;
static void slow_update(LxFsmCtx *c) {
    Slow *s = c->state;
    if (!s->on) { s->on = true; lx_time_set_scale(lx_clampf(lx_fsm_float(c, 0), 0, 4)); }
    s->t += lx_delta_unscaled(c->scene);
    float len = lx_fsm_float(c, 1);
    if (len > 0 && s->t >= len) { lx_time_set_scale(1); s->on = false; lx_fsm_finish(c); ev(c, 2); }
}
static void slow_exit(LxFsmCtx *c) { Slow *s = c->state; if (s->on) lx_time_set_scale(1); }
static const LxFsmActionDesc A_SLOW = {"Slow Motion", "Time", "Slows the game for a while (in real seconds), then back to normal: bullet time, hit pauses.", "slow to {0} for {1} s",
    {P("Time Scale", LX_FSM_FLOAT, "0.3"), PT("Real Seconds", LX_FSM_FLOAT, "1", "0 = until the state ends"), PEV("Finished Event")},
    sizeof(Slow), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, slow_update, slow_exit};
static void billboard(LxFsmCtx *c) { lx_entity_face_camera(c->scene, obj(c, 0), lx_fsm_bool(c, 1)); }
static const LxFsmActionDesc A_BILLBOARD = {"Face Camera", "Transform", "Keeps an object turned to the camera (sprites, name tags, health bars made of objects).", "{0} faces the camera",
    {POBJ("Object"), PT("Upright", LX_FSM_BOOL, "true", "only turn left and right")}, 0, 0, LX_FSM_LATE, billboard, billboard, NULL};

void lx_fsm_register_gamekit_actions(void) {
    static const LxFsmActionDesc *all[] = {
        &A_LOS, &A_PATROL, &A_WANDER, &A_WALK_TO, &A_FLEE,
        &A_SHOOT_RAY, &A_FIRE_PROJECTILE, &A_PROJECTILE, &A_AREA_DAMAGE, &A_APPLY_DAMAGE, &A_SPAWNER,
        &A_INV_ADD, &A_INV_REMOVE, &A_INV_COUNT, &A_INV_MISC, &A_CHECKPOINT, &A_GLOBALS_IO,
        &A_SHOW_MESSAGE, &A_POPUP, &A_OBJECTIVE, &A_FLASH, &A_BAR_OVER, &A_MARKER, &A_DIALOGUE, &A_CHOICE, &A_INTERACTABLE, &A_FORMAT_TIME,
        &A_PLAY_MUSIC, &A_STOP_MUSIC, &A_BUS_VOLUME, &A_LOAD_FADED, &A_DAY_NIGHT, &A_SET_ENV, &A_QUALITY, &A_SLOW, &A_BILLBOARD,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
