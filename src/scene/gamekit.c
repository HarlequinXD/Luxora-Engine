/* The game kit: building blocks for game logic, shared by the State Graph actions and scripts
 * (luxora.h, "Game kit"). Forward is +Z; an object's right is -X. */
#include "../lx_internal.h"
#include <stdio.h>
#include <time.h>

static LxVec3 flat3(LxVec3 v) { return lx_v3(v.x, 0, v.z); }
static LxEntity top_of(LxScene *s, LxEntity e) {
    for (int i = 0; e && lx_entity_parent(s, e) && i < 64; i++) e = lx_entity_parent(s, e);
    return e;
}
static bool tagged_up(LxScene *s, LxEntity e, const char *tag) {
    for (int i = 0; e && i < 64; i++, e = lx_entity_parent(s, e)) if (lx_entity_has_tag(s, e, tag)) return true;
    return false;
}
static float smooth_k(float speed, float dt) { return speed > 0 && dt > 0 ? lx_clampf(1 - expf(-speed * dt), 0, 1) : 1; }

/* ======================================================================
 *  looking, aiming, finding
 * ====================================================================== */
bool lx_pick_at(LxScene *s, LxVec2 canvas, float max_dist, LxRaycastHit *hit) {
    LxRaycastHit tmp;
    if (!hit) hit = &tmp;
    memset(hit, 0, sizeof *hit);
    LxVec3 o, d;
    if (!lx_ui_canvas_ray(s, canvas, &o, &d)) return false;
    if (!(max_dist > 0)) max_dist = 1000;
    if (lx_physics_raycast(s, o, d, max_dist, LX_ALL_LAYERS, hit)) return true;
    float t = 0;
    LxRay ray = {o, d};
    LxEntity e = lx_scene_raycast(s, ray, max_dist, &t);   /* objects without a collider */
    if (!e) return false;
    hit->entity = e;
    hit->point = lx_madd3(o, d, t);
    hit->normal = lx_scale3(d, -1);
    hit->distance = t;
    return true;
}
bool lx_mouse_pick(LxScene *s, float max_dist, LxRaycastHit *hit) {
    if (lx_ui_pointer_over()) { if (hit) memset(hit, 0, sizeof *hit); return false; }
    return lx_pick_at(s, lx_ui_pointer(), max_dist, hit);
}

float lx_entity_angle_to(LxScene *s, LxEntity e, LxVec3 point, bool flat) {
    if (!lx_entity_valid(s, e)) return 0;
    LxVec3 f = lx_entity_forward(s, e), to = lx_sub3(point, lx_entity_world_position(s, e));
    if (flat) {
        f = flat3(f); to = flat3(to);
        if (lx_len3sq(f) < 1e-10f || lx_len3sq(to) < 1e-10f) return 0;
        return -atan2f(lx_cross3(f, to).y, lx_dot3(f, to)) * LX_RAD2DEG;   /* + = to the right */
    }
    float lf = lx_len3(f), lt = lx_len3(to);
    return lf > 1e-6f && lt > 1e-6f ? acosf(lx_clampf(lx_dot3(f, to) / (lf * lt), -1, 1)) * LX_RAD2DEG : 0;
}
LxVec3 lx_entity_transform_point(LxScene *s, LxEntity e, LxVec3 p) { return lx_entity_valid(s, e) ? lx_mat4_point(lx_entity_world_matrix(s, e), p) : p; }
LxVec3 lx_entity_inverse_transform_point(LxScene *s, LxEntity e, LxVec3 p) { return lx_entity_valid(s, e) ? lx_mat4_point(lx_mat4_inverse(lx_entity_world_matrix(s, e)), p) : p; }
LxVec3 lx_entity_transform_direction(LxScene *s, LxEntity e, LxVec3 d) { return lx_entity_valid(s, e) ? lx_quat_rotate(lx_entity_world_rotation(s, e), d) : d; }
LxVec3 lx_entity_inverse_transform_direction(LxScene *s, LxEntity e, LxVec3 d) { return lx_entity_valid(s, e) ? lx_quat_rotate(lx_quat_conj(lx_entity_world_rotation(s, e)), d) : d; }

float lx_entity_turn_towards(LxScene *s, LxEntity e, LxVec3 d, LxVec3 up, bool keep_vertical, float speed, float dt) {
    if (!lx_entity_valid(s, e)) return 0;
    if (keep_vertical) d.y = 0;
    if (!(lx_len3sq(d) > 1e-10f) || !isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z)) return 0;
    if (!(lx_len3sq(up) > 1e-8f)) up = lx_v3(0, 1, 0);
    up = lx_norm3(up);
    LxVec3 f = lx_norm3(d);
    if (fabsf(lx_dot3(f, up)) > 0.999f) up = fabsf(f.x) < 0.9f ? lx_v3(1, 0, 0) : lx_v3(0, 0, 1);
    LxQuat want = lx_quat_look(f, up), cur = lx_entity_world_rotation(s, e);
    if (cur.x * want.x + cur.y * want.y + cur.z * want.z + cur.w * want.w < 0) { want.x = -want.x; want.y = -want.y; want.z = -want.z; want.w = -want.w; }
    float k = !(speed > 0) ? 1 : dt > 0 ? smooth_k(speed, dt) : 0;
    LxQuat r = {cur.x + (want.x - cur.x) * k, cur.y + (want.y - cur.y) * k, cur.z + (want.z - cur.z) * k, cur.w + (want.w - cur.w) * k};
    if (r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w < 1e-12f) r = want;
    r = lx_quat_norm(r);
    lx_entity_set_world_pose(s, e, lx_entity_world_position(s, e), r);
    float dot = fabsf(r.x * want.x + r.y * want.y + r.z * want.z + r.w * want.w);
    return 2 * acosf(lx_clampf(dot, -1, 1)) * LX_RAD2DEG;
}
void lx_entity_face_camera(LxScene *s, LxEntity e, bool upright) {
    LxEntity cam = lx_scene_main_camera(s);
    if (!cam || !lx_entity_valid(s, e)) return;
    /* face the same way as the camera (its back to the camera): text reads the right way round */
    LxVec3 f = lx_entity_forward(s, cam);
    if (upright) f.y = 0;
    if (lx_len3sq(f) < 1e-8f) return;
    lx_entity_set_world_pose(s, e, lx_entity_world_position(s, e), lx_quat_look(lx_norm3(f), lx_v3(0, 1, 0)));
}

