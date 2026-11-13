/* Settings windows: Project Settings (General, Graphics, Sound, Input, Platforms),
 * Build Settings, About and Keyboard Shortcuts. */
#include "editor.h"

static int g_category;
#define SETTINGS_PAGES 6
static UiNode *g_settings_body, *g_nav[SETTINGS_PAGES];
static bool g_dummy_open = true;

static void save_project(UiNode *n) {
    LX_UNUSED(n);
    *lx_graphics_settings() = ED.project.graphics;
    *lx_audio_settings() = ED.project.audio;
    *lx_physics_settings() = ED.project.physics;
    lx_renderer_set_vsync(ED.project.graphics.vsync);
    lx_project_save(&ED.project);
    ed_update_title();
}

static void save_input(void) {
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, ED.project.root, "input.lxinput");
    lx_input_map_save(lx_input_map(), path);
}

static UiNode *row_num(UiNode *body, const char *label, float *v, float step, float a, float b) {
    UiNode *row = ed_prop_row(body, label);
    row->first->w = 170;
    UiNode *n = ui_number(ED.ui, row, v, step);
    n->grow = 1;
    n->minv = a;
    n->maxv = b;
    n->on_change = save_project;
    return n;
}
static UiNode *row_int(UiNode *body, const char *label, int *v, int a, int b) {
    UiNode *row = ed_prop_row(body, label);
    row->first->w = 170;
    UiNode *n = ui_int(ED.ui, row, v, a, b);
    n->grow = 1;
    n->on_change = save_project;
    return n;
}
static UiNode *row_slider(UiNode *body, const char *label, float *v, float a, float b) {
    UiNode *row = ed_prop_row(body, label);
    row->first->w = 170;
    UiNode *n = ui_slider(ED.ui, row, v, a, b);
    n->grow = 1;
    n->on_change = save_project;
    return n;
}
static UiNode *row_bool(UiNode *body, const char *label, bool *v) {
    UiNode *row = ed_prop_row(body, label);
    row->first->w = 170;
    UiNode *n = ui_checkbox(ED.ui, row, "", v);
    n->on_change = save_project;
    return n;
}
static UiNode *row_text(UiNode *body, const char *label, char *buf, int cap) {
    UiNode *row = ed_prop_row(body, label);
    row->first->w = 170;
    UiNode *n = ui_textfield(ED.ui, row, buf, cap);
    n->grow = 1;
    n->on_change = save_project;
    return n;
}
static UiNode *row_drop(UiNode *body, const char *label, int *v, const char *const *opts, int count) {
    UiNode *row = ed_prop_row(body, label);
    row->first->w = 170;
    UiNode *n = ui_dropdown(ED.ui, row, v, opts, count);
    n->grow = 1;
    n->on_change = save_project;
    return n;
}
static UiNode *note(UiNode *body, UiIcon icon, const char *text) {
    UiNode *l = ui_label(ED.ui, body, text);
    l->font = UI_FONT_SMALL;
    l->fg = ui_theme(ED.ui)->text_dim;
    l->icon = icon;
    l->icon_size = 13;
    l->icon_color = icon == UI_ICON_WARNING ? ui_theme(ED.ui)->warning : ui_theme(ED.ui)->text_dim;
    l->h = 22;
    return l;
}

/* ---------------- General ---------------- */
static void build_general(UiNode *b) {
    UiNode *s = ed_section(b, UI_ICON_APP, "Project", &g_dummy_open, NULL);
    row_text(s, "Name", ED.project.name, sizeof ED.project.name);
    row_text(s, "Company", ED.project.company, sizeof ED.project.company);
    row_text(s, "Version", ED.project.version, sizeof ED.project.version);
    UiNode *st = row_text(s, "Startup Scene", ED.project.startup_scene, sizeof ED.project.startup_scene);
    st->accept = "scene";
    st->icon = UI_ICON_SCENE;
    note(s, UI_ICON_INFO, "Drag a scene from the Content Browser onto Startup Scene, or use Scene Manager > Set Startup.");
    s = ed_section(b, UI_ICON_MONITOR, "Desktop Window", &g_dummy_open, NULL);
    row_int(s, "Width", &ED.project.build.window_width, 320, 16384);
    row_int(s, "Height", &ED.project.build.window_height, 240, 16384);
    row_bool(s, "Fullscreen", &ED.project.build.fullscreen);
    s = ed_section(b, UI_ICON_FOLDER, "Locations", &g_dummy_open, NULL);
    UiNode *l = ui_label(ED.ui, s, ED.project.root);
    l->icon = UI_ICON_FOLDER;
    l->icon_color = ed_color_for_icon(UI_ICON_FOLDER);
    l->font = UI_FONT_MONO;
}

/* ---------------- Graphics ---------------- */
static const char *QUALITY[] = {"Low", "Medium", "High", "Ultra"};
static void preset_changed(UiNode *n) {
    lx_graphics_apply_preset(&ED.project.graphics, ED.project.graphics.quality_preset);
    save_project(n);
    extern void ed_settings_rebuild(void);
    ed_settings_rebuild();
}
static const char *PROBE_RES_OPTS[] = {"64", "128", "256", "512"};
static int g_probe_res;
static void probe_res_set(UiNode *n) {
    ED.project.graphics.reflection_resolution = 64 << g_probe_res;
    save_project(n);
}
static void build_graphics(UiNode *b) {
    LxGraphicsSettings *g = &ED.project.graphics;
    UiNode *s = ed_section(b, UI_ICON_MONITOR, "Quality", &g_dummy_open, NULL);
    UiNode *q = row_drop(s, "Preset", &g->quality_preset, QUALITY, 4);
    q->on_change = preset_changed;
    row_slider(s, "Render Scale", &g->render_scale, 0.25f, 2.0f);
    row_bool(s, "V-Sync", &g->vsync);
    row_int(s, "Frame Rate Limit (0 = off)", &g->target_fps, 0, 1000);
    row_slider(s, "Exposure", &g->exposure, 0.1f, 4.0f);
    s = ed_section(b, UI_ICON_SUN, "Virtual Photon Global Illumination", &g_dummy_open, NULL);
    row_bool(s, "Enabled", &g->gi_enabled);
    row_int(s, "Photons per Frame", &g->gi_photons_per_frame, 256, 4000000)->step = 500;
    row_int(s, "Max Bounces", &g->gi_max_bounces, 1, 16);
    row_int(s, "Volume Resolution", &g->gi_grid_resolution, 4, 96);
    row_slider(s, "Intensity", &g->gi_intensity, 0, 4);
    row_slider(s, "Responsiveness", &g->gi_responsiveness, 0.01f, 1);
    note(s, UI_ICON_INFO, "Photons are traced on all CPU cores and converge over frames. More photons = less noise.");
    s = ed_section(b, UI_ICON_PROBE, "Reflections", &g_dummy_open, NULL);
    row_bool(s, "Reflection Probes", &g->reflections_enabled);
    g_probe_res = g->reflection_resolution >= 512 ? 3 : g->reflection_resolution >= 256 ? 2 : g->reflection_resolution >= 128 ? 1 : 0;
    row_drop(s, "Probe Resolution", &g_probe_res, PROBE_RES_OPTS, 4)->on_change = probe_res_set;
    row_bool(s, "Screen-Space Reflections", &g->ssr_enabled);
    row_int(s, "SSR Steps", &g->ssr_steps, 8, 128);
    row_num(s, "SSR Max Distance", &g->ssr_max_distance, 0.5f, 1, 500);
    row_num(s, "SSR Thickness", &g->ssr_thickness, 0.01f, 0.01f, 5);
    row_slider(s, "SSR Max Roughness", &g->ssr_max_roughness, 0.05f, 1);
    note(s, UI_ICON_INFO, "Probes reflect the whole surroundings (an automatic one follows the camera; place Reflection Probe "
                          "objects for rooms). Screen-space reflections add exact detail for what is on screen.");
    s = ed_section(b, UI_ICON_LAYERS, "Shadows", &g_dummy_open, NULL);
    row_int(s, "Resolution", &g->shadow_resolution, 256, 8192)->step = 64;
    row_int(s, "Cascades", &g->shadow_cascades, 1, 4);
    row_num(s, "Distance", &g->shadow_distance, 1, 5, 5000);
    s = ed_section(b, UI_ICON_MESH, "Geometry", &g_dummy_open, NULL);
    row_bool(s, "Automatic LODs", &g->lod_enabled);
    row_slider(s, "LOD Bias", &g->lod_bias, 0.2f, 3);
}

