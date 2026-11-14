/* Gameplay plumbing: prefabs, tags, messages and events, timers, tweens, time scale, random numbers,
 * save data and access to other objects' behaviours. */
#include "../lx_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* ======================================================================
 *  per-scene state
 * ====================================================================== */
typedef struct Timer {
    int id;
    LxEntity owner;
    float remaining, interval;
    LxTimerFn fn;
    void *user;
    char message[LX_NAME_MAX];   /* lx_send_message_after */
    float value;
} Timer;

typedef enum { TW_POSITION, TW_SCALE, TW_EULER, TW_FLOAT } TweenKind;
typedef struct Tween {
    int id;
    TweenKind kind;
    LxEntity e;
    float *target;
    LxVec3 from, to;
    float t, seconds;
    LxEase ease;
    bool loop, ping_pong, forward;
    char done_message[LX_NAME_MAX];
} Tween;

typedef struct LxGameplay {
    Timer *timers;
    int timer_count, timer_cap;
    Tween *tweens;
    int tween_count, tween_cap;
    LxEntity *doomed;
    int doomed_count, doomed_cap;
    int next_id;
    float game_time, unscaled_dt;
} LxGameplay;

static LxGameplay *gp(LxScene *s) {
    if (!s->gameplay) s->gameplay = lx_calloc(1, sizeof(LxGameplay));
    return s->gameplay;
}

void lx_gameplay_free(LxScene *s) {
    LxGameplay *g = s->gameplay;
    if (!g) return;
    lx_free(g->timers);
    lx_free(g->tweens);
    lx_free(g->doomed);
    lx_free(g);
    s->gameplay = NULL;
}

/* ======================================================================
 *  time
 * ====================================================================== */
static float g_time_scale = 1.0f;
void lx_time_set_scale(float scale) { g_time_scale = scale < 0 ? 0 : scale; }
float lx_time_scale(void) { return g_time_scale; }
float lx_gameplay_scale_dt(float dt) { return dt * g_time_scale; }
float lx_game_time(const LxScene *s) { return s->gameplay ? s->gameplay->game_time : 0.0f; }
float lx_delta_unscaled(const LxScene *s) { return s->gameplay ? s->gameplay->unscaled_dt : 0.0f; }

/* ======================================================================
 *  tags
 * ====================================================================== */
void lx_entity_set_tag(LxScene *s, LxEntity e, const char *tag) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) lx_strcpy(d->tag, sizeof d->tag, tag ? tag : "");
}
const char *lx_entity_tag(const LxScene *s, LxEntity e) {
    const LxEntityData *d = lx_entity_data(s, e);
    return d ? d->tag : "";
}
bool lx_entity_has_tag(const LxScene *s, LxEntity e, const char *tag) {
    const LxEntityData *d = lx_entity_data(s, e);
    return d && tag && tag[0] && !strcmp(d->tag, tag);
}
static bool usable(const LxEntityData *d) { return d->alive && !d->pending_destroy; }
static LxEntity handle(const LxScene *s, int i) { return lx_entity_make((uint32_t)i, s->entities[i].generation & 0xfffu); }

int lx_entities_with_tag(const LxScene *s, const char *tag, LxEntity *out, int max) {
    int n = 0;
    if (!tag || !tag[0]) return 0;
    for (int i = 1; i < s->count; i++) {
        const LxEntityData *d = &s->entities[i];
        if (!usable(d) || strcmp(d->tag, tag)) continue;
        if (out && n < max) out[n] = handle(s, i);
        n++;
    }
    return n;
}
LxEntity lx_entity_find_with_tag(const LxScene *s, const char *tag) {
    LxEntity e = 0;
    lx_entities_with_tag(s, tag, &e, 1);
    return e;
}

LxEntity lx_entity_find_child(const LxScene *s, LxEntity parent, const char *name) {
    const LxEntityData *p = lx_entity_data(s, parent);
    if (!p || !name) return 0;
    for (LxEntity c = p->first_child; c; c = lx_entity_next_sibling(s, c)) {
        const LxEntityData *d = lx_entity_data(s, c);
        if (d && !strcmp(d->name, name)) return c;
        LxEntity deeper = lx_entity_find_child(s, c, name);
        if (deeper) return deeper;
    }
    return 0;
}

