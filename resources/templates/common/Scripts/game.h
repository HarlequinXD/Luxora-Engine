/* Shared by every script in this game: the game's state (health, score, ammo...) and small helpers.
 *
 * How the pieces talk to each other:
 *   - g_game (defined in game_manager.c) holds what the HUD shows and what decides winning or losing.
 *   - Objects send each other messages with lx_send_message(scene, target, "Message", value, sender):
 *       "Damage"   value = hit points          (weapons -> enemies, enemies -> the player)
 *       "Hover"    the player is looking at it  (it writes its prompt, e.g. "Open Door")
 *       "Interact" the player pressed Interact  (doors open, pickups get picked up)
 *   - Tags: "Player", "Enemy", "Coin", "Interactable" (can be used with the Interact button).
 *
 * The game manager copies Health, Score, Ammo... into the global variables (Assets/Globals.lxglobals)
 * every frame, so State Graphs can read them too. */
#ifndef GAME_H
#define GAME_H
#include <luxora/luxora.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum GamePhase { PHASE_PLAYING, PHASE_PAUSED, PHASE_WON, PHASE_LOST } GamePhase;

typedef struct GameState {
    GamePhase phase;
    float health, max_health;
    float stamina;              /* 0..1: sprinting and dodging use it up */
    int score;
    int ammo, clip_size, reserve;   /* first person */
    bool reloading;
    float reload_progress;          /* 0..1 while reloading */
    bool aiming;
    float spread;                   /* crosshair size, degrees */
    int coins, coins_total;         /* third person */
    int enemies_left;
    LxEntity lock_target;           /* third person lock-on */
    bool sprinting;
    /* effects, faded by the game manager */
    float hurt;                     /* red flash at the screen edges, 0..1 */
    float shake;                    /* camera shake, 0..1 */
    float hit_marker;               /* "you hit something" on the crosshair, 0..1 */
    float recoil;                   /* degrees of kick for the camera to take (first person) */
    LxVec3 noise_at;                /* a gunshot: enemies within earshot come to look */
    float noise_time;               /* seconds since that noise; large = none */
    /* text */
    char prompt[64];                /* what Interact would do now: written every frame by what the player looks at */
    char message[96];               /* a short note: "+24 ammo", "Health is full" */
    float message_time;
} GameState;

extern GameState g_game;

static inline bool game_playing(void) { return g_game.phase == PHASE_PLAYING; }
static inline void game_message(const char *text) {
    snprintf(g_game.message, sizeof g_game.message, "%s", text);
    g_game.message_time = 2.0f;
}
static inline void game_shake(float amount) { if (amount > g_game.shake) g_game.shake = amount; }

/* how far to close a gap this frame at `speed` per second: frame-rate independent smoothing */
static inline float smooth(float speed, float dt) { return 1.0f - expf(-speed * dt); }
static inline float approach(float from, float to, float step) {
    if (from < to) return from + step > to ? to : from + step;
    return from - step < to ? to : from - step;
}
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float randf(float lo, float hi) { return lx_random_range(lo, hi); }
/* the angle (degrees) to turn from `from` to `to`, -180..180 */
static inline float angle_delta(float from, float to) {
    float d = fmodf(to - from + 540.0f, 360.0f) - 180.0f;
    return d;
}
/* yaw (degrees, around Y) that faces along a flat direction. Forward is +Z (like Unity); with +Y up, an
 * object's right is its -X. */
static inline float yaw_of(LxVec3 dir) { return atan2f(dir.x, dir.z) * (180.0f / 3.14159265f); }
static inline LxVec3 forward_of_yaw(float yaw) {
    float r = yaw * (3.14159265f / 180.0f);
    return lx_v3(sinf(r), 0, cosf(r));
}
static inline LxVec3 right_of(LxVec3 forward) { return lx_v3(-forward.z, 0, forward.x); }
static inline LxVec3 flat(LxVec3 v) { return lx_v3(v.x, 0, v.z); }

/* the top of a hierarchy: a bullet that hits an enemy's arm damages the enemy */
static inline LxEntity root_of(LxScene *s, LxEntity e) {
    for (int i = 0; e && lx_entity_parent(s, e) && i < 32; i++) e = lx_entity_parent(s, e);
    return e;
}
/* the nearest object up the hierarchy (itself included) with a tag, or 0 */
static inline LxEntity tagged_up(LxScene *s, LxEntity e, const char *tag) {
    for (int i = 0; e && i < 32; i++, e = lx_entity_parent(s, e))
        if (lx_entity_has_tag(s, e, tag)) return e;
    return 0;
}

/* the key or button to show in prompts, for what the player used last */
static inline const char *interact_key(void) {
    switch (lx_input_last_device()) {
    case LX_DEVICE_GAMEPAD: return "X";
    case LX_DEVICE_TOUCH: return "USE";
    default: return "E";
    }
}

#endif
