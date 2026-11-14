/* Editor shell: project lifecycle, layout, menus, toolbar, status bar, scene management,
 * undo/redo, selection, play mode and keyboard shortcuts. */
#include "editor.h"
#include <ctype.h>

/* ======================================================================
 *  shared UI helpers
 * ====================================================================== */
UiNode *ed_panel_header(UiNode *parent, UiIcon icon, const char *title) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *h = ui_box(ui, parent, UI_ROW);
    h->h = 32;
    h->bg = t->header;
    ui_pad(h, 10, 0, 6, 0);
    h->gap = 6;
    h->cross_align = UI_ALIGN_CENTER;
    UiNode *l = ui_label(ui, h, title);
    l->icon = icon;
    l->icon_size = 15;
    l->icon_color = t->text_dim;
    l->font = UI_FONT_BOLD;
    l->grow = 1;
    return h;
}

/* n is the section header; its own body is the header's next sibling inside the section */
static void section_toggle(UiNode *n) {
    bool *open = n->user;
    if (open) *open = !*open;
    UiNode *body = n->next;
    if (body) body->hidden = open ? !*open : !body->hidden;
    UiNode *chev = n->first;
    if (chev) chev->icon = (body && !body->hidden) ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_RIGHT;
}

UiNode *ed_section(UiNode *parent, UiIcon icon, const char *title, bool *open, void (*on_menu)(UiNode *)) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *wrap = ui_box(ui, parent, UI_COL);
    UiNode *h = ui_box(ui, wrap, UI_ROW);
    h->h = 30;
    h->bg = t->panel_alt;
    h->bg_hover = t->hover;
    ui_pad(h, 6, 0, 4, 0);
    h->gap = 6;
    h->cross_align = UI_ALIGN_CENTER;
    h->user = open;
    h->on_click = section_toggle;
    bool is_open = open ? *open : true;
    UiNode *chev = ui_label(ui, h, "");
    chev->icon = is_open ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_RIGHT;
    chev->icon_size = 12;
    chev->icon_color = t->text_dim;
    chev->w = 14;
    UiNode *l = ui_label(ui, h, title);
    l->icon = icon;
    l->icon_size = 15;
    l->icon_color = ed_color_for_icon(icon);
    l->font = UI_FONT_BOLD;
    l->grow = 1;
    if (on_menu) {
        UiNode *m = ui_icon_button(ui, h, UI_ICON_DOTS, "Component options", on_menu);
        m->w = 22;
        m->h = 22;
    }
    UiNode *body = ui_box(ui, wrap, UI_COL);
    ui_pad(body, 12, 6, 10, 10);
    body->gap = 4;
    body->hidden = !is_open;
    return body;
}

UiNode *ed_prop_row(UiNode *parent, const char *label) {
    UiCtx *ui = ED.ui;
    UiNode *row = ui_box(ui, parent, UI_ROW);
    row->h = 24;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    UiNode *l = ui_label(ui, row, label);
    l->w = 112;
    l->fg = ui_theme(ui)->text_dim;
    lx_strcpy(l->tooltip, sizeof l->tooltip, label);
    return row;
}

UiNode *ed_vec3_row(UiNode *parent, const char *label, float *v, float step, void (*on_change)(UiNode *)) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *row = ed_prop_row(parent, label);
    uint32_t cols[3] = {t->axis_x, t->axis_y, t->axis_z};
    for (int i = 0; i < 3; i++) {
        UiNode *n = ui_number(ui, row, &v[i], step);
        n->grow = 1;
        n->h = 22;
        n->icon_color = cols[i];
        n->on_change = on_change;
        n->precision = 3;
    }
    return row;
}

void ed_field_changed(UiNode *n) {
    LX_UNUSED(n);
    ed_mark_changed();
}

void ed_relative_asset(char *out, size_t cap, const char *abs_path) {
    size_t n = strlen(ED.project.assets);
    if (!strncmp(abs_path, ED.project.assets, n) && (abs_path[n] == '/' || abs_path[n] == '\\')) lx_strcpy(out, cap, abs_path + n + 1);
    else lx_strcpy(out, cap, abs_path);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
}

/* ---------------- confirm / prompt dialogs ---------------- */
static void (*g_confirm_cb)(void);
static void (*g_prompt_cb)(const char *);
static char g_prompt_text[128];

static void confirm_ok(UiNode *n) { LX_UNUSED(n); ui_close_modal(ED.ui); if (g_confirm_cb) g_confirm_cb(); }
static void dialog_cancel(UiNode *n) { LX_UNUSED(n); ui_close_modal(ED.ui); }
static void prompt_ok(UiNode *n) {
    LX_UNUSED(n);
    char text[128];
    lx_strcpy(text, sizeof text, g_prompt_text);
    ui_close_modal(ED.ui);
    if (g_prompt_cb && text[0]) g_prompt_cb(text);
}

static UiNode *dialog_buttons(UiNode *body, const char *ok, void (*on_ok)(UiNode *), bool danger) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    ui_spacer(ui, body);
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->h = 32;
    row->gap = 8;
    ui_spacer(ui, row);
    UiNode *c = ui_button(ui, row, "Cancel", dialog_cancel);
    c->w = 96;
    UiNode *b = ui_button(ui, row, ok, on_ok);
    b->w = 120;
    b->bg = danger ? t->danger : t->accent;
    b->bg_hover = danger ? ui_lerp_color(t->danger, 0xffffffffu, 0.15f) : t->accent_hover;
    b->fg = t->accent_text;
    return b;
}

void ed_confirm(const char *title, const char *message, const char *ok_text, void (*on_ok)(void)) {
    g_confirm_cb = on_ok;
    UiNode *body = ui_modal(ED.ui, title, 460, 190);
    UiNode *l = ui_label(ED.ui, body, message);
    l->icon = UI_ICON_WARNING;
    l->icon_color = ui_theme(ED.ui)->warning;
    dialog_buttons(body, ok_text, confirm_ok, !strcmp(ok_text, "Delete") || !strcmp(ok_text, "Discard"));
}

void ed_prompt(const char *title, const char *label, const char *initial, void (*on_ok)(const char *)) {
    g_prompt_cb = on_ok;
    lx_strcpy(g_prompt_text, sizeof g_prompt_text, initial ? initial : "");
    UiNode *body = ui_modal(ED.ui, title, 440, 180);
    UiNode *row = ed_prop_row(body, label);
    row->h = 30;
    UiNode *tf = ui_textfield(ED.ui, row, g_prompt_text, sizeof g_prompt_text);
    tf->grow = 1;
    tf->h = 28;
    tf->on_change = NULL;
    ui_focus(ED.ui, tf);
    dialog_buttons(body, "OK", prompt_ok, false);
}

/* ======================================================================
 *  project lifecycle
 * ====================================================================== */
/* an engine source folder has the top-level CMakeLists.txt and the runtime sources */
static bool is_engine_dir(const char *dir) {
    if (!dir || !dir[0]) return false;
    char a[LX_PATH_MAX], b[LX_PATH_MAX];
    lx_path_join(a, sizeof a, dir, "CMakeLists.txt");
    lx_path_join(b, sizeof b, dir, "src/engine.c");
    return lx_file_exists(a) && lx_file_exists(b);
}