/* ======================================================================
 *  messages and events
 * ====================================================================== */
/* A message reaches the object's behaviour (on_message) and its State Graphs (as an event of the same name). */
bool lx_send_message(LxScene *s, LxEntity target, const char *message, float value, LxEntity sender) {
    LxEntityData *d = lx_entity_data(s, target);
    if (!d || !usable(d) || !message) return false;
    bool got = false;
    if (d->comps & LX_COMP_BEHAVIOUR) {
        const LxBehaviourDesc *desc = d->binst.desc ? d->binst.desc : lx_behaviour_find(d->behaviour.name);
        if (desc && desc->on_message) {
            if (desc->state_size && !d->binst.state) {   /* not started yet: give it its state now */
                d->binst.desc = desc;
                d->binst.state = lx_calloc(1, desc->state_size);
            }
            LxBehaviourCtx ctx = {s, target, d->binst.state, d->behaviour.params};
            desc->on_message(&ctx, message, value, sender);
            got = true;
        }
    }
    d = lx_entity_data(s, target);
    if (d && (d->comps & LX_COMP_STATE_GRAPH)) got |= lx_fsm_send_event(s, target, message, value, sender) > 0;
    return got;
}

int lx_broadcast(LxScene *s, const char *message, float value, LxEntity sender) {
    int n = 0, count = s->count;
    for (int i = 1; i < count; i++)
        if (usable(&s->entities[i]) && (s->entities[i].comps & (LX_COMP_BEHAVIOUR | LX_COMP_STATE_GRAPH))) n += lx_send_message(s, handle(s, i), message, value, sender);
    return n;
}

int lx_send_message_to_tag(LxScene *s, const char *tag, const char *message, float value, LxEntity sender) {
    int n = 0, count = s->count;
    for (int i = 1; i < count; i++)
        if (usable(&s->entities[i]) && tag && !strcmp(s->entities[i].tag, tag)) n += lx_send_message(s, handle(s, i), message, value, sender);
    return n;
}

typedef struct Listener { int id; char event[LX_NAME_MAX]; LxEventFn fn; void *user; } Listener;
static Listener *g_listeners;
static int g_listener_count, g_listener_cap, g_listener_next = 1;

int lx_event_listen(const char *event, LxEventFn fn, void *user) {
    if (!event || !fn) return 0;
    Listener l = {g_listener_next++, "", fn, user};
    lx_strcpy(l.event, sizeof l.event, event);
    LX_VEC_PUSH(g_listeners, g_listener_count, g_listener_cap, l);
    return l.id;
}
void lx_event_unlisten(int id) {
    for (int i = 0; i < g_listener_count; i++)
        if (g_listeners[i].id == id) { memmove(&g_listeners[i], &g_listeners[i + 1], sizeof(Listener) * (size_t)(g_listener_count - i - 1)); g_listener_count--; return; }
}
int lx_event_emit(const char *event, float value) {
    int n = 0;
    for (int i = 0; i < g_listener_count; i++) {
        if (strcmp(g_listeners[i].event, event)) continue;
        Listener l = g_listeners[i];   /* a listener may unlisten itself */
        l.fn(event, value, l.user);
        n++;
    }
    return n;
}

/* ======================================================================
 *  timers
 * ====================================================================== */
int lx_timer_start(LxScene *s, LxEntity owner, float delay, float interval, LxTimerFn fn, void *user) {
    if (!fn) return 0;
    LxGameplay *g = gp(s);
    Timer t = {++g->next_id, owner, delay < 0 ? 0 : delay, interval < 0 ? 0 : interval, fn, user, "", 0};
    LX_VEC_PUSH(g->timers, g->timer_count, g->timer_cap, t);
    return t.id;
}
int lx_send_message_after(LxScene *s, LxEntity target, const char *message, float value, float delay) {
    LxGameplay *g = gp(s);
    Timer t = {++g->next_id, target, delay < 0 ? 0 : delay, 0, NULL, NULL, "", value};
    lx_strcpy(t.message, sizeof t.message, message ? message : "");
    LX_VEC_PUSH(g->timers, g->timer_count, g->timer_cap, t);
    return t.id;
}
void lx_timer_cancel(LxScene *s, int id) {
    LxGameplay *g = s->gameplay;
    if (!g) return;
    for (int i = 0; i < g->timer_count; i++)
        if (g->timers[i].id == id) g->timers[i].remaining = -1e30f;   /* removed on the next update */
}

