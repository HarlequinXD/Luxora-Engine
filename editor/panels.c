/* Editor panels: Hierarchy, Inspector, Lighting, Statistics, Content Browser, Console,
 * Audio Mixer and Scene Manager. */
#include "editor.h"
#include <ctype.h>

/* ======================================================================
 *  icons & colours
 * ====================================================================== */
uint32_t ed_color_for_icon(UiIcon icon) {
    switch (icon) {
    case UI_ICON_FOLDER: case UI_ICON_FOLDER_OPEN: return UI_HEX(0xe8b84a);
    case UI_ICON_SCENE: return UI_HEX(0x58a6ff);
    case UI_ICON_MESH: case UI_ICON_CUBE: case UI_ICON_SPHERE: case UI_ICON_PLANE: case UI_ICON_CYLINDER: case UI_ICON_CAPSULE: return UI_HEX(0x7fb3ff);
    case UI_ICON_MATERIAL: return UI_HEX(0xd2a8ff);
    case UI_ICON_TEXTURE: return UI_HEX(0x56d4a8);
    case UI_ICON_SOUND: case UI_ICON_SPEAKER: case UI_ICON_LISTENER: return UI_HEX(0xffa657);
    case UI_ICON_SCRIPT: case UI_ICON_BEHAVIOUR: return UI_HEX(0x7ee787);
    case UI_ICON_CAMERA: return UI_HEX(0x79c0ff);
    case UI_ICON_LIGHT: case UI_ICON_SUN: case UI_ICON_BULB: case UI_ICON_SPOT: return UI_HEX(0xf2cc60);
    case UI_ICON_CROWD: return UI_HEX(0xff7b72);
    case UI_ICON_PROBE: return UI_HEX(0x8be9fd);
    case UI_ICON_COLLIDER: return UI_HEX(0x7ee787);
    case UI_ICON_RIGIDBODY: return UI_HEX(0xffa657);
    case UI_ICON_CHARACTER: return UI_HEX(0x79c0ff);
    case UI_ICON_PLAY: return UI_HEX(0x3fb950);
    case UI_ICON_TRASH: case UI_ICON_ERROR: return UI_HEX(0xe5534b);
    case UI_ICON_WARNING: return UI_HEX(0xe3b341);
    case UI_ICON_GAMEPAD: case UI_ICON_KEYBOARD: case UI_ICON_MOUSE: case UI_ICON_TOUCH: return UI_HEX(0xa5d6ff);
    default: return UI_HEX(0xb1b5bd);
    }
}

UiIcon ed_icon_for_entity(LxScene *s, LxEntity e) {
    uint32_t c = lx_entity_components(s, e);
    if (c & LX_COMP_CAMERA) return UI_ICON_CAMERA;
    if (c & LX_COMP_LIGHT) {
        LxLight *l = lx_get_light(s, e);
        return l->type == LX_LIGHT_DIRECTIONAL ? UI_ICON_SUN : l->type == LX_LIGHT_SPOT ? UI_ICON_SPOT : UI_ICON_BULB;
    }
    if (c & LX_COMP_CROWD) return UI_ICON_CROWD;
    if (c & LX_COMP_CHARACTER) return UI_ICON_CHARACTER;
    if (c & LX_COMP_REFLECTION_PROBE) return UI_ICON_PROBE;
    if (c & LX_COMP_MESH) {
        const char *m = lx_get_mesh(s, e)->mesh;
        if (strstr(m, "sphere")) return UI_ICON_SPHERE;
        if (strstr(m, "plane")) return UI_ICON_PLANE;
        if (strstr(m, "cylinder")) return UI_ICON_CYLINDER;
        if (strstr(m, "capsule")) return UI_ICON_CAPSULE;
        if (strstr(m, "cube")) return UI_ICON_CUBE;
        return UI_ICON_MESH;
    }
    if (c & LX_COMP_AUDIO_SOURCE) return UI_ICON_SPEAKER;
    if (c & LX_COMP_AUDIO_LISTENER) return UI_ICON_LISTENER;
    if (c & LX_COMP_COLLIDER) return UI_ICON_COLLIDER;
    return UI_ICON_ENTITY;
}

static bool ext_is(const char *name, const char *ext) {
    const char *e = lx_path_ext(name);
    for (; *e && *ext; e++, ext++) if (tolower((unsigned char)*e) != *ext) return false;
    return !*e && !*ext;
}

UiIcon ed_icon_for_file(const char *name, bool dir) {
    if (dir) return UI_ICON_FOLDER;
    if (ext_is(name, ".lxscene")) return UI_ICON_SCENE;
    if (ext_is(name, ".lxmat")) return UI_ICON_MATERIAL;
    if (ext_is(name, ".obj")) return UI_ICON_MESH;
    if (ext_is(name, ".png") || ext_is(name, ".tga") || ext_is(name, ".bmp")) return UI_ICON_TEXTURE;
    if (ext_is(name, ".wav")) return UI_ICON_SOUND;
    if (ext_is(name, ".c") || ext_is(name, ".h")) return UI_ICON_SCRIPT;
    if (ext_is(name, ".lxprefab")) return UI_ICON_PREFAB;
    return UI_ICON_FILE;
}

static const char *drag_type_for(UiIcon icon) {
    switch (icon) {
    case UI_ICON_SCENE: return "scene";
    case UI_ICON_MATERIAL: return "material";
    case UI_ICON_MESH: return "mesh";
    case UI_ICON_TEXTURE: return "texture";
    case UI_ICON_SOUND: return "sound";
    case UI_ICON_SCRIPT: return "script";
    case UI_ICON_FOLDER: return "folder";
    default: return "file";
    }
}

static bool contains_ci(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return true;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

/* ======================================================================
 *  console log (thread safe: builds and audio log from other threads)
 * ====================================================================== */
static LxMutex *g_log_lock;

void ed_log_sink(LxLogLevel level, const char *msg, void *user) {
    LX_UNUSED(user);
    if (!g_log_lock) g_log_lock = lx_mutex_create();
    lx_mutex_lock(g_log_lock);
    int idx;
    if (ED.log_count < ED_MAX_LOG) idx = (ED.log_start + ED.log_count++) % ED_MAX_LOG;
    else { idx = ED.log_start; ED.log_start = (ED.log_start + 1) % ED_MAX_LOG; }
    EdLogLine *l = &ED.log[idx];
    l->level = level;
    l->time = lx_time();
    lx_strcpy(l->text, sizeof l->text, msg);
    if (level == LX_LOG_ERROR) ED.errors++;
    else if (level == LX_LOG_WARN) ED.warnings++;
    ED.console_dirty = true;
    lx_mutex_unlock(g_log_lock);
}

/* ======================================================================
 *  HIERARCHY
 * ====================================================================== */
static bool is_expanded(LxEntity e) {
    for (int i = 0; i < ED.expanded_count; i++) if (ED.expanded[i] == e) return true;
    return false;
}
static void toggle_expand(UiNode *n) {
    LxEntity e = (LxEntity)n->tag;
    for (int i = 0; i < ED.expanded_count; i++)
        if (ED.expanded[i] == e) { ED.expanded[i] = ED.expanded[--ED.expanded_count]; ED.hierarchy_hash = 0; return; }
    if (ED.expanded_count < 256) ED.expanded[ED.expanded_count++] = e;
    ED.hierarchy_hash = 0;
}
static void row_click(UiNode *n) { ed_select((LxEntity)n->tag); }
static void row_double(UiNode *n) { ed_select((LxEntity)n->tag); ed_focus_selected(); }
static void row_toggle_active(UiNode *n) {
    LxEntity e = (LxEntity)n->tag;
    lx_entity_set_active(ED.scene, e, !lx_entity_active(ED.scene, e));
    ED.hierarchy_hash = 0;
    ED.inspector_dirty = true;
    ed_mark_changed();
    ed_commit();
}

static LxEntity g_ctx_entity;
static void ctx_rename(UiNode *n) { LX_UNUSED(n); ed_rename_dialog(g_ctx_entity); }
static void ctx_duplicate(UiNode *n) { LX_UNUSED(n); ed_select(g_ctx_entity); ed_duplicate_selected(); }
static void ctx_delete(UiNode *n) { LX_UNUSED(n); ed_select(g_ctx_entity); ed_delete_selected(); }
static void ctx_focus(UiNode *n) { LX_UNUSED(n); ed_select(g_ctx_entity); ed_focus_selected(); }
static void ctx_unparent(UiNode *n) {
    LX_UNUSED(n);
    lx_entity_set_parent(ED.scene, g_ctx_entity, 0);
    ED.hierarchy_hash = 0;
    ed_mark_changed();
    ed_commit();
}
static void ctx_create_child(UiNode *n) {
    LxEntity parent = g_ctx_entity;
    LxEntity e = ed_create(n->text);
    if (e && parent) {
        lx_entity_set_parent(ED.scene, e, parent);
        lx_entity_set_position(ED.scene, e, lx_v3(0, 0, 0));
        if (!is_expanded(parent) && ED.expanded_count < 256) ED.expanded[ED.expanded_count++] = parent;
        ED.hierarchy_hash = 0;
        ed_mark_changed();
        ed_commit();
    }
}

static void row_menu(UiNode *n) {
    g_ctx_entity = (LxEntity)n->tag;
    ed_select(g_ctx_entity);
    LxVec2 m = lx_mouse_pos();
    UiNode *p = ui_popup(ED.ui, m.x, m.y, 210);
    UiNode *it;
    it = ui_menu_item(ED.ui, p, "Rename", "F2", ctx_rename); it->icon = UI_ICON_FILE;
    it = ui_menu_item(ED.ui, p, "Duplicate", "Ctrl+D", ctx_duplicate); it->icon = UI_ICON_COPY;
    it = ui_menu_item(ED.ui, p, "Focus", "F", ctx_focus); it->icon = UI_ICON_FOCUS;
    if (lx_entity_parent(ED.scene, g_ctx_entity)) { it = ui_menu_item(ED.ui, p, "Move to Root", NULL, ctx_unparent); it->icon = UI_ICON_UP; }
    ui_menu_separator(ED.ui, p);
    static const char *kids[] = {"Empty", "Cube", "Sphere", "Point Light", "Audio Source"};
    static const UiIcon kid_icons[] = {UI_ICON_ENTITY, UI_ICON_CUBE, UI_ICON_SPHERE, UI_ICON_BULB, UI_ICON_SPEAKER};
    for (int i = 0; i < 5; i++) {
        char t[64];
        snprintf(t, sizeof t, "%s", kids[i]);
        it = ui_menu_item(ED.ui, p, t, "child", ctx_create_child);
        it->icon = kid_icons[i];
        it->icon_color = ed_color_for_icon(kid_icons[i]);
    }
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "Delete", "Del", ctx_delete); it->icon = UI_ICON_TRASH; it->icon_color = ui_theme(ED.ui)->danger;
}

static void row_drop(UiNode *n, const char *type, const char *payload) {
    LxEntity target = (LxEntity)n->tag;
    if (!strcmp(type, "entity")) {
        LxEntity e = (LxEntity)strtoul(payload, NULL, 10);
        if (e && e != target && lx_entity_valid(ED.scene, e)) {
            /* keep the world position when re-parenting */
            LxVec3 wp = lx_entity_world_position(ED.scene, e);
            lx_entity_set_parent(ED.scene, e, target);
            if (lx_entity_parent(ED.scene, e) == target) {
                LxMat4 inv = lx_mat4_inverse(lx_entity_world_matrix(ED.scene, target));
                lx_entity_set_position(ED.scene, e, lx_mat4_point(inv, wp));
                if (!is_expanded(target) && ED.expanded_count < 256) ED.expanded[ED.expanded_count++] = target;
            }
            ED.hierarchy_hash = 0;
            ed_mark_changed();
            ed_commit();
        }
    } else if (!strcmp(type, "material")) {
        LxMeshRenderer *mr = lx_get_mesh(ED.scene, target);
        if (mr) { lx_strcpy(mr->material, sizeof mr->material, payload); ED.inspector_dirty = true; ed_mark_changed(); ed_commit(); }
    }
}

static void list_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(n);
    if (!strcmp(type, "entity")) {
        LxEntity e = (LxEntity)strtoul(payload, NULL, 10);
        if (e && lx_entity_valid(ED.scene, e) && lx_entity_parent(ED.scene, e)) {
            LxVec3 wp = lx_entity_world_position(ED.scene, e);
            lx_entity_set_parent(ED.scene, e, 0);
            lx_entity_set_position(ED.scene, e, wp);
            ED.hierarchy_hash = 0;
            ed_mark_changed();
            ed_commit();
        }
    } else if (!strcmp(type, "mesh")) {
        LxEntity e = ed_create("Empty");
        lx_add_mesh(ED.scene, e, payload, "builtin:default");
        lx_entity_set_name(ED.scene, e, lx_path_filename(payload));
        ed_mark_changed();
        ed_commit();
    }
}

static void list_menu(UiNode *n) {
    LX_UNUSED(n);
    extern void ed_create_menu_at(float x, float y);
    LxVec2 m = lx_mouse_pos();
    ed_create_menu_at(m.x, m.y);
}

