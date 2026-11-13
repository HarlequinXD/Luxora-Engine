/* Scripted smoke test (--smoke): drives the editor through its windows and tools over a
 * few hundred frames, then quits. Used by CI and local testing to catch crashes and leaks. */
#include "editor.h"

typedef void (*SmokeFn)(void);

static void s_create_all(void) {
    static const char *what[] = {"Empty", "Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Camera", "Directional Light",
                                 "Point Light", "Spot Light", "Audio Source", "Crowd"};
    for (int i = 0; i < 12; i++) ed_create(what[i]);
}
static void s_undo(void) { for (int i = 0; i < 4; i++) ed_undo(); }
static void s_redo(void) { for (int i = 0; i < 2; i++) ed_redo(); }
static void s_select_cube(void) { ed_select(lx_entity_find(ED.scene, "Cube")); ED.gizmo = ED_GIZMO_ROTATE; }
static void s_duplicate(void) { ed_duplicate_selected(); ED.gizmo = ED_GIZMO_SCALE; }
static void s_focus(void) { ed_focus_selected(); }
static void s_delete(void) { ed_delete_selected(); }
static void s_settings(int c) { ui_close_modal(ED.ui); ed_project_settings(c); }
static void s_set0(void) { s_settings(0); }
static void s_set1(void) { s_settings(1); }
static void s_set2(void) { s_settings(2); }
static void s_set3(void) { s_settings(3); }
static void s_set4(void) { s_settings(4); }
static void s_build_settings(void) { ui_close_modal(ED.ui); ed_build_settings(); }
static void s_about(void) { ui_close_modal(ED.ui); ed_about(); }
static void s_shortcuts(void) { ui_close_modal(ED.ui); ed_shortcuts_help(); }
static void s_close(void) { ui_close_modal(ED.ui); ui_close_popups(ED.ui); }
static void s_tabs(void) {
    static int t;
    if (ED.bottom_tabs) ED.bottom_tabs->tab_index = t % 4;
    if (ED.inspector_tabs) ED.inspector_tabs->tab_index = t % 3;
    t++;
}
static void s_play(void) { ed_play(); }
static void s_pause(void) { ed_pause(); }
static void s_step(void) { ed_step(); }
static void s_stop(void) { ed_stop(); }
static void s_material(void) {
    char p[LX_PATH_MAX];
    lx_path_join(p, sizeof p, ED.project.assets, "Materials/Ground.lxmat");
    ed_select_asset(p);
}
static void s_asset_none(void) { ed_select_asset(""); ed_select(lx_entity_find(ED.scene, "Directional Light")); }
static void s_content_folder(void) {
    char p[LX_PATH_MAX];
    lx_path_join(p, sizeof p, ED.project.assets, "Scenes");
    ed_content_open_dir(p);
    ED.content_list_view = !ED.content_list_view;
}
static void s_content_root(void) { ed_content_open_dir(ED.project.assets); }
static void s_save(void) { ed_save_scene(); }
static void s_new_scene(void) { ED.dirty = false; ed_new_scene("Smoke Scene"); }
static void s_reopen(void) { ED.dirty = false; ed_open_scene(ED.project.startup_scene); }
static void s_layout(void) { ed_set_mode(ED_MODE_EDITOR); }
static void s_menu(void) {
    extern void ed_create_menu_at(float x, float y);
    ed_create_menu_at(100, 100);
}
static void s_game_view(void) { ED.game_view = !ED.game_view; ED.show_stats = !ED.show_stats; }
static void s_rename_dialog(void) { ed_rename_dialog(lx_entity_find(ED.scene, "Cube")); }

static const struct { int frame; SmokeFn fn; const char *what; } STEPS[] = {
    {5, s_create_all, "create every object type"},
    {10, s_undo, "undo"},
    {14, s_redo, "redo"},
    {18, s_select_cube, "select"},
    {22, s_duplicate, "duplicate"},
    {26, s_focus, "focus"},
    {30, s_delete, "delete"},
    {34, s_set0, "project settings: general"},
    {40, s_set1, "project settings: graphics"},
    {46, s_set2, "project settings: sound"},
    {52, s_set3, "project settings: input"},
    {60, s_set4, "project settings: platforms"},
    {66, s_build_settings, "build settings"},
    {72, s_about, "about"},
    {78, s_shortcuts, "shortcuts"},
    {84, s_close, "close dialogs"},
    {88, s_tabs, "tabs"},
    {92, s_tabs, "tabs"},
    {96, s_tabs, "tabs"},
    {100, s_tabs, "tabs"},
    {104, s_material, "material inspector"},
    {110, s_asset_none, "light inspector"},
    {114, s_content_folder, "content browser navigation"},
    {120, s_content_root, "content browser root"},
    {124, s_menu, "create menu"},
    {128, s_close, "close menus"},
    {132, s_play, "play"},
    {150, s_pause, "pause"},
    {154, s_step, "step"},
    {158, s_game_view, "game view"},
    {170, s_stop, "stop"},
    {174, s_save, "save scene"},
    {180, s_rename_dialog, "rename dialog"},
    {186, s_close, "close"},
    {190, s_new_scene, "new scene"},
    {196, s_reopen, "reopen startup scene"},
    {202, s_layout, "rebuild layout"},
};

bool ed_smoke_step(int frame) {
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++)
        if (STEPS[i].frame == frame) {
            LX_INFO("[smoke] %s", STEPS[i].what);
            if (ED.mode == ED_MODE_EDITOR) STEPS[i].fn();
        }
    return frame < 215;
}
