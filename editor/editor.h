/* Inferno (the Luxora engine editor): shared state between editor modules. */
#ifndef LX_EDITOR_H
#define LX_EDITOR_H
#include "ui.h"
#include <ctype.h>

#define ED_MAX_UNDO 100
#define ED_MAX_LOG 1000
#define ED_MAX_RECENT 12

typedef enum EdMode { ED_MODE_PROJECT_MANAGER, ED_MODE_EDITOR } EdMode;
typedef enum EdPlayState { ED_EDIT, ED_PLAYING, ED_PAUSED } EdPlayState;
typedef enum EdGizmo { ED_GIZMO_SELECT, ED_GIZMO_MOVE, ED_GIZMO_ROTATE, ED_GIZMO_SCALE } EdGizmo;

typedef struct EdLogLine { LxLogLevel level; char text[300]; double time; } EdLogLine;

typedef struct Editor {
    UiCtx *ui;
    EdMode mode;
    bool quit;
    float dt, fps;
    double time;

    /* project */
    LxProject project;
    bool has_project;
    /* Opening/closing a project frees GPU resources the current frame may already
       be using, so requests made mid-frame (menu clicks) run once the frame ends. */
    bool in_frame;
    bool pending_close;
    char pending_open[LX_PATH_MAX];
    char recent[ED_MAX_RECENT][LX_PATH_MAX];
    char engine_root_pref[LX_PATH_MAX];   /* user-chosen engine folder (Build Settings), "" = automatic */
    int recent_count;

    /* scene */
    LxScene *scene;
    char scene_ref[LX_REF_MAX];      /* relative to Assets, "" = unsaved */
    bool dirty;
    bool save_prompt_open;
    EdPlayState play;
    char *play_snapshot;             /* scene text before entering play mode */
    LxEntity selected;
    char selected_asset[LX_PATH_MAX];/* absolute path of the selected asset ("" = none) */

    /* undo */
    char *undo[ED_MAX_UNDO];
    int undo_count;
    char *redo[ED_MAX_UNDO];
    int redo_count;
    char *snapshot;                  /* state after the last committed change */
    bool change_pending;

    /* viewport & editor camera */
    LxRenderTarget *viewport_rt;
    LxVec3 cam_pos;
    float cam_yaw, cam_pitch, cam_speed;
    float cam_fov;
    EdGizmo gizmo;
    bool gizmo_local, show_grid, show_icons, snap, show_stats;
    float snap_move, snap_rotate, snap_scale;
    bool game_view;                  /* render through the scene's main camera */
    bool game_view_before_play;

    /* panels */
    UiNode *root, *menubar, *toolbar, *status;
    UiNode *hierarchy, *hierarchy_list, *hierarchy_search_box;
    UiNode *inspector_tabs, *inspector, *lighting_panel, *stats_panel;
    UiNode *bottom_tabs, *content_panel, *console_panel, *mixer_panel, *scenes_panel;
    UiNode *viewport;
    uint64_t hierarchy_hash;
    char hierarchy_search[64];
    LxEntity inspector_entity;
    uint32_t inspector_comps;
    const void *inspector_storage;   /* scene entity array the inspector is bound to */
    char inspector_asset[LX_PATH_MAX];
    bool inspector_dirty;
    LxEntity expanded[256];
    int expanded_count;

    /* content browser */
    char content_dir[LX_PATH_MAX];
    char content_search[64];
    bool content_dirty, content_list_view;
    float content_tile;
    UiNode *content_tree, *content_grid, *content_path;

    /* console */
    EdLogLine log[ED_MAX_LOG];
    int log_count, log_start;
    bool console_dirty, console_show[3], console_autoscroll;
    char console_filter[64];
    int errors, warnings;

    /* build */
    volatile long building;
    char build_status[160];
    float build_progress;
} Editor;

extern Editor ED;

/* main / shell */
void ed_set_mode(EdMode mode);
void ed_open_project(const char *root);
void ed_close_project(void);
void ed_request_close_project(void);   /* close and return to the Project Manager */
void ed_run_pending(void);             /* called by the main loop between frames */
void ed_build_shell(void);
void ed_update(float dt);
void ed_render(void);
bool ed_viewport_showing_game(void);
void ed_shortcuts(void);
void ed_save_prefs(void);
void ed_load_prefs(void);
void ed_add_recent(const char *root);
void ed_update_title(void);