bool lx_line_of_sight(LxScene *s, LxEntity from, LxEntity target, float range, float view_angle, float eye_height, float target_height) {
    if (!lx_entity_valid(s, from) || !lx_entity_valid(s, target)) return false;
    LxVec3 eye = lx_add3(lx_entity_world_position(s, from), lx_v3(0, eye_height, 0));
    LxVec3 at = lx_add3(lx_entity_world_position(s, target), lx_v3(0, target_height, 0));
    LxVec3 to = lx_sub3(at, eye);
    float d = lx_len3(to);
    if (d > range) return false;
    if (d < 1e-4f) return true;
    if (view_angle < 360) {
        LxVec3 f = flat3(lx_entity_forward(s, from)), t2 = flat3(to);
        if (lx_len3sq(f) > 1e-8f && lx_len3sq(t2) > 1e-8f && lx_dot3(lx_norm3(f), lx_norm3(t2)) < cosf(view_angle * 0.5f * LX_DEG2RAD)) return false;
    }
    LxRaycastHit hit;
    if (!lx_physics_raycast_ignore(s, eye, lx_scale3(to, 1 / d), d, LX_ALL_LAYERS, top_of(s, from), &hit)) return true;
    return top_of(s, hit.entity) == top_of(s, target);
}

LxEntity lx_find_closest_with_tag(LxScene *s, LxVec3 from, const char *tag, float max_dist, float *out_dist) {
    LxEntity best = 0;
    float bd = max_dist > 0 ? max_dist : 1e30f;
    int n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (!e || !lx_entity_has_tag(s, e, tag)) continue;
        float d = lx_len3(lx_sub3(lx_entity_world_position(s, e), from));
        if (d < bd) { bd = d; best = e; }
    }
    if (out_dist) *out_dist = best ? bd : 0;
    return best;
}
int lx_count_with_tag(LxScene *s, const char *tag) {
    int c = 0, n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) { LxEntity e = lx_scene_entity_at(s, i); if (e && lx_entity_has_tag(s, e, tag)) c++; }
    return c;
}
LxEntity lx_random_with_tag(LxScene *s, const char *tag) {
    int c = lx_count_with_tag(s, tag);
    if (!c) return 0;
    int pick = lx_random_int(0, c - 1), n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (e && lx_entity_has_tag(s, e, tag) && pick-- == 0) return e;
    }
    return 0;
}

/* ======================================================================
 *  combat
 * ====================================================================== */
static LxVec3 spread_dir(LxVec3 d, float deg) {
    d = lx_norm3(d);
    if (!(deg > 0)) return d;
    LxVec3 a = fabsf(d.y) < 0.9f ? lx_norm3(lx_cross3(d, lx_v3(0, 1, 0))) : lx_v3(1, 0, 0), b = lx_cross3(d, a);
    float ang = lx_random_range(0, 6.2832f), r = tanf(lx_random_range(0, 1) * deg * LX_DEG2RAD);
    return lx_norm3(lx_add3(d, lx_add3(lx_scale3(a, cosf(ang) * r), lx_scale3(b, sinf(ang) * r))));
}
bool lx_shoot_ray(LxScene *s, LxVec3 from, LxVec3 dir, float range, float spread_deg, float damage, const char *message,
                  const char *impact_prefab, LxEntity shooter, LxRaycastHit *hit) {
    LxRaycastHit tmp;
    if (!hit) hit = &tmp;
    memset(hit, 0, sizeof *hit);
    if (!(lx_len3sq(dir) > 1e-12f)) return false;
    dir = spread_dir(dir, spread_deg);
    if (!lx_physics_raycast_ignore(s, from, dir, range > 0 ? range : 100, LX_ALL_LAYERS, shooter ? top_of(s, shooter) : 0, hit)) return false;
    if (impact_prefab && impact_prefab[0] && lx_fsm_spawn_allowed(s)) lx_prefab_instantiate(s, impact_prefab, lx_madd3(hit->point, hit->normal, 0.03f), 0);
    if (damage != 0 && message && message[0]) lx_send_message(s, top_of(s, hit->entity), message, damage, shooter);
    return true;
}
int lx_area_damage(LxScene *s, LxVec3 center, float radius, float damage, bool falloff, const char *message, const char *tag, LxEntity sender) {
    if (!(radius > 0)) return 0;
    LxEntity found[256], done[256];
    int n = lx_physics_overlap_sphere(s, center, radius, LX_ALL_LAYERS, found, 256), nd = 0;
    LxEntity me = sender ? top_of(s, sender) : 0;
    for (int i = 0; i < n; i++) {
        LxEntity t = top_of(s, found[i]);
        if (!t || t == me) continue;
        bool dup = false;
        for (int k = 0; k < nd; k++) dup |= done[k] == t;
        if (dup || (tag && tag[0] && !tagged_up(s, found[i], tag))) continue;
        done[nd++] = t;
        float d = lx_len3(lx_sub3(lx_entity_world_position(s, t), center));
        float amount = falloff ? damage * lx_clampf(1 - d / radius, 0.1f, 1) : damage;
        lx_send_message(s, t, message && message[0] ? message : "Damage", amount, sender);
    }
    return nd;
}
int lx_projectile_update(LxScene *s, LxEntity e, LxProjectile *p, float speed, float gravity, float lifetime, float damage,
                         const char *message, const char *ignore_tag, const char *impact_prefab, float dt, LxRaycastHit *hit) {
    if (!p || p->done || !lx_entity_valid(s, e)) return 0;
    if (!p->started) { p->started = true; p->velocity = lx_scale3(lx_entity_forward(s, e), speed); }
    if (dt <= 0) return 0;
    p->age += dt;
    if (lifetime > 0 && p->age >= lifetime) { p->done = true; lx_entity_destroy(s, e); return 2; }
    p->velocity.y -= gravity * dt;
    LxVec3 pos = lx_entity_world_position(s, e), step = lx_scale3(p->velocity, dt);
    float len = lx_len3(step);
    if (len > 1e-6f) {
        LxVec3 dir = lx_scale3(step, 1 / len), from = pos;
        float left = len;
        for (int tries = 0; tries < 4; tries++) {   /* go through what's tagged to be ignored */
            LxRaycastHit h;
            if (!lx_physics_raycast_ignore(s, from, dir, left, LX_ALL_LAYERS, e, &h)) break;
            if (ignore_tag && ignore_tag[0] && tagged_up(s, h.entity, ignore_tag)) {
                from = lx_madd3(h.point, dir, 0.02f);
                left -= h.distance + 0.02f;
                if (left <= 0) break;
                continue;
            }
            if (hit) *hit = h;
            if (impact_prefab && impact_prefab[0] && lx_fsm_spawn_allowed(s)) lx_prefab_instantiate(s, impact_prefab, lx_madd3(h.point, h.normal, 0.03f), 0);
            if (damage != 0) lx_send_message(s, top_of(s, h.entity), message && message[0] ? message : "Damage", damage, e);
            p->done = true;
            lx_entity_destroy(s, e);
            return 1;
        }
    }
    LxVec3 to = lx_add3(pos, step);
    LxQuat rot = lx_len3sq(p->velocity) > 1e-8f ? lx_quat_look(lx_norm3(p->velocity), lx_v3(0, 1, 0)) : lx_entity_world_rotation(s, e);
    lx_entity_set_world_pose(s, e, to, rot);
    return 0;
}
LxEntity lx_fire_projectile(LxScene *s, const char *prefab, LxEntity from, LxVec3 offset, float spread_deg) {
    if (!prefab || !prefab[0] || !lx_entity_valid(s, from) || !lx_fsm_spawn_allowed(s)) return 0;
    LxVec3 at = lx_entity_transform_point(s, from, offset);
    LxEntity e = lx_prefab_instantiate(s, prefab, at, 0);
    if (!e) return 0;
    LxVec3 d = spread_dir(lx_entity_forward(s, from), spread_deg);
    lx_entity_set_world_pose(s, e, at, lx_quat_look(d, lx_v3(0, 1, 0)));
    return e;
}