static void update_timers(LxScene *s, float dt) {
    LxGameplay *g = s->gameplay;
    if (!g) return;
    int n = g->timer_count;   /* timers started by callbacks wait for the next frame */
    for (int i = 0; i < n; i++) {
        if (g->timers[i].remaining < -1e29f) continue;
        g->timers[i].remaining -= dt;
        if (g->timers[i].remaining > 0) continue;
        Timer t = g->timers[i];
        if (t.owner && !lx_entity_valid(s, t.owner)) { g->timers[i].remaining = -1e30f; continue; }
        if (t.interval > 0) g->timers[i].remaining += t.interval;
        else g->timers[i].remaining = -1e30f;
        if (t.fn) t.fn(s, t.owner, t.user);
        else lx_send_message(s, t.owner, t.message, t.value, 0);
        g = s->gameplay;
    }
    int w = 0;
    for (int i = 0; i < g->timer_count; i++)
        if (g->timers[i].remaining > -1e29f) g->timers[w++] = g->timers[i];
    g->timer_count = w;
}

/* ======================================================================
 *  tweens
 * ====================================================================== */
float lx_ease(LxEase ease, float t) {
    t = lx_clampf(t, 0, 1);
    switch (ease) {
    case LX_EASE_IN: return t * t * t;
    case LX_EASE_OUT: { float u = 1 - t; return 1 - u * u * u; }
    case LX_EASE_IN_OUT: return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) * 0.5f;
    case LX_EASE_BACK_OUT: { const float c1 = 1.70158f, c3 = c1 + 1; float u = t - 1; return 1 + c3 * u * u * u + c1 * u * u; }
    case LX_EASE_ELASTIC_OUT:
        if (t <= 0 || t >= 1) return t;
        return powf(2, -10 * t) * sinf((t * 10 - 0.75f) * (2 * LX_PI / 3)) + 1;
    case LX_EASE_BOUNCE_OUT: {
        const float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1 / d1) return n1 * t * t;
        if (t < 2 / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
    default: return t;
    }
}

static int add_tween(LxScene *s, TweenKind kind, LxEntity e, float *target, LxVec3 from, LxVec3 to, float seconds, LxEase ease) {
    LxGameplay *g = gp(s);
    /* a new tween of the same kind on the same thing replaces the old one */
    for (int i = 0; i < g->tween_count; i++)
        if (g->tweens[i].kind == kind && g->tweens[i].e == e && g->tweens[i].target == target) g->tweens[i].seconds = -1;
    Tween t;
    memset(&t, 0, sizeof t);
    t.id = ++g->next_id;
    t.kind = kind;
    t.e = e;
    t.target = target;
    t.from = from;
    t.to = to;
    t.seconds = seconds > 1e-4f ? seconds : 1e-4f;
    t.ease = ease;
    t.forward = true;
    LX_VEC_PUSH(g->tweens, g->tween_count, g->tween_cap, t);
    return t.id;
}
int lx_tween_position(LxScene *s, LxEntity e, LxVec3 to, float seconds, LxEase ease) {
    if (!lx_entity_valid(s, e)) return 0;
    return add_tween(s, TW_POSITION, e, NULL, lx_entity_position(s, e), to, seconds, ease);
}
int lx_tween_scale(LxScene *s, LxEntity e, LxVec3 to, float seconds, LxEase ease) {
    if (!lx_entity_valid(s, e)) return 0;
    return add_tween(s, TW_SCALE, e, NULL, lx_entity_scale(s, e), to, seconds, ease);
}
int lx_tween_euler(LxScene *s, LxEntity e, LxVec3 to, float seconds, LxEase ease) {
    if (!lx_entity_valid(s, e)) return 0;
    return add_tween(s, TW_EULER, e, NULL, lx_entity_euler(s, e), to, seconds, ease);
}
int lx_tween_float(LxScene *s, LxEntity owner, float *value, float to, float seconds, LxEase ease) {
    if (!value) return 0;
    return add_tween(s, TW_FLOAT, owner, value, lx_v3(*value, 0, 0), lx_v3(to, 0, 0), seconds, ease);
}
static Tween *find_tween(LxScene *s, int id) {
    LxGameplay *g = s->gameplay;
    for (int i = 0; g && i < g->tween_count; i++) if (g->tweens[i].id == id) return &g->tweens[i];
    return NULL;
}
void lx_tween_set_done_message(LxScene *s, int id, const char *message) {
    Tween *t = find_tween(s, id);
    if (t) lx_strcpy(t->done_message, sizeof t->done_message, message ? message : "");
}
void lx_tween_set_loop(LxScene *s, int id, bool ping_pong) {
    Tween *t = find_tween(s, id);
    if (!t) return;
    t->loop = true;
    t->ping_pong = ping_pong;
}
void lx_tween_cancel(LxScene *s, int id) {
    Tween *t = find_tween(s, id);
    if (t) t->seconds = -1;
}
void lx_tweens_cancel_for(LxScene *s, LxEntity e) {
    LxGameplay *g = s->gameplay;
    for (int i = 0; g && i < g->tween_count; i++) if (g->tweens[i].e == e) g->tweens[i].seconds = -1;
}