static uint64_t hierarchy_hash(void) {
    LxScene *s = ED.scene;
    uint64_t h = 1469598103934665603ull;
    int n = lx_scene_entity_count(s);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(s, i);
        const char *name = lx_entity_name(s, e);
        h = (h ^ e) * 1099511628211ull;
        h = (h ^ lx_entity_parent(s, e)) * 1099511628211ull;
        h = (h ^ lx_entity_next_sibling(s, e)) * 1099511628211ull;
        h = (h ^ (uint64_t)lx_entity_active(s, e)) * 1099511628211ull;
        h = (h ^ lx_entity_components(s, e)) * 1099511628211ull;
        for (const char *c = name; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    }
    for (const char *c = ED.hierarchy_search; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    h ^= (uint64_t)(uintptr_t)s;
    for (int i = 0; i < ED.expanded_count; i++) h = (h ^ ED.expanded[i]) * 1099511628211ull;
    return h ? h : 1;
}

static bool subtree_matches(LxEntity e) {
    if (contains_ci(lx_entity_name(ED.scene, e), ED.hierarchy_search)) return true;
    for (LxEntity c = lx_entity_first_child(ED.scene, e); c; c = lx_entity_next_sibling(ED.scene, c))
        if (subtree_matches(c)) return true;
    return false;
}

static void add_rows(UiNode *list, LxEntity e, int depth) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxScene *s = ED.scene;
    if (!subtree_matches(e)) return;
    bool searching = ED.hierarchy_search[0] != 0;
    LxEntity first_child = lx_entity_first_child(s, e);
    bool open = searching || is_expanded(e);
    UiNode *row = ui_box(ui, list, UI_ROW);
    row->h = 24;
    row->tag = (intptr_t)e;
    row->bg_hover = t->hover;
    row->radius = 4;
    row->on_click = row_click;
    row->on_double_click = row_double;
    row->on_right_click = row_menu;
    row->drag_type = "entity";
    snprintf(row->drag_payload, sizeof row->drag_payload, "%u", e);
    lx_strcpy(row->text, sizeof row->text, lx_entity_name(s, e));
    row->accept = "entity,material";
    row->on_drop = row_drop;
    row->selected = e == ED.selected;
    row->cross_align = UI_ALIGN_CENTER;
    ui_pad(row, 4 + depth * 16.0f, 0, 4, 0);
    row->gap = 4;
    UiNode *chev = ui_button(ui, row, "", first_child ? toggle_expand : NULL);
    chev->icon = first_child ? (open ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_RIGHT) : UI_ICON_NONE;
    chev->icon_size = 11;
    chev->w = 16;
    chev->h = 18;
    chev->tag = (intptr_t)e;
    chev->fg = t->text_dim;
    chev->bg = 0;
    if (!first_child) chev->disabled = true;
    bool active = lx_entity_active(s, e);
    UiIcon icon = ed_icon_for_entity(s, e);
    UiNode *l = ui_label(ui, row, lx_entity_name(s, e));
    l->icon = icon;
    l->icon_size = 15;
    l->icon_color = active ? ed_color_for_icon(icon) : t->text_disabled;
    l->fg = active ? t->text : t->text_disabled;
    l->grow = 1;
    UiNode *eye = ui_icon_button(ui, row, active ? UI_ICON_EYE : UI_ICON_EYE_OFF, active ? "Disable" : "Enable", row_toggle_active);
    eye->tag = (intptr_t)e;
    eye->w = 22;
    eye->h = 20;
    eye->icon_size = 13;
    eye->fg = active ? ui_alpha(t->text_dim, 0.6f) : t->text_disabled;
    if (open)
        for (LxEntity c = first_child; c; c = lx_entity_next_sibling(s, c)) add_rows(list, c, depth + 1);
}

static void hierarchy_frame(UiNode *list) {
    if (!ED.scene) return;
    uint64_t h = hierarchy_hash();
    if (h == ED.hierarchy_hash) {
        /* cheap selection refresh without rebuilding */
        for (UiNode *r = list->first; r; r = r->next) r->selected = (LxEntity)r->tag == ED.selected && r->tag;
        return;
    }
    ED.hierarchy_hash = h;
    ui_clear(ED.ui, list);
    UiNode *scene_row = ui_label(ED.ui, list, lx_scene_name(ED.scene));
    scene_row->icon = UI_ICON_SCENE;
    scene_row->icon_color = ed_color_for_icon(UI_ICON_SCENE);
    scene_row->font = UI_FONT_BOLD;
    scene_row->h = 26;
    ui_pad(scene_row, 6, 0, 0, 0);
    for (LxEntity r = lx_scene_root_first(ED.scene); r; r = lx_entity_next_sibling(ED.scene, r)) add_rows(list, r, 0);
    UiNode *tail = ui_box(ED.ui, list, UI_COL);
    tail->h = 60;
    tail->accept = "entity,mesh";
    tail->on_drop = list_drop;
    tail->on_right_click = list_menu;
}

static void hierarchy_search_changed(UiNode *n) { LX_UNUSED(n); ED.hierarchy_hash = 0; }
static void hier_add(UiNode *n) { extern void ed_create_menu_at(float x, float y); ed_create_menu_at(n->x, n->y + n->ch); }

UiNode *ed_hierarchy_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *p = ui_box(ui, parent, UI_COL);
    p->bg = t->panel;
    UiNode *h = ed_panel_header(p, UI_ICON_HIERARCHY, "Hierarchy");
    UiNode *add = ui_icon_button(ui, h, UI_ICON_PLUS, "Create (right-click the list for more)", hier_add);
    add->w = 24;
    add->h = 24;
    UiNode *sbox = ui_box(ui, p, UI_COL);
    ui_pad(sbox, 6, 6, 6, 4);
    UiNode *search = ui_textfield(ui, sbox, ED.hierarchy_search, sizeof ED.hierarchy_search);
    search->h = 24;
    search->icon = UI_ICON_SEARCH;
    lx_strcpy(search->hint, sizeof search->hint, "Search");
    search->on_change = hierarchy_search_changed;
    search->tag = 0x6c697665;
    UiNode *list = ui_box(ui, p, UI_COL);
    list->grow = 1;
    list->scroll = true;
    ui_pad(list, 4, 2, 4, 4);
    list->gap = 1;
    list->on_frame = hierarchy_frame;
    list->accept = "entity,mesh";
    list->on_drop = list_drop;
    list->on_right_click = list_menu;
    ED.hierarchy_list = list;
    return p;
}

/* ======================================================================
 *  INSPECTOR
 * ====================================================================== */
static bool g_open_transform = true, g_open_camera = true, g_open_light = true, g_open_mesh = true, g_open_audio = true,
            g_open_listener = true, g_open_behaviour = true, g_open_crowd = true, g_open_material = true;
static bool g_open_probe = true;
static void probe_recapture(UiNode *n) { LX_UNUSED(n); lx_reflections_refresh(); ui_notify(ED.ui, "Reflection probes re-capturing"); }
static const char *LIGHT_TYPES[] = {"Directional", "Point", "Spot"};
static const char *COLLIDER_SHAPES[] = {"Box", "Sphere", "Capsule", "Mesh (static)"};
static bool g_open_collider = true, g_open_body = true, g_open_character = true;
static const char *BUSES[] = {"SFX", "Music", "Voice", "Ambient"};
static const char *g_behaviour_names[64];
static int g_behaviour_index;

static void transform_changed(UiNode *n) {
    LX_UNUSED(n);
    LxEntityData *d = lx_entity_data(ED.scene, ED.inspector_entity);
    if (d) lx_entity_set_euler(ED.scene, ED.inspector_entity, d->euler);
    ed_mark_changed();
}
static void name_changed(UiNode *n) { LX_UNUSED(n); ED.hierarchy_hash = 0; ed_mark_changed(); ed_commit(); }
static void active_changed(UiNode *n) { LX_UNUSED(n); ED.hierarchy_hash = 0; lx_scene_mark_static_dirty(ED.scene); ed_mark_changed(); ed_commit(); }
static void structural_changed(UiNode *n) { LX_UNUSED(n); ed_mark_changed(); ed_commit(); ED.inspector_dirty = true; ED.hierarchy_hash = 0; }

static LxComponentBits g_menu_comp;
static void remove_comp(UiNode *n) {
    LX_UNUSED(n);
    lx_remove_component(ED.scene, ED.inspector_entity, g_menu_comp);
    ED.inspector_dirty = true;
    ED.hierarchy_hash = 0;
    ed_mark_changed();
    ed_commit();
}
static void comp_menu(UiNode *n) {
    g_menu_comp = (LxComponentBits)n->parent->parent->tag;
    UiNode *p = ui_popup(ED.ui, n->x - 140, n->y + n->ch, 170);
    UiNode *it = ui_menu_item(ED.ui, p, "Remove Component", NULL, remove_comp);
    it->icon = UI_ICON_TRASH;
    it->icon_color = ui_theme(ED.ui)->danger;
}

static UiNode *comp_section(UiNode *body, UiIcon icon, const char *title, bool *open, LxComponentBits bit) {
    UiNode *s = ed_section(body, icon, title, open, bit ? comp_menu : NULL);
    s->parent->tag = bit; /* section wrapper remembers the component */
    return s;
}

static void asset_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(type);
    if (n->s) lx_strcpy(n->s, (size_t)n->s_cap, payload);
    ED.inspector_dirty = true;
    ed_mark_changed();
    ed_commit();
}

static char *g_pick_target;
static int g_pick_cap;
static void pick_asset(UiNode *n) {
    if (g_pick_target) lx_strcpy(g_pick_target, (size_t)g_pick_cap, n->text[0] == '(' ? "" : n->hint[0] ? n->hint : n->text);
    ED.inspector_dirty = true;
    lx_scene_mark_static_dirty(ED.scene);
    ed_mark_changed();
    ed_commit();
}

static void collect_assets(UiNode *popup, const char *dir, const char *rel, const char *const *exts, int n_ext, int *count) {
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n && *count < 200; i++) {
        if (e[i].name[0] == '.') continue;
        char full[LX_PATH_MAX], r[LX_REF_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, e[i].name);
        else lx_strcpy(r, sizeof r, e[i].name);
        if (e[i].is_dir) { collect_assets(popup, full, r, exts, n_ext, count); continue; }
        for (int k = 0; k < n_ext; k++)
            if (ext_is(e[i].name, exts[k])) {
                UiNode *it = ui_menu_item(ED.ui, popup, r, NULL, pick_asset);
                it->icon = ed_icon_for_file(e[i].name, false);
                it->icon_color = ed_color_for_icon(it->icon);
                (*count)++;
            }
    }
    lx_free(e);
}

static void browse_assets(UiNode *n) {
    UiNode *field = n->user;
    g_pick_target = field->s;
    g_pick_cap = field->s_cap;
    const char *kind = field->accept;
    UiNode *p = ui_popup(ED.ui, field->x, field->y + field->ch, field->cw / ui_scale(ED.ui) + 30);
    int count = 0;
    if (!strcmp(kind, "mesh")) {
        static const char *b[] = {"builtin:cube", "builtin:sphere", "builtin:plane", "builtin:cylinder", "builtin:capsule"};
        for (int i = 0; i < 5; i++) { UiNode *it = ui_menu_item(ED.ui, p, b[i], NULL, pick_asset); it->icon = UI_ICON_MESH; it->icon_color = ed_color_for_icon(UI_ICON_MESH); }
        static const char *ext[] = {".obj"};
        collect_assets(p, ED.project.assets, "", ext, 1, &count);
    } else if (!strcmp(kind, "material")) {
        static const char *b[] = {"builtin:default", "builtin:metal", "builtin:red", "builtin:green", "builtin:blue", "builtin:emissive", "builtin:concrete", "builtin:fabric", "builtin:grid"};
        for (int i = 0; i < 9; i++) { UiNode *it = ui_menu_item(ED.ui, p, b[i], NULL, pick_asset); it->icon = UI_ICON_MATERIAL; it->icon_color = ed_color_for_icon(UI_ICON_MATERIAL); }
        static const char *ext[] = {".lxmat"};
        collect_assets(p, ED.project.assets, "", ext, 1, &count);
    } else if (!strcmp(kind, "sound")) {
        ui_menu_item(ED.ui, p, "(none)", NULL, pick_asset);
        static const char *ext[] = {".wav"};
        collect_assets(p, ED.project.assets, "", ext, 1, &count);
        if (!count) { UiNode *it = ui_menu_item(ED.ui, p, "Add .wav files to Assets/Sounds", NULL, NULL); it->disabled = true; }
    } else if (!strcmp(kind, "texture")) {
        ui_menu_item(ED.ui, p, "(none)", NULL, pick_asset);
        static const char *ext[] = {".png", ".tga", ".bmp"};
        collect_assets(p, ED.project.assets, "", ext, 3, &count);
    }
}

static UiNode *asset_field(UiNode *parent, const char *label, char *buf, int cap, const char *kind) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *tf = ui_textfield(ED.ui, row, buf, cap);
    tf->grow = 1;
    tf->h = 22;
    tf->accept = kind;
    tf->on_drop = asset_drop;
    tf->on_change = structural_changed;
    tf->icon = !strcmp(kind, "mesh") ? UI_ICON_MESH : !strcmp(kind, "material") ? UI_ICON_MATERIAL : !strcmp(kind, "sound") ? UI_ICON_SOUND : UI_ICON_TEXTURE;
    lx_strcpy(tf->hint, sizeof tf->hint, "Drop an asset or browse");
    UiNode *b = ui_icon_button(ED.ui, row, UI_ICON_DOTS, "Browse", browse_assets);
    b->user = tf;
    b->w = 24;
    b->h = 22;
    b->bg = ui_theme(ED.ui)->panel_alt;
    return tf;
}

static UiNode *float_row(UiNode *parent, const char *label, float *v, float step, float minv, float maxv) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_number(ED.ui, row, v, step);
    n->grow = 1;
    n->h = 22;
    n->minv = minv;
    n->maxv = maxv;
    n->on_change = ed_field_changed;
    return n;
}
static UiNode *slider_row(UiNode *parent, const char *label, float *v, float minv, float maxv) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_slider(ED.ui, row, v, minv, maxv);
    n->grow = 1;
    n->h = 22;
    n->on_change = ed_field_changed;
    return n;
}
static UiNode *bool_row(UiNode *parent, const char *label, bool *v) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_checkbox(ED.ui, row, "", v);
    n->on_change = ed_field_changed;
    return n;
}
static UiNode *color_row(UiNode *parent, const char *label, float *rgb, float maxv) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_color(ED.ui, row, rgb, false);
    n->grow = 1;
    n->h = 22;
    n->maxv = maxv;
    n->on_change = ed_field_changed;
    return n;
}