/* ======================================================================
 *  moving
 * ====================================================================== */
bool lx_walk_to(LxScene *s, LxEntity e, LxVec3 point, float speed, float stop_distance, float turn_speed, float *vy, float dt) {
    if (!lx_entity_valid(s, e)) return false;
    LxVec3 pos = lx_entity_world_position(s, e), to = flat3(lx_sub3(point, pos));
    float d = lx_len3(to);
    bool there = d <= fmaxf(stop_distance, 0.01f);
    if (dt <= 0) return there;
    LxVec3 step = lx_v3(0, 0, 0);
    if (!there && speed > 0) step = lx_scale3(to, fminf(speed * dt, d - fmaxf(stop_distance, 0)) / d);
    if (!there && turn_speed >= 0) lx_entity_turn_towards(s, e, to, lx_v3(0, 1, 0), true, turn_speed, dt);
    if (lx_get_character(s, e)) {
        float fall = 0;
        if (vy) {
            if (lx_character_grounded(s, e) && *vy < 0) *vy = -2;
            *vy = fmaxf(*vy - 20 * dt, -40);
            fall = *vy * dt;
        }
        lx_character_move(s, e, lx_v3(step.x, fall, step.z));
    } else if (lx_len3sq(step) > 0) {
        LxEntity par = lx_entity_parent(s, e);
        LxVec3 world = lx_add3(pos, step);
        lx_entity_set_position(s, e, par ? lx_entity_inverse_transform_point(s, par, world) : world);
    }
    return lx_len3(flat3(lx_sub3(point, lx_entity_world_position(s, e)))) <= fmaxf(stop_distance, 0.01f) + 0.05f;
}

