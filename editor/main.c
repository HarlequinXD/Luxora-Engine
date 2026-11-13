/* Inferno (the Luxora engine editor) entry point.
 *
 * Command line (also used by CI for automated screenshots):
 *   luxora_editor                         start at the Project Manager
 *   luxora_editor <project folder>        open a project directly
 *   --new <parent> <name>                 create a sample project and open it
 *   --screenshot <file.ppm> --frames N    save a screenshot after N frames and quit
 *   --play                                enter play mode after opening
 *   --smoke                               scripted tour of every window and tool, then quit
 *   --build                               build the open project for this platform, then quit
 */
#include "editor.h"

Editor ED;

static void prefs_path(char *out, size_t cap) {
    char dir[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, lx_home_dir(), ".luxora");
    lx_mkdir_p(dir);
    lx_path_join(out, cap, dir, "editor.prefs");
}

void ed_save_prefs(void) {
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora editor preferences\n[editor]\ncamera_speed = %g\ngrid = %d\nsnap = %d\nicons = %d\n", ED.cam_speed, ED.show_grid,
                 ED.snap, ED.show_icons);
    for (int i = 0; i < ED.recent_count; i++) lx_sb_printf(&sb, "recent = %s\n", ED.recent[i]);
    if (ED.engine_root_pref[0]) lx_sb_printf(&sb, "engine_root = %s\n", ED.engine_root_pref);
    char path[LX_PATH_MAX];
    prefs_path(path, sizeof path);
    lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
}

void ed_load_prefs(void) {
    char path[LX_PATH_MAX];
    prefs_path(path, sizeof path);
    char *text = lx_file_read(path, NULL);
    if (!text) return;
    const char *cur = text;
    char k[64], v[LX_PATH_MAX];
    int r;
    ED.recent_count = 0;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r != 1) continue;
        if (!strcmp(k, "recent") && ED.recent_count < ED_MAX_RECENT) lx_strcpy(ED.recent[ED.recent_count++], LX_PATH_MAX, v);
        else if (!strcmp(k, "camera_speed")) ED.cam_speed = (float)atof(v);
        else if (!strcmp(k, "grid")) ED.show_grid = lx_parse_bool(v);
        else if (!strcmp(k, "snap")) ED.snap = lx_parse_bool(v);
        else if (!strcmp(k, "icons")) ED.show_icons = lx_parse_bool(v);
        else if (!strcmp(k, "engine_root")) lx_strcpy(ED.engine_root_pref, sizeof ED.engine_root_pref, v);
    }
    lx_free(text);
}

void ed_add_recent(const char *root) {
    for (int i = 0; i < ED.recent_count; i++)
        if (!strcmp(ED.recent[i], root)) {
            memmove(ED.recent[1], ED.recent[0], (size_t)i * LX_PATH_MAX);
            lx_strcpy(ED.recent[0], LX_PATH_MAX, root);
            ed_save_prefs();
            return;
        }
    int n = ED.recent_count < ED_MAX_RECENT ? ED.recent_count : ED_MAX_RECENT - 1;
    memmove(ED.recent[1], ED.recent[0], (size_t)n * LX_PATH_MAX);
    lx_strcpy(ED.recent[0], LX_PATH_MAX, root);
    ED.recent_count = n + 1;
    ed_save_prefs();
}

void ed_set_mode(EdMode mode) {
    ED.mode = mode;
    ED.save_prompt_open = false;
    ed_chrome_forget();
    ui_close_popups(ED.ui);
    ui_close_modal(ED.ui);
    ui_clear(ED.ui, ui_root(ED.ui));
    ED.hierarchy = ED.inspector = ED.viewport = NULL;
    ED.hierarchy_list = ED.content_grid = ED.content_tree = NULL;
    if (mode == ED_MODE_PROJECT_MANAGER) ed_project_manager_build();
    else ed_build_shell();
    ed_update_title();
}

void ed_update_title(void) {
    char t[256];
    if (ED.mode == ED_MODE_PROJECT_MANAGER || !ED.has_project) snprintf(t, sizeof t, "Inferno - Projects");
    else
        snprintf(t, sizeof t, "Inferno - %s - %s%s%s", ED.project.name, ED.scene ? lx_scene_name(ED.scene) : "", ED.dirty ? " *" : "",
                 ED.play != ED_EDIT ? "  [PLAYING]" : "");
    lx_platform_set_title(t);
    ed_titlebar_set_title(t);
}