/* one preview button (asset or Audio Source) at a time: it toggles Preview <-> Stop */
static UiNode *g_preview_btn;
static void preview_button_sync(void) {
    if (!g_preview_btn) return;
    bool playing = lx_audio_preview_playing();
    UiIcon want = playing ? UI_ICON_STOP : UI_ICON_PLAY;
    if (g_preview_btn->icon != want) {
        g_preview_btn->icon = want;
        ui_set_text(g_preview_btn, "%s", playing ? "Stop" : "Preview");
        g_preview_btn->icon_color = playing ? ui_theme(ED.ui)->danger : ui_theme(ED.ui)->success;
    }
}
static void preview_toggle(UiNode *n) {
    LX_UNUSED(n);
    if (lx_audio_preview_playing()) lx_audio_preview_stop();
    else {
        LxAudioSource *a = lx_get_audio_source(ED.scene, ED.inspector_entity);
        if (a && a->clip[0]) lx_audio_preview(a->clip, a->volume, a->pitch);
        else if (!a) {
            char ref[LX_REF_MAX];
            ed_relative_asset(ref, sizeof ref, ED.inspector_asset);
            lx_audio_preview(ref, 1.0f, 1.0f);
        } else ui_notify(ED.ui, "Choose a clip first");
    }
    preview_button_sync();
}
static UiNode *preview_button(UiNode *parent) {
    UiNode *b = ui_button(ED.ui, parent, "Preview", preview_toggle);
    b->icon = UI_ICON_PLAY;
    b->icon_color = ui_theme(ED.ui)->success;
    b->grow = 1;
    g_preview_btn = b;
    preview_button_sync();
    return b;
}

static void behaviour_changed(UiNode *n) {
    LX_UNUSED(n);
    LxBehaviourRef *b = lx_get_behaviour(ED.scene, ED.inspector_entity);
    if (b && g_behaviour_index >= 0 && g_behaviour_names[g_behaviour_index]) {
        lx_add_behaviour(ED.scene, ED.inspector_entity, g_behaviour_names[g_behaviour_index]);
        ED.inspector_dirty = true;
        ed_mark_changed();
        ed_commit();
    }
}

static void add_component(UiNode *n) {
    LxScene *s = ED.scene;
    LxEntity e = ED.inspector_entity;
    switch ((LxComponentBits)n->tag) {
    case LX_COMP_CAMERA: lx_add_camera(s, e); break;
    case LX_COMP_LIGHT: lx_add_light(s, e, LX_LIGHT_POINT); break;
    case LX_COMP_MESH: lx_add_mesh(s, e, "builtin:cube", "builtin:default"); break;
    case LX_COMP_AUDIO_SOURCE: lx_add_audio_source(s, e, ""); break;
    case LX_COMP_AUDIO_LISTENER: lx_add_audio_listener(s, e); break;
    case LX_COMP_BEHAVIOUR: lx_add_behaviour(s, e, lx_behaviour_count() ? lx_behaviour_at(0)->name : "Rotator"); break;
    case LX_COMP_CROWD: lx_add_crowd(s, e, 500); break;
    case LX_COMP_REFLECTION_PROBE: lx_add_reflection_probe(s, e); break;
    case LX_COMP_COLLIDER: {
        /* pick the shape that fits the mesh */
        LxMeshRenderer *m = lx_get_mesh(s, e);
        LxColliderShape shape = !m ? LX_COLLIDER_BOX : strstr(m->mesh, "sphere") ? LX_COLLIDER_SPHERE
                                : strstr(m->mesh, "capsule") ? LX_COLLIDER_CAPSULE : strncmp(m->mesh, "builtin:", 8) ? LX_COLLIDER_MESH : LX_COLLIDER_BOX;
        lx_add_collider(s, e, shape);
        break;
    }
    case LX_COMP_RIGIDBODY: lx_add_rigidbody(s, e); break;
    case LX_COMP_CHARACTER: lx_add_character(s, e); break;
    }
    ED.inspector_dirty = true;
    ED.hierarchy_hash = 0;
    ed_mark_changed();
    ed_commit();
}

static void add_component_menu(UiNode *b) {
    uint32_t have = lx_entity_components(ED.scene, ED.inspector_entity);
    UiNode *p = ui_popup(ED.ui, b->x, b->y + b->ch, b->cw / ui_scale(ED.ui));
    struct { LxComponentBits bit; const char *name; UiIcon icon; } comps[] = {
        {LX_COMP_MESH, "Mesh Renderer", UI_ICON_MESH}, {LX_COMP_LIGHT, "Light", UI_ICON_BULB}, {LX_COMP_CAMERA, "Camera", UI_ICON_CAMERA},
        {LX_COMP_AUDIO_SOURCE, "Audio Source", UI_ICON_SPEAKER}, {LX_COMP_AUDIO_LISTENER, "Audio Listener", UI_ICON_LISTENER},
        {LX_COMP_BEHAVIOUR, "Behaviour (Script)", UI_ICON_BEHAVIOUR}, {LX_COMP_CROWD, "Crowd", UI_ICON_CROWD},
        {LX_COMP_REFLECTION_PROBE, "Reflection Probe", UI_ICON_PROBE}, {LX_COMP_COLLIDER, "Collider", UI_ICON_COLLIDER},
        {LX_COMP_RIGIDBODY, "Rigidbody", UI_ICON_RIGIDBODY}, {LX_COMP_CHARACTER, "Character Controller", UI_ICON_CHARACTER}};
    for (int i = 0; i < (int)(sizeof comps / sizeof comps[0]); i++) {
        UiNode *it = ui_menu_item(ED.ui, p, comps[i].name, NULL, add_component);
        it->icon = comps[i].icon;
        it->icon_color = ed_color_for_icon(comps[i].icon);
        it->tag = comps[i].bit;
        it->disabled = (have & comps[i].bit) != 0;
    }
}

static bool material_is_file(const char *ref);
static void build_material_editor(UiNode *parent, const char *ref);
static void make_material_editable(UiNode *n);

