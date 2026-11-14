/* Stress tester (--stress FRAMES [SEED]): a "monkey" that uses the editor hard and at random -
 * clicks buttons, menus, checkboxes and dropdowns, types into fields (including nonsense), drags
 * things onto every drop target, creates/deletes/duplicates/reparents objects, undoes and redoes,
 * plays and stops (also mid-frame), saves and reopens - and after every step checks that the editor's
 * state still makes sense. Any broken rule is logged as "[stress] BROKEN: ..." and makes the exit
 * code non-zero; crashes are caught by the sanitizer builds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "editor.h"

bool ed_stress;
int ed_stress_failures;
static uint64_t g_rng = 0x9e3779b97f4a7c15ull;
static long g_actions;
static char g_last_action[256];

#define BROKEN(...) do { ed_stress_failures++; LX_ERROR("[stress] BROKEN (after: %s): " , g_last_action); LX_ERROR(__VA_ARGS__); } while (0)

static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
static int rnd_n(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }
static float rnd_f(float a, float b) { return a + (b - a) * (float)(rnd() & 0xffffff) / (float)0xffffff; }
static bool chance(int percent) { return rnd_n(100) < percent; }

static void act(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_action, sizeof g_last_action, fmt, ap);
    va_end(ap);
    g_actions++;
    if (getenv("LUXORA_STRESS_VERBOSE")) LX_INFO("[stress] #%ld %s", g_actions, g_last_action);
}

/* ======================================================================
 *  invariants
 * ====================================================================== */
static void check_scene(LxScene *s, const char *which) {
    if (!s) return;
    int n = lx_scene_entity_count(s);
    int reached = 0;
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (!lx_entity_valid(s, e)) { BROKEN("%s: entity_at(%d) is not valid", which, i); continue; }
        LxEntity p = lx_entity_parent(s, e);
        if (p && !lx_entity_valid(s, p)) BROKEN("%s: '%s' has a dead parent", which, lx_entity_name(s, e));
        int depth = 0;
        for (LxEntity q = p; q && depth < 5000; q = lx_entity_parent(s, q)) {
            depth++;
            if (q == e) { BROKEN("%s: '%s' is its own ancestor", which, lx_entity_name(s, e)); break; }
        }
        if (depth >= 5000) BROKEN("%s: parent chain of '%s' doesn't end", which, lx_entity_name(s, e));
        int kids = 0;
        for (LxEntity c = lx_entity_first_child(s, e); c; c = lx_entity_next_sibling(s, c)) {
            if (++kids > n) { BROKEN("%s: child list of '%s' loops", which, lx_entity_name(s, e)); break; }
            if (lx_entity_parent(s, c) != e) BROKEN("%s: '%s' is listed under '%s' but its parent differs", which, lx_entity_name(s, c), lx_entity_name(s, e));
        }
        if (!p) reached++;
        LxVec3 wp = lx_entity_world_position(s, e);
        if (!(wp.x == wp.x) || !(wp.y == wp.y) || !(wp.z == wp.z)) BROKEN("%s: '%s' has a NaN position", which, lx_entity_name(s, e));
    }
    /* every object is reachable from the roots */
    int total = 0;
    LxEntity stack[4096];
    int sp = 0;
    for (int i = 0; i < n && sp < 4096; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        if (lx_entity_valid(s, e) && !lx_entity_parent(s, e)) stack[sp++] = e;
    }
    while (sp) {
        LxEntity e = stack[--sp];
        total++;
        if (total > n + 1) break;
        for (LxEntity c = lx_entity_first_child(s, e); c && sp < 4096; c = lx_entity_next_sibling(s, c)) stack[sp++] = c;
    }
    if (total != n && n < 4000) BROKEN("%s: %d objects but %d reachable from the roots", which, n, total);
}

static void check_editor(void) {
    if (ED.mode != ED_MODE_EDITOR || !ED.scene) return;
    if (ED.selected && !lx_entity_valid(ED.scene, ED.selected)) BROKEN("the selection points at a deleted object");
    check_scene(ED.scene, "scene");
    if (ED.play == ED_EDIT && ED.play_snapshot) BROKEN("a play-mode snapshot is left over in edit mode");
    if (ED.undo_count < 0 || ED.undo_count > ED_MAX_UNDO || ED.redo_count < 0 || ED.redo_count > ED_MAX_UNDO) BROKEN("undo stack out of range");
}