static int child_count(LxScene *s, LxEntity p) { int n = 0; for (LxEntity k = p ? lx_entity_first_child(s, p) : 0; k; k = lx_entity_next_sibling(s, k)) n++; return n; }
static LxEntity child_at(LxScene *s, LxEntity p, int i) {
    for (LxEntity k = p ? lx_entity_first_child(s, p) : 0; k; k = lx_entity_next_sibling(s, k)) if (i-- == 0) return k;
    return 0;
}
int lx_patrol_update(LxScene *s, LxEntity e, LxEntity waypoints, LxPatrol *p, float speed, float wait_seconds, LxPatrolMode mode, float dt) {
    int n = child_count(s, waypoints), flags = 0;
    if (!p || n == 0 || p->done || !lx_entity_valid(s, e)) return 0;
    if (p->step == 0) p->step = 1;
    if (p->index < 0 || p->index >= n) p->index = 0;
    if (p->wait > 0) { p->wait -= dt; lx_walk_to(s, e, lx_entity_world_position(s, e), 0, 1, -1, &p->vy, dt); return 0; }
    LxVec3 target = lx_entity_world_position(s, child_at(s, waypoints, p->index));
    if (lx_walk_to(s, e, target, speed, 0.25f, 8, &p->vy, dt)) {
        flags |= LX_PATROL_REACHED;
        p->wait = wait_seconds;
        int next = p->index + p->step;
        if (next >= n || next < 0) {
            if (mode == LX_PATROL_ONCE) { p->done = true; return flags | LX_PATROL_FINISHED; }
            if (mode == LX_PATROL_PING_PONG && n > 1) { p->step = -p->step; next = p->index + p->step; }
            else next = 0;
        }
        p->index = next;
    }
    return flags;
}
bool lx_wander_update(LxScene *s, LxEntity e, LxWander *w, float radius, float speed, float min_wait, float max_wait, float dt) {
    if (!w || !lx_entity_valid(s, e)) return false;
    if (!w->started) { w->started = true; w->home = lx_entity_world_position(s, e); w->wait = lx_random_range(0, fmaxf(max_wait, 0)); }
    if (!w->moving) {
        w->wait -= dt;
        lx_walk_to(s, e, lx_entity_world_position(s, e), 0, 1, -1, &w->vy, dt);   /* still falls */
        if (w->wait <= 0) {
            float a = lx_random_range(0, 6.2832f), r = lx_random_range(0, fmaxf(radius, 0));
            w->target = lx_add3(w->home, lx_v3(cosf(a) * r, 0, sinf(a) * r));
            w->moving = true;
        }
        return false;
    }
    bool there = lx_walk_to(s, e, w->target, speed, 0.3f, 6, &w->vy, dt);
    if (there) { w->moving = false; w->wait = lx_random_range(fminf(min_wait, max_wait), fmaxf(min_wait, max_wait)); }
    return there;
}

LxEntity lx_spawner_update(LxScene *s, LxSpawner *sp, const char *prefab, LxEntity at, float radius, float interval, int max_alive, int total, float dt) {
    if (!sp || !prefab || !prefab[0]) return 0;
    int keep = 0;   /* forget the ones that are gone */
    for (int i = 0; i < sp->alive_count; i++) if (lx_entity_valid(s, sp->alive[i])) sp->alive[keep++] = sp->alive[i];
    sp->alive_count = keep;
    sp->timer -= dt;
    if (sp->timer > 0) return 0;
    if ((total > 0 && sp->spawned >= total) || sp->alive_count >= (max_alive > 0 ? max_alive : 64) || sp->alive_count >= 64) return 0;
    if (!lx_fsm_spawn_allowed(s)) return 0;
    LxVec3 pos = at && lx_entity_valid(s, at) ? lx_entity_world_position(s, at) : lx_v3(0, 0, 0);
    LxQuat rot = at && lx_entity_valid(s, at) ? lx_entity_world_rotation(s, at) : lx_quat_identity();
    int kids = child_count(s, at);
    if (kids) {
        LxEntity k = child_at(s, at, lx_random_int(0, kids - 1));
        pos = lx_entity_world_position(s, k);
        rot = lx_entity_world_rotation(s, k);
    }
    if (radius > 0) { float a = lx_random_range(0, 6.2832f), r = sqrtf(lx_random_range(0, 1)) * radius; pos = lx_add3(pos, lx_v3(cosf(a) * r, 0, sinf(a) * r)); }
    LxEntity e = lx_prefab_instantiate(s, prefab, pos, 0);
    if (!e) return 0;
    lx_entity_set_world_pose(s, e, pos, rot);
    sp->alive[sp->alive_count++] = e;
    sp->spawned++;
    sp->timer = interval > 0 ? interval : 0;
    return e;
}

/* ======================================================================
 *  inventory
 * ====================================================================== */
#define INV_MAX 128
static struct { char name[48]; int count; } g_inv[INV_MAX];
static int g_inv_n;
static int inv_find(const char *item) {
    for (int i = 0; i < g_inv_n; i++) if (!strcmp(g_inv[i].name, item)) return i;
    return -1;
}
int lx_inventory_add(const char *item, int count) {
    if (!item || !item[0]) return 0;
    int i = inv_find(item);
    if (i < 0) {
        if (count <= 0 || g_inv_n >= INV_MAX) return 0;
        i = g_inv_n++;
        lx_strcpy(g_inv[i].name, sizeof g_inv[i].name, item);
        g_inv[i].count = 0;
    }
    long long c = (long long)g_inv[i].count + count;
    g_inv[i].count = c > 1000000000 ? 1000000000 : c < 0 ? 0 : (int)c;
    int now = g_inv[i].count;
    if (now == 0) { memmove(&g_inv[i], &g_inv[i + 1], sizeof g_inv[0] * (size_t)(g_inv_n - i - 1)); g_inv_n--; }
    return now;
}
bool lx_inventory_remove(const char *item, int count) {
    if (count <= 0) return true;
    if (lx_inventory_count(item) < count) return false;
    lx_inventory_add(item, -count);
    return true;
}
int lx_inventory_count(const char *item) { int i = item ? inv_find(item) : -1; return i >= 0 ? g_inv[i].count : 0; }
int lx_inventory_size(void) { return g_inv_n; }
const char *lx_inventory_item(int index, int *count) {
    if (index < 0 || index >= g_inv_n) { if (count) *count = 0; return ""; }
    if (count) *count = g_inv[index].count;
    return g_inv[index].name;
}
void lx_inventory_clear(void) { g_inv_n = 0; }
bool lx_inventory_save(const char *slot) {
    char key[96], text[4096];
    text[0] = 0;
    for (int i = 0; i < g_inv_n; i++) {
        size_t l = strlen(text);
        snprintf(text + l, sizeof text - l, "%s%s=%d", i ? ";" : "", g_inv[i].name, g_inv[i].count);
    }
    snprintf(key, sizeof key, "%s.inventory", slot && slot[0] ? slot : "game");
    lx_save_set_string(key, text);
    return true;   /* stored; the engine writes the save data out at the end of the frame */
}
bool lx_inventory_load(const char *slot) {
    char key[96];
    snprintf(key, sizeof key, "%s.inventory", slot && slot[0] ? slot : "game");
    if (!lx_save_has(key)) return false;
    char text[4096];
    lx_strcpy(text, sizeof text, lx_save_get_string(key, ""));
    lx_inventory_clear();
    for (char *p = text; p && *p;) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        char *eq = strrchr(p, '=');
        if (eq) { *eq = 0; lx_inventory_add(p, atoi(eq + 1)); }
        p = semi ? semi + 1 : NULL;
    }
    return true;
}