static void build_entity_inspector(UiNode *body) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxScene *s = ED.scene;
    LxEntity e = ED.inspector_entity;
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return;
    /* header: icon, active toggle, name */
    UiNode *head = ui_box(ui, body, UI_ROW);
    head->h = 40;
    ui_pad(head, 10, 6, 10, 6);
    head->gap = 8;
    head->cross_align = UI_ALIGN_CENTER;
    UiNode *icon = ui_label(ui, head, "");
    icon->icon = ed_icon_for_entity(s, e);
    icon->icon_color = ed_color_for_icon(icon->icon);
    icon->icon_size = 22;
    icon->w = 26;
    UiNode *act = ui_checkbox(ui, head, "", &d->active);
    act->on_change = active_changed;
    lx_strcpy(act->tooltip, sizeof act->tooltip, "Active");
    UiNode *name = ui_textfield(ui, head, d->name, sizeof d->name);
    name->grow = 1;
    name->h = 28;
    name->font = UI_FONT_BOLD;
    name->on_change = name_changed;

    UiNode *sec = comp_section(body, UI_ICON_MOVE, "Transform", &g_open_transform, 0);
    ed_vec3_row(sec, "Position", &d->position.x, 0.05f, transform_changed);
    ed_vec3_row(sec, "Rotation", &d->euler.x, 0.5f, transform_changed);
    ed_vec3_row(sec, "Scale", &d->scale.x, 0.02f, transform_changed);

    if (d->comps & LX_COMP_CAMERA) {
        sec = comp_section(body, UI_ICON_CAMERA, "Camera", &g_open_camera, LX_COMP_CAMERA);
        bool_row(sec, "Orthographic", &d->camera.orthographic);
        if (d->camera.orthographic) float_row(sec, "Size", &d->camera.ortho_size, 0.1f, 0.01f, 10000);
        else slider_row(sec, "Field of View", &d->camera.fov_deg, 10, 140);
        float_row(sec, "Near Clip", &d->camera.near_clip, 0.01f, 0.001f, 100);
        float_row(sec, "Far Clip", &d->camera.far_clip, 1, 1, 100000);
        UiNode *row = ed_prop_row(sec, "Priority");
        UiNode *pr = ui_int(ui, row, &d->camera.priority, -100, 100);
        pr->grow = 1;
        pr->on_change = ed_field_changed;
        color_row(sec, "Background", &d->camera.clear_color.x, 1);
    }
    if (d->comps & LX_COMP_LIGHT) {
        sec = comp_section(body, d->light.type == LX_LIGHT_DIRECTIONAL ? UI_ICON_SUN : d->light.type == LX_LIGHT_SPOT ? UI_ICON_SPOT : UI_ICON_BULB, "Light",
                           &g_open_light, LX_COMP_LIGHT);
        UiNode *row = ed_prop_row(sec, "Type");
        UiNode *dd = ui_dropdown(ui, row, (int *)&d->light.type, LIGHT_TYPES, 3);
        dd->grow = 1;
        dd->on_change = structural_changed;
        color_row(sec, "Color", &d->light.color.x, 1);
        float_row(sec, "Intensity", &d->light.intensity, 0.05f, 0, 100000);
        if (d->light.type != LX_LIGHT_DIRECTIONAL) float_row(sec, "Range", &d->light.range, 0.1f, 0.01f, 10000);
        if (d->light.type == LX_LIGHT_SPOT) slider_row(sec, "Spot Angle", &d->light.spot_angle_deg, 1, 179);
        if (d->light.type == LX_LIGHT_DIRECTIONAL) bool_row(sec, "Cast Shadows", &d->light.cast_shadows);
        bool_row(sec, "Global Illumination", &d->light.contribute_gi);
    }
    if (d->comps & LX_COMP_MESH) {
        sec = comp_section(body, UI_ICON_MESH, "Mesh Renderer", &g_open_mesh, LX_COMP_MESH);
        asset_field(sec, "Mesh", d->mesh.mesh, sizeof d->mesh.mesh, "mesh");
        asset_field(sec, "Material", d->mesh.material, sizeof d->mesh.material, "material");
        bool_row(sec, "Cast Shadows", &d->mesh.cast_shadows);
        UiNode *st = bool_row(sec, "Static (GI + Sound)", &d->mesh.static_geometry);
        lx_strcpy(st->tooltip, sizeof st->tooltip, "Static geometry bounces virtual photons and reflects/blocks sound");
        LxMesh *m = lx_asset_mesh(d->mesh.mesh);
        UiNode *info = ui_label(ui, sec, "");
        ui_set_text(info, "%d triangles · %d LOD%s", lx_mesh_triangle_count(m), lx_mesh_lod_count(m), lx_mesh_lod_count(m) == 1 ? "" : "s");
        info->font = UI_FONT_SMALL;
        info->fg = t->text_dim;
        char mat_title[LX_REF_MAX + 16];
        snprintf(mat_title, sizeof mat_title, "Material  ·  %s", lx_path_filename(d->mesh.material));
        UiNode *ms = comp_section(body, UI_ICON_MATERIAL, mat_title, &g_open_material, 0);
        if (material_is_file(d->mesh.material)) {
            build_material_editor(ms, d->mesh.material);
            UiNode *shared = ui_label(ui, ms, "Shared by every object that uses this material.");
            shared->font = UI_FONT_SMALL;
            shared->fg = t->text_dim;
        } else {
            UiNode *note = ui_label(ui, ms, "Built-in materials are read-only.");
            note->icon = UI_ICON_LOCK;
            note->icon_size = 13;
            note->fg = t->text_dim;
            UiNode *mk = ui_button(ui, ms, "Make Editable Copy", make_material_editable);
            mk->icon = UI_ICON_MATERIAL;
            mk->icon_color = ed_color_for_icon(UI_ICON_MATERIAL);
            mk->h = 28;
            lx_strcpy(mk->tooltip, sizeof mk->tooltip, "Copies this material into Assets/Materials and assigns it to this object");
        }
    }
    if (d->comps & LX_COMP_AUDIO_SOURCE) {
        sec = comp_section(body, UI_ICON_SPEAKER, "Audio Source", &g_open_audio, LX_COMP_AUDIO_SOURCE);
        asset_field(sec, "Clip", d->audio.clip, sizeof d->audio.clip, "sound");
        slider_row(sec, "Volume", &d->audio.volume, 0, 2);
        slider_row(sec, "Pitch", &d->audio.pitch, 0.25f, 3);
        UiNode *row = ed_prop_row(sec, "Mixer Bus");
        UiNode *bus = ui_dropdown(ui, row, (int *)&d->audio.bus, BUSES, LX_BUS_COUNT);
        bus->grow = 1;
        bus->on_change = ed_field_changed;
        bool_row(sec, "Loop", &d->audio.loop);
        bool_row(sec, "Play On Start", &d->audio.play_on_start);
        bool_row(sec, "Spatial (3D)", &d->audio.spatial);
        float_row(sec, "Min Distance", &d->audio.min_distance, 0.1f, 0.01f, 1000);
        float_row(sec, "Max Distance", &d->audio.max_distance, 0.5f, 0.1f, 10000);
        UiNode *prow = ed_prop_row(sec, "Preview");
        preview_button(prow);
    }
    if (d->comps & LX_COMP_AUDIO_LISTENER) {
        sec = comp_section(body, UI_ICON_LISTENER, "Audio Listener", &g_open_listener, LX_COMP_AUDIO_LISTENER);
        UiNode *l = ui_label(ui, sec, "Hears the scene: direct sound, reflections and reverb.");
        l->font = UI_FONT_SMALL;
        l->fg = t->text_dim;
    }
    if (d->comps & LX_COMP_BEHAVIOUR) {
        sec = comp_section(body, UI_ICON_BEHAVIOUR, "Behaviour", &g_open_behaviour, LX_COMP_BEHAVIOUR);
        int nb = lx_behaviour_count();
        g_behaviour_index = -1;
        for (int i = 0; i < nb && i < 64; i++) {
            g_behaviour_names[i] = lx_behaviour_at(i)->name;
            if (!strcmp(g_behaviour_names[i], d->behaviour.name)) g_behaviour_index = i;
        }
        UiNode *row = ed_prop_row(sec, "Script");
        UiNode *dd = ui_dropdown(ui, row, &g_behaviour_index, g_behaviour_names, nb < 64 ? nb : 64);
        dd->grow = 1;
        dd->on_change = behaviour_changed;
        const LxBehaviourDesc *desc = lx_behaviour_find(d->behaviour.name);
        if (!desc) {
            UiNode *w = ui_label(ui, sec, "Script not compiled into the editor; it runs in builds.");
            w->icon = UI_ICON_WARNING;
            w->icon_color = t->warning;
            w->font = UI_FONT_SMALL;
        } else
            for (int i = 0; i < LX_BEHAVIOUR_PARAMS; i++)
                if (desc->param_names[i]) float_row(sec, desc->param_names[i], &d->behaviour.params[i], 0.05f, -1e9f, 1e9f);
    }
    if (d->comps & LX_COMP_COLLIDER) {
        sec = comp_section(body, UI_ICON_COLLIDER, d->collider.is_trigger ? "Collider (Trigger)" : "Collider", &g_open_collider,
                           LX_COMP_COLLIDER);
        UiNode *row = ed_prop_row(sec, "Shape");
        UiNode *dd = ui_dropdown(ui, row, (int *)&d->collider.shape, COLLIDER_SHAPES, 4);
        dd->grow = 1;
        dd->on_change = structural_changed;
        ed_vec3_row(sec, "Center", &d->collider.center.x, 0.02f, ed_field_changed);
        switch (d->collider.shape) {
        case LX_COLLIDER_BOX: ed_vec3_row(sec, "Size", &d->collider.size.x, 0.02f, ed_field_changed); break;
        case LX_COLLIDER_SPHERE: float_row(sec, "Radius", &d->collider.radius, 0.01f, 0.001f, 10000)->on_change = ed_field_changed; break;
        case LX_COLLIDER_CAPSULE:
            float_row(sec, "Radius", &d->collider.radius, 0.01f, 0.001f, 10000)->on_change = ed_field_changed;
            float_row(sec, "Height", &d->collider.height, 0.02f, 0.001f, 10000)->on_change = ed_field_changed;
            break;
        case LX_COLLIDER_MESH: {
            UiNode *l = ui_label(ui, sec, (d->comps & LX_COMP_MESH) ? "Uses the Mesh Renderer's triangles. Static scenery only (a Rigidbody "
                                                                       "simulates it as its bounding box)."
                                                                     : "Needs a Mesh Renderer.");
            l->font = UI_FONT_SMALL;
            l->fg = (d->comps & LX_COMP_MESH) ? t->text_dim : t->warning;
            l->wrap = true;
            break;
        }
        }
        UiNode *tr = bool_row(sec, "Is Trigger", &d->collider.is_trigger);
        tr->on_change = structural_changed;
        lx_strcpy(tr->tooltip, sizeof tr->tooltip, "Detects things entering it (on_trigger_enter) instead of blocking them");
        if (!d->collider.is_trigger) {
            slider_row(sec, "Friction", &d->collider.friction, 0, 1)->on_change = ed_field_changed;
            slider_row(sec, "Bounciness", &d->collider.bounciness, 0, 1)->on_change = ed_field_changed;
        }
        UiNode *lr = ed_prop_row(sec, "Layer");
        UiNode *ln = ui_int(ui, lr, &d->collider.layer, 0, 31);
        ln->grow = 1;
        ln->on_change = ed_field_changed;
        if (!(d->comps & LX_COMP_RIGIDBODY)) {
            UiNode *hint = ui_label(ui, sec, "Static: never moves. Add a Rigidbody to make it fall, roll and get pushed.");
            hint->font = UI_FONT_SMALL;
            hint->fg = t->text_dim;
            hint->wrap = true;
        }
    }
    if (d->comps & LX_COMP_RIGIDBODY) {
        sec = comp_section(body, UI_ICON_RIGIDBODY, "Rigidbody", &g_open_body, LX_COMP_RIGIDBODY);
        float_row(sec, "Mass (kg)", &d->body.mass, 0.1f, 0.001f, 1e7f)->on_change = ed_field_changed;
        float_row(sec, "Damping", &d->body.linear_damping, 0.01f, 0, 100)->on_change = ed_field_changed;
        float_row(sec, "Angular Damping", &d->body.angular_damping, 0.01f, 0, 100)->on_change = ed_field_changed;
        bool_row(sec, "Use Gravity", &d->body.use_gravity)->on_change = ed_field_changed;
        UiNode *k = bool_row(sec, "Is Kinematic", &d->body.is_kinematic);
        k->on_change = ed_field_changed;
        lx_strcpy(k->tooltip, sizeof k->tooltip, "Moved by your scripts, not by physics; still pushes other bodies");
        bool_row(sec, "Freeze Rotation", &d->body.freeze_rotation)->on_change = ed_field_changed;
        if (!(d->comps & (LX_COMP_COLLIDER | LX_COMP_CHARACTER))) {
            UiNode *w = ui_label(ui, sec, "Needs a Collider to collide with anything.");
            w->icon = UI_ICON_WARNING;
            w->icon_color = t->warning;
            w->font = UI_FONT_SMALL;
        }
    }
    if (d->comps & LX_COMP_CHARACTER) {
        sec = comp_section(body, UI_ICON_CHARACTER, "Character Controller", &g_open_character, LX_COMP_CHARACTER);
        float_row(sec, "Radius", &d->character.radius, 0.01f, 0.05f, 100)->on_change = ed_field_changed;
        float_row(sec, "Height", &d->character.height, 0.02f, 0.1f, 100)->on_change = ed_field_changed;
        float_row(sec, "Step Height", &d->character.step_height, 0.01f, 0, 10)->on_change = ed_field_changed;
        slider_row(sec, "Slope Limit", &d->character.slope_limit_deg, 1, 89)->on_change = ed_field_changed;
        float_row(sec, "Push Power", &d->character.push_power, 0.05f, 0, 100)->on_change = ed_field_changed;
        UiNode *hint = ui_label(ui, sec, "Move it from a script with lx_character_move(). The origin is at the feet.");
        hint->font = UI_FONT_SMALL;
        hint->fg = t->text_dim;
        hint->wrap = true;
    }
    if (d->comps & LX_COMP_REFLECTION_PROBE) {
        sec = comp_section(body, UI_ICON_PROBE, "Reflection Probe", &g_open_probe, LX_COMP_REFLECTION_PROBE);
        ed_vec3_row(sec, "Box Size", &d->probe.size.x, 0.1f, ed_field_changed);
        slider_row(sec, "Intensity", &d->probe.intensity, 0, 4)->on_change = ed_field_changed;
        float_row(sec, "Blend Distance", &d->probe.blend_distance, 0.05f, 0, 100)->on_change = ed_field_changed;
        bool_row(sec, "Box Projection", &d->probe.box_projection)->on_change = ed_field_changed;
        bool_row(sec, "Realtime", &d->probe.realtime)->on_change = ed_field_changed;
        UiNode *hint = ui_label(ui, sec, "Captures the scene around it; surfaces inside the box reflect it. Re-captures automatically when "
                                         "the scene changes. Realtime keeps updating for moving objects.");
        hint->font = UI_FONT_SMALL;
        hint->fg = t->text_dim;
        hint->wrap = true;
        UiNode *rc = ui_button(ui, sec, "Re-capture Now", probe_recapture);
        rc->icon = UI_ICON_REFRESH;
        rc->h = 26;
    }
    if (d->comps & LX_COMP_CROWD) {
        sec = comp_section(body, UI_ICON_CROWD, "Crowd", &g_open_crowd, LX_COMP_CROWD);
        UiNode *row = ed_prop_row(sec, "Agents");
        UiNode *n = ui_int(ui, row, &d->crowd.agent_count, 1, 200000);
        n->grow = 1;
        n->step = 10;
        n->on_change = ed_field_changed;
        float_row(sec, "Area Radius", &d->crowd.area_radius, 0.2f, 1, 10000);
        float_row(sec, "Walk Speed", &d->crowd.agent_speed, 0.05f, 0, 50);
        asset_field(sec, "Agent Mesh", d->crowd.mesh, sizeof d->crowd.mesh, "mesh");
        asset_field(sec, "Material", d->crowd.material, sizeof d->crowd.material, "material");
        UiNode *hint = ui_label(ui, sec, "Agents simulate in Play mode.");
        hint->font = UI_FONT_SMALL;
        hint->fg = t->text_dim;
    }
    UiNode *add_row = ui_box(ui, body, UI_ROW);
    ui_pad(add_row, 40, 14, 40, 14);
    add_row->h = 60;
    UiNode *add = ui_button(ui, add_row, "Add Component", add_component_menu);
    add->icon = UI_ICON_PLUS;
    add->grow = 1;
    add->h = 30;
    add->border = t->border_light;
}

/* ---------------- material editing (shared by the asset inspector and Mesh Renderer) ----------------
 * Widgets bind straight to the cached material the renderer draws with, so every change is
 * visible immediately. The .lxmat file is written when the edit gesture ends. */
static char g_mat_pending[LX_REF_MAX];

static bool material_is_file(const char *ref) {
    if (!ref || !ref[0] || !strncmp(ref, "builtin:", 8)) return false;
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    return lx_file_exists(path);
}

static void material_changed(UiNode *n) {
    lx_strcpy(g_mat_pending, sizeof g_mat_pending, n->path);
    if (ED.scene) lx_scene_mark_static_dirty(ED.scene); /* albedo/absorption feed GI and acoustics */
}

static void material_flush(void) {
    if (!g_mat_pending[0] || lx_mouse_down(LX_MOUSE_LEFT) || ui_wants_keyboard(ED.ui) || ui_popup_open(ED.ui)) return;
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, g_mat_pending);
    if (!lx_material_save(lx_asset_material(g_mat_pending), path)) ui_notify(ED.ui, "Could not save %s", g_mat_pending);
    g_mat_pending[0] = 0;
}

static UiNode *mat_bind(UiNode *n, const char *ref) {
    lx_strcpy(n->path, sizeof n->path, ref);
    n->on_change = material_changed;
    return n;
}

static void build_material_editor(UiNode *parent, const char *ref) {
    LxMaterial *m = lx_asset_material(ref);
    UiNode *c = color_row(parent, "Albedo", &m->albedo.x, 1);
    mat_bind(c, ref);
    mat_bind(slider_row(parent, "Metallic", &m->metallic, 0, 1), ref);
    mat_bind(slider_row(parent, "Roughness", &m->roughness, 0.02f, 1), ref);
    mat_bind(color_row(parent, "Emission", &m->emissive.x, 1), ref);
    mat_bind(float_row(parent, "Emission Strength", &m->emissive_strength, 0.1f, 0, 1000), ref);
    UiNode *tx = asset_field(parent, "Albedo Map", m->albedo_map, sizeof m->albedo_map, "texture");
    mat_bind(tx, ref);
    tx = asset_field(parent, "Normal Map", m->normal_map, sizeof m->normal_map, "texture");
    mat_bind(tx, ref);
    mat_bind(float_row(parent, "UV Scale", &m->uv_scale, 0.05f, 0.001f, 1000), ref);
    UiNode *a1 = mat_bind(slider_row(parent, "Sound Absorption", &m->acoustic_absorption, 0, 1), ref);
    lx_strcpy(a1->tooltip, sizeof a1->tooltip, "Sound energy swallowed on each reflection (carpet ~0.6, concrete ~0.02)");
    UiNode *a2 = mat_bind(slider_row(parent, "Sound Transmission", &m->acoustic_transmission, 0, 1), ref);
    lx_strcpy(a2->tooltip, sizeof a2->tooltip, "Sound passing through the surface (walls block most)");
}

/* built-in materials have no file: copy one into Materials/ so it can be edited */
static void make_material_editable(UiNode *n) {
    LxMeshRenderer *mr = lx_get_mesh(ED.scene, ED.inspector_entity);
    if (!mr) return;
    LxMaterial copy = *lx_asset_material(mr->material);
    char base[LX_NAME_MAX], ref[LX_REF_MAX], path[LX_PATH_MAX];
    size_t k = 0;
    for (const char *c = lx_entity_name(ED.scene, ED.inspector_entity); *c && k < sizeof base - 1; c++)
        base[k++] = (isalnum((unsigned char)*c) || *c == '_' || *c == '-') ? *c : '_';
    base[k] = 0;
    if (!base[0]) lx_strcpy(base, sizeof base, "Material");
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(ref, sizeof ref, "Materials/%s %d.lxmat", base, i + 1);
        else snprintf(ref, sizeof ref, "Materials/%s.lxmat", base);
        lx_asset_path(path, sizeof path, ref);
        if (!lx_file_exists(path)) break;
    }
    lx_strcpy(copy.name, sizeof copy.name, base);
    if (!lx_material_save(&copy, path)) { ui_notify(ED.ui, "Could not create %s", ref); return; }
    lx_strcpy(mr->material, sizeof mr->material, ref);
    ED.inspector_dirty = true;
    ED.content_dirty = true;
    ed_mark_changed();
    ed_commit();
    ui_notify(ED.ui, "Created %s - edit it right here", ref);
    LX_UNUSED(n);
}

static void open_external(UiNode *n) { LX_UNUSED(n); lx_platform_open_folder(ED.inspector_asset); }