/* Where the engine source is, in order: LUXORA_ENGINE_ROOT, the folder chosen in Build Settings,
 * "Engine" next to the editor (the downloadable package), the editor's own folder and its parents
 * (a source build), and the folder the editor was compiled in. */
const char *ed_engine_root(void) {
    static char root[LX_PATH_MAX];
    const char *env = getenv("LUXORA_ENGINE_ROOT");
    if (is_engine_dir(env)) { lx_strcpy(root, sizeof root, env); return root; }
    if (is_engine_dir(ED.engine_root_pref)) { lx_strcpy(root, sizeof root, ED.engine_root_pref); return root; }
    const char *exe = lx_exe_dir();
    if (exe[0]) {
        lx_path_join(root, sizeof root, exe, "Engine");
        if (is_engine_dir(root)) return root;
        char dir[LX_PATH_MAX];
        lx_strcpy(dir, sizeof dir, exe);
        for (int up = 0; up < 5 && dir[0]; up++) {
            if (is_engine_dir(dir)) { lx_strcpy(root, sizeof root, dir); return root; }
            char parent[LX_PATH_MAX];
            lx_path_parent(parent, sizeof parent, dir);
            if (!strcmp(parent, dir)) break;
            lx_strcpy(dir, sizeof dir, parent);
        }
    }
#ifdef LUXORA_ENGINE_ROOT
    if (is_engine_dir(LUXORA_ENGINE_ROOT)) { lx_strcpy(root, sizeof root, LUXORA_ENGINE_ROOT); return root; }
#endif
    root[0] = 0;
    return root;
}

/* The game player that ships with the editor: Runtime/<Platform>/LuxoraPlayer next to the editor
 * (the download). Source builds find the luxora_player built alongside the editor. */
bool ed_prebuilt_player(LxPlatform target, char *out, size_t cap) {
    if (target == LX_PLATFORM_ANDROID) return false;
    const char *exe = lx_exe_dir(), *ext = target == LX_PLATFORM_WINDOWS ? ".exe" : "";
    const char *env = getenv("LUXORA_PLAYER");
    if (env && lx_file_exists(env)) { lx_strcpy(out, cap, env); return true; }
    if (!exe[0]) { out[0] = 0; return false; }
    char rel[96];
    snprintf(rel, sizeof rel, "Runtime/%s/LuxoraPlayer%s", lx_platform_name(target), ext);
    lx_path_join(out, cap, exe, rel);
    if (lx_file_exists(out)) return true;
    if (target == lx_platform_current()) {
        snprintf(rel, sizeof rel, "luxora_player%s", ext); /* a source build: build/luxora_player */
        lx_path_join(out, cap, exe, rel);
        if (lx_file_exists(out)) return true;
    }
    out[0] = 0;
    return false;
}

void ed_open_project(const char *root) {
    if (ED.in_frame) {
        lx_strcpy(ED.pending_open, sizeof ED.pending_open, root);
        return;
    }
    LxProject p;
    if (!lx_project_load(&p, root)) {
        ui_notify(ED.ui, "Could not open project at %s", root);
        LX_ERROR("Could not open project at %s", root);
        return;
    }
    if (ED.has_project) ed_close_project();
    ED.project = p;
    ED.has_project = true;
    lx_engine_set_project(&p);
    {   /* play mode keeps its save data inside the project, not with the real game's */
        char save[LX_PATH_MAX];
        lx_path_join(save, sizeof save, p.root, "Library/SaveData.txt");
        lx_save_set_file(save);
    }
    lx_renderer_set_vsync(p.graphics.vsync);
    ed_add_recent(p.root);
    lx_strcpy(ED.content_dir, sizeof ED.content_dir, p.assets);
    ED.scene = NULL;
    ed_scene_replaced();
    ED.selected = 0;
    ED.selected_asset[0] = 0;
    ED.play = ED_EDIT;
    ED.cam_pos = lx_v3(4.5f, 3.2f, 7.5f);
    ED.cam_yaw = 31.0f;
    ED.cam_pitch = -16.0f;
    /* reopen where you left off: the last scene you worked on and the view of it */
    char state[LX_PATH_MAX], last[LX_REF_MAX] = "";
    lx_path_join(state, sizeof state, p.root, "Library/EditorState.txt");
    char *st = lx_file_read(state, NULL);
    if (st) {
        const char *cur = st;
        char k[64], v[LX_REF_MAX];
        int r;
        while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
            if (r != 1) continue;
            if (!strcmp(k, "scene")) lx_strcpy(last, sizeof last, v);
            else if (!strcmp(k, "camera")) {
                float a, b, c, yaw, pitch;
                if (sscanf(v, "%f %f %f %f %f", &a, &b, &c, &yaw, &pitch) == 5) {
                    ED.cam_pos = lx_v3(a, b, c);
                    ED.cam_yaw = yaw;
                    ED.cam_pitch = pitch;
                }
            }
        }
        lx_free(st);
    }
    if (!(last[0] && ed_open_scene(last)) && !ed_open_scene(p.startup_scene)) ed_new_scene("Main");
    ed_set_mode(ED_MODE_EDITOR);
    LX_INFO("Opened project '%s' (%s)", p.name, p.root);
}

/* Library/EditorState.txt: the scene you were working on and the editor camera */
void ed_remember_scene(void) {
    if (!ED.has_project || !ED.scene_ref[0]) return;
    char path[LX_PATH_MAX], dir[LX_PATH_MAX], text[LX_REF_MAX + 160];
    lx_path_join(dir, sizeof dir, ED.project.root, "Library");
    lx_mkdir_p(dir);
    lx_path_join(path, sizeof path, dir, "EditorState.txt");
    snprintf(text, sizeof text, "scene = %s\ncamera = %g %g %g %g %g\n", ED.scene_ref, ED.cam_pos.x, ED.cam_pos.y, ED.cam_pos.z,
             ED.cam_yaw, ED.cam_pitch);
    lx_file_write(path, text, strlen(text));
}

void ed_request_close_project(void) {
    if (ED.in_frame) { ED.pending_close = true; return; }
    ed_close_project();
    ed_set_mode(ED_MODE_PROJECT_MANAGER);
}

void ed_run_pending(void) {
    if (ED.pending_close) {
        ED.pending_close = false;
        ed_request_close_project();
    }
    if (ED.pending_open[0]) {
        char root[LX_PATH_MAX];
        lx_strcpy(root, sizeof root, ED.pending_open);
        ED.pending_open[0] = 0;
        ed_open_project(root);
        if (!ED.has_project && ED.mode != ED_MODE_PROJECT_MANAGER) ed_set_mode(ED_MODE_PROJECT_MANAGER);
    }
}

void ed_close_project(void) {
    if (!ED.has_project) return;
    if (ED.play != ED_EDIT) ed_stop();
    ed_materials_flush(true);
    ed_remember_scene();
    lx_audio_stop_all();
    if (ED.scene) lx_scene_destroy(ED.scene);
    ED.scene = NULL;
    ed_scene_replaced();
    ed_reset_history();
    if (ED.viewport_rt) { lx_render_target_destroy(ED.viewport_rt); ED.viewport_rt = NULL; }
    ed_thumbs_shutdown();
    lx_assets_clear();
    lx_gi_reset();
    ed_scripts_close();
    ED.has_project = false;
}

/* ======================================================================
 *  scenes
 * ====================================================================== */