/* ======================================================================
 *  checkpoints, saving the globals
 * ====================================================================== */
void lx_checkpoint_set(LxScene *s, LxEntity e) {
    if (!lx_entity_valid(s, e)) return;
    LxVec3 p = lx_entity_world_position(s, e);
    lx_save_set_string("checkpoint.scene", lx_scene_name(s));
    lx_save_set_float("checkpoint.x", p.x);
    lx_save_set_float("checkpoint.y", p.y);
    lx_save_set_float("checkpoint.z", p.z);
    lx_save_set_float("checkpoint.yaw", lx_entity_euler(s, e).y);
}
bool lx_checkpoint_get(LxVec3 *pos, float *yaw, char *scene, size_t cap) {
    if (!lx_save_has("checkpoint.x")) return false;
    if (pos) *pos = lx_v3(lx_save_get_float("checkpoint.x", 0), lx_save_get_float("checkpoint.y", 0), lx_save_get_float("checkpoint.z", 0));
    if (yaw) *yaw = lx_save_get_float("checkpoint.yaw", 0);
    if (scene && cap) lx_strcpy(scene, cap, lx_save_get_string("checkpoint.scene", ""));
    return true;
}
bool lx_checkpoint_respawn(LxScene *s, LxEntity e) {
    LxVec3 p;
    float yaw;
    char scene[LX_NAME_MAX];
    if (!lx_entity_valid(s, e) || !lx_checkpoint_get(&p, &yaw, scene, sizeof scene) || strcmp(scene, lx_scene_name(s))) return false;
    lx_entity_set_world_pose(s, e, p, lx_quat_euler_deg(lx_v3(0, yaw, 0)));
    LxRigidbody *rb = lx_get_rigidbody(s, e);
    if (rb) rb->velocity = lx_v3(0, 0, 0);
    return true;
}
int lx_globals_save(const char *slot) {
    LxGlobals *g = lx_fsm_globals();
    int n = 0;
    for (int i = 0; g && i < g->var_count; i++) {
        LxFsmValue *v = lx_fsm_global_value(i);
        if (!v) continue;
        char key[160], text[LX_REF_MAX];
        snprintf(key, sizeof key, "%s.global.%s", slot && slot[0] ? slot : "game", g->vars[i].name);
        lx_fsm_value_format(v, text, sizeof text, NULL);
        lx_save_set_string(key, text);
        n++;
    }
    /* No write from here: setting the values marks the save data dirty and the engine writes it out at
       the end of the frame. Writing the whole file per call made saving every frame cost a full rewrite
       each time. Call lx_save_write() yourself if you need it on disk this instant. */
    return n;
}
int lx_globals_load(const char *slot) {
    LxGlobals *g = lx_fsm_globals();
    int n = 0;
    for (int i = 0; g && i < g->var_count; i++) {
        LxFsmValue *v = lx_fsm_global_value(i);
        char key[160];
        snprintf(key, sizeof key, "%s.global.%s", slot && slot[0] ? slot : "game", g->vars[i].name);
        if (!v || !lx_save_has(key)) continue;
        if (v->type == LX_FSM_ARRAY) {
            LxArray fresh = lx_array_from_text(lx_save_get_string(key, ""));
            if (fresh) { if (lx_array_valid(v->i)) lx_array_copy(v->i, fresh); lx_array_destroy(fresh); }
        } else {
            LxFsmType t = v->type;
            lx_fsm_value_parse(v, t, lx_save_get_string(key, ""), NULL);
        }
        n++;
    }
    return n;
}

/* ======================================================================
 *  time and the world
 * ====================================================================== */
void lx_date_time(int out[7]) {
    time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    out[0] = t.tm_year + 1900; out[1] = t.tm_mon + 1; out[2] = t.tm_mday;
    out[3] = t.tm_hour; out[4] = t.tm_min; out[5] = t.tm_sec; out[6] = t.tm_wday;
}
void lx_format_time(char *out, size_t cap, float seconds, int style) {
    if (!out || !cap) return;
    if (!isfinite(seconds)) seconds = 0;
    bool neg = seconds < 0;
    double s = fabs((double)seconds);
    long whole = (long)s;
    int cs = (int)((s - (double)whole) * 100);
    long h = whole / 3600, m = (whole / 60) % 60, sec = whole % 60;
    if (style == 1) snprintf(out, cap, "%s%ld:%02ld:%02ld", neg ? "-" : "", h, m, sec);
    else if (style == 2) snprintf(out, cap, "%s%ld:%02ld.%02d", neg ? "-" : "", whole / 60, sec, cs);
    else snprintf(out, cap, "%s%ld:%02ld", neg ? "-" : "", whole / 60, sec);
}
static LxVec3 mix3(LxVec3 a, LxVec3 b, float k) { return lx_lerp3(a, b, lx_clampf(k, 0, 1)); }
void lx_day_night_apply(LxScene *s, LxEntity sun, float hour) {
    hour = fmodf(fmodf(hour, 24) + 24, 24);
    float a = (hour - 6) / 12 * LX_PI;           /* 6: sunrise in the east, 12: overhead, 18: sunset */
    float elev = sinf(a);                        /* above the horizon: > 0 */
    float day = lx_clampf(elev * 3 + 0.3f, 0, 1), dusk = lx_clampf(1 - fabsf(elev) * 4, 0, 1);
    if (sun && lx_entity_valid(s, sun)) {
        /* the light shines from the sun's position on an arc east -> overhead -> west, tilted a little south */
        LxVec3 from = lx_norm3(lx_v3(cosf(a), elev, 0.35f));
        lx_entity_set_world_pose(s, sun, lx_entity_world_position(s, sun), lx_quat_look(lx_neg3(from), lx_v3(0, 1, 0)));
        LxLight *l = lx_get_light(s, sun);
        if (l) {
            l->intensity = 3.2f * powf(lx_clampf(elev, 0, 1), 0.5f);
            l->color = mix3(lx_v3(1, 0.95f, 0.88f), lx_v3(1, 0.5f, 0.25f), dusk);
        }
    }
    LxEnvironment *env = lx_scene_environment(s);
    if (!env) return;
    LxVec3 zen_day = lx_v3(0.22f, 0.36f, 0.62f), hor_day = lx_v3(0.75f, 0.72f, 0.68f);
    LxVec3 zen_night = lx_v3(0.01f, 0.015f, 0.04f), hor_night = lx_v3(0.03f, 0.035f, 0.07f);
    env->sky_zenith = mix3(zen_night, zen_day, day);
    env->sky_horizon = mix3(mix3(hor_night, hor_day, day), lx_v3(0.95f, 0.5f, 0.3f), dusk * 0.7f);
    env->fog_color = env->sky_horizon;
    env->ambient_intensity = 0.15f + 0.85f * day;
}