/* text round trip: what's saved loads back the same */
static void check_roundtrip(void) {
    if (!ED.scene) return;
    char *a = lx_scene_serialize(ED.scene);
    LxScene *s2 = lx_scene_deserialize(a);
    if (!s2) { BROKEN("a saved scene doesn't load back"); lx_free(a); return; }
    check_scene(s2, "reloaded scene");
    char *b = lx_scene_serialize(s2);
    if (strcmp(a, b)) {
        size_t i = 0;
        while (a[i] && a[i] == b[i]) i++;
        size_t st = i > 80 ? i - 80 : 0;
        BROKEN("save -> load -> save changes the scene near: ...%.160s... vs ...%.160s...", a + st, b + st);
    }
    lx_scene_destroy(s2);
    lx_free(a);
    lx_free(b);
}

/* ======================================================================
 *  helpers
 * ====================================================================== */
static LxEntity random_entity(void) {
    int n = ED.scene ? lx_scene_entity_count(ED.scene) : 0;
    return n ? lx_scene_entity_at(ED.scene, rnd_n(n)) : 0;
}

#define MAX_ASSETS 512
static char g_assets[MAX_ASSETS][LX_REF_MAX];
static char g_asset_type[MAX_ASSETS][16];
static int g_asset_count;
static const char *type_of(const char *name, bool dir) {
    if (dir) return "folder";
    const char *x = lx_path_ext(name);
    if (!strcmp(x, ".lxmat")) return "material";
    if (!strcmp(x, ".lxprefab")) return "prefab";
    if (!strcmp(x, ".fbx") || !strcmp(x, ".FBX")) return "model";
    if (!strcmp(x, ".lxscene")) return "scene";
    if (!strcmp(x, ".png") || !strcmp(x, ".jpg") || !strcmp(x, ".tga")) return "texture";
    if (!strcmp(x, ".wav") || !strcmp(x, ".ogg")) return "sound";
    if (!strcmp(x, ".obj") || !strcmp(x, ".lxmesh")) return "mesh";
    if (!strcmp(x, ".c")) return "script";
    return "file";
}
static void scan(const char *dir) {
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n && g_asset_count < MAX_ASSETS; i++) {
        char full[LX_PATH_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        ed_relative_asset(g_assets[g_asset_count], LX_REF_MAX, full);
        lx_strcpy(g_asset_type[g_asset_count], 16, type_of(e[i].name, e[i].is_dir));
        g_asset_count++;
        if (e[i].is_dir) scan(full);
    }
    lx_free(e);
}
static void rescan(void) {
    g_asset_count = 0;
    if (ED.has_project) scan(ED.project.assets);
}
static const char *random_asset(const char *type) {
    int idx[MAX_ASSETS], k = 0;
    for (int i = 0; i < g_asset_count; i++) if (!type || !strcmp(g_asset_type[i], type)) idx[k++] = i;
    return k ? g_assets[idx[rnd_n(k)]] : NULL;
}

/* interactive nodes of the topmost layer: an open menu, else a dialog, else the editor */
#define MAX_NODES 4096
static UiNode *g_nodes[MAX_NODES];
static int g_node_count;
static bool visible(UiNode *n) {
    for (UiNode *p = n; p; p = p->parent) if (p->hidden) return false;
    return true;
}
static void collect(UiNode *n) {
    if (!n || n->hidden || g_node_count >= MAX_NODES) return;
    bool interactive = !n->disabled &&
        (n->on_click || n->on_double_click || n->on_right_click || n->on_drop || n->type == UI_CHECKBOX || n->type == UI_DROPDOWN ||
         n->type == UI_NUMBER || n->type == UI_SLIDER || (n->type == UI_TEXTFIELD && n->s) || n->type == UI_COLOR || n->type == UI_TABS);
    if (interactive) g_nodes[g_node_count++] = n;
    for (UiNode *c = n->first; c; c = c->next) collect(c);
}
static void collect_layer(void) {
    g_node_count = 0;
    UiNode *pop[16];
    int np = ui_popup_nodes(ED.ui, pop, 16);
    if (np) { collect(pop[np - 1]); return; }
    UiNode *m = ui_modal_node(ED.ui);
    if (m) { collect(m); return; }
    collect(ui_root(ED.ui));
}