static void update_tweens(LxScene *s, float dt) {
    LxGameplay *g = s->gameplay;
    if (!g) return;
    int n = g->tween_count;
    for (int i = 0; i < n; i++) {
        Tween *t = &g->tweens[i];
        if (t->seconds < 0) continue;
        if (t->kind != TW_FLOAT && !lx_entity_valid(s, t->e)) { t->seconds = -1; continue; }
        t->t += dt / t->seconds;
        bool done = t->t >= 1.0f;
        float k = lx_ease(t->ease, t->forward ? lx_minf(t->t, 1) : 1 - lx_minf(t->t, 1));
        LxVec3 v = lx_lerp3(t->from, t->to, k);
        switch (t->kind) {
        case TW_POSITION: lx_entity_set_position(s, t->e, v); break;
        case TW_SCALE: lx_entity_set_scale(s, t->e, v); break;
        case TW_EULER: lx_entity_set_euler(s, t->e, v); break;
        case TW_FLOAT: *t->target = v.x; break;
        }
        if (!done) continue;
        if (t->loop) {
            t->t -= 1.0f;
            if (t->ping_pong) t->forward = !t->forward;
            continue;
        }
        Tween fin = *t;
        t->seconds = -1;
        if (fin.done_message[0] && fin.e) lx_send_message(s, fin.e, fin.done_message, 1.0f, 0);
        g = s->gameplay;
    }
    int w = 0;
    for (int i = 0; i < g->tween_count; i++)
        if (g->tweens[i].seconds >= 0) g->tweens[w++] = g->tweens[i];
    g->tween_count = w;
}

/* ======================================================================
 *  random
 * ====================================================================== */
static LxRng g_rng = {0x853c49e6748fea9bULL, 0xda3e39cb94b95bdbULL};
void lx_random_seed(uint64_t seed) { g_rng = lx_rng_seed(seed, 54); }
float lx_random(void) { return lx_rng_f(&g_rng); }
float lx_random_range(float a, float b) { return a + (b - a) * lx_rng_f(&g_rng); }
int lx_random_int(int a, int b) {
    if (b < a) { int t = a; a = b; b = t; }
    int64_t span = (int64_t)b - a + 1;   /* up to 2^32: computed wide, so no overflow */
    return (int)((int64_t)a + (int64_t)(lx_rng_u32(&g_rng) % (uint64_t)span));
}
LxVec3 lx_random_in_sphere(float r) { return lx_scale3(lx_rng_sphere(&g_rng), r * cbrtf(lx_rng_f(&g_rng))); }
bool lx_random_chance(float p) { return lx_rng_f(&g_rng) < p; }

/* ======================================================================
 *  save data: a small key = value file
 * ====================================================================== */
typedef struct SaveEntry { char key[64]; char value[256]; } SaveEntry;
static SaveEntry *g_save;
static int g_save_count, g_save_cap;
static char g_save_path[LX_PATH_MAX];
static bool g_save_loaded, g_save_dirty;