/* ---------------- Physics ---------------- */
void ed_settings_rebuild(void);
static int g_phys_hz;
static bool g_layers[8][8];
static void phys_rate_changed(UiNode *n) {
    if (g_phys_hz < 10) g_phys_hz = 10;
    ED.project.physics.fixed_timestep = 1.0f / (float)g_phys_hz;
    save_project(n);
}
static void layer_changed(UiNode *n) {
    int i = (int)n->tag / 8, j = (int)n->tag % 8;
    g_layers[j][i] = g_layers[i][j];   /* collisions are symmetric */
    uint32_t *mask = ED.project.physics.layer_mask;
    for (int a = 0; a < 8; a++)
        for (int b = 0; b < 8; b++) {
            if (g_layers[a][b]) mask[a] |= 1u << b;
            else mask[a] &= ~(1u << b);
        }
    save_project(n);
    ed_settings_rebuild();
}
static void build_physics(UiNode *b) {
    UiCtx *ui = ED.ui;
    LxPhysicsSettings *p = &ED.project.physics;
    UiNode *s = ed_section(b, UI_ICON_RIGIDBODY, "Simulation", &g_dummy_open, NULL);
    ed_vec3_row(s, "Gravity (m/s²)", &p->gravity.x, 0.05f, save_project);
    g_phys_hz = (int)(1.0f / (p->fixed_timestep > 0 ? p->fixed_timestep : 1.0f / 60.0f) + 0.5f);
    row_int(s, "Physics Rate (Hz)", &g_phys_hz, 10, 1000)->on_change = phys_rate_changed;
    row_int(s, "Solver Iterations", &p->solver_iterations, 1, 100);
    note(s, UI_ICON_INFO, "Higher rates and more iterations make tall stacks and fast objects more stable at a CPU cost. "
                          "60 Hz with 10 iterations suits most games.");
    s = ed_section(b, UI_ICON_COLLIDER, "Layer Collisions (layers 0-7)", &g_dummy_open, NULL);
    note(s, UI_ICON_INFO, "A Collider's Layer decides what it can hit. Untick a box to make two layers pass through each other. "
                          "Queries take a layer mask too.");
    UiNode *head = ui_box(ui, s, UI_ROW);
    head->h = 22;
    UiNode *pad = ui_label(ui, head, "");
    pad->w = 70;
    for (int j = 0; j < 8; j++) {
        char t[8];
        snprintf(t, sizeof t, "%d", j);
        UiNode *l = ui_label(ui, head, t);
        l->w = 32;
        l->center_text = true;
    }
    for (int i = 0; i < 8; i++) {
        UiNode *row = ui_box(ui, s, UI_ROW);
        row->h = 26;
        char t[16];
        snprintf(t, sizeof t, "Layer %d", i);
        UiNode *l = ui_label(ui, row, t);
        l->w = 70;
        for (int j = 0; j < 8; j++) {
            g_layers[i][j] = (p->layer_mask[i] >> j) & 1u;
            UiNode *spacer = ui_box(ui, row, UI_ROW);
            spacer->w = 32;
            if (j < i) continue;   /* the matrix is symmetric: show one half */
            UiNode *cb = ui_checkbox(ui, spacer, "", &g_layers[i][j]);
            cb->tag = i * 8 + j;
            cb->on_change = layer_changed;
        }
    }
}

/* ---------------- Sound ---------------- */
static void build_sound(UiNode *b) {
    LxAudioSettings *a = &ED.project.audio;
    UiNode *s = ed_section(b, UI_ICON_SPEAKER, "Mixer", &g_dummy_open, NULL);
    row_slider(s, "Master Volume", &a->master_volume, 0, 1.5f);
    static const char *bus[] = {"SFX Volume", "Music Volume", "Voice Volume", "Ambient Volume"};
    for (int i = 0; i < LX_BUS_COUNT; i++) row_slider(s, bus[i], &a->bus_volume[i], 0, 1.5f);
    row_int(s, "Sample Rate (Hz)", &a->sample_rate, 22050, 96000);
    note(s, UI_ICON_INFO, "Sample rate changes apply the next time the editor or game starts.");
    s = ed_section(b, UI_ICON_SOUND, "Sound Propagation", &g_dummy_open, NULL);
    row_bool(s, "Ray-Traced Propagation", &a->propagation);
    row_num(s, "Speed of Sound (m/s)", &a->speed_of_sound, 1, 1, 10000);
    row_int(s, "Rays per Source", &a->rays_per_source, 16, 8192)->step = 8;
    row_int(s, "Max Reflections", &a->max_bounces, 1, 32);
    UiNode *air = row_num(s, "Air Absorption (/m)", &a->air_absorption, 0.0005f, 0, 1);
    air->precision = 4;
    row_bool(s, "Doppler Effect", &a->doppler);
    note(s, UI_ICON_INFO, "Static meshes block, reflect and reverberate sound using each material's acoustic settings.");
}

/* ---------------- Platforms ---------------- */
static const char *ORIENT[] = {"Landscape", "Portrait"};
static void build_platforms(UiNode *b) {
    LxBuildSettings *bs = &ED.project.build;
    UiNode *s = ed_section(b, UI_ICON_ANDROID, "Android", &g_dummy_open, NULL);
    row_text(s, "Package Name", bs->android_package, sizeof bs->android_package);
    row_int(s, "Minimum API Level", &bs->android_min_sdk, 26, 35);
    row_drop(s, "Orientation", &bs->android_orientation, ORIENT, 2);
    note(s, UI_ICON_INFO, "Vulkan + AAudio need API 26+. Your Galaxy-class phones run API 34+.");
    s = ed_section(b, UI_ICON_WINDOWS, "Windows", &g_dummy_open, NULL);
    note(s, UI_ICON_INFO, "Windows builds use the desktop window settings from General.");
    s = ed_section(b, UI_ICON_LINUX, "Linux", &g_dummy_open, NULL);
    note(s, UI_ICON_INFO, "Linux builds use X11 + Vulkan and need libvulkan1 on the target machine.");
}

/* ======================================================================
 *  INPUT MANAGER
 * ====================================================================== */
static int g_action = 0;
static int g_capture_binding = -1;   /* index being rebound, -1 none */
static int g_touch_drag = -1;
static UiNode *g_input_detail, *g_input_list, *g_capture_label;
static const char *TOUCH_MODES[] = {"Auto (touch devices)", "Always", "Never"};
static const char *TOUCH_TYPES[] = {"Button", "Stick", "Look Area"};
static void input_rebuild(void);

static void input_changed(UiNode *n) { LX_UNUSED(n); save_input(); }
static void pick_action(UiNode *n) { g_action = (int)n->tag; g_capture_binding = -1; input_rebuild(); }

static void add_action_named(const char *name) {
    LxInputAction *a = lx_input_add_action(lx_input_map(), name, false);
    if (a) g_action = (int)(a - lx_input_map()->actions);
    save_input();
    ed_project_settings(3);
}
static void add_axis_named(const char *name) {
    LxInputAction *a = lx_input_add_action(lx_input_map(), name, true);
    if (a) g_action = (int)(a - lx_input_map()->actions);
    save_input();
    ed_project_settings(3);
}
static void m_add_action(UiNode *n) { LX_UNUSED(n); ed_prompt("New Action", "Name", "NewAction", add_action_named); }
static void m_add_axis(UiNode *n) { LX_UNUSED(n); ed_prompt("New Axis", "Name", "NewAxis", add_axis_named); }