/* words people type, and things they shouldn't */
static const char *WORDS[] = {"Player", "", " ", "a", "Enemy 2", "../../etc", "C:\\Windows", "#mesh0", "name with spaces  ", "ÄÖÜ ñ 日本",
                              "%s%s%n", "very long name that keeps going and going and going and going and going and going and going and going",
                              "0", "-1", "1e9", "nan", "inf", "-0", "3.14159", "100000000000", "Models/x.fbx#9", "builtin:cube", "\"quoted\"",
                              "line\nbreak", "=", "[section]", "key = value"};
static const char *random_text(void) { return WORDS[rnd_n((int)(sizeof WORDS / sizeof WORDS[0]))]; }

static const char *node_label(UiNode *n) {
    static char buf[200];
    snprintf(buf, sizeof buf, "%s%s%s", n->text[0] ? n->text : n->tooltip[0] ? n->tooltip : "?", n->path[0] ? " " : "", n->path);
    return buf;
}

/* clicks a real user could do that shouldn't happen in an automated run */
static bool forbidden(UiNode *n) {
    const char *t = n->text[0] ? n->text : n->tooltip;
    static const char *NO[] = {"Exit", "Quit", "Build", "Build and Run", "Build & Run", "Open Folder", "Show in", "Open in", "Report", "Website",
                               "Documentation", "Close Window", "Delete Project", "Remove from list"};
    for (size_t i = 0; i < sizeof NO / sizeof NO[0]; i++) if (strstr(t, NO[i])) return true;
    if (n->icon == UI_ICON_WIN_CLOSE || n->icon == UI_ICON_WIN_MIN || n->icon == UI_ICON_WIN_MAX || n->icon == UI_ICON_BUILD || n->icon == UI_ICON_EXTERNAL) return true;
    return false;
}

/* ======================================================================
 *  actions
 * ====================================================================== */
static void ui_monkey(void) {
    collect_layer();
    if (!g_node_count) { ui_close_popups(ED.ui); return; }
    UiNode *n = g_nodes[rnd_n(g_node_count)];
    if (forbidden(n) || !visible(n)) return;
    const char *what = node_label(n);
    char label[220];
    lx_strcpy(label, sizeof label, what);
    if (n->on_drop && n->accept && chance(30)) {
        /* drag something onto it */
        static const char *TYPES[] = {"entity", "mesh", "material", "prefab", "model", "texture", "sound", "scene", "script", "file", "folder"};
        const char *type = TYPES[rnd_n(11)];
        char payload[LX_REF_MAX];
        if (!strcmp(type, "entity")) snprintf(payload, sizeof payload, "%u", random_entity());
        else {
            const char *a = random_asset(type);
            if (!a) a = chance(50) ? random_text() : "builtin:cube";
            lx_strcpy(payload, sizeof payload, a);
        }
        act("drop %s '%s' on [%s]", type, payload, label);
        ui_test_drop(ED.ui, n, type, payload);
        return;
    }
    switch (n->type) {
    case UI_TEXTFIELD: {
        const char *t = random_text();
        act("type '%s' into [%s]", t, label);
        ui_test_type(ED.ui, n, t);
        return;
    }
    case UI_NUMBER: {
        char v[32];
        if (chance(70)) snprintf(v, sizeof v, "%g", rnd_f(-1000, 1000));
        else lx_strcpy(v, sizeof v, random_text());
        act("type number '%s' into [%s]", v, label);
        ui_test_type(ED.ui, n, v);
        return;
    }
    case UI_SLIDER:
        act("drag slider [%s]", label);
        if (n->f) *n->f = rnd_f(n->minv, n->maxv);
        else if (n->i) *n->i = (int)rnd_f(n->minv, n->maxv);
        if (n->on_change) n->on_change(n);
        return;
    default: break;
    }
    if (n->on_right_click && chance(25)) {
        act("right-click [%s]", label);
        ui_test_right_click(ED.ui, n);
        return;
    }
    bool dbl = n->on_double_click && chance(30);
    act("%s [%s]", dbl ? "double-click" : "click", label);
    ui_test_click(ED.ui, n, dbl);
}