/* The scene in memory was swapped (open, new, undo, Stop) or freed: fields still bound to the old one
 * are switched off right now - even mid-frame, before anything draws or clicks them - and rebuilt. */
void ed_scene_replaced(void) {
    ED.scene_gen++;
    ED.inspector_dirty = true;
    ED.hierarchy_hash = 0;
    ed_panels_unbind_scene();
}
void ed_set_scene(LxScene *s, const char *ref) {
    if (ED.scene && ED.scene != s) {
        lx_audio_stop_all();
        lx_scene_destroy(ED.scene);
    }
    ED.scene = s;
    ed_scene_replaced();
    lx_strcpy(ED.scene_ref, sizeof ED.scene_ref, ref ? ref : "");
    ED.selected = 0;
    ED.dirty = false;
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ED.expanded_count = 0;
    ed_reset_history();
    lx_gi_reset();
    ed_update_title();
}

void ed_new_scene(const char *name) {
    ed_set_scene(lx_scene_create_default(name), "");
    ED.dirty = true;
    ed_update_title();
}

bool ed_open_scene(const char *ref) {
    if (!ref || !ref[0]) return false;
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    LxScene *s = lx_scene_load(path);
    if (!s) return false;
    ed_set_scene(s, ref);
    ed_remember_scene();
    LX_INFO("Opened scene %s", ref);
    return true;
}

static void save_as_named(const char *name) {
    char ref[LX_REF_MAX], path[LX_PATH_MAX];
    snprintf(ref, sizeof ref, "Scenes/%s.lxscene", name);
    lx_asset_path(path, sizeof path, ref);
    lx_scene_set_name(ED.scene, name);
    if (lx_scene_save(ED.scene, path)) {
        lx_strcpy(ED.scene_ref, sizeof ED.scene_ref, ref);
        ED.dirty = false;
        ED.content_dirty = true;
        ui_notify(ED.ui, "Saved %s", ref);
        /* first scene in the project becomes part of the build automatically */
        LxBuildSettings *b = &ED.project.build;
        bool listed = false;
        for (int i = 0; i < b->scene_count; i++) listed |= !strcmp(b->scenes[i], ref);
        if (!listed && b->scene_count < LX_MAX_BUILD_SCENES) {
            lx_strcpy(b->scenes[b->scene_count++], LX_REF_MAX, ref);
            lx_project_save(&ED.project);
        }
    }
    ed_update_title();
}

void ed_save_scene_as_dialog(void) {
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode before saving"); return; }
    ed_prompt("Save Scene As", "Scene name", lx_scene_name(ED.scene), save_as_named);
}

bool ed_save_scene(void) {
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode before saving"); return false; }
    if (!ED.scene_ref[0]) { ed_save_scene_as_dialog(); return false; }
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ED.scene_ref);
    ed_materials_flush(true);
    if (!lx_scene_save(ED.scene, path)) { ui_notify(ED.ui, "Save failed: %s", path); return false; }
    ed_remember_scene();
    ED.dirty = false;
    ui_notify(ED.ui, "Scene saved");
    ed_update_title();
    return true;
}

static void new_scene_named(const char *name) {
    ed_new_scene(name);
    save_as_named(name);
}
void ed_new_scene_dialog(void) { ed_prompt("New Scene", "Scene name", "New Scene", new_scene_named); }

/* ---- save prompt: Save / Don't Save / Cancel, styled like the rest of the editor ---- */
static void (*g_after_save)(void);
typedef void (*ThenFn)(void);
static ThenFn save_prompt_take(void) {
    ThenFn then = g_after_save;
    g_after_save = NULL;
    ED.save_prompt_open = false;
    ui_close_modal(ED.ui);
    return then;
}
static void sp_save(UiNode *n) {
    LX_UNUSED(n);
    ThenFn then = save_prompt_take();
    if (ed_save_scene() && then) then();   /* an unsaved new scene opens Save As instead; then the user retries */
}
static void sp_dont_save(UiNode *n) {
    LX_UNUSED(n);
    ThenFn then = save_prompt_take();
    ED.dirty = false;
    if (then) then();
}
static void sp_cancel(UiNode *n) { LX_UNUSED(n); save_prompt_take(); }

void ed_save_prompt(const char *action, void (*then)(void)) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    ui_close_popups(ui);
    g_after_save = then;
    ED.save_prompt_open = true;
    UiNode *body = ui_modal(ui, "Unsaved Changes", 520, 214);
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->gap = 14;
    row->h = 76;
    UiNode *ic = ui_label(ui, row, "");
    ic->icon = UI_ICON_SAVE;
    ic->icon_size = 30;
    ic->icon_color = t->accent;
    ic->w = 40;
    ic->h = 40;
    UiNode *col = ui_box(ui, row, UI_COL);
    col->grow = 1;
    col->gap = 6;
    char msg[LX_NAME_MAX + 96];
    snprintf(msg, sizeof msg, "Save changes to \"%s\" before %s?", ED.scene ? lx_scene_name(ED.scene) : "the scene", action);
    UiNode *m = ui_label(ui, col, msg);
    m->font = UI_FONT_BOLD;
    m->wrap = true;
    UiNode *sub = ui_label(ui, col, "Your changes will be lost if you don't save them.");
    sub->fg = t->text_dim;
    sub->wrap = true;
    ui_spacer(ui, body);
    UiNode *br = ui_box(ui, body, UI_ROW);
    br->h = 32;
    br->gap = 8;
    UiNode *dont = ui_button(ui, br, "Don't Save", sp_dont_save);
    dont->w = 120;
    dont->fg = t->danger;
    ui_spacer(ui, br);
    UiNode *c = ui_button(ui, br, "Cancel", sp_cancel);
    c->w = 96;
    UiNode *sv = ui_button(ui, br, "Save", sp_save);
    sv->icon = UI_ICON_SAVE;
    sv->w = 120;
    sv->bg = t->accent;
    sv->bg_hover = t->accent_hover;
    sv->fg = t->accent_text;
    sv->icon_color = t->accent_text;
}

bool ed_confirm_discard(void (*then)(void)) {
    if (!ED.dirty) return true;
    ed_save_prompt("continuing", then);
    return false;
}

/* ======================================================================
 *  undo / redo (whole-scene snapshots: simple and always correct)
 * ====================================================================== */
static char *selected_name(void) { return lx_strdup(ED.selected ? lx_entity_name(ED.scene, ED.selected) : ""); }

void ed_reset_history(void) {
    for (int i = 0; i < ED.undo_count; i++) lx_free(ED.undo[i]);
    for (int i = 0; i < ED.redo_count; i++) lx_free(ED.redo[i]);
    ED.undo_count = ED.redo_count = 0;
    lx_free(ED.snapshot);
    ED.snapshot = ED.scene ? lx_scene_serialize(ED.scene) : NULL;
    ED.change_pending = false;
}

void ed_mark_changed(void) {
    if (ED.play != ED_EDIT) return;
    ED.change_pending = true;
    ED.dirty = true;
    lx_scene_mark_static_dirty(ED.scene);
}