static void delete_action(UiNode *n) {
    LX_UNUSED(n);
    LxInputMap *m = lx_input_map();
    if (g_action < 0 || g_action >= m->action_count) return;
    memmove(&m->actions[g_action], &m->actions[g_action + 1], sizeof(LxInputAction) * (size_t)(m->action_count - g_action - 1));
    m->action_count--;
    if (g_action >= m->action_count) g_action = m->action_count - 1;
    save_input();
    input_rebuild();
}

static void remove_binding(UiNode *n) {
    LxInputAction *a = &lx_input_map()->actions[g_action];
    int i = (int)n->tag;
    if (i < 0 || i >= a->binding_count) return;
    memmove(&a->bindings[i], &a->bindings[i + 1], sizeof(LxBinding) * (size_t)(a->binding_count - i - 1));
    a->binding_count--;
    save_input();
    input_rebuild();
}

static void start_capture(UiNode *n) { g_capture_binding = (int)n->tag; input_rebuild(); }

static void add_binding_of(UiNode *n) {
    LxInputAction *a = &lx_input_map()->actions[g_action];
    LxBinding b = {(LxBindingType)(n->tag >> 8), (int)(n->tag & 0xff), 0, 1.0f};
    if (b.type == LX_BIND_KEY_AXIS) { b.code = LX_KEY_D; b.code2 = LX_KEY_A; }
    if (b.type == LX_BIND_MOUSE_AXIS) b.scale = b.code == LX_MOUSE_AXIS_Y ? -0.08f : 0.08f;
    if (lx_input_add_binding(a, b)) {
        save_input();
        if (b.type == LX_BIND_KEY || b.type == LX_BIND_KEY_AXIS) g_capture_binding = a->binding_count - 1;
    } else ui_notify(ED.ui, "An action can have at most %d bindings", LX_MAX_ACTION_BINDINGS);
    input_rebuild();
}

static void add_binding_menu(UiNode *btn) {
    LxInputMap *m = lx_input_map();
    LxInputAction *a = &m->actions[g_action];
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y + btn->ch, 260);
    UiNode *it;
#define ADD(TEXT_, ICON_, TYPE_, CODE_) do { it = ui_menu_item(ED.ui, p, TEXT_, NULL, add_binding_of); it->icon = ICON_; it->icon_color = ed_color_for_icon(ICON_); it->tag = ((TYPE_) << 8) | (CODE_); } while (0)
    if (a->is_axis) ADD("Keyboard: Key Pair (press to set)", UI_ICON_KEYBOARD, LX_BIND_KEY_AXIS, 0);
    else ADD("Keyboard: Key (press to set)", UI_ICON_KEYBOARD, LX_BIND_KEY, 0);
    if (a->is_axis) {
        ADD("Mouse: Move X", UI_ICON_MOUSE, LX_BIND_MOUSE_AXIS, LX_MOUSE_AXIS_X);
        ADD("Mouse: Move Y", UI_ICON_MOUSE, LX_BIND_MOUSE_AXIS, LX_MOUSE_AXIS_Y);
        ADD("Mouse: Wheel", UI_ICON_MOUSE, LX_BIND_MOUSE_AXIS, LX_MOUSE_AXIS_WHEEL);
    } else {
        ADD("Mouse: Left Button", UI_ICON_MOUSE, LX_BIND_MOUSE_BUTTON, LX_MOUSE_LEFT);
        ADD("Mouse: Right Button", UI_ICON_MOUSE, LX_BIND_MOUSE_BUTTON, LX_MOUSE_RIGHT);
        ADD("Mouse: Middle Button", UI_ICON_MOUSE, LX_BIND_MOUSE_BUTTON, LX_MOUSE_MIDDLE);
    }
    ui_menu_separator(ED.ui, p);
    for (int i = 0; i < LX_PAD_AXIS_COUNT; i++) {
        char t[64];
        snprintf(t, sizeof t, "Gamepad: %s", lx_pad_axis_name((LxGamepadAxis)i));
        ADD(t, UI_ICON_GAMEPAD, LX_BIND_PAD_AXIS, i);
    }
    if (!a->is_axis)
        for (int i = 0; i < LX_PAD_BUTTON_COUNT; i++) {
            char t[64];
            snprintf(t, sizeof t, "Gamepad: %s", lx_pad_button_name((LxGamepadButton)i));
            ADD(t, UI_ICON_GAMEPAD, LX_BIND_PAD_BUTTON, i);
        }
    ui_menu_separator(ED.ui, p);
    for (int i = 0; i < m->touch_count; i++) {
        char t[64];
        if (m->touch[i].type == LX_TOUCH_BUTTON && !a->is_axis) { snprintf(t, sizeof t, "Touch: %s", m->touch[i].name); ADD(t, UI_ICON_TOUCH, LX_BIND_TOUCH_BUTTON, i); }
        if (m->touch[i].type != LX_TOUCH_BUTTON && a->is_axis) {
            snprintf(t, sizeof t, "Touch: %s X", m->touch[i].name); ADD(t, UI_ICON_TOUCH, LX_BIND_TOUCH_STICK_X, i);
            snprintf(t, sizeof t, "Touch: %s Y", m->touch[i].name); ADD(t, UI_ICON_TOUCH, LX_BIND_TOUCH_STICK_Y, i);
        }
    }
#undef ADD
}

static UiIcon binding_icon(LxBindingType t) {
    switch (t) {
    case LX_BIND_KEY: case LX_BIND_KEY_AXIS: return UI_ICON_KEYBOARD;
    case LX_BIND_MOUSE_BUTTON: case LX_BIND_MOUSE_AXIS: return UI_ICON_MOUSE;
    case LX_BIND_PAD_BUTTON: case LX_BIND_PAD_AXIS: return UI_ICON_GAMEPAD;
    default: return UI_ICON_TOUCH;
    }
}

/* live value bar */
static void live_draw(UiNode *n, UiCtx *ui) {
    LxInputMap *m = lx_input_map();
    if (n->tag < 0 || n->tag >= m->action_count) return;
    const UiTheme *t = ui_theme(ui);
    LxInputAction *a = &m->actions[n->tag];
    float v = a->is_axis ? lx_axis(a->name) : (lx_action_down(a->name) ? 1.0f : 0.0f);
    ui_draw_rect(ui, n->x, n->y + n->ch * 0.3f, n->cw, n->ch * 0.4f, t->field, 3);
    if (a->is_axis) {
        float c = n->x + n->cw * 0.5f, w = lx_clampf(v, -1, 1) * n->cw * 0.5f;
        ui_draw_rect(ui, w < 0 ? c + w : c, n->y + n->ch * 0.3f, fabsf(w), n->ch * 0.4f, t->accent, 3);
        ui_draw_rect(ui, c - 0.5f, n->y + n->ch * 0.2f, 1, n->ch * 0.6f, t->text_dim, 0);
    } else if (v > 0) ui_draw_rect(ui, n->x, n->y + n->ch * 0.3f, n->cw, n->ch * 0.4f, t->success, 3);
}