static void build_asset_inspector(UiNode *body) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    const char *path = ED.inspector_asset;
    const char *name = lx_path_filename(path);
    UiIcon icon = ed_icon_for_file(name, lx_dir_exists(path));
    UiNode *head = ui_box(ui, body, UI_ROW);
    head->h = 44;
    ui_pad(head, 10, 6, 10, 6);
    head->gap = 8;
    head->cross_align = UI_ALIGN_CENTER;
    UiNode *l = ui_label(ui, head, name);
    l->icon = icon;
    l->icon_size = 22;
    l->icon_color = ed_color_for_icon(icon);
    l->font = UI_FONT_BOLD;
    l->grow = 1;
    char ref[LX_REF_MAX];
    ed_relative_asset(ref, sizeof ref, path);
    UiNode *pl = ui_label(ui, body, ref);
    pl->font = UI_FONT_SMALL;
    pl->fg = t->text_dim;
    ui_pad(pl, 12, 0, 8, 0);
    if (icon == UI_ICON_MATERIAL) {
        UiNode *sec = ed_section(body, UI_ICON_MATERIAL, "Material", &g_open_material, NULL);
        build_material_editor(sec, ref);
        UiNode *hint = ui_label(ui, body, "Changes show instantly and are saved automatically.");
        hint->font = UI_FONT_SMALL;
        hint->fg = t->text_dim;
        hint->icon = UI_ICON_SAVE;
        hint->icon_size = 12;
        ui_pad(hint, 12, 4, 0, 0);
    } else if (icon == UI_ICON_TEXTURE) {
        LxTexture *tex = lx_asset_texture(ref);
        if (tex) {
            static uint32_t preview;
            static char preview_path[LX_PATH_MAX];
            if (strcmp(preview_path, path)) {
                if (preview) lx_ui_texture_free(preview);
                preview = lx_ui_texture_from_rgba(tex->pixels, tex->width, tex->height);
                lx_strcpy(preview_path, sizeof preview_path, path);
            }
            UiNode *img = ui_image(ui, body, preview);
            img->h = 240;
            img->tex_aspect = (float)tex->width / (float)tex->height;
            ui_pad(img, 12, 8, 12, 8);
            UiNode *info = ui_label(ui, body, "");
            ui_set_text(info, "%d x %d · RGBA8 · mipmapped on upload", tex->width, tex->height);
            info->font = UI_FONT_SMALL;
            info->fg = t->text_dim;
            ui_pad(info, 12, 0, 0, 0);
        }
    } else if (icon == UI_ICON_MESH) {
        LxMesh *m = lx_asset_mesh(ref);
        UiNode *info = ui_label(ui, body, "");
        LxAabb b = lx_mesh_bounds(m);
        ui_set_text(info, "%d triangles · %d LODs · size %.2f x %.2f x %.2f", lx_mesh_triangle_count(m), lx_mesh_lod_count(m),
                    (double)(b.max.x - b.min.x), (double)(b.max.y - b.min.y), (double)(b.max.z - b.min.z));
        ui_pad(info, 12, 8, 0, 0);
        UiNode *hint = ui_label(ui, body, "Drag into the viewport or hierarchy to place it.");
        hint->font = UI_FONT_SMALL;
        hint->fg = t->text_dim;
        ui_pad(hint, 12, 0, 0, 0);
    } else if (icon == UI_ICON_SOUND) {
        LxSound *snd = lx_asset_sound(ref);
        UiNode *info = ui_label(ui, body, "");
        if (snd) ui_set_text(info, "%.2f s · %d Hz · mono (spatialised at runtime)", (double)lx_sound_duration(snd), snd->sample_rate);
        else ui_set_text(info, "Unsupported WAV format (use PCM 8/16/24/32-bit or float)");
        ui_pad(info, 12, 8, 0, 0);
        UiNode *row = ui_box(ui, body, UI_ROW);
        ui_pad(row, 12, 8, 12, 8);
        row->h = 46;
        UiNode *b = preview_button(row);
        b->disabled = !snd;
    } else if (icon == UI_ICON_SCRIPT || icon == UI_ICON_FILE || icon == UI_ICON_SCENE) {
        UiNode *row = ui_box(ui, body, UI_ROW);
        ui_pad(row, 12, 8, 12, 8);
        row->h = 46;
        UiNode *b = ui_button(ui, row, icon == UI_ICON_SCRIPT ? "Open in Code Editor" : "Open Externally", open_external);
        b->icon = UI_ICON_EXTERNAL;
        b->grow = 1;
        if (icon == UI_ICON_SCRIPT) {
            UiNode *hint = ui_label(ui, body, "Scripts are compiled into your game when you build. Register behaviours with LX_SCRIPT_REGISTER.");
            hint->font = UI_FONT_SMALL;
            hint->fg = t->text_dim;
            ui_pad(hint, 12, 0, 8, 0);
            hint->h = 36;
        }
    }
}

static void inspector_frame(UiNode *body) {
    if (!ED.scene) return;
    material_flush();
    preview_button_sync();   /* flips back to "Preview" when the sound ends */
    LxEntityData *d = lx_entity_data(ED.scene, ED.selected);
    uint32_t comps = d ? d->comps : 0;
    bool asset_changed = strcmp(ED.inspector_asset, ED.selected_asset) != 0;
    /* an asset is shown and nothing changed: keep the existing widgets (they edit live data) */
    if (!ED.inspector_dirty && ED.selected_asset[0] && !asset_changed && body->first) return;
    if (!ED.inspector_dirty && ED.inspector_entity == ED.selected && comps == ED.inspector_comps &&
        ED.inspector_storage == (const void *)ED.scene->entities && !asset_changed && d) {
        return;
    }
    if (!ED.inspector_dirty && !d && !ED.selected_asset[0] && !asset_changed && ED.inspector_entity == 0 && body->first) return;
    ED.inspector_dirty = false;
    /* a sound you were previewing stops when you select something else */
    if (asset_changed || ED.inspector_entity != (d ? ED.selected : 0)) lx_audio_preview_stop();
    ED.inspector_entity = d ? ED.selected : 0;
    ED.inspector_comps = comps;
    ED.inspector_storage = ED.scene->entities;
    lx_strcpy(ED.inspector_asset, sizeof ED.inspector_asset, ED.selected_asset);
    g_preview_btn = NULL;
    ui_clear(ED.ui, body);
    if (ED.selected_asset[0]) build_asset_inspector(body);
    else if (d) build_entity_inspector(body);
    else {
        UiNode *empty = ui_box(ED.ui, body, UI_COL);
        empty->h = 200;
        empty->align = UI_ALIGN_CENTER;
        empty->gap = 8;
        UiNode *ic = ui_label(ED.ui, empty, "");
        ic->icon = UI_ICON_INSPECTOR;
        ic->icon_size = 40;
        ic->icon_color = ui_theme(ED.ui)->text_disabled;
        ic->center_text = true;
        ic->h = 48;
        UiNode *l = ui_label(ED.ui, empty, "Select an object or asset to inspect it.");
        l->center_text = true;
        l->fg = ui_theme(ED.ui)->text_dim;
    }
}

UiNode *ed_inspector_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Inspector");
    p->icon = UI_ICON_INSPECTOR;
    p->bg = ui_theme(ui)->panel;
    UiNode *body = ui_box(ui, p, UI_COL);
    body->grow = 1;
    body->scroll = true;
    body->gap = 2;
    body->on_frame = inspector_frame;
    return p;
}

/* ======================================================================
 *  LIGHTING (environment + GI + shadows)
 * ====================================================================== */
static const char *QUALITY[] = {"Low", "Medium", "High", "Ultra"};
static const char *SHADOW_RES[] = {"512", "1024", "2048", "4096"};
static const char *PROBE_RES[] = {"64", "128", "256", "512"};
static int g_quality, g_shadow_res_idx, g_probe_res_idx;
static bool g_open_refl = true;
static LxScene *g_lighting_scene;
static bool g_open_env = true, g_open_gi = true, g_open_shadow = true;

static void graphics_changed(UiNode *n) {
    LX_UNUSED(n);
    ED.project.graphics = *lx_graphics_settings();
    lx_project_save(&ED.project);
}
static void quality_changed(UiNode *n) {
    lx_graphics_apply_preset(lx_graphics_settings(), g_quality);
    g_shadow_res_idx = lx_graphics_settings()->shadow_resolution >= 4096 ? 3 : lx_graphics_settings()->shadow_resolution >= 2048 ? 2 : lx_graphics_settings()->shadow_resolution >= 1024 ? 1 : 0;
    graphics_changed(n);
}
static void shadow_res_changed(UiNode *n) {
    lx_graphics_settings()->shadow_resolution = 512 << g_shadow_res_idx;
    graphics_changed(n);
}
static void probe_res_changed(UiNode *n) {
    lx_graphics_settings()->reflection_resolution = 64 << g_probe_res_idx;
    graphics_changed(n);
}
static void reset_gi(UiNode *n) { LX_UNUSED(n); lx_gi_reset(); ui_notify(ED.ui, "Global illumination restarted"); }
static void env_changed(UiNode *n) { LX_UNUSED(n); ed_mark_changed(); }

static void lighting_frame(UiNode *body) {
    if (g_lighting_scene == ED.scene && body->first) return;
    g_lighting_scene = ED.scene;
    UiCtx *ui = ED.ui;
    ui_clear(ui, body);
    if (!ED.scene) return;
    LxEnvironment *env = lx_scene_environment(ED.scene);
    LxGraphicsSettings *g = lx_graphics_settings();
    g_quality = g->quality_preset;
    g_shadow_res_idx = g->shadow_resolution >= 4096 ? 3 : g->shadow_resolution >= 2048 ? 2 : g->shadow_resolution >= 1024 ? 1 : 0;
    UiNode *sec = ed_section(body, UI_ICON_GLOBE, "Environment (scene)", &g_open_env, NULL);
    color_row(sec, "Sky Zenith", &env->sky_zenith.x, 1)->on_change = env_changed;
    color_row(sec, "Sky Horizon", &env->sky_horizon.x, 1)->on_change = env_changed;
    color_row(sec, "Ground", &env->ground_color.x, 1)->on_change = env_changed;
    slider_row(sec, "Sky Light", &env->ambient_intensity, 0, 3)->on_change = env_changed;
    slider_row(sec, "Exposure", &env->exposure, 0.1f, 4)->on_change = env_changed;
    color_row(sec, "Fog Color", &env->fog_color.x, 1)->on_change = env_changed;
    UiNode *fog = float_row(sec, "Fog Density", &env->fog_density, 0.001f, 0, 1);
    fog->precision = 4;
    fog->on_change = env_changed;

    sec = ed_section(body, UI_ICON_SUN, "Global Illumination (project)", &g_open_gi, NULL);
    UiNode *row = ed_prop_row(sec, "Quality Preset");
    UiNode *q = ui_dropdown(ui, row, &g_quality, QUALITY, 4);
    q->grow = 1;
    q->on_change = quality_changed;
    bool_row(sec, "Virtual Photons", &g->gi_enabled)->on_change = graphics_changed;
    row = ed_prop_row(sec, "Photons / Frame");
    UiNode *ph = ui_int(ui, row, &g->gi_photons_per_frame, 256, 2000000);
    ph->grow = 1;
    ph->step = 200;
    ph->on_change = graphics_changed;
    row = ed_prop_row(sec, "Bounces");
    UiNode *bo = ui_int(ui, row, &g->gi_max_bounces, 1, 16);
    bo->grow = 1;
    bo->on_change = graphics_changed;
    row = ed_prop_row(sec, "Volume Resolution");
    UiNode *gr = ui_int(ui, row, &g->gi_grid_resolution, 4, 96);
    gr->grow = 1;
    gr->on_change = graphics_changed;
    slider_row(sec, "Intensity", &g->gi_intensity, 0, 4)->on_change = graphics_changed;
    slider_row(sec, "Responsiveness", &g->gi_responsiveness, 0.01f, 1)->on_change = graphics_changed;
    UiNode *rb = ui_button(ui, sec, "Restart Photon Accumulation", reset_gi);
    rb->icon = UI_ICON_REFRESH;
    rb->h = 26;

    sec = ed_section(body, UI_ICON_PROBE, "Reflections (project)", &g_open_refl, NULL);
    bool_row(sec, "Reflection Probes", &g->reflections_enabled)->on_change = graphics_changed;
    g_probe_res_idx = g->reflection_resolution >= 512 ? 3 : g->reflection_resolution >= 256 ? 2 : g->reflection_resolution >= 128 ? 1 : 0;
    row = ed_prop_row(sec, "Probe Resolution");
    UiNode *pr = ui_dropdown(ui, row, &g_probe_res_idx, PROBE_RES, 4);
    pr->grow = 1;
    pr->on_change = probe_res_changed;
    bool_row(sec, "Screen-Space Reflections", &g->ssr_enabled)->on_change = graphics_changed;
    row = ed_prop_row(sec, "SSR Steps");
    UiNode *st = ui_int(ui, row, &g->ssr_steps, 8, 128);
    st->grow = 1;
    st->on_change = graphics_changed;
    float_row(sec, "SSR Max Distance", &g->ssr_max_distance, 0.5f, 1, 500)->on_change = graphics_changed;
    float_row(sec, "SSR Thickness", &g->ssr_thickness, 0.01f, 0.01f, 5)->on_change = graphics_changed;
    slider_row(sec, "SSR Max Roughness", &g->ssr_max_roughness, 0.05f, 1)->on_change = graphics_changed;
    UiNode *rr = ui_button(ui, sec, "Re-capture Probes", probe_recapture);
    rr->icon = UI_ICON_REFRESH;
    rr->h = 26;

    sec = ed_section(body, UI_ICON_LAYERS, "Shadows & Detail (project)", &g_open_shadow, NULL);
    row = ed_prop_row(sec, "Resolution");
    UiNode *sr = ui_dropdown(ui, row, &g_shadow_res_idx, SHADOW_RES, 4);
    sr->grow = 1;
    sr->on_change = shadow_res_changed;
    row = ed_prop_row(sec, "Cascades");
    UiNode *cs = ui_int(ui, row, &g->shadow_cascades, 1, 4);
    cs->grow = 1;
    cs->on_change = graphics_changed;
    float_row(sec, "Shadow Distance", &g->shadow_distance, 1, 5, 2000)->on_change = graphics_changed;
    bool_row(sec, "Automatic LODs", &g->lod_enabled)->on_change = graphics_changed;
    slider_row(sec, "LOD Bias", &g->lod_bias, 0.2f, 3)->on_change = graphics_changed;
    slider_row(sec, "Render Scale", &g->render_scale, 0.25f, 2)->on_change = graphics_changed;
}