void ed_commit(void) {
    if (!ED.scene || ED.play != ED_EDIT) return;
    ED.change_pending = false;
    char *now = lx_scene_serialize(ED.scene);
    if (ED.snapshot && !strcmp(now, ED.snapshot)) { lx_free(now); return; }
    if (ED.undo_count == ED_MAX_UNDO) {
        lx_free(ED.undo[0]);
        memmove(ED.undo, ED.undo + 1, sizeof(char *) * (ED_MAX_UNDO - 1));
        ED.undo_count--;
    }
    if (ED.snapshot) ED.undo[ED.undo_count++] = ED.snapshot;
    ED.snapshot = now;
    for (int i = 0; i < ED.redo_count; i++) lx_free(ED.redo[i]);
    ED.redo_count = 0;
    ED.dirty = true;
    ed_update_title();
}

static void restore(char *text) {
    char *sel = selected_name();
    LxScene *s = lx_scene_deserialize(text);
    char ref[LX_REF_MAX];
    lx_strcpy(ref, sizeof ref, ED.scene_ref);
    lx_audio_stop_all();
    lx_scene_destroy(ED.scene);
    ED.scene = s;
    ed_scene_replaced();
    lx_strcpy(ED.scene_ref, sizeof ED.scene_ref, ref);
    ED.selected = sel[0] ? lx_entity_find(s, sel) : 0;
    lx_free(sel);
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ED.dirty = true;
    lx_scene_mark_static_dirty(s);
    ed_update_title();
}

void ed_undo(void) {
    if (ED.play != ED_EDIT) return;
    if (ED.change_pending) ed_commit();
    if (!ED.undo_count) { ui_notify(ED.ui, "Nothing to undo"); return; }
    char *prev = ED.undo[--ED.undo_count];
    if (ED.redo_count < ED_MAX_UNDO) ED.redo[ED.redo_count++] = ED.snapshot;
    else lx_free(ED.snapshot);
    ED.snapshot = prev;
    restore(prev);
}

void ed_redo(void) {
    if (ED.play != ED_EDIT || !ED.redo_count) return;
    char *next = ED.redo[--ED.redo_count];
    if (ED.undo_count < ED_MAX_UNDO) ED.undo[ED.undo_count++] = ED.snapshot;
    else lx_free(ED.snapshot);
    ED.snapshot = next;
    restore(next);
}

/* ======================================================================
 *  selection & entity actions
 * ====================================================================== */
void ed_select(LxEntity e) {
    if (e && !lx_entity_valid(ED.scene, e)) e = 0;
    if (ED.selected == e && !ED.selected_asset[0]) return;
    ED.selected = e;
    ED.selected_asset[0] = 0;
    ED.inspector_dirty = true;
    if (ED.inspector_tabs) ED.inspector_tabs->tab_index = 0;
    /* expand the parents so the selection is visible in the hierarchy */
    for (LxEntity p = e ? lx_entity_parent(ED.scene, e) : 0; p; p = lx_entity_parent(ED.scene, p)) {
        bool found = false;
        for (int i = 0; i < ED.expanded_count; i++) found |= ED.expanded[i] == p;
        if (!found && ED.expanded_count < 256) ED.expanded[ED.expanded_count++] = p;
        ED.hierarchy_hash = 0;
    }
}

void ed_select_asset(const char *abs_path) {
    lx_strcpy(ED.selected_asset, sizeof ED.selected_asset, abs_path ? abs_path : "");
    ED.inspector_dirty = true;
    if (ED.inspector_tabs) ED.inspector_tabs->tab_index = 0;
}

LxVec3 ed_spawn_point(void) {
    /* in front of the editor camera, on the ground if looking down */
    LxQuat q = lx_quat_euler_deg(lx_v3(ED.cam_pitch, ED.cam_yaw, 0));
    LxVec3 f = lx_quat_rotate(q, lx_v3(0, 0, -1));
    LxVec3 p = lx_madd3(ED.cam_pos, f, 8.0f);
    if (f.y < -0.1f) {
        float t = -ED.cam_pos.y / f.y;
        if (t > 0 && t < 40) p = lx_madd3(ED.cam_pos, f, t);
    }
    p.x = roundf(p.x * 2) / 2;
    p.z = roundf(p.z * 2) / 2;
    return p;
}

LxEntity ed_create(const char *what) {
    if (!ED.scene) return 0;
    LxScene *s = ED.scene;
    LxVec3 at = ed_spawn_point();
    LxEntity e = 0;
    static const char *prims[] = {"Cube", "Sphere", "Plane", "Cylinder", "Capsule"};
    for (int i = 0; i < 5; i++)
        if (!strcmp(what, prims[i])) {
            e = lx_spawn_primitive(s, (LxPrimitive)i, prims[i], lx_v3(at.x, i == 2 ? 0 : (i >= 3 ? 1.0f : 0.5f), at.z));
        }
    if (!e && !strcmp(what, "Physics Cube")) {
        e = lx_spawn_primitive(s, LX_PRIM_CUBE, "Physics Cube", lx_v3(at.x, 2.5f, at.z));
        lx_add_rigidbody(s, e);
    } else if (!e && !strcmp(what, "Physics Sphere")) {
        e = lx_spawn_primitive(s, LX_PRIM_SPHERE, "Physics Sphere", lx_v3(at.x, 2.5f, at.z));
        lx_add_rigidbody(s, e);
    } else if (!e && !strcmp(what, "Character")) {
        /* a character controller with a capsule body to see it by (origin at the feet) */
        e = lx_entity_create(s, "Player");
        lx_entity_set_position(s, e, lx_v3(at.x, 0, at.z));
        lx_add_character(s, e);
        LxEntity bodyv = lx_spawn_primitive(s, LX_PRIM_CAPSULE, "Body", lx_v3(0, 0.9f, 0));
        lx_remove_component(s, bodyv, LX_COMP_COLLIDER);
        lx_entity_set_scale(s, bodyv, lx_v3(0.7f, 0.9f, 0.7f));
        lx_entity_set_parent(s, bodyv, e);
    } else if (!e && !strcmp(what, "Trigger Zone")) {
        e = lx_entity_create(s, "Trigger Zone");
        lx_entity_set_position(s, e, lx_v3(at.x, 1.0f, at.z));
        LxCollider *c = lx_add_collider(s, e, LX_COLLIDER_BOX);
        c->size = lx_v3(4, 2, 4);
        c->is_trigger = true;
    }
    if (!e) {
        e = lx_entity_create(s, what);
        lx_entity_set_position(s, e, lx_v3(at.x, 1.5f, at.z));
        if (!strcmp(what, "Camera")) lx_add_camera(s, e);
        else if (!strcmp(what, "Directional Light")) { lx_add_light(s, e, LX_LIGHT_DIRECTIONAL); lx_entity_set_euler(s, e, lx_v3(-50, -30, 0)); lx_entity_set_position(s, e, lx_v3(at.x, 6, at.z)); }
        else if (!strcmp(what, "Point Light")) { lx_add_light(s, e, LX_LIGHT_POINT); lx_entity_set_position(s, e, lx_v3(at.x, 2.5f, at.z)); }
        else if (!strcmp(what, "Spot Light")) { lx_add_light(s, e, LX_LIGHT_SPOT); lx_entity_set_position(s, e, lx_v3(at.x, 4, at.z)); lx_entity_set_euler(s, e, lx_v3(-90, 0, 0)); }
        else if (!strcmp(what, "Audio Source")) lx_add_audio_source(s, e, "");
        else if (!strcmp(what, "Reflection Probe")) { lx_add_reflection_probe(s, e); lx_entity_set_position(s, e, lx_v3(at.x, 2.0f, at.z)); }
        else if (!strcmp(what, "Crowd")) { lx_add_crowd(s, e, 500); lx_entity_set_position(s, e, lx_v3(at.x, 0.5f, at.z)); }
        else if (!strcmp(what, "Empty")) lx_entity_set_position(s, e, at);
    }
    ed_select(e);
    ed_mark_changed();
    ed_commit();
    return e;
}