int main(int argc, char **argv) {
    const char *open_dir = NULL, *new_parent = NULL, *new_name = NULL, *shot = NULL;
    int shot_frames = 60;
    bool auto_play = false, smoke = false, build_game = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--new") && i + 2 < argc) { new_parent = argv[++i]; new_name = argv[++i]; }
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shot_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--play")) auto_play = true;
        else if (!strcmp(argv[i], "--smoke")) smoke = true;
        else if (!strcmp(argv[i], "--build")) build_game = true;
        else if (argv[i][0] != '-') open_dir = argv[i];
    }
    memset(&ED, 0, sizeof ED);
    ED.cam_speed = 6.0f;
    ED.show_grid = true;
    ED.show_icons = true;
    ED.cam_fov = 60.0f;
    ED.gizmo = ED_GIZMO_MOVE;
    ED.snap_move = 0.5f;
    ED.snap_rotate = 15.0f;
    ED.snap_scale = 0.1f;
    ED.content_tile = 84;
    ED.console_show[0] = ED.console_show[1] = ED.console_show[2] = true;
    ED.console_autoscroll = true;
    lx_log_add_sink(ed_log_sink, NULL);
    LX_INFO("Inferno %s (Luxora Engine)", LX_VERSION_STRING);
    lx_jobs_init(0);
    lx_register_builtin_behaviours();
    extern void lx_game_register_scripts(void);
    lx_game_register_scripts();
    ed_load_prefs();

    LxWindowDesc wd = {"Inferno", 1600, 940, true, true, false, true};   /* borderless: Inferno draws its own title bar */
    if (!lx_platform_init(&wd)) return 1;
    if (!lx_renderer_init()) {
        lx_platform_shutdown();
        fprintf(stderr, "Inferno needs a GPU with Vulkan 1.3 (or 1.2 + dynamic rendering).\n");
        return 2;
    }
    lx_audio_init();
    ED.ui = ui_create();
    ed_chrome_init();

    static char new_root[LX_PATH_MAX];
    if (new_parent && new_name) {
        LxProject p;
        lx_mkdir_p(new_parent);
        /* if it already exists we simply open it */
        if (lx_project_create(&p, new_parent, new_name, LX_TEMPLATE_SAMPLE_3D)) lx_strcpy(new_root, sizeof new_root, p.root);
        else lx_path_join(new_root, sizeof new_root, new_parent, new_name);
        open_dir = new_root;
    }
    if (open_dir) ed_open_project(open_dir);
    if (!ED.has_project) ed_set_mode(ED_MODE_PROJECT_MANAGER);
    if (auto_play && ED.has_project) ed_play();

    double last = lx_time();
    int frame = 0, build_exit = 0;
    float fps_acc = 0;
    int fps_frames = 0;
    while (!ED.quit) {
        if (!lx_platform_pump()) {
            if (ED.mode == ED_MODE_EDITOR && ED.dirty && ED.play == ED_EDIT) {
                /* the window close button: save prompts would block automation; autosave a recovery copy */
                char path[LX_PATH_MAX];
                lx_path_join(path, sizeof path, ED.project.root, "Library/recovery.lxscene");
                lx_scene_save(ED.scene, path);
                LX_WARN("Unsaved scene written to %s", path);
            }
            break;
        }
        double now = lx_time();
        ED.dt = (float)(now - last);
        if (ED.dt > 0.1f) ED.dt = 0.1f;
        last = now;
        ED.time += ED.dt;
        fps_acc += ED.dt;
        if (++fps_frames >= 20) { ED.fps = (float)fps_frames / fps_acc; fps_acc = 0; fps_frames = 0; }
        lx_input_update(ED.dt);
        if (lx_platform_take_close_request()) ed_request_quit();   /* window close button / Alt+F4 */
        if (ED.mode == ED_MODE_EDITOR) ed_update(ED.dt);
        if (lx_renderer_begin_frame()) {
            ED.in_frame = true;
            if (ED.mode == ED_MODE_EDITOR) ed_render();
            ed_chrome_frame_before_ui();
            ui_frame(ED.ui, ED.dt, lx_v4(0.1f, 0.1f, 0.11f, 1));
            ed_chrome_frame_after_ui();
            if (shot && frame == shot_frames) lx_renderer_screenshot(shot);
            lx_renderer_end_frame();
            ED.in_frame = false;
            frame++;
            if (shot && frame > shot_frames && !smoke) ED.quit = true;
        } else {
            /* minimised or the swapchain is being rebuilt: UI events still ran, nothing is in flight */
            ED.in_frame = false;
            lx_sleep_ms(16);
        }
        ed_run_pending();
        if (build_game && ED.has_project) {
            extern int ed_last_build_result;
            static bool started;
            if (!started && frame > 2) { started = true; ed_build(lx_platform_current(), false); }
            if (started && ed_last_build_result) { ED.quit = true; build_exit = ed_last_build_result == 1 ? 0 : 4; }
        }
        if (smoke) {
            extern bool ed_smoke_step(int frame);
            static int smoke_frame;
            if (!ed_smoke_step(smoke_frame++)) ED.quit = true;
        }
    }
    lx_renderer_wait_idle();
    int errors = lx_renderer_validation_errors();
    if (ED.has_project) ed_close_project();
    ui_destroy(ED.ui);
    lx_audio_shutdown();
    lx_renderer_shutdown();
    lx_platform_shutdown();
    lx_jobs_shutdown();
    if (errors) fprintf(stderr, "Vulkan validation reported %d errors\n", errors);
    extern int ed_smoke_failures;
    if (ed_smoke_failures) fprintf(stderr, "%d smoke checks failed\n", ed_smoke_failures);
    return errors ? 3 : ed_smoke_failures ? 5 : build_exit;
}