/* ======================================================================
 *  music and faded scene changes (run from the frame start)
 * ====================================================================== */
static struct {
    char clip[LX_REF_MAX];
    LxEntity now, old;
    float vol, now_vol, old_vol, fade;
    LxScene *scene;
} MU;
static struct { char ref[LX_REF_MAX]; float t, seconds; LxVec4 color; bool active; } FADE;

static LxEntity music_source(LxScene *s, const char *clip, float volume) {
    LxEntity e = lx_entity_create(s, "Music");
    LxAudioSource *a = lx_add_audio_source(s, e, clip);
    if (a) { a->loop = true; a->spatial = false; a->bus = LX_BUS_MUSIC; a->volume = 0; a->play_on_start = false; }
    lx_audio_play(s, e);
    (void)volume;
    return e;
}
void lx_music_play(LxScene *s, const char *clip, float volume, float fade_seconds) {
    if (!s || !clip) return;
    if (MU.scene != s) memset(&MU, 0, sizeof MU);   /* another scene: the old music went with it */
    MU.scene = s;
    if (!strcmp(MU.clip, clip) && MU.now && lx_entity_valid(s, MU.now)) { MU.vol = volume; return; }
    if (MU.old && lx_entity_valid(s, MU.old)) lx_entity_destroy(s, MU.old);
    MU.old = MU.now;
    MU.old_vol = MU.now_vol;
    MU.now = clip[0] ? music_source(s, clip, volume) : 0;
    MU.now_vol = 0;
    MU.vol = volume;
    MU.fade = fade_seconds > 0 ? fade_seconds : 0.001f;
    lx_strcpy(MU.clip, sizeof MU.clip, clip);
}
void lx_music_stop(float fade_seconds) {
    if (!MU.scene) return;
    lx_music_play(MU.scene, "", 0, fade_seconds);
}
/* A scene is being destroyed. The music keeps the scene it plays in, so it can cross-fade one track
 * into the next, and lx_music_stop goes back through that pointer - which would reach into a freed
 * scene after a scene change. Anything else here that holds a scene must let go in the same way. */
void lx_gamekit_scene_gone(LxScene *s) {
    if (MU.scene == s) memset(&MU, 0, sizeof MU);
}
const char *lx_music_current(void) { return MU.clip; }
void lx_load_scene_faded(const char *ref, float seconds, LxVec4 color) {
    if (!ref || !ref[0]) return;
    lx_strcpy(FADE.ref, sizeof FADE.ref, ref);
    FADE.t = 0;
    FADE.seconds = seconds > 0 ? seconds : 0.01f;
    FADE.color = color;
    FADE.active = true;
}
void lx_gamekit_frame(LxScene *s, float dt, float real_dt) {
    (void)dt;
    if (MU.scene == s) {
        float step = real_dt / (MU.fade > 0 ? MU.fade : 0.001f);
        MU.now_vol = fminf(MU.now_vol + step * MU.vol, MU.vol);
        MU.old_vol = fmaxf(MU.old_vol - step * fmaxf(MU.vol, 0.01f), 0);
        LxAudioSource *a = MU.now && lx_entity_valid(s, MU.now) ? lx_get_audio_source(s, MU.now) : NULL;
        if (a) a->volume = MU.now_vol;
        if (MU.old && lx_entity_valid(s, MU.old)) {
            LxAudioSource *o = lx_get_audio_source(s, MU.old);
            if (o) o->volume = MU.old_vol;
            if (MU.old_vol <= 0) { lx_entity_destroy(s, MU.old); MU.old = 0; }
        }
    }
    if (FADE.active) {
        FADE.t += real_dt;
        float k = lx_clampf(FADE.t / FADE.seconds, 0, 1);
        LxVec2 cs = lx_ui_canvas_size();
        LxVec4 c = FADE.color;
        lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
        lx_ui_rect(0, 0, cs.x, cs.y, LX_RGBA((int)(lx_clampf(c.x, 0, 1) * 255), (int)(lx_clampf(c.y, 0, 1) * 255), (int)(lx_clampf(c.z, 0, 1) * 255), (int)(k * 255)), 0);
        if (k >= 1) {
            FADE.active = false;
            lx_load_scene(FADE.ref);
            lx_ui_flash(lx_v4(c.x, c.y, c.z, 1), FADE.seconds);   /* and back in on the other side */
        }
    }
}

/* ======================================================================
 *  game UI helpers
 * ====================================================================== */