static const char *CREATE[] = {"Empty", "Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Camera", "Directional Light", "Point Light",
                               "Spot Light", "Reflection Probe", "Audio Source", "Crowd", "Physics Cube", "Player"};

static void editor_action(void) {
    if (ED.mode != ED_MODE_EDITOR || !ED.scene) return;
    switch (rnd_n(22)) {
    case 0: case 1: {
        const char *w = CREATE[rnd_n((int)(sizeof CREATE / sizeof CREATE[0]))];
        act("create %s", w);
        LxEntity e = ed_create(w);
        if (e && chance(40)) { LxEntity p = random_entity(); if (p && p != e) lx_entity_set_parent(ED.scene, e, p); }
        break;
    }
    case 2: act("select a random object"); ed_select(random_entity()); break;
    case 3: act("delete selected"); ed_delete_selected(); break;
    case 4: act("duplicate selected"); ed_duplicate_selected(); break;
    case 5: case 6: act("undo"); ed_undo(); break;
    case 7: act("redo"); ed_redo(); break;
    case 8: act("focus selected"); ed_focus_selected(); break;
    case 9:
        if (ED.play == ED_EDIT) { act("play"); ed_play(); }
        else if (chance(50)) { act("stop"); ed_stop(); }
        else { act("pause"); ed_pause(); }
        break;
    case 10: act("step"); ed_step(); break;
    case 11: {
        const char *a = random_asset(NULL);
        if (a) {
            char p[LX_PATH_MAX];
            lx_asset_path(p, sizeof p, a);
            act("select asset %s", a);
            ed_select_asset(p);
        }
        break;
    }
    case 12: act("select nothing"); ed_select(0); ed_select_asset(""); break;
    case 13:
        if (ED.play == ED_EDIT && ED.selected && chance(50)) {
            char dir[LX_PATH_MAX];
            lx_path_join(dir, sizeof dir, ED.project.assets, "Prefabs");
            act("save selected as a prefab");
            ed_prefab_create(ED.selected, dir);
            rescan();
        } else if (ED.selected) { act("apply prefab"); ed_prefab_apply(ED.selected); }
        break;
    case 14: {
        const char *p = random_asset("prefab");
        if (p) { act("place prefab %s", p); ed_prefab_place(p, ed_spawn_point(), chance(30) ? random_entity() : 0); }
        break;
    }
    case 15: {
        const char *m = random_asset("model");
        if (m) { act("place model %s", m); ed_model_place(m, ed_spawn_point(), chance(20) ? random_entity() : 0); }
        break;
    }
    case 16: {
        LxEntity a = random_entity(), b = chance(20) ? 0 : random_entity();
        if (a) {
            act("reparent '%s' under '%s'", lx_entity_name(ED.scene, a), b ? lx_entity_name(ED.scene, b) : "(root)");
            /* through the hierarchy's own drop handler, like dragging a row */
            char payload[32];
            snprintf(payload, sizeof payload, "%u", a);
            if (b) {
                ED.selected = b;
                ED.hierarchy_hash = 0;
            }
            collect(ED.hierarchy_list);
            if (ED.hierarchy_list) ui_test_drop(ED.ui, ED.hierarchy_list, "entity", payload);
        }
        break;
    }
    case 17:
        if (ED.play == ED_EDIT) { act("save scene"); ed_save_scene(); rescan(); }
        break;
    case 18: {
        const char *sc = random_asset("scene");
        if (sc && ED.play == ED_EDIT) { act("open scene %s", sc); ED.dirty = false; ed_open_scene(sc); }
        break;
    }
    case 19: {
        /* add a random component through the real menu */
        UiNode *n = NULL;
        g_node_count = 0;
        collect(ED.inspector);
        for (int i = 0; i < g_node_count; i++) if (!strcmp(g_nodes[i]->text, "Add Component")) n = g_nodes[i];
        if (n) { act("Add Component"); ui_test_click(ED.ui, n, false); }
        break;
    }
    case 20:
        act("camera move");
        ED.cam_pos = lx_v3(rnd_f(-20, 20), rnd_f(0, 10), rnd_f(-20, 20));
        ED.cam_yaw = rnd_f(-180, 180);
        ED.cam_pitch = rnd_f(-80, 80);
        break;
    case 21:
        if (ED.selected) {
            act("move/rotate/scale selected");
            lx_entity_set_position(ED.scene, ED.selected, lx_v3(rnd_f(-10, 10), rnd_f(-2, 5), rnd_f(-10, 10)));
            lx_entity_set_euler(ED.scene, ED.selected, lx_v3(rnd_f(-180, 180), rnd_f(-180, 180), rnd_f(-180, 180)));
            if (chance(20)) lx_entity_set_scale(ED.scene, ED.selected, lx_v3(rnd_f(-2, 3), rnd_f(0, 3), rnd_f(0.01f, 3)));
            ed_mark_changed();
            ed_commit();
        }
        break;
    }
}