static void save_load(void) {
    if (g_save_loaded) return;
    g_save_loaded = true;
    g_save_count = 0;
    if (!g_save_path[0]) return;
    char *text = lx_file_read(g_save_path, NULL);
    if (!text) return;
    const char *cur = text;
    char k[64], v[256];
    int r;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v)))
        if (r == 1) {
            SaveEntry e;
            lx_strcpy(e.key, sizeof e.key, k);
            lx_strcpy(e.value, sizeof e.value, v);
            LX_VEC_PUSH(g_save, g_save_count, g_save_cap, e);
        }
    lx_free(text);
}
void lx_save_set_file(const char *path) {
    if (g_save_dirty) lx_save_write();
    lx_strcpy(g_save_path, sizeof g_save_path, path ? path : "");
    g_save_loaded = false;
}
const char *lx_save_file(void) { return g_save_path; }

static SaveEntry *save_find(const char *key) {
    save_load();
    for (int i = 0; i < g_save_count; i++) if (!strcmp(g_save[i].key, key)) return &g_save[i];
    return NULL;
}
static void save_put(const char *key, const char *value) {
    if (!key || !key[0]) return;
    SaveEntry *e = save_find(key);
    if (!e) {
        SaveEntry n;
        memset(&n, 0, sizeof n);
        lx_strcpy(n.key, sizeof n.key, key);
        LX_VEC_PUSH(g_save, g_save_count, g_save_cap, n);
        e = &g_save[g_save_count - 1];
    } else if (!strcmp(e->value, value)) return;
    lx_strcpy(e->value, sizeof e->value, value);
    for (char *c = e->value; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    g_save_dirty = true;
}
void lx_save_set_int(const char *key, int value) { char b[32]; snprintf(b, sizeof b, "%d", value); save_put(key, b); }
void lx_save_set_float(const char *key, float value) { char b[48]; snprintf(b, sizeof b, "%.9g", value); save_put(key, b); }
void lx_save_set_string(const char *key, const char *value) { save_put(key, value ? value : ""); }
int lx_save_get_int(const char *key, int fallback) { SaveEntry *e = save_find(key); return e ? atoi(e->value) : fallback; }
float lx_save_get_float(const char *key, float fallback) { SaveEntry *e = save_find(key); return e ? (float)atof(e->value) : fallback; }
const char *lx_save_get_string(const char *key, const char *fallback) { SaveEntry *e = save_find(key); return e ? e->value : fallback; }
bool lx_save_has(const char *key) { return save_find(key) != NULL; }
void lx_save_delete(const char *key) {
    SaveEntry *e = save_find(key);
    if (!e) return;
    *e = g_save[--g_save_count];
    g_save_dirty = true;
}
void lx_save_clear(void) { save_load(); g_save_count = 0; g_save_dirty = true; }
bool lx_save_write(void) {
    if (!g_save_path[0]) return false;
    save_load();
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora save data\n");
    for (int i = 0; i < g_save_count; i++) lx_sb_printf(&sb, "%s = %s\n", g_save[i].key, g_save[i].value);
    char dir[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, g_save_path);
    if (dir[0]) lx_mkdir_p(dir);
    bool ok = lx_file_write(g_save_path, sb.data, sb.len);
    lx_free(sb.data);
    if (ok) g_save_dirty = false;
    else LX_WARN("Could not write save data to %s", g_save_path);
    return ok;
}

/* ======================================================================
 *  other objects' behaviours
 * ====================================================================== */
void *lx_behaviour_state(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d ? d->binst.state : NULL;
}
float *lx_behaviour_params(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && (d->comps & LX_COMP_BEHAVIOUR) ? d->behaviour.params : NULL;
}
const char *lx_behaviour_name(const LxScene *s, LxEntity e) {
    const LxEntityData *d = lx_entity_data(s, e);
    return d && (d->comps & LX_COMP_BEHAVIOUR) ? d->behaviour.name : "";
}

/* ======================================================================
 *  prefabs
 * ====================================================================== */
const char *lx_entity_prefab(const LxScene *s, LxEntity e) {
    const LxEntityData *d = lx_entity_data(s, e);
    return d ? d->prefab : "";
}
void lx_entity_set_prefab(LxScene *s, LxEntity e, const char *ref) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d) lx_strcpy(d->prefab, sizeof d->prefab, ref ? ref : "");
}