void lx_ui_bar_over(LxScene *s, LxVec3 world, float value01, float width, float height, uint32_t fill, uint32_t back) {
    LxVec2 p;
    if (!lx_ui_world_to_canvas(s, world, &p)) return;
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    lx_ui_bar(p.x - width * 0.5f, p.y - height * 0.5f, width, height, lx_clampf(value01, 0, 1), fill, back);
}
void lx_ui_marker(LxScene *s, LxVec3 world, const char *label, uint32_t color, bool clamp_to_edge, bool show_distance) {
    LxVec2 cs = lx_ui_canvas_size(), p;
    bool front = lx_ui_world_to_canvas(s, world, &p);
    LxEntity cam = lx_scene_main_camera(s);
    if (!cam) return;
    float m = 60;
    bool inside = front && p.x >= m && p.y >= m && p.x <= cs.x - m && p.y <= cs.y - m;
    if (!inside) {
        if (!clamp_to_edge) return;
        /* off screen: point from the middle towards it, on the edge */
        LxVec3 local = lx_entity_inverse_transform_direction(s, cam, lx_sub3(world, lx_entity_world_position(s, cam)));
        LxVec2 dir = lx_v2(-local.x, -local.y);   /* camera right is -X; canvas y grows down */
        float l = sqrtf(dir.x * dir.x + dir.y * dir.y);
        if (l < 1e-5f) dir = lx_v2(0, 1), l = 1;
        dir.x /= l; dir.y /= l;
        float sx = dir.x != 0 ? (cs.x * 0.5f - m) / fabsf(dir.x) : 1e9f, sy = dir.y != 0 ? (cs.y * 0.5f - m) / fabsf(dir.y) : 1e9f, t = fminf(sx, sy);
        p = lx_v2(cs.x * 0.5f + dir.x * t, cs.y * 0.5f + dir.y * t);
    }
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    lx_ui_circle(p.x, p.y, 16, LX_RGBA(0, 0, 0, 120));
    lx_ui_circle(p.x, p.y, 11, color);
    char text[96];
    text[0] = 0;
    if (label && label[0]) lx_strcpy(text, sizeof text, label);
    if (show_distance) {
        float d = lx_len3(lx_sub3(world, lx_entity_world_position(s, cam)));
        size_t l = strlen(text);
        snprintf(text + l, sizeof text - l, "%s%d m", l ? "  " : "", (int)roundf(d));
    }
    if (text[0]) {
        lx_ui_font(LX_FONT_BOLD);
        float w = lx_ui_text_width(26, text);
        lx_ui_text(p.x - w * 0.5f + 1, p.y + 22 + 1, 26, LX_RGBA(0, 0, 0, 160), "%s", text);
        lx_ui_text(p.x - w * 0.5f, p.y + 22, 26, color, "%s", text);
        lx_ui_font(LX_FONT_REGULAR);
    }
}
static const char *interact_key(void) {
    switch (lx_input_last_device()) {
    case LX_DEVICE_GAMEPAD: return "X";
    case LX_DEVICE_TOUCH: return "USE";
    default: return "E";
    }
}
void lx_ui_prompt(const char *text) {
    if (!text || !text[0]) return;
    LxVec2 cs = lx_ui_canvas_size();
    char line[160];
    snprintf(line, sizeof line, "[%s]  %s", interact_key(), text);
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    lx_ui_font(LX_FONT_BOLD);
    float w = lx_ui_text_width(36, line);
    lx_ui_rect(cs.x * 0.5f - w * 0.5f - 24, cs.y * 0.66f - 12, w + 48, 60, LX_RGBA(0, 0, 0, 150), 30);
    lx_ui_text(cs.x * 0.5f - w * 0.5f, cs.y * 0.66f, 36, LX_COLOR_WHITE, "%s", line);
    lx_ui_font(LX_FONT_REGULAR);
}
static bool advance_pressed(void) {
    return lx_action_pressed("Interact") || lx_action_pressed("Jump") || lx_key_pressed(LX_KEY_ENTER) || lx_mouse_pressed(LX_MOUSE_LEFT) ||
           lx_touch_phase(0) == LX_TOUCH_BEGAN;
}
/* word wrap: draws lines no wider than `width`, `shown` characters of the text in all */
static void wrapped(float x, float y, float width, float size, uint32_t color, const char *text, size_t len) {
    char line[512];
    size_t start = 0;
    float lh = size * 1.3f;
    while (start < len) {
        size_t end = start, last_space = 0;
        bool broke = false;
        while (end < len && text[end] != '\n') {
            if (text[end] == ' ') last_space = end;
            size_t n = end - start + 1 < sizeof line ? end - start + 1 : sizeof line - 1;
            memcpy(line, text + start, n);
            line[n] = 0;
            if (lx_ui_text_width(size, line) > width && last_space > start) { end = last_space; broke = true; break; }
            end++;
        }
        size_t n = end - start < sizeof line ? end - start : sizeof line - 1;
        memcpy(line, text + start, n);
        line[n] = 0;
        lx_ui_text(x, y, size, color, "%s", line);
        y += lh;
        start = end + ((broke || (end < len && text[end] == '\n')) ? 1 : 0);
    }
}
bool lx_ui_dialogue(LxDialogue *d, const char *speaker, const char *text, float cps, float dt) {
    if (!d || !text || d->done) return false;
    bool first = !d->started;
    d->started = true;
    /* the current page */
    const char *page = text;
    for (int i = 0; i < d->page && page; i++) { page = strchr(page, '|'); if (page) page++; }
    if (!page) { d->done = true; return true; }
    size_t len = strcspn(page, "|");
    d->t += dt > 0 ? dt : 0;
    size_t shown = cps > 0 ? (size_t)(d->t * cps) : len;
    if (shown > len) shown = len;
    while (shown < len && ((unsigned char)page[shown] & 0xC0) == 0x80) shown++;   /* whole UTF-8 characters */
    if (!first && advance_pressed()) {
        if (shown < len) { d->t = 1e6f; shown = len; }
        else {
            d->page++;
            d->t = 0;
            if (!strchr(page, '|')) { d->done = true; return true; }
        }
    }
    LxVec2 cs = lx_ui_canvas_size();
    float w = fminf(cs.x - 120, 1400), h = 250, x = (cs.x - w) * 0.5f, y = cs.y - h - 50;
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    lx_ui_rect(x, y, w, h, LX_RGBA(12, 12, 18, 225), 18);
    lx_ui_rect_outline(x, y, w, h, LX_RGBA(255, 255, 255, 60), 18, 2);
    if (speaker && speaker[0]) {
        lx_ui_font(LX_FONT_BOLD);
        float sw = lx_ui_text_width(34, speaker);
        lx_ui_rect(x + 30, y - 26, sw + 40, 52, LX_RGBA(255, 196, 90, 255), 12);
        lx_ui_text(x + 50, y - 16, 34, LX_RGBA(30, 20, 10, 255), "%s", speaker);
    }
    lx_ui_font(LX_FONT_REGULAR);
    wrapped(x + 44, y + 50, w - 88, 38, LX_COLOR_WHITE, page, shown);
    if (shown >= len && fmodf((float)lx_time(), 1.0f) < 0.6f) {   /* "go on" */
        lx_ui_font(LX_FONT_BOLD);
        lx_ui_text(x + w - 70, y + h - 60, 36, LX_RGBA(255, 196, 90, 255), "%s", strchr(page, '|') ? ">" : "x");
    }
    lx_ui_font(LX_FONT_REGULAR);
    return false;
}
int lx_ui_choice(LxChoice *c, const char *title, const char *const *options, int count) {
    if (!c || !options || count <= 0) return -1;
    if (count > 8) count = 8;
    bool first = !c->started;
    c->started = true;
    if (c->selected < 0 || c->selected >= count) c->selected = 0;
    /* keys and stick: one step per push */
    float stick = lx_axis("MoveY");
    int step = 0;
    if (lx_key_pressed(LX_KEY_UP) || lx_key_pressed(LX_KEY_W)) step = -1;
    if (lx_key_pressed(LX_KEY_DOWN) || lx_key_pressed(LX_KEY_S)) step = 1;
    if (!step && fabsf(stick) > 0.6f && fabsf(c->stick) <= 0.6f && !lx_key_down(LX_KEY_W) && !lx_key_down(LX_KEY_S) && !lx_key_down(LX_KEY_UP) && !lx_key_down(LX_KEY_DOWN))
        step = stick > 0 ? -1 : 1;
    c->stick = stick;
    c->selected = (c->selected + step + count) % count;
    LxVec2 cs = lx_ui_canvas_size();
    float bw = 620, bh = 76, gap = 14, top = cs.y * 0.5f - (count * (bh + gap)) * 0.5f + (title && title[0] ? 40 : 0);
    lx_ui_anchor(LX_ANCHOR_TOP_LEFT);
    float panel_top = top - (title && title[0] ? 110 : 40);
    lx_ui_rect(cs.x * 0.5f - bw * 0.5f - 40, panel_top, bw + 80, top - panel_top + count * (bh + gap) + 30, LX_RGBA(10, 10, 16, 215), 22);
    if (title && title[0]) {
        lx_ui_font(LX_FONT_BOLD);
        float tw = lx_ui_text_width(46, title);
        lx_ui_text(cs.x * 0.5f - tw * 0.5f, panel_top + 26, 46, LX_COLOR_WHITE, "%s", title);
    }
    LxVec2 ptr = lx_ui_pointer();
    int picked = -1;
    for (int i = 0; i < count; i++) {
        float x = cs.x * 0.5f - bw * 0.5f, y = top + (float)i * (bh + gap);
        bool hover = ptr.x >= x && ptr.x <= x + bw && ptr.y >= y && ptr.y <= y + bh;
        if (hover && (lx_mouse_delta().x != 0 || lx_mouse_delta().y != 0)) c->selected = i;
        bool sel = c->selected == i;
        lx_ui_rect(x, y, bw, bh, sel ? LX_RGBA(255, 196, 90, 255) : LX_RGBA(255, 255, 255, 28), 14);
        lx_ui_font(sel ? LX_FONT_BOLD : LX_FONT_REGULAR);
        float tw = lx_ui_text_width(36, options[i] ? options[i] : "");
        lx_ui_text(cs.x * 0.5f - tw * 0.5f, y + 18, 36, sel ? LX_RGBA(30, 20, 10, 255) : LX_COLOR_WHITE, "%s", options[i] ? options[i] : "");
        if (!first && hover && lx_mouse_pressed(LX_MOUSE_LEFT)) picked = i;
    }
    lx_ui_font(LX_FONT_REGULAR);
    if (picked < 0 && !first && (lx_action_pressed("Interact") || lx_action_pressed("Jump") || lx_key_pressed(LX_KEY_ENTER))) picked = c->selected;
    if (picked >= 0) c->started = false;   /* ready for next time */
    return picked;
}
bool lx_interactable_update(LxScene *s, LxEntity e, LxEntity player, float distance, bool need_facing, const char *prompt) {
    if (!lx_entity_valid(s, e) || !lx_entity_valid(s, player)) return false;
    LxVec3 to = flat3(lx_sub3(lx_entity_world_position(s, e), lx_entity_world_position(s, player)));
    float d = lx_len3(to);
    if (d > distance) return false;
    if (need_facing && d > 0.3f) {
        LxVec3 f = flat3(lx_entity_forward(s, player));
        if (lx_len3sq(f) > 1e-8f && lx_dot3(lx_norm3(f), lx_scale3(to, 1 / d)) < 0.35f) return false;
    }
    lx_ui_prompt(prompt && prompt[0] ? prompt : "Use");
    return lx_action_pressed("Interact");
}