UiNode *ed_lighting_panel(UiNode *parent) {
    UiNode *p = ui_box(ED.ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Lighting");
    p->icon = UI_ICON_SUN;
    p->bg = ui_theme(ED.ui)->panel;
    UiNode *body = ui_box(ED.ui, p, UI_COL);
    body->grow = 1;
    body->scroll = true;
    body->gap = 2;
    body->on_frame = lighting_frame;
    return p;
}

/* ======================================================================
 *  STATISTICS
 * ====================================================================== */
static void stats_frame(UiNode *body) {
    const LxRenderStats *st = lx_render_stats();
    UiNode *l = body->first;
    char buf[160];
    int i = 0;
#define STAT(...) do { if (l) { snprintf(buf, sizeof buf, __VA_ARGS__); ui_set_text(l->last, "%s", buf); l = l->next; } i++; } while (0)
    STAT("%.0f", (double)ED.fps);
    STAT("%.2f ms", (double)st->frame_ms);
    STAT("%d", st->draw_calls);
    STAT("%d", st->instances_drawn);
    STAT("%d", st->instances_culled);
    STAT("%lld", (long long)st->triangles);
    STAT("%.2f ms", (double)st->cull_ms);
    STAT("%d", st->gi_photons_traced);
    STAT("%.2f ms", (double)st->gi_ms);
    STAT("%d active, %d faces", st->probes_active, st->probe_faces_rendered);
    STAT("%d", ED.scene ? lx_scene_entity_count(ED.scene) : 0);
    STAT("%d", lx_jobs_thread_count());
    STAT("%s", lx_audio_backend_name());
    STAT("%d connected", lx_gamepad_count());
    STAT("%s", lx_input_last_device() == LX_DEVICE_GAMEPAD ? "Gamepad" : lx_input_last_device() == LX_DEVICE_TOUCH ? "Touch" : "Keyboard & Mouse");
#undef STAT
    LX_UNUSED(i);
}

UiNode *ed_stats_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Stats");
    p->icon = UI_ICON_STATS;
    p->bg = ui_theme(ui)->panel;
    UiNode *body = ui_box(ui, p, UI_COL);
    body->grow = 1;
    ui_pad(body, 12, 10, 12, 10);
    body->gap = 2;
    static const char *names[] = {"Frame rate", "Render CPU time", "Draw calls", "Instances drawn", "Instances culled", "Triangles",
                                  "Culling time", "Photons / frame", "GI trace time", "Reflection probes", "Entities", "Worker threads",
                                  "Audio device", "Gamepads", "Last input"};
    for (int i = 0; i < 15; i++) {
        UiNode *row = ed_prop_row(body, names[i]);
        row->first->w = 130;
        UiNode *v = ui_label(ui, row, "-");
        v->grow = 1;
        v->font = UI_FONT_MONO;
    }
    body->on_frame = stats_frame;
    return p;
}

/* ======================================================================
 *  CONTENT BROWSER
 * ====================================================================== */
typedef struct Thumb { char path[LX_PATH_MAX]; uint32_t tex; } Thumb;
static Thumb g_thumbs[64];
static int g_thumb_next;
static char g_ctx_path[LX_PATH_MAX];

static uint32_t thumbnail(const char *path) {
    for (int i = 0; i < 64; i++) if (g_thumbs[i].tex && !strcmp(g_thumbs[i].path, path)) return g_thumbs[i].tex;
    int w, h;
    uint8_t *px = lx_image_load(path, &w, &h);
    if (!px) return 0;
    /* box-downsample to <= 128 px */
    int step = 1;
    while (w / step > 128 || h / step > 128) step++;
    int tw = w / step, th = h / step;
    uint8_t *small = lx_alloc((size_t)tw * (size_t)th * 4);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) memcpy(small + ((size_t)y * (size_t)tw + (size_t)x) * 4, px + ((size_t)(y * step) * (size_t)w + (size_t)(x * step)) * 4, 4);
    lx_free(px);
    Thumb *t = &g_thumbs[g_thumb_next];
    g_thumb_next = (g_thumb_next + 1) % 64;
    if (t->tex) lx_ui_texture_free(t->tex);
    t->tex = lx_ui_texture_from_rgba(small, tw, th);
    lx_strcpy(t->path, sizeof t->path, path);
    lx_free(small);
    return t->tex;
}

void ed_content_open_dir(const char *dir) {
    lx_strcpy(ED.content_dir, sizeof ED.content_dir, dir);
    ED.content_dirty = true;
}
void ed_content_refresh(void) { ED.content_dirty = true; }

static void tile_click(UiNode *n) {
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, ED.content_dir, n->text);
    ED.selected = 0;
    ed_select_asset(path);
    for (UiNode *c = n->parent->first; c; c = c->next) c->selected = c == n;
}

static void open_scene_now(void);
static char g_scene_to_open[LX_REF_MAX];
static void open_scene_now(void) { ed_open_scene(g_scene_to_open); }

static void tile_open(UiNode *n) {
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, ED.content_dir, n->text);
    if (lx_dir_exists(path)) { ed_content_open_dir(path); return; }
    UiIcon icon = ed_icon_for_file(n->text, false);
    if (icon == UI_ICON_SCENE) {
        if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode first"); return; }
        ed_relative_asset(g_scene_to_open, sizeof g_scene_to_open, path);
        if (ed_confirm_discard(open_scene_now)) open_scene_now();
    } else if (icon == UI_ICON_SCRIPT || icon == UI_ICON_FILE) lx_platform_open_folder(path);
    else tile_click(n);
}

static void go_up(UiNode *n) {
    LX_UNUSED(n);
    if (!strcmp(ED.content_dir, ED.project.assets)) return;
    char up[LX_PATH_MAX];
    lx_path_parent(up, sizeof up, ED.content_dir);
    ed_content_open_dir(up);
}
static void go_crumb(UiNode *n) { ed_content_open_dir(n->path); }
static void toggle_view(UiNode *n) { LX_UNUSED(n); ED.content_list_view = !ED.content_list_view; ED.content_dirty = true; }
static void content_search_changed(UiNode *n) { LX_UNUSED(n); ED.content_dirty = true; }
static void refresh_click(UiNode *n) { LX_UNUSED(n); ED.content_dirty = true; }

/* create assets */
static void create_folder_named(const char *name) {
    char p[LX_PATH_MAX];
    lx_path_join(p, sizeof p, ED.content_dir, name);
    if (!lx_mkdir_p(p)) ui_notify(ED.ui, "Could not create folder");
    ED.content_dirty = true;
}
static void create_material_named(const char *name) {
    char p[LX_PATH_MAX], file[LX_NAME_MAX + 8];
    snprintf(file, sizeof file, "%s.lxmat", name);
    lx_path_join(p, sizeof p, ED.content_dir, file);
    LxMaterial m = lx_material_default();
    lx_strcpy(m.name, sizeof m.name, name);
    lx_material_save(&m, p);
    ED.content_dirty = true;
    ed_select_asset(p);
}
static void create_scene_named(const char *name) {
    char p[LX_PATH_MAX], file[LX_NAME_MAX + 10];
    snprintf(file, sizeof file, "%s.lxscene", name);
    lx_path_join(p, sizeof p, ED.content_dir, file);
    LxScene *s = lx_scene_create_default(name);
    lx_scene_save(s, p);
    lx_scene_destroy(s);
    ED.content_dirty = true;
}
static void create_script_named(const char *name) {
    char p[LX_PATH_MAX], file[LX_NAME_MAX + 4], ident[LX_NAME_MAX];
    size_t k = 0;
    for (const char *c = name; *c && k < sizeof ident - 1; c++) ident[k++] = (isalnum((unsigned char)*c)) ? *c : '_';
    ident[k] = 0;
    snprintf(file, sizeof file, "%s.c", ident);
    lx_path_join(p, sizeof p, ED.content_dir, file);
    LxStrBuf sb = {0};
    lx_sb_printf(&sb,
                 "/* %s behaviour. Attach it to an entity with Add Component > Behaviour. */\n#include <luxora/luxora.h>\n\n"
                 "typedef struct %sState { float time; } %sState;\n\n"
                 "static void %s_start(LxBehaviourCtx *ctx) {\n    LX_INFO(\"%s started on %%s\", lx_entity_name(ctx->scene, ctx->self));\n}\n\n"
                 "static void %s_update(LxBehaviourCtx *ctx, float dt) {\n    %sState *s = ctx->state;\n    s->time += dt;\n"
                 "    /* read the Input Manager: */\n    LxVec2 move = lx_axis2(\"MoveX\", \"MoveY\");\n"
                 "    LxVec3 p = lx_entity_position(ctx->scene, ctx->self);\n    p.x += move.x * ctx->params[0] * dt;\n"
                 "    p.z -= move.y * ctx->params[0] * dt;\n    lx_entity_set_position(ctx->scene, ctx->self, p);\n"
                 "    if (lx_action_pressed(\"Jump\")) LX_INFO(\"Jump!\");\n}\n\n"
                 "LX_SCRIPT_REGISTER(%s) {\n    LxBehaviourDesc d = {0};\n    d.name = \"%s\";\n    d.state_size = sizeof(%sState);\n"
                 "    d.param_names[0] = \"Speed\";\n    d.param_defaults[0] = 4.0f;\n    d.on_start = %s_start;\n    d.on_update = %s_update;\n"
                 "    lx_register_behaviour(&d);\n}\n",
                 name, ident, ident, ident, name, ident, ident, ident, name, ident, ident, ident);
    lx_file_write(p, sb.data, sb.len);
    lx_free(sb.data);
    ED.content_dirty = true;
    ui_notify(ED.ui, "Script created - it is compiled into your next build");
}
static void m_new_folder(UiNode *n) { LX_UNUSED(n); ed_prompt("New Folder", "Name", "New Folder", create_folder_named); }
static void m_new_material(UiNode *n) { LX_UNUSED(n); ed_prompt("New Material", "Name", "New Material", create_material_named); }
static void m_new_scene_asset(UiNode *n) { LX_UNUSED(n); ed_prompt("New Scene", "Name", "New Scene", create_scene_named); }
static void m_new_script(UiNode *n) { LX_UNUSED(n); ed_prompt("New Script", "Behaviour name", "PlayerController", create_script_named); }

static void create_asset_menu_at(float x, float y) {
    UiNode *p = ui_popup(ED.ui, x, y, 200);
    UiNode *it;
    it = ui_menu_item(ED.ui, p, "Folder", NULL, m_new_folder); it->icon = UI_ICON_FOLDER; it->icon_color = ed_color_for_icon(UI_ICON_FOLDER);
    it = ui_menu_item(ED.ui, p, "Material", NULL, m_new_material); it->icon = UI_ICON_MATERIAL; it->icon_color = ed_color_for_icon(UI_ICON_MATERIAL);
    it = ui_menu_item(ED.ui, p, "Scene", NULL, m_new_scene_asset); it->icon = UI_ICON_SCENE; it->icon_color = ed_color_for_icon(UI_ICON_SCENE);
    it = ui_menu_item(ED.ui, p, "Script (Behaviour)", NULL, m_new_script); it->icon = UI_ICON_SCRIPT; it->icon_color = ed_color_for_icon(UI_ICON_SCRIPT);
}
static void create_asset_menu(UiNode *n) { create_asset_menu_at(n->x, n->y + n->ch); }
static void grid_menu(UiNode *n) { LX_UNUSED(n); LxVec2 m = lx_mouse_pos(); create_asset_menu_at(m.x, m.y); }

static void rename_asset_done(const char *name) {
    char parent[LX_PATH_MAX], to[LX_PATH_MAX];
    lx_path_parent(parent, sizeof parent, g_ctx_path);
    char final[LX_NAME_MAX + 16];
    const char *ext = lx_path_ext(g_ctx_path);
    if (lx_dir_exists(g_ctx_path) || !ext[0] || strchr(name, '.')) lx_strcpy(final, sizeof final, name);
    else snprintf(final, sizeof final, "%s%s", name, ext);
    lx_path_join(to, sizeof to, parent, final);
    if (!lx_path_rename(g_ctx_path, to)) ui_notify(ED.ui, "Rename failed");
    ED.content_dirty = true;
    ed_select_asset(to);
}
static void m_rename_asset(UiNode *n) {
    LX_UNUSED(n);
    char base[LX_NAME_MAX];
    lx_strcpy(base, sizeof base, lx_path_filename(g_ctx_path));
    char *dot = strrchr(base, '.');
    if (dot && !lx_dir_exists(g_ctx_path)) *dot = 0;
    ed_prompt("Rename Asset", "Name", base, rename_asset_done);
}
static void delete_asset_now(void) {
    if (!lx_path_remove(g_ctx_path)) ui_notify(ED.ui, "Delete failed");
    if (!strcmp(ED.selected_asset, g_ctx_path)) ed_select_asset("");
    ED.content_dirty = true;
}
static void m_delete_asset(UiNode *n) {
    LX_UNUSED(n);
    char msg[LX_PATH_MAX + 64];
    snprintf(msg, sizeof msg, "Delete '%s'? This can't be undone.", lx_path_filename(g_ctx_path));
    ed_confirm("Delete Asset", msg, "Delete", delete_asset_now);
}
static void m_show_asset(UiNode *n) {
    LX_UNUSED(n);
    char parent[LX_PATH_MAX];
    lx_path_parent(parent, sizeof parent, g_ctx_path);
    lx_platform_open_folder(lx_dir_exists(g_ctx_path) ? g_ctx_path : parent);
}
static void m_open_asset(UiNode *n) {
    LX_UNUSED(n);
    for (UiNode *c = ED.content_grid ? ED.content_grid->first : NULL; c; c = c->next)
        if (!strcmp(c->text, lx_path_filename(g_ctx_path))) { tile_open(c); return; }
}
static void m_set_startup(UiNode *n) {
    LX_UNUSED(n);
    ed_relative_asset(ED.project.startup_scene, sizeof ED.project.startup_scene, g_ctx_path);
    lx_project_save(&ED.project);
    ui_notify(ED.ui, "Startup scene set");
}