static void capture_frame(UiNode *n) {
    LX_UNUSED(n);
    if (g_capture_binding < 0) return;
    LxInputMap *m = lx_input_map();
    if (g_action < 0 || g_action >= m->action_count) { g_capture_binding = -1; return; }
    LxInputAction *a = &m->actions[g_action];
    if (g_capture_binding >= a->binding_count) { g_capture_binding = -1; return; }
    if (lx_key_pressed(LX_KEY_ESCAPE)) { g_capture_binding = -1; input_rebuild(); return; }
    LxBinding *b = &a->bindings[g_capture_binding];
    LxBinding got;
    /* ignore the click that started the capture */
    if (lx_mouse_pressed(LX_MOUSE_LEFT) && ui_is_inside(ED.ui, n)) return;
    if (b->type == LX_BIND_KEY_AXIS) {
        /* two presses: positive then negative */
        static int stage;
        if (lx_input_capture(&got, true) && got.type == LX_BIND_KEY_AXIS) {
            if (stage == 0) { b->code = got.code; stage = 1; if (g_capture_label) ui_set_text(g_capture_label, "Now press the NEGATIVE key (Esc to cancel)"); return; }
            b->code2 = got.code;
            stage = 0;
            g_capture_binding = -1;
            save_input();
            input_rebuild();
        }
        return;
    }
    if (lx_input_capture(&got, a->is_axis)) {
        if (a->is_axis && got.type == LX_BIND_PAD_BUTTON) return;
        got.scale = b->scale != 0 ? b->scale : 1.0f;
        if (got.type == LX_BIND_MOUSE_AXIS) got.scale = got.code == LX_MOUSE_AXIS_Y ? -0.08f : 0.08f;
        *b = got;
        g_capture_binding = -1;
        save_input();
        input_rebuild();
    }
}

/* touch layout preview: a phone-shaped canvas where controls can be dragged */
static void touch_canvas_draw(UiNode *n, UiCtx *ui) {
    const UiTheme *t = ui_theme(ui);
    LxInputMap *m = lx_input_map();
    ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, UI_HEX(0x0e0f12), 14 * ui_scale(ui));
    ui_draw_border(ui, n->x, n->y, n->cw, n->ch, t->border_light, 14 * ui_scale(ui), 2);
    float shortest = lx_minf(n->cw, n->ch);
    for (int i = 0; i < m->touch_count; i++) {
        LxTouchControl *c = &m->touch[i];
        bool hot = i == g_touch_drag;
        if (c->type == LX_TOUCH_LOOK_AREA) {
            float x = n->x + c->x * n->cw, y = n->y + c->y * n->ch, w = c->size * n->cw, h = c->height * n->ch;
            ui_draw_rect(ui, x, y, w, h, UI_RGBA(88, 166, 255, hot ? 40 : 22), 6);
            ui_draw_border(ui, x, y, w, h, UI_RGBA(88, 166, 255, 110), 6, 1);
            ui_draw_text(ui, UI_FONT_SMALL, x + 8, y + 6, c->name, ui_alpha(t->text, 0.7f));
            continue;
        }
        float cx = n->x + c->x * n->cw, cy = n->y + c->y * n->ch, r = c->size * shortest;
        ui_draw_circle(ui, cx, cy, r, UI_RGBA(255, 255, 255, hot ? 80 : 40), 28);
        ui_draw_ring(ui, cx, cy, r, 1.5f, UI_RGBA(255, 255, 255, 140), 28);
        if (c->type == LX_TOUCH_STICK) ui_draw_circle(ui, cx, cy, r * 0.45f, UI_RGBA(255, 255, 255, 110), 20);
        float tw = ui_text_width(ui, UI_FONT_SMALL, c->name);
        ui_draw_text(ui, UI_FONT_SMALL, cx - tw * 0.5f, cy + r + 4, c->name, t->text_dim);
    }
    ui_draw_text(ui, UI_FONT_SMALL, n->x + 12, n->y + n->ch - 22, "Drag controls to position them", t->text_disabled);
}

static void touch_canvas_frame(UiNode *n) {
    LxInputMap *m = lx_input_map();
    LxVec2 mp = lx_mouse_pos();
    if (g_touch_drag >= 0) {
        if (!lx_mouse_down(LX_MOUSE_LEFT)) { g_touch_drag = -1; save_input(); input_rebuild(); return; }
        LxTouchControl *c = &m->touch[g_touch_drag];
        float nx = lx_clampf((mp.x - n->x) / lx_maxf(1, n->cw), 0, 1), ny = lx_clampf((mp.y - n->y) / lx_maxf(1, n->ch), 0, 1);
        if (c->type == LX_TOUCH_LOOK_AREA) { c->x = lx_clampf(nx - c->size * 0.5f, 0, 1 - c->size); c->y = lx_clampf(ny - c->height * 0.5f, 0, 1 - c->height); }
        else { c->x = nx; c->y = ny; }
        return;
    }
    if (!ui_is_hot(ED.ui, n) || !lx_mouse_pressed(LX_MOUSE_LEFT)) return;
    float shortest = lx_minf(n->cw, n->ch);
    for (int i = m->touch_count - 1; i >= 0; i--) {
        LxTouchControl *c = &m->touch[i];
        if (c->type == LX_TOUCH_LOOK_AREA) continue;
        float cx = n->x + c->x * n->cw, cy = n->y + c->y * n->ch, r = c->size * shortest;
        if ((mp.x - cx) * (mp.x - cx) + (mp.y - cy) * (mp.y - cy) < r * r) { g_touch_drag = i; return; }
    }
    for (int i = m->touch_count - 1; i >= 0; i--) {
        LxTouchControl *c = &m->touch[i];
        if (c->type != LX_TOUCH_LOOK_AREA) continue;
        float x = n->x + c->x * n->cw, y = n->y + c->y * n->ch;
        if (mp.x >= x && mp.y >= y && mp.x < x + c->size * n->cw && mp.y < y + c->height * n->ch) { g_touch_drag = i; return; }
    }
}

static void add_touch_named(const char *name) {
    LxInputMap *m = lx_input_map();
    if (m->touch_count >= LX_MAX_TOUCH_CONTROLS) return;
    LxTouchControl *c = &m->touch[m->touch_count++];
    memset(c, 0, sizeof *c);
    lx_strcpy(c->name, sizeof c->name, name);
    c->type = LX_TOUCH_BUTTON;
    c->x = 0.7f;
    c->y = 0.6f;
    c->size = 0.07f;
    save_input();
    ed_project_settings(3);
}
static void m_add_touch(UiNode *n) { LX_UNUSED(n); ed_prompt("New Touch Control", "Name", "Action Button", add_touch_named); }
static void remove_touch(UiNode *n) {
    LxInputMap *m = lx_input_map();
    int i = (int)n->tag;
    if (i < 0 || i >= m->touch_count) return;
    /* drop bindings to this control and shift indices of later ones */
    for (int a = 0; a < m->action_count; a++) {
        LxInputAction *ac = &m->actions[a];
        for (int k = 0; k < ac->binding_count; k++) {
            LxBinding *b = &ac->bindings[k];
            if (b->type < LX_BIND_TOUCH_BUTTON) continue;
            if (b->code == i) { memmove(b, b + 1, sizeof *b * (size_t)(ac->binding_count - k - 1)); ac->binding_count--; k--; }
            else if (b->code > i) b->code--;
        }
    }
    memmove(&m->touch[i], &m->touch[i + 1], sizeof(LxTouchControl) * (size_t)(m->touch_count - i - 1));
    m->touch_count--;
    save_input();
    ed_project_settings(3);
}
static void reset_input_now(void) { lx_input_map_defaults(lx_input_map()); save_input(); g_action = 0; ed_project_settings(3); }
static void reset_input(UiNode *n) { LX_UNUSED(n); ed_confirm("Reset Input", "Replace all actions and touch controls with the defaults?", "Reset", reset_input_now); }
static void action_name_changed(UiNode *n) { LX_UNUSED(n); save_input(); input_rebuild(); }

