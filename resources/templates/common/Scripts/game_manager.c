/* Game Manager: starts the game, draws the HUD, runs the pause menu, and decides when you win or lose.
 * Put it on one empty object in the scene.
 *
 * Params: Goal (0 = defeat every enemy, 1 = collect every coin), Max Health, Clip Size, Start Reserve Ammo. */
#include "game.h"

GameState g_game;   /* the one copy every script shares (declared in game.h) */

typedef struct Manager {
    bool started;
    float end_time;         /* seconds on the win / lose screen */
    float fps_hud;          /* 1 = first person HUD (ammo, crosshair) */
} Manager;

enum { GOAL_ENEMIES, GOAL_COINS };

static int count_tag(LxScene *s, const char *tag) {
    int n = 0, total = lx_scene_entity_count(s);
    for (int i = 0; i < total; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (e && lx_entity_has_tag(s, e, tag)) n++;
    }
    return n;
}

static void set_paused(bool paused) {
    g_game.phase = paused ? PHASE_PAUSED : PHASE_PLAYING;
    lx_time_set_scale(paused ? 0.0f : 1.0f);
}

static void manager_start(LxBehaviourCtx *ctx) {
    Manager *m = ctx->state;
    memset(&g_game, 0, sizeof g_game);
    g_game.phase = PHASE_PLAYING;
    g_game.max_health = g_game.health = ctx->params[1] > 0 ? ctx->params[1] : 100;
    g_game.stamina = 1;
    g_game.clip_size = g_game.ammo = (int)ctx->params[2];
    g_game.reserve = (int)ctx->params[3];
    g_game.coins_total = count_tag(ctx->scene, "Coin");
    g_game.enemies_left = count_tag(ctx->scene, "Enemy");
    g_game.noise_time = 1e6f;
    m->fps_hud = lx_entity_find_with_tag(ctx->scene, "Weapon") ? 1.0f : 0.0f;
    m->started = true;
    lx_time_set_scale(1);
}

/* ---------------- the HUD ---------------- */
static void bar(float x, float y, float w, float h, float value, uint32_t fill, uint32_t back) {
    lx_ui_rect(x - 3, y - 3, w + 6, h + 6, LX_RGBA(0, 0, 0, 120), (h + 6) * 0.5f);
    lx_ui_bar(x, y, w, h, clampf(value, 0, 1), fill, back);
}

static void draw_crosshair(void) {
    LxVec2 cs = lx_ui_canvas_size();
    float cx = cs.x * 0.5f, cy = cs.y * 0.5f;
    float gap = 6 + g_game.spread * 9, len = 14;
    uint32_t c = LX_RGBA(255, 255, 255, g_game.aiming ? 120 : 220);
    lx_ui_line(cx - gap - len, cy, cx - gap, cy, 3, c);
    lx_ui_line(cx + gap, cy, cx + gap + len, cy, 3, c);
    lx_ui_line(cx, cy - gap - len, cx, cy - gap, 3, c);
    lx_ui_line(cx, cy + gap, cx, cy + gap + len, 3, c);
    lx_ui_circle(cx, cy, 2.5f, c);
    if (g_game.hit_marker > 0) {   /* an X when a shot lands */
        uint32_t h = LX_RGBA(255, 90, 70, (int)(255 * g_game.hit_marker));
        float a = 10, b = 22;
        lx_ui_line(cx - b, cy - b, cx - a, cy - a, 3, h);
        lx_ui_line(cx + a, cy - a, cx + b, cy - b, 3, h);
        lx_ui_line(cx - b, cy + b, cx - a, cy + a, 3, h);
        lx_ui_line(cx + a, cy + a, cx + b, cy + b, 3, h);
    }
}