bool lx_prefab_save(const LxScene *s, LxEntity root, const char *path) {
    char *text = lx_entity_serialize(s, root);
    if (!text) return false;
    char dir[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, path);
    if (dir[0]) lx_mkdir_p(dir);
    bool ok = lx_file_write(path, text, strlen(text));
    lx_free(text);
    if (!ok) LX_ERROR("Could not save prefab %s", path);
    return ok;
}

LxEntity lx_prefab_instantiate_text(LxScene *s, const char *text, LxVec3 position, LxEntity parent) {
    LxEntity e = lx_scene_deserialize_into(s, text, parent);
    if (e) {
        lx_entity_set_position(s, e, position);
        lx_scene_update_transforms(s);
    }
    return e;
}

/* prefab files are read once and kept */
typedef struct PrefabCache { char ref[LX_REF_MAX]; char *text; } PrefabCache;
static PrefabCache *g_prefabs;
static int g_prefab_count, g_prefab_cap;

void lx_prefab_cache_clear(void) {
    for (int i = 0; i < g_prefab_count; i++) lx_free(g_prefabs[i].text);
    g_prefab_count = 0;
}

LxEntity lx_prefab_instantiate(LxScene *s, const char *ref, LxVec3 position, LxEntity parent) {
    if (!ref || !ref[0]) return 0;
    const char *text = NULL;
    for (int i = 0; i < g_prefab_count; i++)
        if (!strcmp(g_prefabs[i].ref, ref)) { text = g_prefabs[i].text; break; }
    if (!text) {
        char path[LX_PATH_MAX];
        lx_asset_path(path, sizeof path, ref);
        char *t = lx_file_read(path, NULL);
        if (!t) { LX_ERROR("Prefab '%s' not found", ref); return 0; }
        PrefabCache c;
        lx_strcpy(c.ref, sizeof c.ref, ref);
        c.text = t;
        LX_VEC_PUSH(g_prefabs, g_prefab_count, g_prefab_cap, c);
        text = t;
    }
    LxEntity e = lx_prefab_instantiate_text(s, text, position, parent);
    if (e) lx_entity_set_prefab(s, e, ref);
    return e;
}

/* ======================================================================
 *  frame hooks (called by lx_scene_simulate)
 * ====================================================================== */
void lx_gameplay_begin_frame(LxScene *s, float dt, float unscaled_dt) {
    LxGameplay *g = gp(s);
    g->game_time += dt;
    g->unscaled_dt = unscaled_dt;
    update_timers(s, dt);
}

void lx_gameplay_after_update(LxScene *s, float dt) { update_tweens(s, dt); }

void lx_gameplay_entity_destroyed(LxScene *s, LxEntity e) {
    LxGameplay *g = s->gameplay;
    if (!g) return;
    for (int i = 0; i < g->tween_count; i++) if (g->tweens[i].e == e) g->tweens[i].seconds = -1;
    for (int i = 0; i < g->timer_count; i++) if (g->timers[i].owner == e) g->timers[i].remaining = -1e30f;
}

/* objects destroyed while the frame runs: queued (and hidden) until it ends */
bool lx_gameplay_defer_destroy(LxScene *s, LxEntity e) {
    if (!s->in_update) return false;
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return true;
    if (!d->pending_destroy) {
        d->pending_destroy = true;
        d->active = false;   /* gone from rendering, physics and updates right away */
        LxGameplay *g = gp(s);
        LX_VEC_PUSH(g->doomed, g->doomed_count, g->doomed_cap, e);
    }
    return true;
}

void lx_gameplay_end_frame(LxScene *s) {
    LxGameplay *g = s->gameplay;
    if (g && g->doomed_count) {
        bool was = s->in_update;
        s->in_update = false;
        for (int i = 0; i < g->doomed_count; i++) lx_entity_destroy(s, g->doomed[i]);
        g->doomed_count = 0;
        s->in_update = was;
    }
    if (g_save_dirty) lx_save_write();
}