static void input_rebuild(void) {
    if (!g_input_detail || !g_input_list) return;
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxInputMap *m = lx_input_map();
    /* list */
    ui_clear(ui, g_input_list);
    for (int i = 0; i < m->action_count; i++) {
        LxInputAction *a = &m->actions[i];
        UiNode *row = ui_box(ui, g_input_list, UI_ROW);
        row->h = 30;
        row->gap = 6;
        ui_pad(row, 8, 0, 8, 0);
        row->cross_align = UI_ALIGN_CENTER;
        row->radius = 4;
        row->bg_hover = t->hover;
        row->selected = i == g_action;
        row->tag = i;
        row->on_click = pick_action;
        UiNode *l = ui_label(ui, row, a->name);
        l->icon = a->is_axis ? UI_ICON_JOYSTICK : UI_ICON_GAMEPAD;
        l->icon_color = a->is_axis ? UI_HEX(0xd2a8ff) : UI_HEX(0xa5d6ff);
        l->grow = 1;
        UiNode *live = ui_new(ui, row, UI_CUSTOM);
        live->w = 46;
        live->tag = i;
        live->on_draw = live_draw;
        live->disabled = true;
    }
    /* detail */
    ui_clear(ui, g_input_detail);
    if (g_action < 0 || g_action >= m->action_count) {
        ui_label(ui, g_input_detail, "Select or add an action.")->fg = t->text_dim;
        return;
    }
    LxInputAction *a = &m->actions[g_action];
    UiNode *head = ui_box(ui, g_input_detail, UI_ROW);
    head->h = 32;
    head->gap = 8;
    head->cross_align = UI_ALIGN_CENTER;
    UiNode *name = ui_textfield(ui, head, a->name, sizeof a->name);
    name->grow = 1;
    name->h = 28;
    name->font = UI_FONT_BOLD;
    name->on_change = action_name_changed;
    UiNode *kind = ui_label(ui, head, a->is_axis ? "Axis (-1 to 1)" : "Button action");
    kind->fg = t->text_dim;
    kind->w = 120;
    UiNode *del = ui_icon_button(ui, head, UI_ICON_TRASH, "Delete action", delete_action);
    del->icon_color = t->danger;
    UiNode *use = ui_label(ui, g_input_detail, "");
    if (a->is_axis) ui_set_text(use, "In code:  float v = lx_axis(\"%s\");", a->name);
    else ui_set_text(use, "In code:  if (lx_action_pressed(\"%s\")) { ... }", a->name);
    use->font = UI_FONT_MONO;
    use->fg = UI_HEX(0x7ee787);
    use->h = 26;
    if (a->is_axis) {
        UiNode *r1 = ed_prop_row(g_input_detail, "Dead Zone");
        UiNode *dz = ui_slider(ui, r1, &a->dead_zone, 0, 0.9f);
        dz->grow = 1;
        dz->on_change = input_changed;
        UiNode *r2 = ed_prop_row(g_input_detail, "Sensitivity");
        UiNode *se = ui_slider(ui, r2, &a->sensitivity, 0.05f, 5);
        se->grow = 1;
        se->on_change = input_changed;
    }
    UiNode *bh = ui_box(ui, g_input_detail, UI_ROW);
    bh->h = 30;
    bh->cross_align = UI_ALIGN_CENTER;
    UiNode *bl = ui_label(ui, bh, "Bindings");
    bl->font = UI_FONT_BOLD;
    bl->grow = 1;
    UiNode *add = ui_button(ui, bh, "Add Binding", add_binding_menu);
    add->icon = UI_ICON_PLUS;
    add->w = 130;
    add->h = 26;
    g_capture_label = NULL;
    for (int i = 0; i < a->binding_count; i++) {
        LxBinding *b = &a->bindings[i];
        UiNode *row = ui_box(ui, g_input_detail, UI_ROW);
        row->h = 30;
        row->gap = 6;
        row->bg = t->panel_alt;
        row->radius = 5;
        ui_pad(row, 8, 0, 6, 0);
        row->cross_align = UI_ALIGN_CENTER;
        char desc[96];
        lx_binding_describe(m, b, desc, sizeof desc);
        UiIcon ic = binding_icon(b->type);
        UiNode *l = ui_label(ui, row, i == g_capture_binding ? (b->type == LX_BIND_KEY_AXIS ? "Press the POSITIVE key (Esc to cancel)" : "Press any key, button or move a stick...") : desc);
        l->icon = ic;
        l->icon_color = ed_color_for_icon(ic);
        l->grow = 1;
        if (i == g_capture_binding) { l->fg = t->warning; g_capture_label = l; }
        UiNode *sc = ui_number(ui, row, &b->scale, 0.01f);
        sc->w = 70;
        sc->h = 22;
        sc->on_change = input_changed;
        lx_strcpy(sc->tooltip, sizeof sc->tooltip, "Scale (negative inverts)");
        bool can_rebind = b->type <= LX_BIND_PAD_AXIS;
        if (can_rebind) {
            UiNode *rb = ui_button(ui, row, i == g_capture_binding ? "Listening" : "Rebind", start_capture);
            rb->tag = i;
            rb->w = 86;
            rb->h = 24;
            rb->selected = i == g_capture_binding;
        }
        UiNode *rm = ui_icon_button(ui, row, UI_ICON_CLOSE, "Remove binding", remove_binding);
        rm->tag = i;
        rm->w = 24;
        rm->h = 24;
    }
    if (!a->binding_count) note(g_input_detail, UI_ICON_WARNING, "No bindings yet - add keyboard, gamepad or touch inputs.");
}

static void build_input(UiNode *b) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxInputMap *m = lx_input_map();
    if (g_action >= m->action_count) g_action = 0;
    UiNode *top = ui_box(ui, b, UI_ROW);
    top->h = 330;
    top->gap = 10;
    /* actions list */
    UiNode *left = ui_box(ui, top, UI_COL);
    left->w = 250;
    left->gap = 6;
    UiNode *lh = ui_box(ui, left, UI_ROW);
    lh->h = 28;
    lh->gap = 4;
    UiNode *aa = ui_button(ui, lh, "Action", m_add_action);
    aa->icon = UI_ICON_PLUS;
    aa->grow = 1;
    UiNode *ax = ui_button(ui, lh, "Axis", m_add_axis);
    ax->icon = UI_ICON_PLUS;
    ax->grow = 1;
    g_input_list = ui_box(ui, left, UI_COL);
    g_input_list->grow = 1;
    g_input_list->scroll = true;
    g_input_list->bg = t->field;
    g_input_list->radius = 6;
    ui_pad(g_input_list, 4, 4, 4, 4);
    g_input_list->gap = 1;
    /* details */
    g_input_detail = ui_box(ui, top, UI_COL);
    g_input_detail->grow = 1;
    g_input_detail->scroll = true;
    g_input_detail->gap = 6;
    g_input_detail->on_frame = capture_frame;
    /* devices */
    UiNode *s = ed_section(b, UI_ICON_GAMEPAD, "Devices", &g_dummy_open, NULL);
    UiNode *r = ed_prop_row(s, "Trigger Threshold");
    r->first->w = 170;
    UiNode *tt = ui_slider(ui, r, &m->trigger_threshold, 0.05f, 0.95f);
    tt->grow = 1;
    tt->on_change = input_changed;
    r = ed_prop_row(s, "Touch Controls");
    r->first->w = 170;
    UiNode *tm = ui_dropdown(ui, r, &m->touch_controls_mode, TOUCH_MODES, 3);
    tm->grow = 1;
    tm->on_change = input_changed;
    char pads[160];
    snprintf(pads, sizeof pads, "%d gamepad%s connected%s%s", lx_gamepad_count(), lx_gamepad_count() == 1 ? "" : "s",
             lx_gamepad_count() ? ": " : " - plug one in to test bindings live", lx_gamepad_count() ? lx_gamepad_name(0) : "");
    note(s, UI_ICON_GAMEPAD, pads);
    note(s, UI_ICON_INFO, "Xbox-style layout: A/B/X/Y, bumpers, triggers, sticks and D-pad. XInput, Linux joystick and Android controllers.");
    /* touch layout */
    s = ed_section(b, UI_ICON_TOUCH, "Touch Controls (mobile)", &g_dummy_open, NULL);
    UiNode *tr = ui_box(ui, s, UI_ROW);
    tr->h = 230;
    tr->gap = 12;
    UiNode *canvas = ui_new(ui, tr, UI_CUSTOM);
    canvas->w = 410;
    canvas->on_draw = touch_canvas_draw;
    canvas->on_frame = touch_canvas_frame;
    UiNode *tl = ui_box(ui, tr, UI_COL);
    tl->grow = 1;
    tl->gap = 4;
    tl->scroll = true;
    for (int i = 0; i < m->touch_count; i++) {
        LxTouchControl *c = &m->touch[i];
        UiNode *row = ui_box(ui, tl, UI_ROW);
        row->h = 26;
        row->gap = 4;
        UiNode *nm = ui_textfield(ui, row, c->name, sizeof c->name);
        nm->grow = 1;
        nm->on_change = input_changed;
        UiNode *ty = ui_dropdown(ui, row, (int *)&c->type, TOUCH_TYPES, 3);
        ty->w = 96;
        ty->on_change = input_changed;
        UiNode *sz = ui_number(ui, row, &c->size, 0.005f);
        sz->w = 60;
        sz->minv = 0.02f;
        sz->maxv = 1.0f;
        sz->on_change = input_changed;
        lx_strcpy(sz->tooltip, sizeof sz->tooltip, "Size (fraction of the screen)");
        UiNode *rm = ui_icon_button(ui, row, UI_ICON_TRASH, "Remove control", remove_touch);
        rm->tag = i;
        rm->w = 24;
    }
    UiNode *at = ui_button(ui, tl, "Add Touch Control", m_add_touch);
    at->icon = UI_ICON_PLUS;
    at->h = 26;
    UiNode *rst = ui_button(ui, b, "Reset to Defaults", reset_input);
    rst->icon = UI_ICON_REFRESH;
    rst->h = 28;
    rst->w = 180;
    input_rebuild();
}