static void draw_lock_on(LxScene *s) {
    if (!g_game.lock_target || !lx_entity_valid(s, g_game.lock_target)) return;
    LxVec2 p;
    if (!lx_ui_world_to_canvas(s, lx_add3(lx_entity_world_position(s, g_game.lock_target), lx_v3(0, 1.1f, 0)), &p)) return;
    uint32_t c = LX_RGBA(255, 200, 80, 230);
    float r = 26, k = 12;
    lx_ui_line(p.x - r, p.y - r, p.x - r + k, p.y - r, 3, c); lx_ui_line(p.x - r, p.y - r, p.x - r, p.y - r + k, 3, c);
    lx_ui_line(p.x + r, p.y - r, p.x + r - k, p.y - r, 3, c); lx_ui_line(p.x + r, p.y - r, p.x + r, p.y - r + k, 3, c);
    lx_ui_line(p.x - r, p.y + r, p.x - r + k, p.y + r, 3, c); lx_ui_line(p.x - r, p.y + r, p.x - r, p.y + r - k, 3, c);
    lx_ui_line(p.x + r, p.y + r, p.x + r - k, p.y + r, 3, c); lx_ui_line(p.x + r, p.y + r, p.x + r, p.y + r - k, 3, c);
}

static void draw_hud(LxBehaviourCtx *ctx, Manager *m) {
    LxVec2 cs = lx_ui_canvas_size();
    char text[96];
    /* hurt: red at the edges */
    if (g_game.hurt > 0) {
        uint32_t red = LX_RGBA(200, 20, 10, (int)(140 * g_game.hurt));
        lx_ui_rect(0, 0, cs.x, 60, red, 0);
        lx_ui_rect(0, cs.y - 60, cs.x, 60, red, 0);
        lx_ui_rect(0, 0, 60, cs.y, red, 0);
        lx_ui_rect(cs.x - 60, 0, 60, cs.y, red, 0);
    }
    /* health and stamina, bottom left */
    lx_ui_anchor(LX_ANCHOR_BOTTOM_LEFT);
    float hp = g_game.max_health > 0 ? g_game.health / g_game.max_health : 0;
    bar(48, -84, 420, 26, hp, hp > 0.3f ? LX_RGBA(70, 200, 110, 255) : LX_RGBA(230, 70, 60, 255), LX_RGBA(40, 20, 20, 200));
    bar(48, -46, 300, 10, g_game.stamina, LX_RGBA(240, 200, 90, 255), LX_RGBA(40, 35, 20, 180));
    snprintf(text, sizeof text, "%d", (int)ceilf(g_game.health));
    lx_ui_font(LX_FONT_BOLD);
    lx_ui_text(52, -140, 44, LX_COLOR_WHITE, "%s", text);
    /* score and goal, top right */
    lx_ui_anchor(LX_ANCHOR_TOP_RIGHT);
    snprintf(text, sizeof text, "SCORE  %d", g_game.score);
    lx_ui_font(LX_FONT_BOLD);
    lx_ui_text(-48 - lx_ui_text_width(42, text), 40, 42, LX_COLOR_WHITE, "%s", text);
    if (ctx->params[0] == GOAL_COINS) snprintf(text, sizeof text, "Coins  %d / %d", g_game.coins, g_game.coins_total);
    else snprintf(text, sizeof text, "Enemies left  %d", g_game.enemies_left);
    lx_ui_text(-48 - lx_ui_text_width(32, text), 96, 32, LX_RGBA(255, 220, 160, 255), "%s", text);
    /* ammo, bottom right */
    if (m->fps_hud > 0) {
        lx_ui_anchor(LX_ANCHOR_BOTTOM_RIGHT);
        lx_ui_font(LX_FONT_BOLD);
        snprintf(text, sizeof text, "%d", g_game.ammo);
        uint32_t c = g_game.ammo == 0 ? LX_RGBA(240, 80, 70, 255) : LX_COLOR_WHITE;
        float w = lx_ui_text_width(72, text);
        lx_ui_text(-180 - w, -128, 72, c, "%s", text);
        snprintf(text, sizeof text, "/ %d", g_game.reserve);
        lx_ui_text(-170, -104, 40, LX_RGBA(220, 220, 220, 200), "%s", text);
        if (g_game.reloading) {
            bar(-330, -40, 280, 8, g_game.reload_progress, LX_RGBA(255, 210, 120, 255), LX_RGBA(30, 30, 30, 180));
            lx_ui_text(-330, -76, 26, LX_RGBA(255, 210, 120, 255), "RELOADING");
        } else if (g_game.ammo == 0 && g_game.reserve > 0) lx_ui_text(-330, -76, 26, LX_RGBA(240, 80, 70, 255), "PRESS R TO RELOAD");
        lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
        draw_crosshair();
    }
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    draw_lock_on(ctx->scene);
    /* what Interact would do */
    lx_ui_font(LX_FONT_BOLD);
    if (g_game.prompt[0]) {
        snprintf(text, sizeof text, "[%s]  %s", interact_key(), g_game.prompt);
        float w = lx_ui_text_width(36, text);
        lx_ui_rect(cs.x * 0.5f - w * 0.5f - 24, cs.y * 0.66f - 12, w + 48, 60, LX_RGBA(0, 0, 0, 150), 30);
        lx_ui_text(cs.x * 0.5f - w * 0.5f, cs.y * 0.66f, 36, LX_COLOR_WHITE, "%s", text);
    }
    if (g_game.message_time > 0) {
        int a = (int)(255 * clampf(g_game.message_time, 0, 1));
        float w = lx_ui_text_width(34, g_game.message);
        lx_ui_text(cs.x * 0.5f - w * 0.5f, cs.y * 0.74f, 34, LX_RGBA(255, 230, 150, a), "%s", g_game.message);
    }
    lx_ui_font(LX_FONT_REGULAR);
}