void ed_delete_selected(void) {
    if (!ED.selected || ED.play != ED_EDIT) return;
    lx_entity_destroy(ED.scene, ED.selected);
    ED.selected = 0;
    ED.inspector_dirty = true;
    ed_mark_changed();
    ed_commit();
}

void ed_duplicate_selected(void) {
    if (!ED.selected) return;
    LxEntity c = lx_entity_duplicate(ED.scene, ED.selected);
    LxVec3 p = lx_entity_position(ED.scene, c);
    lx_entity_set_position(ED.scene, c, lx_v3(p.x + 1, p.y, p.z));
    char name[LX_NAME_MAX + 8];
    snprintf(name, sizeof name, "%s", lx_entity_name(ED.scene, c));
    ed_select(c);
    ed_mark_changed();
    ed_commit();
}

void ed_focus_selected(void) {
    if (!ED.selected) return;
    LxScene *s = ED.scene;
    LxVec3 c = lx_entity_world_position(s, ED.selected);
    float radius = 1.0f;
    LxMeshRenderer *mr = lx_get_mesh(s, ED.selected);
    if (mr) {
        LxAabb b = lx_aabb_transform(lx_mesh_bounds(lx_asset_mesh(mr->mesh)), lx_entity_world_matrix(s, ED.selected));
        c = lx_aabb_center(b);
        radius = lx_maxf(0.5f, lx_len3(lx_aabb_extent(b)));
    }
    LxQuat q = lx_quat_euler_deg(lx_v3(ED.cam_pitch, ED.cam_yaw, 0));
    LxVec3 f = lx_quat_rotate(q, lx_v3(0, 0, -1));
    ED.cam_pos = lx_madd3(c, f, -radius * 2.8f);
}

static LxEntity g_rename_target;
static void rename_done(const char *text) {
    if (lx_entity_valid(ED.scene, g_rename_target)) {
        lx_entity_set_name(ED.scene, g_rename_target, text);
        ED.hierarchy_hash = 0;
        ED.inspector_dirty = true;
        ed_mark_changed();
        ed_commit();
    }
}
void ed_rename_dialog(LxEntity e) {
    if (!e) return;
    g_rename_target = e;
    ed_prompt("Rename", "Name", lx_entity_name(ED.scene, e), rename_done);
}

/* ======================================================================
 *  play mode
 * ====================================================================== */
void ed_play(void) {
    if (ED.play == ED_PAUSED) { ED.play = ED_PLAYING; return; }
    if (ED.play != ED_EDIT || !ED.scene) return;
    if (ED.change_pending) ed_commit();
    if (!ed_scripts_refresh(false)) { ui_notify(ED.ui, "Fix the script errors before playing (see the Console)"); return; }
    lx_audio_preview_stop();
    ED.play_snapshot = lx_scene_serialize(ED.scene);
    ED.play = ED_PLAYING;
    ED.game_view_before_play = ED.game_view;
    ED.game_view = lx_scene_main_camera(ED.scene) != 0; /* play shows the game; Stop restores the previous view */
    LX_INFO("Play mode started");
    ed_update_title();
}

void ed_pause(void) {
    if (ED.play == ED_PLAYING) ED.play = ED_PAUSED;
    else if (ED.play == ED_PAUSED) ED.play = ED_PLAYING;
}

void ed_stop(void) {
    if (ED.play == ED_EDIT) return;
    lx_audio_stop_all();
    lx_scene_stop_simulation(ED.scene);
    char *sel = selected_name();
    char ref[LX_REF_MAX];
    lx_strcpy(ref, sizeof ref, ED.scene_ref);
    bool dirty = ED.dirty;
    LxScene *s = lx_scene_deserialize(ED.play_snapshot);
    lx_scene_destroy(ED.scene);
    ED.scene = s;
    ed_scene_replaced();
    lx_strcpy(ED.scene_ref, sizeof ED.scene_ref, ref);
    lx_free(ED.play_snapshot);
    ED.play_snapshot = NULL;
    ED.selected = sel[0] ? lx_entity_find(s, sel) : 0;
    lx_free(sel);
    ED.dirty = dirty;
    ED.play = ED_EDIT;
    ED.game_view = ED.game_view_before_play;
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    lx_gi_reset();
    LX_INFO("Play mode stopped - scene restored");
    ed_update_title();
}

void ed_step(void) {
    if (ED.play == ED_EDIT) { ed_play(); ED.play = ED_PAUSED; }
    if (ED.play == ED_PAUSED) {
        if (ED.viewport) lx_game_ui_begin_frame(ED.viewport->x, ED.viewport->y, ED.viewport->cw, ED.viewport->ch, true);
        lx_scene_simulate(ED.scene, 1.0f / 60.0f);
        lx_audio_update(ED.scene, 1.0f / 60.0f);
    }
}

/* ======================================================================
 *  menus
 * ====================================================================== */
static void m_new_scene(UiNode *n) { LX_UNUSED(n); if (ed_confirm_discard(ed_new_scene_dialog)) ed_new_scene_dialog(); }
static void m_save(UiNode *n) { LX_UNUSED(n); ed_save_scene(); }
static void m_save_as(UiNode *n) { LX_UNUSED(n); ed_save_scene_as_dialog(); }
static void m_project_settings(UiNode *n) { ed_project_settings(n ? (int)n->tag : 0); }
static void m_build_settings(UiNode *n) { LX_UNUSED(n); ed_build_settings(); }
static void m_open_folder(UiNode *n) { LX_UNUSED(n); lx_platform_open_folder(ED.project.root); }
static void close_project_now(void) { ed_request_close_project(); }
static void m_close_project(UiNode *n) {
    LX_UNUSED(n);
    if (ED.dirty && ED.play == ED_EDIT) ed_save_prompt("closing the project", close_project_now);
    else close_project_now();
}
static void m_exit(UiNode *n) { LX_UNUSED(n); ed_request_quit(); }
static void m_undo(UiNode *n) { LX_UNUSED(n); ed_undo(); }
static void m_redo(UiNode *n) { LX_UNUSED(n); ed_redo(); }
static void m_duplicate(UiNode *n) { LX_UNUSED(n); ed_duplicate_selected(); }
static void m_delete(UiNode *n) { LX_UNUSED(n); ed_delete_selected(); }
static void m_rename(UiNode *n) { LX_UNUSED(n); ed_rename_dialog(ED.selected); }
static void m_focus(UiNode *n) { LX_UNUSED(n); ed_focus_selected(); }
static void m_deselect(UiNode *n) { LX_UNUSED(n); ed_select(0); }
static void m_create(UiNode *n) { ed_create(n->text); }
static void m_play(UiNode *n) { LX_UNUSED(n); if (ED.play == ED_EDIT) ed_play(); else ed_stop(); }
static void m_pause(UiNode *n) { LX_UNUSED(n); ed_pause(); }
static void m_step(UiNode *n) { LX_UNUSED(n); ed_step(); }
static void m_build_current(UiNode *n) { ed_build(ED.project.build.target, n->tag == 1); }
static void m_about(UiNode *n) { LX_UNUSED(n); ed_about(); }
static void m_shortcuts(UiNode *n) { LX_UNUSED(n); ed_shortcuts_help(); }
static void m_show_tab(UiNode *n) {
    UiNode *tabs = n->user;
    if (tabs) tabs->tab_index = (int)n->tag;
}
static void m_reset_layout(UiNode *n) {
    LX_UNUSED(n);
    ed_set_mode(ED_MODE_EDITOR);
    ui_notify(ED.ui, "Layout reset");
}