/* ======================================================================
 *  window
 * ====================================================================== */
static void build_category(void) {
    ui_clear(ED.ui, g_settings_body);
    g_input_detail = g_input_list = NULL;
    for (int i = 0; i < SETTINGS_PAGES; i++) if (g_nav[i]) g_nav[i]->selected = (int)g_nav[i]->tag == g_category;
    switch (g_category) {
    case 0: build_general(g_settings_body); break;
    case 1: build_graphics(g_settings_body); break;
    case 2: build_sound(g_settings_body); break;
    case 3: build_input(g_settings_body); break;
    case 5: build_physics(g_settings_body); break;
    default: build_platforms(g_settings_body); break;
    }
}
void ed_settings_rebuild(void) { if (g_settings_body) build_category(); }
static void nav_pick(UiNode *n) { g_category = (int)n->tag; build_category(); }
static void close_settings(UiNode *n) { save_project(n); save_input(); g_settings_body = NULL; ui_close_modal(ED.ui); }

void ed_project_settings(int category) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    g_category = category;
    ED.project.graphics = *lx_graphics_settings();
    ED.project.audio = *lx_audio_settings();
    UiNode *body = ui_modal(ui, "Project Settings", 1020, 680);
    body->dir = UI_ROW;
    body->gap = 12;
    UiNode *nav = ui_box(ui, body, UI_COL);
    nav->w = 190;
    nav->gap = 2;
    /* page ids stay stable (menus open pages by id); Physics is id 5 but listed after Sound */
    static const char *names[] = {"General", "Graphics", "Sound", "Physics", "Input Manager", "Platforms"};
    static const UiIcon icons[] = {UI_ICON_APP, UI_ICON_MONITOR, UI_ICON_SPEAKER, UI_ICON_RIGIDBODY, UI_ICON_GAMEPAD, UI_ICON_PHONE};
    static const int ids[] = {0, 1, 2, 5, 3, 4};
    for (int i = 0; i < SETTINGS_PAGES; i++) {
        UiNode *b = ui_button(ui, nav, names[i], nav_pick);
        b->icon = icons[i];
        b->icon_color = ed_color_for_icon(icons[i]);
        b->center_text = false;
        b->bg = 0;
        b->h = 32;
        b->tag = ids[i];
        b->bg_active = t->selection;
        g_nav[i] = b;
    }
    ui_spacer(ui, nav);
    UiNode *done = ui_button(ui, nav, "Done", close_settings);
    done->h = 32;
    done->bg = t->accent;
    done->bg_hover = t->accent_hover;
    done->fg = t->accent_text;
    UiNode *note_l = ui_label(ui, nav, "Changes save automatically.");
    note_l->font = UI_FONT_SMALL;
    note_l->fg = t->text_dim;
    g_settings_body = ui_box(ui, body, UI_COL);
    g_settings_body->grow = 1;
    g_settings_body->scroll = true;
    g_settings_body->gap = 4;
    build_category();
}

/* ======================================================================
 *  BUILD SETTINGS
 * ====================================================================== */
static UiNode *g_build_body;
static int g_build_platform;
static void build_settings_fill(void);
static void bs_pick(UiNode *n) { g_build_platform = (int)n->tag; build_settings_fill(); }
static void bs_switch(UiNode *n) {
    LX_UNUSED(n);
    ED.project.build.target = (LxPlatform)g_build_platform;
    lx_project_save(&ED.project);
    ui_notify(ED.ui, "Target platform: %s", lx_platform_name(ED.project.build.target));
    build_settings_fill();
}
static void bs_build(UiNode *n) {
    ED.project.build.target = (LxPlatform)g_build_platform;
    lx_project_save(&ED.project);
    ui_close_modal(ED.ui);
    g_build_body = NULL;
    ed_build((LxPlatform)g_build_platform, n->tag == 1);
}
static void bs_add_current(UiNode *n) {
    LX_UNUSED(n);
    if (!ED.scene_ref[0]) { ui_notify(ED.ui, "Save the scene first"); return; }
    LxBuildSettings *b = &ED.project.build;
    for (int i = 0; i < b->scene_count; i++) if (!strcmp(b->scenes[i], ED.scene_ref)) return;
    if (b->scene_count < LX_MAX_BUILD_SCENES) lx_strcpy(b->scenes[b->scene_count++], LX_REF_MAX, ED.scene_ref);
    lx_project_save(&ED.project);
    build_settings_fill();
}
static void bs_remove(UiNode *n) {
    LxBuildSettings *b = &ED.project.build;
    int i = (int)n->tag;
    memmove(b->scenes[i], b->scenes[i + 1], (size_t)(b->scene_count - i - 1) * LX_REF_MAX);
    b->scene_count--;
    lx_project_save(&ED.project);
    build_settings_fill();
}
static void bs_close(UiNode *n) { LX_UNUSED(n); g_build_body = NULL; ui_close_modal(ED.ui); }
static void engine_folder_changed(UiNode *n) {
    LX_UNUSED(n);
    for (char *p = ED.engine_root_pref; *p; p++) if (*p == '\\') *p = '/';
    ed_save_prefs();
}

/* ---- Game Icon ---- */
#include "app_icon.h"
static UiNode *g_icon_preview, *g_icon_hint;
static uint32_t g_icon_tex, g_default_icon_tex;
static char g_icon_tex_ref[LX_REF_MAX];

static uint32_t default_game_icon(void) {
    if (!g_default_icon_tex) {
        int w, h;
        uint8_t *px = lx_png_decode(lx_game_icon_64_png, sizeof lx_game_icon_64_png, &w, &h);
        if (px) { g_default_icon_tex = lx_ui_texture_from_rgba(px, w, h); lx_free(px); }
    }
    return g_default_icon_tex;
}

/* preview texture for the current icon setting (re-made when the setting changes) */
static uint32_t icon_preview_texture(const char **problem) {
    const char *ref = ED.project.build.icon;
    *problem = NULL;
    if (!ref[0]) return default_game_icon();
    if (g_icon_tex && !strcmp(g_icon_tex_ref, ref)) return g_icon_tex;
    if (g_icon_tex) { lx_ui_texture_free(g_icon_tex); g_icon_tex = 0; }
    lx_strcpy(g_icon_tex_ref, sizeof g_icon_tex_ref, ref);
    char path[LX_PATH_MAX];
    lx_path_join(path, sizeof path, ED.project.assets, ref);
    int w, h;
    uint8_t *px = lx_image_load(path, &w, &h);
    if (!px) { *problem = "Image not found or not a PNG/TGA/BMP - the default icon will be used."; return default_game_icon(); }
    if (w != h) *problem = "Not square: the centre will be cropped.";
    else if (w < 256) *problem = "Small image: 512x512 or larger looks best.";
    uint8_t *sq = lx_image_square(px, w, h, 96);
    g_icon_tex = lx_ui_texture_from_rgba(sq, 96, 96);
    lx_free(sq);
    lx_free(px);
    return g_icon_tex;
}