/* ---------------- menus ---------------- */
static bool menu_button(const char *label, float y) {
    LxVec2 cs = lx_ui_canvas_size();
    return lx_ui_button(label, cs.x * 0.5f - 200, y, 400, 84);
}
static void restart(LxScene *s) {
    char ref[LX_REF_MAX];
    snprintf(ref, sizeof ref, "Scenes/%s.lxscene", lx_scene_name(s));
    lx_time_set_scale(1);
    lx_load_scene(ref);
}
static void title(const char *text, float y, uint32_t color) {
    LxVec2 cs = lx_ui_canvas_size();
    lx_ui_font(LX_FONT_BOLD);
    lx_ui_text(cs.x * 0.5f - lx_ui_text_width(110, text) * 0.5f, y, 110, color, "%s", text);
    lx_ui_font(LX_FONT_REGULAR);
}
static void draw_pause(LxBehaviourCtx *ctx) {
    LxVec2 cs = lx_ui_canvas_size();
    lx_ui_rect(0, 0, cs.x, cs.y, LX_RGBA(0, 0, 0, 150), 0);
    title("PAUSED", cs.y * 0.5f - 300, LX_COLOR_WHITE);
    if (menu_button("Resume", cs.y * 0.5f - 90)) set_paused(false);
    if (menu_button("Restart", cs.y * 0.5f + 20)) restart(ctx->scene);
    if (menu_button("Quit", cs.y * 0.5f + 130)) lx_quit();
}
static void draw_end(LxBehaviourCtx *ctx, Manager *m) {
    LxVec2 cs = lx_ui_canvas_size();
    bool won = g_game.phase == PHASE_WON;
    float a = clampf(m->end_time / 1.2f, 0, 1);
    lx_ui_rect(0, 0, cs.x, cs.y, won ? LX_RGBA(5, 25, 10, (int)(160 * a)) : LX_RGBA(40, 0, 0, (int)(190 * a)), 0);
    if (m->end_time < 0.6f) return;
    const char *head = won ? (ctx->params[0] == GOAL_COINS ? "ALL COINS FOUND" : "AREA CLEARED") : "YOU DIED";
    title(head, cs.y * 0.5f - 300, won ? LX_RGBA(150, 255, 150, 255) : LX_RGBA(255, 80, 70, 255));
    char text[64];
    snprintf(text, sizeof text, "Score  %d", g_game.score);
    lx_ui_text(cs.x * 0.5f - lx_ui_text_width(52, text) * 0.5f, cs.y * 0.5f - 170, 52, LX_COLOR_WHITE, "%s", text);
    if (menu_button(won ? "Play Again" : "Try Again", cs.y * 0.5f - 40)) restart(ctx->scene);
    if (menu_button("Quit", cs.y * 0.5f + 70)) lx_quit();
}