/* scene management */
void ed_new_scene(const char *name);
bool ed_open_scene(const char *ref);
bool ed_save_scene(void);
void ed_save_scene_as_dialog(void);
void ed_new_scene_dialog(void);
void ed_set_scene(LxScene *s, const char *ref);
bool ed_confirm_discard(void (*then)(void));
/* "Save changes to <scene> before <action>?" with Save / Don't Save / Cancel; then() runs after Save or Don't Save */
void ed_save_prompt(const char *action, void (*then)(void));
/* window chrome (chrome.c) */
void ed_chrome_init(void);
uint32_t ed_app_icon_texture(void);
UiNode *ed_app_icon(UiNode *parent, float size);
UiNode *ed_titlebar_begin(UiNode *parent);
void ed_titlebar_end(UiNode *bar, const char *title);
void ed_titlebar_set_title(const char *title);
void ed_chrome_forget(void);
void ed_chrome_frame_before_ui(void);
void ed_chrome_frame_after_ui(void);
void ed_request_quit(void);   /* close button, Alt+F4, File > Exit: asks to save first */

/* undo */
void ed_mark_changed(void);      /* something changed; committed when the edit gesture ends */
void ed_commit(void);
void ed_undo(void);
void ed_redo(void);
void ed_reset_history(void);

/* selection & entity actions */
void ed_select(LxEntity e);
void ed_select_asset(const char *abs_path);
LxEntity ed_create(const char *what);  /* "Empty", "Cube", "Point Light", ... */
void ed_delete_selected(void);
void ed_duplicate_selected(void);
void ed_focus_selected(void);
void ed_rename_dialog(LxEntity e);

/* play mode */
void ed_play(void);
void ed_pause(void);
void ed_stop(void);
void ed_step(void);

/* panels */
UiNode *ed_hierarchy_panel(UiNode *parent);
UiNode *ed_inspector_panel(UiNode *parent);
UiNode *ed_lighting_panel(UiNode *parent);
UiNode *ed_stats_panel(UiNode *parent);
UiNode *ed_content_panel(UiNode *parent);
UiNode *ed_console_panel(UiNode *parent);
UiNode *ed_mixer_panel(UiNode *parent);
UiNode *ed_scenes_panel(UiNode *parent);
UiNode *ed_viewport_panel(UiNode *parent);
void ed_content_refresh(void);
void ed_content_open_dir(const char *abs_dir);
void ed_log_sink(LxLogLevel level, const char *msg, void *user);
UiIcon ed_icon_for_entity(LxScene *s, LxEntity e);
UiIcon ed_icon_for_file(const char *name, bool dir);
uint32_t ed_color_for_icon(UiIcon icon);

/* windows */
void ed_project_settings(int category);  /* 0 general, 1 graphics, 2 sound, 3 input, 4 platforms */
void ed_build_settings(void);
void ed_about(void);
void ed_shortcuts_help(void);

/* project manager */
void ed_project_manager_build(void);

/* build pipeline */
void ed_build(LxPlatform target, bool run);
void ed_build_poll(void);
const char *ed_engine_root(void);       /* engine source folder, "" when none was found */
bool ed_prebuilt_player(LxPlatform target, char *out, size_t cap);   /* ready-made player shipped with the editor */
/* how a build for `target` would be made, and what's missing (Build Settings shows this) */
typedef struct EdBuildPlan {
    bool prebuilt;                       /* no compiling needed */
    bool can_build;
    char prebuilt_path[LX_PATH_MAX];
    char engine[LX_PATH_MAX];
    int custom_count;                    /* behaviours from your scripts used by scenes/prefabs */
    char custom[4][LX_NAME_MAX];
    int scripts;                         /* .c files under Assets */
    bool cmake;
    char problem[512];                   /* why it can't build, in plain words */
} EdBuildPlan;
void ed_build_plan(LxPlatform target, EdBuildPlan *plan);

/* helpers shared by panels */
UiNode *ed_panel_header(UiNode *parent, UiIcon icon, const char *title);
UiNode *ed_section(UiNode *parent, UiIcon icon, const char *title, bool *open, void (*on_menu)(UiNode *));
UiNode *ed_prop_row(UiNode *parent, const char *label);
UiNode *ed_vec3_row(UiNode *parent, const char *label, float *v, float step, void (*on_change)(UiNode *));
void ed_field_changed(UiNode *n);
void ed_relative_asset(char *out, size_t cap, const char *abs_path);
void ed_confirm(const char *title, const char *message, const char *ok_text, void (*on_ok)(void));
void ed_prompt(const char *title, const char *label, const char *initial, void (*on_ok)(const char *text));


void ed_materials_flush(bool force);
void ed_remember_scene(void);           /* editor.c: reopen this scene next time */   /* panels.c: write edited materials */

/* scripts.c: compiling Assets/Scripts with the built-in compiler */
bool ed_scripts_compile(const char *out_path, bool *had_scripts);
bool ed_scripts_compile_dir(const char *assets, const char *sdk_dir, const char *out_path, bool *had_scripts);
bool ed_scripts_refresh(bool force);
void ed_scripts_poll(void);
void ed_scripts_close(void);
int ed_script_errors(void);
bool ed_scripts_supported(void);
void ed_script_sdk_dir(char *out, size_t cap);
#endif