static UiNode *menu_at(UiNode *anchor) { return ui_popup(ED.ui, anchor->x, anchor->y + anchor->ch, 230); }

static UiNode *item(UiNode *p, const char *text, const char *key, UiIcon icon, void (*cb)(UiNode *)) {
    UiNode *it = ui_menu_item(ED.ui, p, text, key, cb);
    it->icon = icon;
    it->icon_color = ed_color_for_icon(icon);
    return it;
}

static void file_menu(UiNode *a) {
    UiNode *p = menu_at(a);
    item(p, "New Scene", "Ctrl+N", UI_ICON_SCENE, m_new_scene);
    item(p, "Save Scene", "Ctrl+S", UI_ICON_SAVE, m_save);
    item(p, "Save Scene As...", "Ctrl+Shift+S", UI_ICON_SAVE, m_save_as);
    ui_menu_separator(ED.ui, p);
    item(p, "Project Settings...", NULL, UI_ICON_GEAR, m_project_settings);
    item(p, "Build Settings...", "Ctrl+Shift+B", UI_ICON_BUILD, m_build_settings);
    item(p, "Open Project Folder", NULL, UI_ICON_FOLDER_OPEN, m_open_folder);
    ui_menu_separator(ED.ui, p);
    item(p, "Close Project", NULL, UI_ICON_BACK, m_close_project);
    item(p, "Exit", "Alt+F4", UI_ICON_CLOSE, m_exit);
}

static void edit_menu(UiNode *a) {
    UiNode *p = menu_at(a);
    UiNode *u = item(p, "Undo", "Ctrl+Z", UI_ICON_UNDO, m_undo);
    u->disabled = !ED.undo_count && !ED.change_pending;
    UiNode *r = item(p, "Redo", "Ctrl+Y", UI_ICON_REDO, m_redo);
    r->disabled = !ED.redo_count;
    ui_menu_separator(ED.ui, p);
    bool sel = ED.selected != 0;
    item(p, "Duplicate", "Ctrl+D", UI_ICON_COPY, m_duplicate)->disabled = !sel;
    item(p, "Rename", "F2", UI_ICON_FILE, m_rename)->disabled = !sel;
    item(p, "Delete", "Del", UI_ICON_TRASH, m_delete)->disabled = !sel;
    ui_menu_separator(ED.ui, p);
    item(p, "Focus Selection", "F", UI_ICON_FOCUS, m_focus)->disabled = !sel;
    item(p, "Deselect", "Esc", UI_ICON_SELECT, m_deselect);
}

static void create_items(UiNode *p) {
    item(p, "Empty", NULL, UI_ICON_ENTITY, m_create);
    ui_menu_separator(ED.ui, p);
    item(p, "Cube", NULL, UI_ICON_CUBE, m_create);
    item(p, "Sphere", NULL, UI_ICON_SPHERE, m_create);
    item(p, "Plane", NULL, UI_ICON_PLANE, m_create);
    item(p, "Cylinder", NULL, UI_ICON_CYLINDER, m_create);
    item(p, "Capsule", NULL, UI_ICON_CAPSULE, m_create);
    ui_menu_separator(ED.ui, p);
    item(p, "Camera", NULL, UI_ICON_CAMERA, m_create);
    item(p, "Directional Light", NULL, UI_ICON_SUN, m_create);
    item(p, "Point Light", NULL, UI_ICON_BULB, m_create);
    item(p, "Spot Light", NULL, UI_ICON_SPOT, m_create);
    item(p, "Reflection Probe", NULL, UI_ICON_PROBE, m_create);
    ui_menu_separator(ED.ui, p);
    ui_menu_separator(ED.ui, p);
    item(p, "Character", NULL, UI_ICON_CHARACTER, m_create);
    item(p, "Physics Cube", NULL, UI_ICON_RIGIDBODY, m_create);
    item(p, "Physics Sphere", NULL, UI_ICON_RIGIDBODY, m_create);
    item(p, "Trigger Zone", NULL, UI_ICON_COLLIDER, m_create);
    ui_menu_separator(ED.ui, p);
    item(p, "Audio Source", NULL, UI_ICON_SPEAKER, m_create);
    item(p, "Crowd", NULL, UI_ICON_CROWD, m_create);
}
static void create_menu(UiNode *a) { create_items(menu_at(a)); }
void ed_create_menu_at(float x, float y) { create_items(ui_popup(ED.ui, x, y, 220)); }

static void window_menu(UiNode *a) {
    UiNode *p = menu_at(a);
    UiNode *it;
    it = item(p, "Inspector", NULL, UI_ICON_INSPECTOR, m_show_tab); it->user = ED.inspector_tabs; it->tag = 0;
    it = item(p, "Lighting", NULL, UI_ICON_SUN, m_show_tab); it->user = ED.inspector_tabs; it->tag = 1;
    it = item(p, "Statistics", NULL, UI_ICON_STATS, m_show_tab); it->user = ED.inspector_tabs; it->tag = 2;
    ui_menu_separator(ED.ui, p);
    it = item(p, "Content Browser", NULL, UI_ICON_FOLDER, m_show_tab); it->user = ED.bottom_tabs; it->tag = 0;
    it = item(p, "Console", NULL, UI_ICON_CONSOLE, m_show_tab); it->user = ED.bottom_tabs; it->tag = 1;
    it = item(p, "Audio Mixer", NULL, UI_ICON_SOUND, m_show_tab); it->user = ED.bottom_tabs; it->tag = 2;
    it = item(p, "Scene Manager", NULL, UI_ICON_SCENE, m_show_tab); it->user = ED.bottom_tabs; it->tag = 3;
    ui_menu_separator(ED.ui, p);
    it = item(p, "Input Manager...", NULL, UI_ICON_GAMEPAD, m_project_settings); it->tag = 3;
    it = item(p, "Graphics Settings...", NULL, UI_ICON_MONITOR, m_project_settings); it->tag = 1;
    it = item(p, "Sound Settings...", NULL, UI_ICON_SPEAKER, m_project_settings); it->tag = 2;
    ui_menu_separator(ED.ui, p);
    item(p, "Reset Layout", NULL, UI_ICON_LAYERS, m_reset_layout);
}