/* ---------------- every frame ---------------- */
static void publish_globals(void) {   /* for State Graphs (Assets/Globals.lxglobals) */
    lx_fsm_set_global_float("Health", g_game.health);
    lx_fsm_set_global_float("Max Health", g_game.max_health);
    lx_fsm_set_global_float("Score", (float)g_game.score);
    lx_fsm_set_global_float("Ammo", (float)g_game.ammo);
    lx_fsm_set_global_float("Reserve Ammo", (float)g_game.reserve);
    lx_fsm_set_global_float("Coins", (float)g_game.coins);
    lx_fsm_set_global_float("Game Phase", (float)g_game.phase);
}

static void manager_update(LxBehaviourCtx *ctx, float dt) {
    Manager *m = ctx->state;
    if (!m->started) manager_start(ctx);
    float rdt = lx_delta_unscaled(ctx->scene);   /* menus work while time is frozen */
    if (rdt <= 0 || rdt > 0.1f) rdt = dt > 0 ? dt : 1.0f / 60;
    LxScene *s = ctx->scene;
    g_game.enemies_left = count_tag(s, "Enemy");
    g_game.coins = g_game.coins_total - count_tag(s, "Coin");
    /* win / lose */
    if (g_game.phase == PHASE_PLAYING) {
        if (g_game.health <= 0) { g_game.health = 0; g_game.phase = PHASE_LOST; m->end_time = 0; }
        else if (ctx->params[0] == GOAL_COINS ? g_game.coins_total > 0 && g_game.coins >= g_game.coins_total : g_game.enemies_left == 0) {
            g_game.phase = PHASE_WON;
            m->end_time = 0;
        }
    }
    /* pause with Esc / Start */
    if (lx_action_pressed("Pause")) {
        if (g_game.phase == PHASE_PLAYING) set_paused(true);
        else if (g_game.phase == PHASE_PAUSED) set_paused(false);
    }
    bool playing = g_game.phase == PHASE_PLAYING;
    lx_mouse_lock(playing);
    lx_mouse_show(!playing);
    /* effects fade */
    g_game.hurt = approach(g_game.hurt, 0, dt * 1.8f);
    g_game.shake = approach(g_game.shake, 0, dt * 2.5f);
    g_game.hit_marker = approach(g_game.hit_marker, 0, dt * 4.0f);
    g_game.message_time = approach(g_game.message_time, 0, rdt);
    g_game.noise_time += dt;
    if (g_game.phase == PHASE_WON || g_game.phase == PHASE_LOST) m->end_time += rdt;
    /* draw */
    if (g_game.phase != PHASE_LOST || m->end_time < 0.6f) draw_hud(ctx, m);
    if (g_game.phase == PHASE_PAUSED) draw_pause(ctx);
    else if (g_game.phase == PHASE_WON || g_game.phase == PHASE_LOST) draw_end(ctx, m);
    g_game.prompt[0] = 0;   /* written again next frame by whatever the player looks at */
    publish_globals();
}

static void manager_destroy(LxBehaviourCtx *ctx) {
    (void)ctx;
    lx_time_set_scale(1);
}

LX_SCRIPT_REGISTER(game_manager) {
    LxBehaviourDesc d = {0};
    d.name = "Game Manager";
    d.state_size = sizeof(Manager);
    d.param_names[0] = "Goal (0 enemies, 1 coins)";
    d.param_names[1] = "Max Health";
    d.param_names[2] = "Clip Size";
    d.param_names[3] = "Start Reserve Ammo";
    d.param_defaults[0] = 0;
    d.param_defaults[1] = 100;
    d.param_defaults[2] = 12;
    d.param_defaults[3] = 48;
    d.on_start = manager_start;
    d.on_update = manager_update;
    d.on_destroy = manager_destroy;
    lx_register_behaviour(&d);
}
