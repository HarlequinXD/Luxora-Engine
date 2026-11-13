/* Application runtime: window, renderer, audio, scene loading and the main loop for games. */
#include "lx_internal.h"

static struct {
    LxScene *scene;
    LxProject project;
    bool has_project;
    char pending_scene[LX_REF_MAX];
    bool quit;
} E;

LxScene *lx_active_scene(void) { return E.scene; }
const LxProject *lx_active_project(void) { return E.has_project ? &E.project : NULL; }
void lx_quit(void) { E.quit = true; }
bool lx_load_scene(const char *ref) {
    lx_strcpy(E.pending_scene, sizeof E.pending_scene, ref);
    return true;
}

void lx_engine_set_project(const LxProject *p) {
    E.project = *p;
    E.has_project = true;
    lx_assets_set_root(p->assets);
    *lx_graphics_settings() = p->graphics;
    *lx_audio_settings() = p->audio;
    char input[LX_PATH_MAX];
    lx_path_join(input, sizeof input, p->root, "input.lxinput");
    if (!lx_input_map_load(lx_input_map(), input)) lx_input_map_defaults(lx_input_map());
}

static LxScene *load_scene_ref(const char *ref) {
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    LxScene *s = lx_scene_load(path);
    if (!s) LX_ERROR("Scene '%s' could not be loaded", ref);
    return s;
}

int lx_run(const LxAppDesc *d) {
    lx_jobs_init(0);
    lx_register_builtin_behaviours();
    if (d->on_init) d->on_init();
    const char *title = d->title ? d->title : "Luxora Game";
    LxWindowDesc wd = {title, d->width ? d->width : 1280, d->height ? d->height : 720, true, false, false};
    if (d->project_dir) {
        LxProject p;
        if (lx_project_load(&p, d->project_dir)) {
            lx_engine_set_project(&p);
            wd.title = E.project.name;
            wd.width = E.project.build.window_width;
            wd.height = E.project.build.window_height;
            wd.fullscreen = E.project.build.fullscreen;
            wd.vsync = E.project.graphics.vsync;
        } else LX_WARN("No project found in %s", d->project_dir);
    }
    if (!lx_platform_init(&wd)) return 1;
    if (E.has_project && E.project.build.icon[0]) {
        /* the game's icon for the window/taskbar (Windows also has it embedded in the .exe) */
        char ip[LX_PATH_MAX];
        lx_asset_path(ip, sizeof ip, E.project.build.icon);
        int iw, ih;
        uint8_t *px = lx_image_load(ip, &iw, &ih);
        if (px) {
            uint8_t *sq = lx_image_square(px, iw, ih, 128);
            lx_platform_set_icon(sq, 128, 128);
            lx_free(sq);
            lx_free(px);
        }
    }
    if (!lx_renderer_init()) { lx_platform_shutdown(); return 2; }
    if (E.has_project) *lx_graphics_settings() = E.project.graphics;
    lx_renderer_set_vsync(lx_graphics_settings()->vsync);
    lx_audio_init();

    const char *start = d->start_scene ? d->start_scene : (E.has_project ? E.project.startup_scene : NULL);
    E.scene = start ? load_scene_ref(start) : NULL;
    if (!E.scene) E.scene = lx_scene_create_default("Main");
    if (d->on_start) d->on_start(E.scene);

    double last = lx_time();
    /* automation hook used by CI: LUXORA_SCREENSHOT=file.ppm LUXORA_SCREENSHOT_FRAME=N */
    const char *shot = getenv("LUXORA_SCREENSHOT");
    int shot_frame = getenv("LUXORA_SCREENSHOT_FRAME") ? atoi(getenv("LUXORA_SCREENSHOT_FRAME")) : 120;
    int frame = 0;
    while (!E.quit && lx_platform_pump()) {
        double now = lx_time();
        float dt = (float)(now - last);
        if (dt > 0.1f) dt = 0.1f;
        last = now;
        if (E.pending_scene[0]) {
            LxScene *next = load_scene_ref(E.pending_scene);
            E.pending_scene[0] = 0;
            if (next) {
                lx_audio_stop_all();
                lx_scene_destroy(E.scene);
                E.scene = next;
                lx_gi_reset();
                if (d->on_start) d->on_start(E.scene);
            }
        }
        lx_input_update(dt);
        lx_scene_simulate(E.scene, dt);
        if (d->on_update) d->on_update(E.scene, dt);
        lx_audio_update(E.scene, dt);
        if (lx_renderer_begin_frame()) {
            int w, h;
            lx_window_size(&w, &h);
            LxRenderView view;
            LxEntity cam = lx_scene_main_camera(E.scene);
            if (cam && lx_camera_view(E.scene, cam, w, h, &view)) lx_render_scene(E.scene, &view, NULL);
            lx_input_draw_touch_controls();
            if (shot && frame == shot_frame) lx_renderer_screenshot(shot);
            lx_renderer_end_frame();
            if (shot && frame > shot_frame) E.quit = true;
            frame++;
        }
        int target = lx_graphics_settings()->target_fps;
        if (target > 0) {
            double budget = 1.0 / target - (lx_time() - now);
            if (budget > 0.001) lx_sleep_ms((int)(budget * 1000.0));
        }
    }
    if (d->on_shutdown) d->on_shutdown();
    lx_renderer_wait_idle();
    int validation = lx_renderer_validation_errors();
    lx_audio_shutdown();
    lx_scene_destroy(E.scene);
    lx_renderer_shutdown();
    lx_platform_shutdown();
    lx_jobs_shutdown();
    return validation ? 3 : 0;
}