static void build_menu(UiNode *a) {
    UiNode *p = menu_at(a);
    item(p, "Build Settings...", "Ctrl+Shift+B", UI_ICON_GEAR, m_build_settings);
    ui_menu_separator(ED.ui, p);
    char text[96];
    snprintf(text, sizeof text, "Build for %s", lx_platform_name(ED.project.build.target));
    UiNode *b = item(p, text, "Ctrl+B", UI_ICON_BUILD, m_build_current);
    b->tag = 0;
    b->disabled = lx_atomic_load(&ED.building) != 0;
    b = item(p, "Build and Run", "Ctrl+Shift+R", UI_ICON_PLAY, m_build_current);
    b->tag = 1;
    b->disabled = lx_atomic_load(&ED.building) != 0;
}

static void play_menu(UiNode *a) {
    UiNode *p = menu_at(a);
    item(p, ED.play == ED_EDIT ? "Play" : "Stop", "Ctrl+P", ED.play == ED_EDIT ? UI_ICON_PLAY : UI_ICON_STOP, m_play);
    item(p, "Pause", "Ctrl+Shift+P", UI_ICON_PAUSE, m_pause)->disabled = ED.play == ED_EDIT;
    item(p, "Step Frame", "Ctrl+Alt+P", UI_ICON_STEP, m_step);
}

static void help_menu(UiNode *a) {
    UiNode *p = menu_at(a);
    item(p, "Keyboard Shortcuts", "F1", UI_ICON_KEYBOARD, m_shortcuts);
    item(p, "About Luxora Editor", NULL, UI_ICON_INFO, m_about);
}

/* ======================================================================
 *  toolbar & status bar
 * ====================================================================== */
static void tb_gizmo(UiNode *n) { ED.gizmo = (EdGizmo)n->tag; }
static void tb_space(UiNode *n) { LX_UNUSED(n); ED.gizmo_local = !ED.gizmo_local; }
static void tb_snap(UiNode *n) { LX_UNUSED(n); ED.snap = !ED.snap; ed_save_prefs(); }
static void tb_grid(UiNode *n) { LX_UNUSED(n); ED.show_grid = !ED.show_grid; ed_save_prefs(); }

static void toolbar_frame(UiNode *n) {
    /* keep toggle states live */
    for (UiNode *c = n->first; c; c = c->next) {
        for (UiNode *b = c->first; b; b = b->next) {
            if (b->on_click == tb_gizmo) b->selected = (int)b->tag == (int)ED.gizmo;
            else if (b->on_click == tb_space) { b->icon = ED.gizmo_local ? UI_ICON_LOCAL : UI_ICON_GLOBE; lx_strcpy(b->tooltip, sizeof b->tooltip, ED.gizmo_local ? "Local space (click for global)" : "Global space (click for local)"); }
            else if (b->on_click == tb_snap) b->selected = ED.snap;
            else if (b->on_click == tb_grid) b->selected = ED.show_grid;
            else if (b->on_click == m_play) {
                b->icon = ED.play == ED_EDIT ? UI_ICON_PLAY : UI_ICON_STOP;
                b->selected = ED.play != ED_EDIT;
                b->bg_active = ED.play != ED_EDIT ? ui_theme(ED.ui)->success : 0;
                lx_strcpy(b->tooltip, sizeof b->tooltip, ED.play == ED_EDIT ? "Play (Ctrl+P)" : "Stop (Ctrl+P)");
            } else if (b->on_click == m_pause) b->selected = ED.play == ED_PAUSED;
            else if (b->on_click == m_build_current) {
                bool busy = lx_atomic_load(&ED.building) != 0;
                b->disabled = busy;
                ui_set_text(b, "%s", busy ? "Building..." : "Build");
            }
        }
    }
}

static void status_frame(UiNode *n) {
    const LxRenderStats *st = lx_render_stats();
    UiNode *msg = n->first, *stats = n->last;
    if (ED.log_count) {
        const EdLogLine *l = &ED.log[(ED.log_start + ED.log_count - 1) % ED_MAX_LOG];
        ui_set_text(msg, "%s", l->text);
        msg->icon = l->level == LX_LOG_ERROR ? UI_ICON_ERROR : l->level == LX_LOG_WARN ? UI_ICON_WARNING : UI_ICON_INFO;
        msg->icon_color = l->level == LX_LOG_ERROR ? ui_theme(ED.ui)->danger : l->level == LX_LOG_WARN ? ui_theme(ED.ui)->warning : ui_theme(ED.ui)->text_dim;
    }
    if (lx_atomic_load(&ED.building)) ui_set_text(msg, "%s", ED.build_status);
    char tris[32];
    if (st->triangles > 1000000) snprintf(tris, sizeof tris, "%.2fM", (double)st->triangles / 1e6);
    else snprintf(tris, sizeof tris, "%.1fK", (double)st->triangles / 1e3);
    ui_set_text(stats, "%.0f FPS   %d draws   %s tris   %d photons   %d entities   %s", (double)ED.fps, st->draw_calls, tris,
                st->gi_photons_traced, ED.scene ? lx_scene_entity_count(ED.scene) : 0, lx_audio_backend_name());
}

static UiNode *tool(UiNode *parent, UiIcon icon, const char *tip, void (*cb)(UiNode *), intptr_t tag) {
    UiNode *b = ui_icon_button(ED.ui, parent, icon, tip, cb);
    b->tag = tag;
    b->w = 30;
    b->h = 28;
    b->icon_size = 17;
    b->bg_active = ui_theme(ED.ui)->selection;
    return b;
}