static void icon_refresh(void) {
    const char *problem;
    if (g_icon_preview) g_icon_preview->texture = icon_preview_texture(&problem);
    else icon_preview_texture(&problem);
    if (g_icon_hint) {
        ui_set_text(g_icon_hint, "%s", problem ? problem
                                       : ED.project.build.icon[0] ? "Used for the Windows .exe, the Android launcher and the game window."
                                                                  : "Default icon. Choose a square PNG (512x512 or larger) to use your own.");
        g_icon_hint->fg = problem ? ui_theme(ED.ui)->warning : ui_theme(ED.ui)->text_dim;
    }
}
static void icon_changed(UiNode *n) { LX_UNUSED(n); lx_project_save(&ED.project); icon_refresh(); }
static void icon_set(const char *ref) {
    lx_strcpy(ED.project.build.icon, sizeof ED.project.build.icon, ref);
    lx_project_save(&ED.project);
    build_settings_fill();
}
static void icon_drop(UiNode *n, const char *type, const char *payload) { LX_UNUSED(n); LX_UNUSED(type); icon_set(payload); }
static void icon_pick(UiNode *n) { icon_set(n->text); }
static void icon_clear(UiNode *n) { LX_UNUSED(n); icon_set(""); }

static void collect_images(UiNode *menu, const char *dir, const char *rel, int *count) {
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n && *count < 200; i++) {
        char full[LX_PATH_MAX], ref[LX_REF_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (rel[0]) snprintf(ref, sizeof ref, "%s/%s", rel, e[i].name);
        else lx_strcpy(ref, sizeof ref, e[i].name);
        if (e[i].is_dir) { collect_images(menu, full, ref, count); continue; }
        const char *x = lx_path_ext(e[i].name);
        if (strcmp(x, ".png") && strcmp(x, ".PNG") && strcmp(x, ".tga") && strcmp(x, ".bmp")) continue;
        UiNode *it = ui_menu_item(ED.ui, menu, ref, NULL, icon_pick);
        it->icon = UI_ICON_TEXTURE;
        it->icon_color = ed_color_for_icon(UI_ICON_TEXTURE);
        (*count)++;
    }
    lx_free(e);
}
static void icon_choose(UiNode *b) {
    UiNode *p = ui_popup(ED.ui, b->x, b->y + b->ch, 320);
    int count = 0;
    collect_images(p, ED.project.assets, "", &count);
    if (!count) {
        UiNode *it = ui_menu_item(ED.ui, p, "No images yet - add a PNG to Assets/Textures", NULL, NULL);
        it->disabled = true;
    }
}

static void icon_section(UiNode *right) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *hl = ui_label(ui, right, "Game Icon");
    hl->font = UI_FONT_BOLD;
    UiNode *row = ui_box(ui, right, UI_ROW);
    row->h = 72;
    row->gap = 12;
    row->cross_align = UI_ALIGN_CENTER;
    const char *problem;
    g_icon_preview = ui_image(ui, row, icon_preview_texture(&problem));
    g_icon_preview->w = 64;
    g_icon_preview->h = 64;
    g_icon_preview->bg = t->field;
    g_icon_preview->radius = 8;
    g_icon_preview->accept = "texture";
    g_icon_preview->on_drop = icon_drop;
    lx_strcpy(g_icon_preview->tooltip, sizeof g_icon_preview->tooltip, "Drop an image here");
    UiNode *col = ui_box(ui, row, UI_COL);
    col->grow = 1;
    col->gap = 6;
    UiNode *fr = ui_box(ui, col, UI_ROW);
    fr->h = 26;
    fr->gap = 6;
    UiNode *tf = ui_textfield(ui, fr, ED.project.build.icon, sizeof ED.project.build.icon);
    tf->grow = 1;
    tf->icon = UI_ICON_TEXTURE;
    tf->accept = "texture";
    tf->on_drop = icon_drop;
    tf->on_change = icon_changed;
    lx_strcpy(tf->hint, sizeof tf->hint, "Default (game controller)");
    UiNode *ch = ui_button(ui, fr, "Choose...", icon_choose);
    ch->icon = UI_ICON_FOLDER_OPEN;
    ch->w = 110;
    UiNode *cl = ui_button(ui, fr, "Reset", icon_clear);
    cl->w = 70;
    cl->disabled = !ED.project.build.icon[0];
    g_icon_hint = ui_label(ui, col, "");
    g_icon_hint->font = UI_FONT_SMALL;
    g_icon_hint->wrap = true;
    icon_refresh();
}
static void bs_save(UiNode *n) { LX_UNUSED(n); lx_project_save(&ED.project); }