static void tile_menu(UiNode *n) {
    lx_path_join(g_ctx_path, sizeof g_ctx_path, ED.content_dir, n->text);
    tile_click(n);
    LxVec2 m = lx_mouse_pos();
    UiNode *p = ui_popup(ED.ui, m.x, m.y, 210);
    UiNode *it;
    it = ui_menu_item(ED.ui, p, "Open", "Double-click", m_open_asset); it->icon = UI_ICON_OPEN;
    if (ed_icon_for_file(n->text, false) == UI_ICON_SCENE) { it = ui_menu_item(ED.ui, p, "Set as Startup Scene", NULL, m_set_startup); it->icon = UI_ICON_HOME; }
    it = ui_menu_item(ED.ui, p, "Rename", NULL, m_rename_asset); it->icon = UI_ICON_FILE;
    it = ui_menu_item(ED.ui, p, "Show in File Manager", NULL, m_show_asset); it->icon = UI_ICON_FOLDER_OPEN;
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "Delete", NULL, m_delete_asset); it->icon = UI_ICON_TRASH; it->icon_color = ui_theme(ED.ui)->danger;
}

static void folder_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(type);
    char from[LX_PATH_MAX], dir[LX_PATH_MAX], to[LX_PATH_MAX];
    lx_asset_path(from, sizeof from, payload);
    lx_path_join(dir, sizeof dir, ED.content_dir, n->text);
    lx_path_join(to, sizeof to, dir, lx_path_filename(from));
    if (!strcmp(from, dir)) return;
    if (lx_path_rename(from, to)) ui_notify(ED.ui, "Moved %s", lx_path_filename(from));
    ED.content_dirty = true;
}

static void tree_click(UiNode *n) { ed_content_open_dir(n->path); }

static void add_tree(UiNode *list, const char *dir, int depth) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    if (depth > 6) return;
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n; i++) {
        if (!e[i].is_dir || e[i].name[0] == '.') continue;
        char full[LX_PATH_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        UiNode *b = ui_button(ui, list, e[i].name, tree_click);
        lx_strcpy(b->path, sizeof b->path, full);
        b->icon = !strncmp(ED.content_dir, full, strlen(full)) ? UI_ICON_FOLDER_OPEN : UI_ICON_FOLDER;
        b->icon_color = ed_color_for_icon(UI_ICON_FOLDER);
        b->center_text = false;
        b->bg = 0;
        b->h = 24;
        b->icon_size = 14;
        ui_pad(b, depth * 14.0f, 0, 0, 0);
        b->selected = !strcmp(ED.content_dir, full);
        b->bg_active = t->selection;
        b->tag = 0x74726565;
        add_tree(list, full, depth + 1);
    }
    lx_free(e);
}

static void content_frame(UiNode *panel) {
    LX_UNUSED(panel);
    if (!ED.content_dirty || !ED.content_grid) return;
    ED.content_dirty = false;
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    if (!lx_dir_exists(ED.content_dir)) lx_strcpy(ED.content_dir, sizeof ED.content_dir, ED.project.assets);
    /* folder tree */
    ui_clear(ui, ED.content_tree);
    UiNode *rootb = ui_button(ui, ED.content_tree, "Assets", tree_click);
    lx_strcpy(rootb->path, sizeof rootb->path, ED.project.assets);
    rootb->icon = UI_ICON_FOLDER_OPEN;
    rootb->icon_color = ed_color_for_icon(UI_ICON_FOLDER);
    rootb->center_text = false;
    rootb->bg = 0;
    rootb->h = 24;
    rootb->selected = !strcmp(ED.content_dir, ED.project.assets);
    rootb->bg_active = t->selection;
    rootb->tag = 0x74726565;
    rootb->font = UI_FONT_BOLD;
    add_tree(ED.content_tree, ED.project.assets, 1);
    /* breadcrumbs */
    ui_clear(ui, ED.content_path);
    char rel[LX_PATH_MAX];
    ed_relative_asset(rel, sizeof rel, ED.content_dir);
    char acc[LX_PATH_MAX];
    lx_strcpy(acc, sizeof acc, ED.project.assets);
    UiNode *c0 = ui_button(ui, ED.content_path, "Assets", go_crumb);
    lx_strcpy(c0->path, sizeof c0->path, acc);
    c0->bg = 0;
    c0->h = 24;
    if (strcmp(ED.content_dir, ED.project.assets)) {
        char *tok = rel, *slash;
        while (tok && *tok) {
            slash = strchr(tok, '/');
            if (slash) *slash = 0;
            UiNode *sep = ui_label(ui, ED.content_path, "");
            sep->icon = UI_ICON_CHEVRON_RIGHT;
            sep->icon_size = 10;
            sep->w = 12;
            sep->icon_color = t->text_dim;
            char next[LX_PATH_MAX];
            lx_path_join(next, sizeof next, acc, tok);
            lx_strcpy(acc, sizeof acc, next);
            UiNode *c = ui_button(ui, ED.content_path, tok, go_crumb);
            lx_strcpy(c->path, sizeof c->path, acc);
            c->bg = 0;
            c->h = 24;
            tok = slash ? slash + 1 : NULL;
        }
    }
    /* grid */
    UiNode *grid = ED.content_grid;
    ui_clear(ui, grid);
    grid->wrap = !ED.content_list_view;
    grid->scroll = true;
    grid->dir = ED.content_list_view ? UI_COL : UI_ROW;
    LxDirEntry *e;
    int n = lx_dir_list(ED.content_dir, &e);
    int shown = 0;
    float tile = ED.content_tile;
    for (int i = 0; i < n; i++) {
        if (e[i].name[0] == '.') continue;
        if (!contains_ci(e[i].name, ED.content_search)) continue;
        UiIcon icon = ed_icon_for_file(e[i].name, e[i].is_dir);
        char full[LX_PATH_MAX], ref[LX_REF_MAX];
        lx_path_join(full, sizeof full, ED.content_dir, e[i].name);
        ed_relative_asset(ref, sizeof ref, full);
        UiNode *tl = ui_box(ui, grid, ED.content_list_view ? UI_ROW : UI_COL);
        lx_strcpy(tl->text, sizeof tl->text, e[i].name);
        tl->bg_hover = t->hover;
        tl->radius = 6;
        tl->on_click = tile_click;
        tl->on_double_click = tile_open;
        tl->on_right_click = tile_menu;
        tl->drag_type = drag_type_for(icon);
        lx_strcpy(tl->drag_payload, sizeof tl->drag_payload, ref);
        tl->selected = !strcmp(ED.selected_asset, full);
        lx_strcpy(tl->tooltip, sizeof tl->tooltip, ref);
        if (e[i].is_dir) { tl->accept = "mesh,material,texture,sound,scene,script,file,folder"; tl->on_drop = folder_drop; }
        uint32_t thumb = icon == UI_ICON_TEXTURE ? thumbnail(full) : 0;
        if (ED.content_list_view) {
            tl->h = 26;
            ui_pad(tl, 8, 0, 8, 0);
            tl->gap = 8;
            tl->cross_align = UI_ALIGN_CENTER;
            UiNode *l = ui_label(ui, tl, e[i].name);
            l->icon = icon;
            l->icon_color = ed_color_for_icon(icon);
            l->grow = 1;
            UiNode *sz = ui_label(ui, tl, "");
            if (!e[i].is_dir) ui_set_text(sz, e[i].size > 1024 * 1024 ? "%.1f MB" : "%.1f KB", e[i].size > 1024 * 1024 ? (double)e[i].size / 1048576.0 : (double)e[i].size / 1024.0);
            sz->w = 90;
            sz->fg = t->text_dim;
            sz->font = UI_FONT_SMALL;
        } else {
            tl->w = tile;
            tl->h = tile + 22;
            ui_pad(tl, 6, 6, 6, 4);
            tl->gap = 2;
            if (thumb) {
                UiNode *img = ui_image(ui, tl, thumb);
                img->h = tile - 18;
                img->tex_aspect = 1.0f;
            } else {
                UiNode *ic = ui_label(ui, tl, "");
                ic->icon = icon;
                ic->icon_size = tile * 0.55f;
                ic->icon_color = ed_color_for_icon(icon);
                ic->center_text = true;
                ic->h = tile - 18;
            }
            UiNode *nm = ui_label(ui, tl, e[i].name);
            nm->font = UI_FONT_SMALL;
            nm->center_text = true;
            nm->h = 18;
        }
        shown++;
    }
    lx_free(e);
    if (!shown) {
        UiNode *msg = ui_label(ui, grid, ED.content_search[0] ? "No assets match your search." : "This folder is empty. Right-click to create assets.");
        msg->fg = t->text_dim;
        msg->w = 400;
        msg->h = 40;
        ui_pad(msg, 10, 0, 0, 0);
    }
}

static void tile_size_changed(UiNode *n) { LX_UNUSED(n); ED.content_dirty = true; }

UiNode *ed_content_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Content Browser");
    p->icon = UI_ICON_FOLDER;
    p->bg = t->panel;
    p->on_frame = content_frame;
    UiNode *bar = ui_box(ui, p, UI_ROW);
    bar->h = 34;
    ui_pad(bar, 6, 4, 6, 4);
    bar->gap = 4;
    bar->cross_align = UI_ALIGN_CENTER;
    UiNode *add = ui_button(ui, bar, "Create", create_asset_menu);
    add->icon = UI_ICON_PLUS;
    add->h = 26;
    add->bg = t->panel_alt;
    ui_icon_button(ui, bar, UI_ICON_UP, "Parent folder", go_up);
    ui_icon_button(ui, bar, UI_ICON_REFRESH, "Refresh", refresh_click);
    ED.content_path = ui_box(ui, bar, UI_ROW);
    ED.content_path->grow = 1;
    ED.content_path->clip = true;
    ED.content_path->cross_align = UI_ALIGN_CENTER;
    UiNode *search = ui_textfield(ui, bar, ED.content_search, sizeof ED.content_search);
    search->w = 200;
    search->h = 26;
    search->icon = UI_ICON_SEARCH;
    lx_strcpy(search->hint, sizeof search->hint, "Search assets");
    search->on_change = content_search_changed;
    search->tag = 0x6c697665;
    UiNode *size = ui_slider(ui, bar, &ED.content_tile, 56, 160);
    size->w = 90;
    size->h = 22;
    size->precision = 0;
    size->on_change = tile_size_changed;
    lx_strcpy(size->tooltip, sizeof size->tooltip, "Tile size");
    UiNode *view = ui_icon_button(ui, bar, UI_ICON_TILES, "Toggle list / tiles", toggle_view);
    view->w = 28;
    UiNode *split = ui_split(ui, p, UI_ROW, 0.2f);
    split->split_min = 120;
    UiNode *tree = ui_box(ui, split, UI_COL);
    tree->scroll = true;
    tree->bg = t->panel_alt;
    ui_pad(tree, 4, 4, 4, 4);
    tree->gap = 1;
    ED.content_tree = tree;
    UiNode *grid = ui_box(ui, split, UI_ROW);
    grid->wrap = true;
    grid->scroll = true;
    ui_pad(grid, 8, 8, 8, 8);
    grid->gap = 6;
    grid->on_right_click = grid_menu;
    ED.content_grid = grid;
    ED.content_dirty = true;
    return p;
}

/* ======================================================================
 *  CONSOLE
 * ====================================================================== */
static UiNode *g_console_list, *g_console_toggles[3];
static void console_clear(UiNode *n) {
    LX_UNUSED(n);
    if (g_log_lock) lx_mutex_lock(g_log_lock);
    ED.log_count = ED.log_start = 0;
    ED.errors = ED.warnings = 0;
    ED.console_dirty = true;
    if (g_log_lock) lx_mutex_unlock(g_log_lock);
}
static void console_toggle(UiNode *n) { ED.console_show[n->tag] = !ED.console_show[n->tag]; ED.console_dirty = true; }
static void console_filter_changed(UiNode *n) { LX_UNUSED(n); ED.console_dirty = true; }
static void console_copy(UiNode *n) { lx_platform_clipboard_set(n->text); ui_notify(ED.ui, "Copied to clipboard"); }

static void console_frame(UiNode *panel) {
    LX_UNUSED(panel);
    if (!ED.console_dirty || !g_console_list) return;
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    if (g_log_lock) lx_mutex_lock(g_log_lock);
    ED.console_dirty = false;
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < ED.log_count; i++) counts[ED.log[(ED.log_start + i) % ED_MAX_LOG].level]++;
    static const char *names[3] = {"Info", "Warnings", "Errors"};
    for (int k = 0; k < 3; k++) {
        ui_set_text(g_console_toggles[k], "%d %s", counts[k], names[k]);
        g_console_toggles[k]->selected = ED.console_show[k];
    }
    bool at_bottom = g_console_list->scroll_pos >= g_console_list->scroll_max - 4;
    ui_clear(ui, g_console_list);
    int start = ED.log_count > 400 ? ED.log_count - 400 : 0;
    for (int i = start; i < ED.log_count; i++) {
        const EdLogLine *l = &ED.log[(ED.log_start + i) % ED_MAX_LOG];
        if (!ED.console_show[l->level] || !contains_ci(l->text, ED.console_filter)) continue;
        UiNode *row = ui_button(ui, g_console_list, l->text, NULL);
        row->on_double_click = console_copy;
        row->center_text = false;
        row->font = UI_FONT_MONO;
        row->h = 22;
        row->bg = (i & 1) ? t->panel_alt : 0;
        row->radius = 0;
        row->icon = l->level == LX_LOG_ERROR ? UI_ICON_ERROR : l->level == LX_LOG_WARN ? UI_ICON_WARNING : UI_ICON_INFO;
        row->icon_color = l->level == LX_LOG_ERROR ? t->danger : l->level == LX_LOG_WARN ? t->warning : t->text_dim;
        row->icon_size = 14;
        row->fg = l->level == LX_LOG_ERROR ? ui_lerp_color(t->text, t->danger, 0.4f) : t->text;
        lx_strcpy(row->tooltip, sizeof row->tooltip, "Double-click to copy");
    }
    if (g_log_lock) lx_mutex_unlock(g_log_lock);
    if (ED.console_autoscroll || at_bottom) g_console_list->scroll_pos = 1e9f;
}