void ed_build_shell(void) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *root = ui_root(ui);
    root->dir = UI_COL;
    ED.root = root;

    /* title bar: app icon, menus, draggable title, window controls */
    UiNode *mb = ed_titlebar_begin(root);
    static const char *names[] = {"File", "Edit", "Create", "Window", "Build", "Play", "Help"};
    void (*cbs[])(UiNode *) = {file_menu, edit_menu, create_menu, window_menu, build_menu, play_menu, help_menu};
    for (int i = 0; i < 7; i++) {
        UiNode *b = ui_button(ui, mb, names[i], cbs[i]);
        b->bg = 0;
        b->h = 24;
    }
    ed_titlebar_end(mb, "");
    ED.menubar = mb;

    /* toolbar */
    UiNode *tb = ui_box(ui, root, UI_ROW);
    tb->h = 40;
    tb->bg = t->panel;
    ui_pad(tb, 8, 0, 8, 0);
    tb->cross_align = UI_ALIGN_CENTER;
    tb->on_frame = toolbar_frame;
    UiNode *left = ui_box(ui, tb, UI_ROW);
    left->gap = 2;
    left->h = 30;
    left->cross_align = UI_ALIGN_CENTER;
    tool(left, UI_ICON_SELECT, "Select (Q)", tb_gizmo, ED_GIZMO_SELECT);
    tool(left, UI_ICON_MOVE, "Move (W)", tb_gizmo, ED_GIZMO_MOVE);
    tool(left, UI_ICON_ROTATE, "Rotate (E)", tb_gizmo, ED_GIZMO_ROTATE);
    tool(left, UI_ICON_SCALE, "Scale (R)", tb_gizmo, ED_GIZMO_SCALE);
    ui_separator(ui, left)->w = 12;
    tool(left, UI_ICON_GLOBE, "Global space", tb_space, 0);
    tool(left, UI_ICON_GRID, "Snap to grid", tb_snap, 0)->selected = ED.snap;
    tool(left, UI_ICON_LAYERS, "Show grid", tb_grid, 0);
    UiNode *l1 = ui_spacer(ui, tb);
    LX_UNUSED(l1);
    UiNode *mid = ui_box(ui, tb, UI_ROW);
    mid->gap = 3;
    mid->h = 30;
    mid->bg = t->field;
    mid->radius = 6;
    ui_pad(mid, 3, 1, 3, 1);
    UiNode *play = tool(mid, UI_ICON_PLAY, "Play (Ctrl+P)", m_play, 0);
    play->icon_color = t->success;
    tool(mid, UI_ICON_PAUSE, "Pause", m_pause, 0);
    tool(mid, UI_ICON_STEP, "Step one frame", m_step, 0);
    ui_spacer(ui, tb);
    UiNode *right = ui_box(ui, tb, UI_ROW);
    right->gap = 6;
    right->h = 30;
    UiNode *settings = ui_button(ui, right, "", m_project_settings);
    settings->icon = UI_ICON_GEAR;
    lx_strcpy(settings->tooltip, sizeof settings->tooltip, "Project Settings");
    settings->w = 32;
    settings->bg = 0;
    UiNode *build = ui_button(ui, right, "Build", m_build_current);
    build->icon = UI_ICON_BUILD;
    build->w = 110;
    build->bg = t->accent;
    build->bg_hover = t->accent_hover;
    build->fg = t->accent_text;
    build->tag = 0;
    lx_strcpy(build->tooltip, sizeof build->tooltip, "Build the game for the platform selected in Build Settings (Ctrl+B)");
    ED.toolbar = tb;

    /* main area: hierarchy | (viewport / bottom tabs) | inspector */
    UiNode *outer = ui_split(ui, root, UI_ROW, 0.17f);
    outer->split_min = 180;
    ED.hierarchy = ed_hierarchy_panel(outer);
    UiNode *right_split = ui_split(ui, outer, UI_ROW, 0.76f);
    right_split->split_min = 260;
    UiNode *center = ui_split(ui, right_split, UI_COL, 0.68f);
    center->split_min = 140;
    ED.viewport = ed_viewport_panel(center);
    UiNode *bottom = ui_tabs(ui, center);
    ED.bottom_tabs = bottom;
    ED.content_panel = ed_content_panel(bottom);
    ED.console_panel = ed_console_panel(bottom);
    ED.mixer_panel = ed_mixer_panel(bottom);
    ED.scenes_panel = ed_scenes_panel(bottom);
    UiNode *insp = ui_tabs(ui, right_split);
    ED.inspector_tabs = insp;
    ED.inspector = ed_inspector_panel(insp);
    ED.lighting_panel = ed_lighting_panel(insp);
    ED.stats_panel = ed_stats_panel(insp);

    /* status bar */
    UiNode *sb = ui_box(ui, root, UI_ROW);
    sb->h = 24;
    sb->bg = t->header;
    ui_pad(sb, 10, 0, 10, 0);
    sb->gap = 10;
    sb->on_frame = status_frame;
    UiNode *msg = ui_label(ui, sb, "Ready");
    msg->font = UI_FONT_SMALL;
    msg->grow = 1;
    msg->icon = UI_ICON_INFO;
    msg->icon_size = 13;
    msg->fg = t->text_dim;
    UiNode *stats = ui_label(ui, sb, "");
    stats->font = UI_FONT_SMALL;
    stats->fg = t->text_dim;
    stats->w = 560;
    ED.status = sb;
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ED.content_dirty = true;
}

/* ======================================================================
 *  per-frame update
 * ====================================================================== */
void ed_shortcuts(void) {
    if (ui_wants_keyboard(ED.ui) || ui_has_modal(ED.ui)) return;
    bool ctrl = lx_key_down(LX_KEY_CTRL), shift = lx_key_down(LX_KEY_SHIFT), alt = lx_key_down(LX_KEY_ALT);
    bool looking = lx_mouse_down(LX_MOUSE_RIGHT);
    if (ctrl) {
        if (lx_key_pressed(LX_KEY_S)) { if (shift) ed_save_scene_as_dialog(); else ed_save_scene(); }
        if (lx_key_pressed(LX_KEY_Z)) { if (shift) ed_redo(); else ed_undo(); }
        if (lx_key_pressed(LX_KEY_Y)) ed_redo();
        if (lx_key_pressed(LX_KEY_D)) ed_duplicate_selected();
        if (lx_key_pressed(LX_KEY_N)) m_new_scene(NULL);
        if (lx_key_pressed(LX_KEY_P)) { if (alt) ed_step(); else if (shift) ed_pause(); else m_play(NULL); }
        if (lx_key_pressed(LX_KEY_B)) { if (shift) ed_build_settings(); else ed_build(ED.project.build.target, false); }
        if (lx_key_pressed(LX_KEY_R) && shift) ed_build(ED.project.build.target, true);
        return;
    }
    if (ED.play == ED_PLAYING) return; /* the game owns the keyboard while playing */
    if (!looking) {
        if (lx_key_pressed(LX_KEY_Q)) ED.gizmo = ED_GIZMO_SELECT;
        if (lx_key_pressed(LX_KEY_W)) ED.gizmo = ED_GIZMO_MOVE;
        if (lx_key_pressed(LX_KEY_E)) ED.gizmo = ED_GIZMO_ROTATE;
        if (lx_key_pressed(LX_KEY_R)) ED.gizmo = ED_GIZMO_SCALE;
        if (lx_key_pressed(LX_KEY_F)) ed_focus_selected();
    }
    if (lx_key_pressed(LX_KEY_DELETE)) ed_delete_selected();
    if (lx_key_pressed(LX_KEY_F2)) ed_rename_dialog(ED.selected);
    if (lx_key_pressed(LX_KEY_F1)) ed_shortcuts_help();
    if (lx_key_pressed(LX_KEY_ESCAPE) && !ui_popup_open(ED.ui)) ed_select(0);
}

void ed_update(float dt) {
    {   /* thumbnails finished in the background: show them (the Content Browser redraws at most 5 times a second) */
        static double last_refresh;
        static bool waiting;
        if (ed_thumbs_poll()) waiting = true;
        if (waiting && lx_time() - last_refresh > 0.2) {
            waiting = false;
            last_refresh = lx_time();
            ED.content_dirty = true;
            if (ED.selected_asset[0] && ed_icon_for_file(ED.selected_asset, false) == UI_ICON_TEXTURE) ED.inspector_dirty = true;
        }
    }
    ed_build_poll();
    if (ED.play == ED_EDIT) ed_scripts_poll();
    ed_materials_flush(false);
    if (!ED.scene) return;
    ed_shortcuts();
    /* edit gestures end when the mouse is released and no field is being typed in */
    if (ED.change_pending && !lx_mouse_down(LX_MOUSE_LEFT) && !ui_wants_keyboard(ED.ui)) ed_commit();
    if (ED.viewport) /* the game's HUD is laid out over the viewport */
        lx_game_ui_begin_frame(ED.viewport->x, ED.viewport->y, ED.viewport->cw, ED.viewport->ch, ED.play == ED_PLAYING);
    if (ED.play == ED_PLAYING) lx_scene_simulate(ED.scene, dt);
    else lx_scene_update_transforms(ED.scene);
    lx_audio_update(ED.scene, dt);
    static bool was_dirty;
    if (was_dirty != ED.dirty) { was_dirty = ED.dirty; ed_update_title(); }
}