/* checks that compare before/after */
static void consistency_action(void) {
    if (ED.mode != ED_MODE_EDITOR || !ED.scene || ED.play != ED_EDIT || ui_has_modal(ED.ui)) return;
    ed_commit();
    switch (rnd_n(3)) {
    case 0: {
        act("check: undo then redo returns to the same scene");
        if (!ED.undo_count) break;
        char *a = lx_scene_serialize(ED.scene);
        ed_undo();
        ed_redo();
        char *b = lx_scene_serialize(ED.scene);
        if (strcmp(a, b)) BROKEN("undo + redo doesn't give back the same scene");
        lx_free(a);
        lx_free(b);
        break;
    }
    case 1: {
        act("check: play then stop restores the scene");
        char *a = lx_scene_serialize(ED.scene);
        ed_play();
        if (ED.play != ED_PLAYING) { lx_free(a); break; }
        for (int i = 0; i < 3; i++) lx_scene_simulate(ED.scene, 1.0f / 60);
        ed_stop();
        char *b = lx_scene_serialize(ED.scene);
        if (strcmp(a, b)) {
            size_t i = 0;
            while (a[i] && a[i] == b[i]) i++;
            size_t st = i > 60 ? i - 60 : 0;
            BROKEN("play + stop changed the scene near: ...%.140s... vs ...%.140s...", a + st, b + st);
        }
        lx_free(a);
        lx_free(b);
        break;
    }
    case 2: act("check: save/load round trip"); check_roundtrip(); break;
    }
}

/* a request made between frames vs one made while the frame is being built (a real click) */
static int g_in_frame_kind;
void ed_stress_in_frame(void) {
    if (!ed_stress) return;
    int k = g_in_frame_kind;
    g_in_frame_kind = 0;
    if (k == 1) ui_monkey();
    else if (k == 2) editor_action();
}

bool ed_stress_step(int frame, int frames) {
    static bool started;
    if (!started) {
        started = true;
        const char *seed = getenv("LUXORA_STRESS_SEED");
        if (seed) g_rng = strtoull(seed, NULL, 10) * 0x9e3779b97f4a7c15ull + 1;
        LX_INFO("[stress] seed %s, %d frames", seed ? seed : "default", frames);
    }
    if (frame < 5) return true;
    if (frame % 50 == 0) rescan();
    /* the monkey sometimes ends up in the Project Manager (Close Project): open the project again */
    if (ED.mode != ED_MODE_EDITOR && frame % 20 == 0 && !ui_has_modal(ED.ui)) {
        static char root[LX_PATH_MAX];
        if (!root[0] && getenv("LUXORA_STRESS_PROJECT")) lx_strcpy(root, sizeof root, getenv("LUXORA_STRESS_PROJECT"));
        if (root[0]) { act("reopen the project"); ed_open_project(root); }
    }
    int actions = 1;   /* one per frame, as a person would: the editor gets a frame to catch up in between */
    for (int i = 0; i < actions; i++) {
        int r = rnd_n(100);
        if (r < 55) {
            if (chance(50)) g_in_frame_kind = 1;
            else ui_monkey();
        } else if (r < 92) {
            if (chance(30)) g_in_frame_kind = 2;
            else editor_action();
        } else consistency_action();
        check_editor();
        if (ED.quit) { LX_WARN("[stress] something asked the editor to quit (after: %s) - continuing", g_last_action); ED.quit = false; }
    }
    if (frame % 97 == 0) check_roundtrip();
    if (frame + 1 >= frames) {
        LX_INFO("[stress] done: %ld actions, %d broken rule(s)", g_actions, ed_stress_failures);
        return false;
    }
    return true;
}