UiNode *ed_console_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Console");
    p->icon = UI_ICON_CONSOLE;
    p->bg = t->panel;
    p->on_frame = console_frame;
    UiNode *bar = ui_box(ui, p, UI_ROW);
    bar->h = 34;
    ui_pad(bar, 6, 4, 6, 4);
    bar->gap = 4;
    UiNode *clr = ui_button(ui, bar, "Clear", console_clear);
    clr->icon = UI_ICON_TRASH;
    clr->h = 26;
    clr->bg = t->panel_alt;
    static const UiIcon ic[3] = {UI_ICON_INFO, UI_ICON_WARNING, UI_ICON_ERROR};
    uint32_t cols[3] = {t->text_dim, t->warning, t->danger};
    for (int k = 0; k < 3; k++) {
        UiNode *b = ui_button(ui, bar, "", console_toggle);
        b->icon = ic[k];
        b->icon_color = cols[k];
        b->tag = k;
        b->h = 26;
        b->w = 110;
        b->bg_active = t->selection;
        g_console_toggles[k] = b;
    }
    UiNode *auto_cb = ui_checkbox(ui, bar, "Auto-scroll", &ED.console_autoscroll);
    auto_cb->w = 110;
    ui_spacer(ui, bar);
    UiNode *f = ui_textfield(ui, bar, ED.console_filter, sizeof ED.console_filter);
    f->w = 220;
    f->h = 26;
    f->icon = UI_ICON_FILTER;
    lx_strcpy(f->hint, sizeof f->hint, "Filter messages");
    f->on_change = console_filter_changed;
    f->tag = 0x6c697665;
    g_console_list = ui_box(ui, p, UI_COL);
    g_console_list->grow = 1;
    g_console_list->scroll = true;
    ED.console_dirty = true;
    return p;
}

/* ======================================================================
 *  AUDIO MIXER
 * ====================================================================== */
static UiNode *g_mixer_sources;
static uint64_t g_mixer_hash;
static void audio_changed(UiNode *n) {
    LX_UNUSED(n);
    ED.project.audio = *lx_audio_settings();
    lx_project_save(&ED.project);
}
static void mixer_play(UiNode *n) { lx_audio_play(ED.scene, (LxEntity)n->tag); }
static void mixer_stop(UiNode *n) { lx_audio_stop(ED.scene, (LxEntity)n->tag); }
static void mixer_select(UiNode *n) { ed_select((LxEntity)n->tag); }
static void stop_all(UiNode *n) { LX_UNUSED(n); lx_audio_stop_all(); }

static void mixer_frame(UiNode *panel) {
    LX_UNUSED(panel);
    if (!ED.scene || !g_mixer_sources) return;
    uint64_t h = (uint64_t)(uintptr_t)ED.scene ^ ED.hierarchy_hash;
    if (h == g_mixer_hash) return;
    g_mixer_hash = h;
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    ui_clear(ui, g_mixer_sources);
    int n = lx_scene_entity_count(ED.scene), found = 0;
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        LxAudioSource *a = lx_get_audio_source(ED.scene, e);
        if (!a) continue;
        UiNode *row = ui_box(ui, g_mixer_sources, UI_ROW);
        row->h = 28;
        row->gap = 6;
        row->cross_align = UI_ALIGN_CENTER;
        UiNode *nb = ui_button(ui, row, lx_entity_name(ED.scene, e), mixer_select);
        nb->icon = UI_ICON_SPEAKER;
        nb->icon_color = ed_color_for_icon(UI_ICON_SPEAKER);
        nb->center_text = false;
        nb->bg = 0;
        nb->w = 170;
        nb->tag = (intptr_t)e;
        UiNode *clip = ui_label(ui, row, a->clip[0] ? a->clip : "(no clip)");
        clip->fg = t->text_dim;
        clip->font = UI_FONT_SMALL;
        clip->grow = 1;
        UiNode *vol = ui_slider(ui, row, &a->volume, 0, 2);
        vol->w = 120;
        vol->on_change = ed_field_changed;
        UiNode *p = ui_icon_button(ui, row, UI_ICON_PLAY, "Play", mixer_play);
        p->tag = (intptr_t)e;
        p->icon_color = t->success;
        UiNode *s = ui_icon_button(ui, row, UI_ICON_STOP, "Stop", mixer_stop);
        s->tag = (intptr_t)e;
        found++;
    }
    if (!found) {
        UiNode *l = ui_label(ui, g_mixer_sources, "No audio sources in this scene. Create one from Create > Audio Source.");
        l->fg = t->text_dim;
    }
}

UiNode *ed_mixer_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxAudioSettings *as = lx_audio_settings();
    UiNode *p = ui_box(ui, parent, UI_ROW);
    lx_strcpy(p->text, sizeof p->text, "Audio Mixer");
    p->icon = UI_ICON_SOUND;
    p->bg = t->panel;
    p->on_frame = mixer_frame;
    ui_pad(p, 10, 10, 10, 10);
    p->gap = 16;
    /* channel strips */
    UiNode *strips = ui_box(ui, p, UI_ROW);
    strips->gap = 10;
    strips->w = 430;
    static const char *names[] = {"Master", "SFX", "Music", "Voice", "Ambient"};
    for (int i = 0; i < 5; i++) {
        UiNode *col = ui_box(ui, strips, UI_COL);
        col->w = 78;
        col->bg = t->panel_alt;
        col->radius = 6;
        ui_pad(col, 6, 8, 6, 8);
        col->gap = 6;
        UiNode *l = ui_label(ui, col, names[i]);
        l->font = UI_FONT_BOLD;
        l->center_text = true;
        UiNode *ic = ui_label(ui, col, "");
        ic->icon = i == 0 ? UI_ICON_SPEAKER : i == 2 ? UI_ICON_SOUND : i == 3 ? UI_ICON_LISTENER : UI_ICON_SPEAKER;
        ic->icon_color = i == 0 ? t->accent : ed_color_for_icon(UI_ICON_SOUND);
        ic->icon_size = 26;
        ic->center_text = true;
        ic->h = 34;
        UiNode *s = ui_slider(ui, col, i == 0 ? &as->master_volume : &as->bus_volume[i - 1], 0, 1.5f);
        s->h = 24;
        s->on_change = audio_changed;
        ui_spacer(ui, col);
    }
    UiNode *right = ui_box(ui, p, UI_COL);
    right->grow = 1;
    right->gap = 6;
    UiNode *top = ui_box(ui, right, UI_ROW);
    top->h = 26;
    top->gap = 8;
    UiNode *t1 = ui_label(ui, top, "Audio Sources in Scene");
    t1->font = UI_FONT_BOLD;
    t1->grow = 1;
    UiNode *sa = ui_button(ui, top, "Stop All", stop_all);
    sa->icon = UI_ICON_STOP;
    sa->w = 100;
    g_mixer_sources = ui_box(ui, right, UI_COL);
    g_mixer_sources->grow = 1;
    g_mixer_sources->scroll = true;
    g_mixer_sources->gap = 2;
    g_mixer_hash = 0;
    return p;
}

/* ======================================================================
 *  SCENE MANAGER
 * ====================================================================== */
static UiNode *g_scene_list;
static bool g_scenes_dirty = true;
static char g_scene_action[LX_REF_MAX];

static void scene_open(UiNode *n) {
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode first"); return; }
    lx_strcpy(g_scene_to_open, sizeof g_scene_to_open, n->drag_payload);
    if (ed_confirm_discard(open_scene_now)) open_scene_now();
    g_scenes_dirty = true;
}
static void scene_startup(UiNode *n) {
    lx_strcpy(ED.project.startup_scene, sizeof ED.project.startup_scene, n->drag_payload);
    lx_project_save(&ED.project);
    g_scenes_dirty = true;
}
static void scene_build_toggle(UiNode *n) {
    LxBuildSettings *b = &ED.project.build;
    lx_strcpy(g_scene_action, sizeof g_scene_action, n->drag_payload);
    for (int i = 0; i < b->scene_count; i++)
        if (!strcmp(b->scenes[i], g_scene_action)) {
            memmove(b->scenes[i], b->scenes[i + 1], (size_t)(b->scene_count - i - 1) * LX_REF_MAX);
            b->scene_count--;
            lx_project_save(&ED.project);
            g_scenes_dirty = true;
            return;
        }
    if (b->scene_count < LX_MAX_BUILD_SCENES) lx_strcpy(b->scenes[b->scene_count++], LX_REF_MAX, g_scene_action);
    lx_project_save(&ED.project);
    g_scenes_dirty = true;
}
static void scenes_new(UiNode *n) { LX_UNUSED(n); ed_new_scene_dialog(); g_scenes_dirty = true; }
static void scenes_refresh(UiNode *n) { LX_UNUSED(n); g_scenes_dirty = true; }

static void find_scenes(const char *dir, const char *rel, UiNode *list, int *count) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n; i++) {
        if (e[i].name[0] == '.') continue;
        char full[LX_PATH_MAX], r[LX_REF_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, e[i].name);
        else lx_strcpy(r, sizeof r, e[i].name);
        if (e[i].is_dir) { find_scenes(full, r, list, count); continue; }
        if (!ext_is(e[i].name, ".lxscene")) continue;
        bool startup = !strcmp(ED.project.startup_scene, r), current = !strcmp(ED.scene_ref, r), in_build = false;
        for (int k = 0; k < ED.project.build.scene_count; k++) in_build |= !strcmp(ED.project.build.scenes[k], r);
        UiNode *row = ui_box(ui, list, UI_ROW);
        row->h = 32;
        row->gap = 8;
        ui_pad(row, 8, 0, 8, 0);
        row->cross_align = UI_ALIGN_CENTER;
        row->bg = current ? t->selection : ((*count & 1) ? t->panel_alt : 0);
        row->radius = 4;
        row->on_double_click = scene_open;
        lx_strcpy(row->drag_payload, sizeof row->drag_payload, r);
        UiNode *l = ui_label(ui, row, r);
        l->icon = UI_ICON_SCENE;
        l->icon_color = ed_color_for_icon(UI_ICON_SCENE);
        l->grow = 1;
        if (startup) { UiNode *b = ui_label(ui, row, "Startup"); b->icon = UI_ICON_HOME; b->icon_size = 13; b->font = UI_FONT_SMALL; b->fg = t->accent; b->w = 80; }
        if (current) { UiNode *b = ui_label(ui, row, "Open"); b->font = UI_FONT_SMALL; b->fg = t->text_dim; b->w = 44; }
        UiNode *ob = ui_button(ui, row, "Open", scene_open);
        ob->w = 70;
        ob->h = 24;
        lx_strcpy(ob->drag_payload, sizeof ob->drag_payload, r);
        ob->drag_type = NULL;
        UiNode *sb2 = ui_button(ui, row, "Set Startup", scene_startup);
        sb2->w = 100;
        sb2->h = 24;
        sb2->disabled = startup;
        lx_strcpy(sb2->drag_payload, sizeof sb2->drag_payload, r);
        UiNode *bb = ui_button(ui, row, in_build ? "In Build" : "Add to Build", scene_build_toggle);
        bb->w = 110;
        bb->h = 24;
        bb->icon = in_build ? UI_ICON_CHECK : UI_ICON_PLUS;
        bb->selected = in_build;
        lx_strcpy(bb->drag_payload, sizeof bb->drag_payload, r);
        (*count)++;
    }
    lx_free(e);
}

static void scenes_frame(UiNode *panel) {
    LX_UNUSED(panel);
    static char last_ref[LX_REF_MAX];
    if (strcmp(last_ref, ED.scene_ref)) { lx_strcpy(last_ref, sizeof last_ref, ED.scene_ref); g_scenes_dirty = true; }
    if (ED.content_dirty) g_scenes_dirty = true;
    if (!g_scenes_dirty || !g_scene_list) return;
    g_scenes_dirty = false;
    ui_clear(ED.ui, g_scene_list);
    int count = 0;
    find_scenes(ED.project.assets, "", g_scene_list, &count);
    if (!count) ui_label(ED.ui, g_scene_list, "No scenes saved yet. Use File > Save Scene.")->fg = ui_theme(ED.ui)->text_dim;
}

UiNode *ed_scenes_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Scenes");
    p->icon = UI_ICON_SCENE;
    p->bg = t->panel;
    p->on_frame = scenes_frame;
    UiNode *bar = ui_box(ui, p, UI_ROW);
    bar->h = 34;
    ui_pad(bar, 6, 4, 6, 4);
    bar->gap = 4;
    UiNode *nb = ui_button(ui, bar, "New Scene", scenes_new);
    nb->icon = UI_ICON_PLUS;
    nb->h = 26;
    nb->bg = t->panel_alt;
    ui_icon_button(ui, bar, UI_ICON_REFRESH, "Refresh", scenes_refresh);
    ui_spacer(ui, bar);
    UiNode *hint = ui_label(ui, bar, "Double-click a scene to open it");
    hint->font = UI_FONT_SMALL;
    hint->fg = t->text_dim;
    hint->w = 220;
    g_scene_list = ui_box(ui, p, UI_COL);
    g_scene_list->grow = 1;
    g_scene_list->scroll = true;
    ui_pad(g_scene_list, 6, 4, 6, 4);
    g_scene_list->gap = 2;
    g_scenes_dirty = true;
    return p;
}