static void build_settings_fill(void) {
    if (!g_build_body) return;
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    g_icon_preview = g_icon_hint = NULL;
    ui_clear(ui, g_build_body);
    UiNode *cols = ui_box(ui, g_build_body, UI_ROW);
    cols->grow = 1;
    cols->gap = 14;
    UiNode *left = ui_box(ui, cols, UI_COL);
    left->w = 230;
    left->gap = 4;
    UiNode *pl = ui_label(ui, left, "Platforms");
    pl->font = UI_FONT_BOLD;
    static const UiIcon picons[] = {UI_ICON_WINDOWS, UI_ICON_LINUX, UI_ICON_ANDROID};
    for (int i = 0; i < LX_PLATFORM_COUNT; i++) {
        UiNode *b = ui_button(ui, left, lx_platform_name((LxPlatform)i), bs_pick);
        b->icon = picons[i];
        b->icon_color = i == 2 ? UI_HEX(0x7ee787) : i == 0 ? UI_HEX(0x58a6ff) : UI_HEX(0xe3b341);
        b->icon_size = 20;
        b->center_text = false;
        b->h = 40;
        b->tag = i;
        b->bg = t->panel_alt;
        b->bg_active = t->selection;
        b->selected = i == g_build_platform;
        if (i == (int)ED.project.build.target) lx_strcpy(b->hint, sizeof b->hint, "Target");
    }
    UiNode *right = ui_box(ui, cols, UI_COL);
    right->grow = 1;
    right->gap = 6;
    UiNode *sl = ui_label(ui, right, "Scenes in Build (first = startup in builds)");
    sl->font = UI_FONT_BOLD;
    UiNode *list = ui_box(ui, right, UI_COL);
    list->h = 170;
    list->scroll = true;
    list->bg = t->field;
    list->radius = 6;
    ui_pad(list, 6, 6, 6, 6);
    list->gap = 2;
    LxBuildSettings *bs = &ED.project.build;
    for (int i = 0; i < bs->scene_count; i++) {
        UiNode *row = ui_box(ui, list, UI_ROW);
        row->h = 26;
        row->gap = 6;
        row->cross_align = UI_ALIGN_CENTER;
        char t2[LX_REF_MAX + 8];
        snprintf(t2, sizeof t2, "%d   %s", i, bs->scenes[i]);
        UiNode *l = ui_label(ui, row, t2);
        l->icon = UI_ICON_SCENE;
        l->icon_color = ed_color_for_icon(UI_ICON_SCENE);
        l->grow = 1;
        UiNode *rm = ui_icon_button(ui, row, UI_ICON_CLOSE, "Remove from build", bs_remove);
        rm->tag = i;
    }
    UiNode *addc = ui_button(ui, right, "Add Open Scene", bs_add_current);
    addc->icon = UI_ICON_PLUS;
    addc->h = 28;
    addc->w = 160;
    icon_section(right);
    UiNode *opt = ui_label(ui, right, "Options");
    opt->font = UI_FONT_BOLD;
    UiNode *r = ed_prop_row(right, "Development Build");
    r->first->w = 170;
    UiNode *cb = ui_checkbox(ui, r, "", &bs->development_build);
    cb->on_change = bs_save;
    r = ed_prop_row(right, "Output Folder");
    r->first->w = 170;
    UiNode *of = ui_textfield(ui, r, bs->output_dir, sizeof bs->output_dir);
    of->grow = 1;
    of->on_change = bs_save;
    /* how this build will be made, or what's missing */
    EdBuildPlan plan;
    ed_build_plan((LxPlatform)g_build_platform, &plan);
    char msg[384];
    if (plan.prebuilt) {
        if (plan.scripts)
            snprintf(msg, sizeof msg, "Ready: your %d script%s compile with the editor's built-in compiler and the game is packaged "
                                      "with the player that ships with the editor - nothing else to install.",
                     plan.scripts, plan.scripts == 1 ? "" : "s");
        else snprintf(msg, sizeof msg, "Ready: the game is packaged with the player that ships with the editor - nothing else to install.");
        note(right, UI_ICON_SUCCESS, msg);
    } else if (!plan.can_build) note(right, UI_ICON_WARNING, plan.problem);
    else if (g_build_platform == LX_PLATFORM_ANDROID) {
        note(right, UI_ICON_INFO, "Needs the Android SDK + NDK (ANDROID_HOME), JDK 17 and Gradle. Produces a signed debug APK.");
        note(right, UI_ICON_PHONE, "Build and Run installs on a USB-connected phone with adb.");
    }
    if (plan.custom_count) {
        snprintf(msg, sizeof msg, "Your scenes use %d behaviour%s that no script defines (e.g. \"%s\"): %s.", plan.custom_count,
                 plan.custom_count == 1 ? "" : "s", plan.custom[0], plan.custom_count == 1 ? "it will do nothing" : "they will do nothing");
        note(right, UI_ICON_WARNING, msg);
    }
    if (g_build_platform == LX_PLATFORM_ANDROID) {
        r = ed_prop_row(right, "Engine Source");
        r->first->w = 170;
        UiNode *ef = ui_textfield(ui, r, ED.engine_root_pref, sizeof ED.engine_root_pref);
        ef->grow = 1;
        ef->on_change = engine_folder_changed;
        lx_strcpy(ef->hint, sizeof ef->hint, plan.engine[0] ? plan.engine : "Folder of a Luxora engine source checkout");
        lx_strcpy(ef->tooltip, sizeof ef->tooltip, "Only Android builds use this");
    }
    ui_spacer(ui, right);
    UiNode *btns = ui_box(ui, g_build_body, UI_ROW);
    btns->h = 34;
    btns->gap = 8;
    UiNode *c = ui_button(ui, btns, "Close", bs_close);
    c->w = 90;
    ui_spacer(ui, btns);
    if (g_build_platform != (int)ED.project.build.target) {
        UiNode *sw = ui_button(ui, btns, "Switch Platform", bs_switch);
        sw->w = 150;
        sw->icon = UI_ICON_REFRESH;
    }
    UiNode *b1 = ui_button(ui, btns, "Build", bs_build);
    b1->icon = UI_ICON_BUILD;
    b1->w = 110;
    b1->tag = 0;
    UiNode *b2 = ui_button(ui, btns, "Build and Run", bs_build);
    b2->icon = UI_ICON_PLAY;
    b2->w = 150;
    b2->tag = 1;
    b2->bg = t->accent;
    b2->bg_hover = t->accent_hover;
    b2->fg = t->accent_text;
    bool busy = lx_atomic_load(&ED.building) != 0;
    b1->disabled = b2->disabled = busy;
}

void ed_build_settings(void) {
    g_build_platform = (int)ED.project.build.target;
    g_build_body = ui_modal(ED.ui, "Build Settings", 820, 650);
    build_settings_fill();
}

/* ======================================================================
 *  ABOUT & SHORTCUTS
 * ====================================================================== */
static void close_modal(UiNode *n) { LX_UNUSED(n); ui_close_modal(ED.ui); }

void ed_about(void) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *body = ui_modal(ui, "About Inferno", 520, 404);
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->h = 56;
    row->gap = 12;
    row->cross_align = UI_ALIGN_CENTER;
    ed_app_icon(row, 52);
    UiNode *col = ui_box(ui, row, UI_COL);
    col->grow = 1;
    ui_label(ui, col, "Inferno")->font = UI_FONT_TITLE;
    UiNode *v = ui_label(ui, col, "Luxora Engine " LX_VERSION_STRING " · written in C11 · Vulkan 1.3");
    v->fg = t->text_dim;
    static const char *lines[] = {"Physically based rendering with cascaded shadows",
                                  "Reflection probes (GGX pre-filtered) + screen-space reflections",
                                  "Virtual photon global illumination (CPU, all cores)",
                                  "Ray-traced acoustics: speed of sound, occlusion, reflections, reverb",
                                  "Data-oriented crowds, automatic LODs, instanced batching",
                                  "Input Manager: keyboard, mouse, gamepad and touch",
                                  "Builds for Windows, Linux and Android"};
    for (int i = 0; i < (int)(sizeof lines / sizeof lines[0]); i++) {
        UiNode *l = ui_label(ui, body, lines[i]);
        l->icon = UI_ICON_CHECK;
        l->icon_color = t->success;
    }
    UiNode *f = ui_label(ui, body, "UI font: Inter (SIL OFL). Mono font: DejaVu Sans Mono.");
    f->font = UI_FONT_SMALL;
    f->fg = t->text_dim;
    ui_spacer(ui, body);
    UiNode *btns = ui_box(ui, body, UI_ROW);
    btns->h = 32;
    ui_spacer(ui, btns);
    UiNode *ok = ui_button(ui, btns, "Close", close_modal);
    ok->w = 100;
}

void ed_shortcuts_help(void) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *body = ui_modal(ui, "Keyboard Shortcuts", 640, 600);
    static const char *keys[][2] = {
        {"Right mouse + WASD/QE", "Fly the camera (scroll changes speed)"}, {"Middle mouse drag", "Pan"},
        {"Alt + Left mouse drag", "Orbit around the selection"}, {"Mouse wheel", "Move forward / back"},
        {"F", "Focus the selection"}, {"Q / W / E / R", "Select / Move / Rotate / Scale tool"},
        {"Ctrl while dragging a gizmo", "Snap"}, {"Ctrl+Z / Ctrl+Y", "Undo / Redo"}, {"Ctrl+D", "Duplicate"},
        {"Delete", "Delete selection"}, {"F2", "Rename"}, {"Ctrl+S / Ctrl+Shift+S", "Save scene / Save as"},
        {"Ctrl+N", "New scene"}, {"Ctrl+P", "Play / Stop"}, {"Ctrl+Shift+P", "Pause"}, {"Ctrl+Alt+P", "Step one frame"},
        {"Ctrl+B / Ctrl+Shift+B", "Build / Build Settings"}, {"Ctrl+Shift+R", "Build and Run"}, {"Esc", "Deselect / close menus"},
        {"F1", "This window"}};
    UiNode *list = ui_box(ui, body, UI_COL);
    list->grow = 1;
    list->scroll = true;
    list->gap = 2;
    for (int i = 0; i < 20; i++) {
        UiNode *row = ui_box(ui, list, UI_ROW);
        row->h = 24;
        row->gap = 10;
        UiNode *k = ui_label(ui, row, keys[i][0]);
        k->w = 230;
        k->font = UI_FONT_MONO;
        k->fg = UI_HEX(0x79c0ff);
        ui_label(ui, row, keys[i][1])->grow = 1;
    }
    UiNode *btns = ui_box(ui, body, UI_ROW);
    btns->h = 32;
    ui_spacer(ui, btns);
    UiNode *ok = ui_button(ui, btns, "Close", close_modal);
    ok->w = 100;
    LX_UNUSED(t);
}
