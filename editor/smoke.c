/* Scripted smoke test (--smoke): drives the editor through its windows and tools over a
 * few hundred frames, then quits. Used by CI and local testing to catch crashes and leaks. */
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "editor.h"
#include "graph_editor.h"

typedef void (*SmokeFn)(void);
int ed_smoke_failures;
#define SMOKE_CHECK(cond, ...) do { if (!(cond)) { ed_smoke_failures++; LX_ERROR("[smoke] FAILED: " __VA_ARGS__); } else LX_INFO("[smoke] ok: " __VA_ARGS__); } while (0)

static UiNode *find_node(UiNode *n, const void *bound_float, const char *text) {
    if (!n) return NULL;
    if (bound_float && n->f == bound_float) return n;
    if (text && n->type == UI_BUTTON && !strcmp(n->text, text)) return n;
    for (UiNode *c = n->first; c; c = c->next) {
        UiNode *r = find_node(c, bound_float, text);
        if (r) return r;
    }
    return NULL;
}

static void s_game_edit(void) {
    ED.game_view = true;
    SMOKE_CHECK(ed_viewport_showing_game(), "Game view shows the main camera in edit mode");
}
static void s_game_back(void) {
    ED.game_view = false;
    SMOKE_CHECK(!ed_viewport_showing_game(), "Scene view returns to the editor camera");
}
static void s_game_play(void) {
    ed_play();
    SMOKE_CHECK(ED.game_view && ed_viewport_showing_game(), "Play switches to the Game view");
}
static void s_game_stop(void) {
    ed_stop();
    SMOKE_CHECK(!ED.game_view, "Stop restores the Scene view");
}

static char g_reopen[LX_PATH_MAX];
static void s_close_project(void) {
    lx_strcpy(g_reopen, sizeof g_reopen, ED.project.root);
    ED.dirty = false;
    /* exactly what File > Close Project does: it is clicked while a frame is being built */
    ED.in_frame = true;
    UiNode *menu_item = find_node(ui_root(ED.ui), NULL, "Close Project");
    if (menu_item && menu_item->on_click) menu_item->on_click(menu_item);
    else ed_request_close_project();
    SMOKE_CHECK(ED.has_project, "Close Project waits for the frame to finish");
    ED.in_frame = false;
}
static void s_closed(void) {
    SMOKE_CHECK(!ED.has_project && ED.mode == ED_MODE_PROJECT_MANAGER, "Close Project returns to the Project Manager");
}
static void s_reopen_project(void) {
    ED.in_frame = true;
    ed_open_project(g_reopen);
    ED.in_frame = false;
}
static void s_reopened(void) {
    SMOKE_CHECK(ED.has_project && ED.mode == ED_MODE_EDITOR && ED.scene, "Project reopens from the Project Manager");
}

/* collapse the Transform section through its header, as a click would */
static UiNode *find_section_header(UiNode *n, const char *title) {
    if (!n) return NULL;
    if (n->type == UI_LABEL && !strcmp(n->text, title) && n->parent && n->parent->user) return n->parent;
    for (UiNode *c = n->first; c; c = c->next) {
        UiNode *r = find_section_header(c, title);
        if (r) return r;
    }
    return NULL;
}
static void s_collapse(void) {
    ed_select(lx_entity_find(ED.scene, "Cube"));
}
static void s_collapse_click(void) {
    UiNode *h = find_section_header(ED.inspector, "Transform");
    SMOKE_CHECK(h != NULL, "found the Transform section header");
    if (!h) return;
    UiNode *wrap = h->parent, *body = h->next, *after = wrap->next;
    h->on_click(h);
    SMOKE_CHECK(body && body->hidden, "collapsing Transform hides its own fields");
    SMOKE_CHECK(!wrap->hidden && after && !after->hidden, "collapsing Transform leaves the components below visible");
    h->on_click(h);
    SMOKE_CHECK(body && !body->hidden, "expanding Transform shows its fields again");
}

static void s_quit_prompt(void) {
    SMOKE_CHECK(ED.menubar && ED.menubar->first && ED.menubar->first->type == UI_IMAGE, "title bar shows the app icon");
    ED.dirty = true;
    ed_request_quit();   /* what the window's close button does */
    SMOKE_CHECK(ED.save_prompt_open && ui_has_modal(ED.ui) && !ED.quit, "closing with unsaved changes asks to save");
}
static void s_prompt_shot(void) {
    const char *shot = getenv("LUXORA_SMOKE_SHOT");   /* CI: picture of the save prompt + title bar */
    if (shot && shot[0]) lx_renderer_screenshot(shot);
}
/* CI: a close, low-angle look at the sample's cobblestone path (parallax occlusion, normal, AO maps) */
static void s_parallax_view(void) {
    ED.game_view = false;
    ED.selected = 0;
    ED.cam_pos = lx_v3(1.0f, 0.75f, 5.3f);
    ED.cam_yaw = 0.0f;
    ED.cam_pitch = -20.0f;
}
static void s_parallax_shot(void) {
    const char *shot = getenv("LUXORA_SMOKE_SHOT");
    if (!shot || !shot[0]) return;
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, shot);
    lx_path_join(path, sizeof path, dir[0] ? dir : ".", "parallax.ppm");
    lx_renderer_screenshot(path);
}
static void s_material_view(void) {
    char p[LX_PATH_MAX];
    lx_path_join(p, sizeof p, ED.project.assets, "Materials/Cobblestone.lxmat");
    ED.selected = 0;
    ed_select_asset(p);
}
static UiNode *find_label(UiNode *n, const char *text) {
    if (!n) return NULL;
    if (n->type == UI_LABEL && !strcmp(n->text, text)) return n;
    for (UiNode *c = n->first; c; c = c->next) {
        UiNode *r = find_label(c, text);
        if (r) return r;
    }
    return NULL;
}
static void s_material_shot(void) {
    UiNode *base = find_label(ui_root(ED.ui), "BASE COLOR"), *height = find_label(ui_root(ED.ui), "HEIGHT (PARALLAX)");
    SMOKE_CHECK(base && height && base->y < height->y, "material inspector groups, colour first");
    const char *shot = getenv("LUXORA_SMOKE_SHOT");
    if (!shot || !shot[0]) return;
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, shot);
    lx_path_join(path, sizeof path, dir[0] ? dir : ".", "material_inspector.ppm");
    lx_renderer_screenshot(path);
}
/* prefabs: make one from an object, place copies, edit one and apply to all */
#define NEAR_SMOKE(a, b) (fabsf((a) - (b)) < 1e-3f)
static LxEntity g_pf_src, g_pf_a, g_pf_b;
static char g_pf_ref[LX_REF_MAX];
static void s_prefab_make(void) {
    g_pf_src = ed_create("Cube");
    lx_entity_set_name(ED.scene, g_pf_src, "Barrel");
    lx_entity_set_tag(ED.scene, g_pf_src, "Prop");
    LxEntity kid = ed_create("Sphere");
    lx_entity_set_parent(ED.scene, kid, g_pf_src);
    char dir[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, ED.project.assets, "Prefabs");
    SMOKE_CHECK(ed_prefab_create(g_pf_src, dir), "Save as Prefab");
    lx_strcpy(g_pf_ref, sizeof g_pf_ref, lx_entity_prefab(ED.scene, g_pf_src));
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, g_pf_ref);
    SMOKE_CHECK(lx_file_exists(path) && !strcmp(g_pf_ref, "Prefabs/Barrel.lxprefab"), "prefab file written (%s)", g_pf_ref);
    /* the drag-and-drop targets are wired up */
    SMOKE_CHECK(ED.content_grid && ED.content_grid->accept && strstr(ED.content_grid->accept, "entity"), "Content Browser accepts objects from the Hierarchy");
    SMOKE_CHECK(ED.hierarchy_list && strstr(ED.hierarchy_list->accept, "prefab"), "Hierarchy accepts prefabs");
}
static void s_prefab_place(void) {
    g_pf_a = ed_prefab_place(g_pf_ref, lx_v3(4, 0, 0), 0);
    g_pf_b = ed_prefab_place(g_pf_ref, lx_v3(-4, 0, 0), 0);
    SMOKE_CHECK(g_pf_a && g_pf_b && lx_entity_first_child(ED.scene, g_pf_a) && lx_entity_has_tag(ED.scene, g_pf_b, "Prop"),
                "placed prefab copies keep children and tags");
    SMOKE_CHECK(NEAR_SMOKE(lx_entity_position(ED.scene, g_pf_a).x, 4), "placed where dropped");
    lx_entity_set_scale(ED.scene, g_pf_a, lx_v3(2, 3, 2));
    LxEntity extra = ed_create("Point Light");
    lx_entity_set_parent(ED.scene, extra, g_pf_a);
    ed_prefab_apply(g_pf_a);
}
static void s_prefab_applied(void) {
    int copies = 0, lights = 0;
    int n = lx_scene_entity_count(ED.scene);
    for (int i = 0; i < n; i++) {
        LxEntity x = lx_scene_entity_at(ED.scene, i);
        if (strcmp(lx_entity_prefab(ED.scene, x), g_pf_ref)) continue;
        copies++;
        for (LxEntity c = lx_entity_first_child(ED.scene, x); c; c = lx_entity_next_sibling(ED.scene, c)) lights += lx_get_light(ED.scene, c) != NULL;
    }
    SMOKE_CHECK(copies == 3 && lights == 3, "Apply updates every copy (%d copies, %d with the new light)", copies, lights);
    SMOKE_CHECK(ED.selected && ed_prefab_root(ED.selected) && !strcmp(lx_entity_prefab(ED.scene, ed_prefab_root(ED.selected)), g_pf_ref),
                "the selection is still part of a copy");
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, g_pf_ref);
    ED.selected = 0;
    ed_select_asset(path);
}
static void s_prefab_shot(void) {
    const char *shot = getenv("LUXORA_SMOKE_SHOT");
    if (!shot || !shot[0]) return;
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, shot);
    lx_path_join(path, sizeof path, dir[0] ? dir : ".", "prefabs.ppm");
    lx_renderer_screenshot(path);
}

/* models: import two Mixamo characters as humanoids, open the Avatar window, and play one
 * character's dance on the other (retargeting). Needs LUXORA_SMOKE_FBX_DIR (CI downloads them). */
static char g_md_char[LX_REF_MAX], g_md_dance[LX_REF_MAX];
static LxEntity g_md_ent;
static void smoke_shot(const char *name) {
    const char *shot = getenv("LUXORA_SMOKE_SHOT");
    if (!shot || !shot[0]) return;
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, shot);
    lx_path_join(path, sizeof path, dir[0] ? dir : ".", name);
    lx_renderer_screenshot(path);
}
static bool copy_model(const char *dir, const char *name, char *ref_out) {
    char src[LX_PATH_MAX], dst_dir[LX_PATH_MAX], dst[LX_PATH_MAX];
    lx_path_join(src, sizeof src, dir, name);
    size_t n;
    char *data = lx_file_read(src, &n);
    if (!data) return false;
    lx_path_join(dst_dir, sizeof dst_dir, ED.project.assets, "Models");
    lx_mkdir_p(dst_dir);
    lx_path_join(dst, sizeof dst, dst_dir, name);
    bool ok = lx_file_write(dst, data, n);
    lx_free(data);
    snprintf(ref_out, LX_REF_MAX, "Models/%s", name);
    /* Humanoid, avatar mapped from the bone names */
    LxModel *m = lx_asset_model(ref_out);
    if (!m) return false;
    LxModelImport imp = *lx_model_settings(m);
    imp.rig = LX_RIG_HUMANOID;
    int mapped = lx_avatar_automap(m, imp.avatar);
    ok = ok && lx_model_import_save(dst, &imp);
    lx_model_reload(ref_out);
    SMOKE_CHECK(mapped >= 20 && lx_avatar_is_valid(imp.avatar, NULL, 0), "%s: avatar automapped (%d bones)", name, mapped);
    return ok;
}
static void s_model_import(void) {
    const char *dir = getenv("LUXORA_SMOKE_FBX_DIR");
    if (!dir || !dir[0]) { LX_INFO("[smoke] LUXORA_SMOKE_FBX_DIR not set - skipping models"); return; }
    if (!copy_model(dir, "mixamo.fbx", g_md_char) || !copy_model(dir, "Samba Dancing.fbx", g_md_dance)) {
        SMOKE_CHECK(false, "copy the sample models");
        return;
    }
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, g_md_char);
    ED.selected = 0;
    ed_select_asset(path);
}
static void s_model_inspector_shot(void) {
    if (!g_md_char[0]) return;
    SMOKE_CHECK(find_node(ui_root(ED.ui), NULL, "Configure Avatar...") != NULL, "model inspector shows the Rig section");
    smoke_shot("model_inspector.ppm");
    ed_avatar_window(g_md_char);
}
static void s_avatar_shot(void) {
    if (!g_md_char[0]) return;
    extern UiNode *ui_modal_node(UiCtx *ui);
    SMOKE_CHECK(find_node(ui_modal_node(ED.ui), NULL, "Automap") != NULL, "Avatar window is open");
    smoke_shot("avatar.ppm");
    ui_close_modal(ED.ui);
}
static void s_model_place(void) {
    if (!g_md_char[0]) return;
    g_md_ent = ed_model_place(g_md_char, lx_v3(1.0f, 0, 4.1f), 0);
    LxAnimator *a = g_md_ent ? lx_get_animator(ED.scene, g_md_ent) : NULL;
    SMOKE_CHECK(a != NULL, "placed model has an Animator");
    if (!a) return;
    LxModel *dance = lx_asset_model(g_md_dance);
    snprintf(a->clip, sizeof a->clip, "%s#%s", g_md_dance, dance ? lx_model_clip_name(dance, 0) : "0");
    lx_animator_play(ED.scene, g_md_ent, a->clip, 0);
    lx_animator_update(ED.scene, g_md_ent, 3.4f);   /* the editor's preview: a pose part-way into the dance */
    SMOKE_CHECK(lx_animator_playing(ED.scene, g_md_ent), "another humanoid's clip plays on the character (retargeted)");
    ED.selected = 0;
    ED.cam_pos = lx_v3(1.0f, 1.0f, 6.7f);
    ED.cam_yaw = 0.0f;
    ED.cam_pitch = -6.0f;
}
static LxEntity find_named(LxEntity e, const char *part) {
    for (LxEntity c = lx_entity_first_child(ED.scene, e); c; c = lx_entity_next_sibling(ED.scene, c)) {
        if (strstr(lx_entity_name(ED.scene, c), part)) return c;
        LxEntity d = find_named(c, part);
        if (d) return d;
    }
    return 0;
}
static void s_model_anim_shot(void) {
    if (!g_md_ent) return;
    smoke_shot("animated_character.ppm");
}
static void s_model_reset(void) {
    if (!g_md_ent) return;
    LxEntity hips = find_named(g_md_ent, "Hips");
    LxQuat before = hips ? lx_entity_rotation(ED.scene, hips) : lx_quat_identity();
    lx_animator_reset_pose(ED.scene, g_md_ent);
    LxQuat after = hips ? lx_entity_rotation(ED.scene, hips) : lx_quat_identity();
    SMOKE_CHECK(!lx_animator_playing(ED.scene, g_md_ent) && (!hips || fabsf(before.w - after.w) + fabsf(before.y - after.y) > 1e-4f),
                "stopping the preview returns to the rest pose");
}
static float g_md_hips_y;
static void s_model_play(void) {
    if (!g_md_ent) return;
    ed_play();
    SMOKE_CHECK(ED.play == ED_PLAYING, "entered play mode with a character");
}
static void s_model_playing(void) {
    if (!g_md_ent) return;
    LxEntity c = lx_entity_find(ED.scene, "mixamo");
    SMOKE_CHECK(c && lx_animator_playing(ED.scene, c) && lx_animator_time(ED.scene, c) > 0.05f, "character animates in play mode (t=%.2f)",
                c ? lx_animator_time(ED.scene, c) : -1.0f);
    g_md_hips_y = 0;
}
/* Stop is clicked while the frame is being recorded: the play scene (and its animated character's
 * GPU buffers) is destroyed while that frame still uses them */
static bool g_stop_in_frame;
void ed_smoke_in_frame(void) {
    if (!g_stop_in_frame) return;
    g_stop_in_frame = false;
    ed_stop();
}
static void s_model_stop(void) {
    if (!g_md_ent) return;
    g_stop_in_frame = true;
}
static void s_model_stopped(void) {
    if (!g_md_ent) return;
    SMOKE_CHECK(ED.play == ED_EDIT && lx_entity_find(ED.scene, "mixamo"), "stopped play mode, the character is back in the edit scene");
}
/* a folder of 4K textures: the editor keeps running while thumbnails are made, and they all arrive */
static char g_big_dir[LX_PATH_MAX];
static double g_big_last, g_big_worst;
static bool g_big_skip;
static double g_frame_times[32], g_frame_prev, g_big_limit = 0.4;
static int g_frame_n;
static bool g_big_done;
static void s_big_make(void) {
    lx_path_join(g_big_dir, sizeof g_big_dir, ED.project.assets, "Textures/Big 4K Textures With Long Names");
    lx_mkdir_p(g_big_dir);
    int n = 4096;
    uint8_t *px = lx_alloc((size_t)n * n * 4);
    uint32_t r = 12345;
    for (int img = 0; img < 4; img++) {
        for (size_t i = 0; i < (size_t)n * n; i++) {
            r = r * 1664525u + 1013904223u;
            px[i * 4] = (uint8_t)((i % (size_t)n) * 255 / (size_t)n);
            px[i * 4 + 1] = (uint8_t)(r >> 24);
            px[i * 4 + 2] = (uint8_t)(img * 60);
            px[i * 4 + 3] = 255;
        }
        size_t sz;
        uint8_t *png = lx_png_encode(px, n, n, &sz);
        char path[LX_PATH_MAX], name[96];
        snprintf(name, sizeof name, "Ch47_nonPBR_MovementAnimsetPro_Texture_Albedo_%d.png", img);
        lx_path_join(path, sizeof path, g_big_dir, name);
        lx_file_write(path, png, sz);
        lx_free(png);
    }
    lx_free(px);
}
static void s_big_open(void) {
    /* a software GPU (CI's lavapipe) draws every frame slowly: allow a few normal frames' worth */
    int n = g_frame_n < 32 ? g_frame_n : 32;
    double t[32];
    memcpy(t, g_frame_times, sizeof t);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) if (t[j] < t[i]) { double x = t[i]; t[i] = t[j]; t[j] = x; }
    double typical = n ? t[n / 2] : 0;   /* median: the 4K files being written aren't a normal frame */
    g_big_limit = typical * 4 > 0.4 ? typical * 4 : 0.4;
    ED.content_list_view = false;   /* tiles */
    ed_content_open_dir(g_big_dir);
    g_big_last = lx_time();
    g_big_worst = 0;
}
/* runs every frame while waiting */
static double g_big_start;
static void s_big_check(void);
static void big_watch(void) {
    if (g_big_done || !g_big_last) return;
    double now = lx_time();
    if (g_big_skip) g_big_skip = false;
    else if (now - g_big_last > g_big_worst) g_big_worst = now - g_big_last;
    g_big_last = now;
    if (!g_big_start) g_big_start = now;
    /* done when every picture is in, or after 60 s */
    LxDirEntry *e;
    int n = lx_dir_list(g_big_dir, &e), ready = 0;
    for (int i = 0; i < n; i++) {
        char p[LX_PATH_MAX];
        lx_path_join(p, sizeof p, g_big_dir, e[i].name);
        ready += ed_thumb_get(p, e[i].size, 128, NULL, NULL, NULL) != 0;
    }
    lx_free(e);
    if ((n && ready == n && now - g_big_start > 0.5) || now - g_big_start > 60) {
        LX_INFO("[smoke] thumbnails ready after %.1f s", now - g_big_start);
        s_big_check();
    }
}
static void s_big_check(void) {
    g_big_done = true;
    LxDirEntry *e;
    int n = lx_dir_list(g_big_dir, &e), ready = 0;
    for (int i = 0; i < n; i++) {
        char p[LX_PATH_MAX];
        lx_path_join(p, sizeof p, g_big_dir, e[i].name);
        ready += ed_thumb_get(p, e[i].size, 128, NULL, NULL, NULL) != 0;
    }
    lx_free(e);
    SMOKE_CHECK(n == 4 && ready == n, "4K texture thumbnails made in the background (%d of %d)", ready, n);
    SMOKE_CHECK(g_big_worst < g_big_limit, "the editor kept running while they loaded (slowest frame %.0f ms, limit %.0f ms)", g_big_worst * 1000, g_big_limit * 1000);
    /* names fit their tiles */
    bool fit = true;
    for (UiNode *tl = ED.content_grid ? ED.content_grid->first : NULL; tl; tl = tl->next)
        for (UiNode *c = tl->first; c; c = c->next)
            if (c->type == UI_LABEL && c->text[0] && c->w > ED.content_tile) fit = false;
    SMOKE_CHECK(fit, "file names stay inside their tiles");
    char lib[LX_PATH_MAX];
    lx_path_join(lib, sizeof lib, ED.project.root, "Library/Thumbnails");
    LxDirEntry *t;
    int tc = lx_dir_list(lib, &t);
    lx_free(t);
    SMOKE_CHECK(tc >= 4, "thumbnails saved for next time (%d files)", tc);
    smoke_shot("content_browser_4k.ppm");
}
static void s_quit_cancel(void) {
    UiNode *cancel = find_node(ui_root(ED.ui), NULL, "Cancel");
    extern UiNode *ui_modal_node(UiCtx *ui);
    if (!cancel) cancel = find_node(ui_modal_node(ED.ui), NULL, "Cancel");
    SMOKE_CHECK(cancel != NULL, "save prompt has Cancel");
    if (cancel) cancel->on_click(cancel);
    SMOKE_CHECK(!ui_has_modal(ED.ui) && !ED.quit && ED.dirty, "Cancel keeps the editor open with the changes");
    ED.dirty = false;
}

#include "app_icon.h"
static void s_game_icon(void) {
    /* a custom game icon: written as a PNG into the project and picked in Build Settings */
    int w, h;
    uint8_t *px = lx_png_decode(lx_app_icon_64_png, sizeof lx_app_icon_64_png, &w, &h);
    uint8_t *big = lx_image_resize(px, w, h, 256, 256);
    size_t n;
    uint8_t *png = lx_png_encode(big, 256, 256, &n);
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, ED.project.assets, "Textures");
    lx_mkdir_p(dir);
    lx_path_join(path, sizeof path, dir, "GameIcon.png");
    SMOKE_CHECK(lx_file_write(path, png, n), "write a game icon image");
    lx_free(png); lx_free(big); lx_free(px);
    lx_strcpy(ED.project.build.icon, sizeof ED.project.build.icon, "Textures/GameIcon.png");
    lx_project_save(&ED.project);
    ed_build_settings();
    LxProject check;
    SMOKE_CHECK(lx_project_load(&check, ED.project.root) && !strcmp(check.build.icon, "Textures/GameIcon.png"), "game icon saved in the project");
}

static UiNode *g_first_widget;
static void s_mat_select(void) {
    char p[LX_PATH_MAX];
    lx_path_join(p, sizeof p, ED.project.assets, "Materials/Ground.lxmat");
    ED.selected = 0;
    ed_select_asset(p);
}
static void s_mat_capture(void) {
    g_first_widget = find_node(ED.inspector, &lx_asset_material("Materials/Ground.lxmat")->roughness, NULL);
    SMOKE_CHECK(g_first_widget != NULL, "material inspector shows a roughness slider");
}
static void s_mat_edit(void) {
    UiNode *n = find_node(ED.inspector, &lx_asset_material("Materials/Ground.lxmat")->roughness, NULL);
    SMOKE_CHECK(n == g_first_widget && n, "material inspector widgets are stable across frames (not rebuilt)");
    if (n) { *n->f = 0.123f; n->on_change(n); }
}
static void s_mat_verify(void) {
    char p[LX_PATH_MAX];
    lx_path_join(p, sizeof p, ED.project.assets, "Materials/Ground.lxmat");
    LxMaterial m;
    SMOKE_CHECK(lx_material_load(&m, p) && fabsf(m.roughness - 0.123f) < 1e-4f, "material edit saved to disk (roughness %.3f)", (double)m.roughness);
}
static void s_make_editable(void) {
    ed_select(lx_entity_find(ED.scene, "Cube"));
    ED.inspector_dirty = true;
}
static void s_make_editable_click(void) {
    UiNode *b = find_node(ED.inspector, NULL, "Make Editable Copy");
    SMOKE_CHECK(b != NULL, "built-in material offers Make Editable Copy");
    if (b) b->on_click(b);
}
static void s_make_editable_verify(void) {
    LxMeshRenderer *mr = lx_get_mesh(ED.scene, lx_entity_find(ED.scene, "Cube"));
    char p[LX_PATH_MAX];
    lx_asset_path(p, sizeof p, mr ? mr->material : "");
    SMOKE_CHECK(mr && !strncmp(mr->material, "Materials/", 10) && lx_file_exists(p), "cube now uses an editable material (%s)", mr ? mr->material : "-");
    UiNode *slider = mr ? find_node(ED.inspector, &lx_asset_material(mr->material)->metallic, NULL) : NULL;
    SMOKE_CHECK(slider != NULL, "material properties are editable inline on the Mesh Renderer");
}

static void s_create_all(void) {
    static const char *what[] = {"Empty", "Cube", "Sphere", "Plane", "Cylinder", "Capsule", "Camera", "Directional Light",
                                 "Point Light", "Spot Light", "Reflection Probe", "Audio Source", "Crowd"};
    for (int i = 0; i < 13; i++) ed_create(what[i]);
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
static void s_set5(void) { s_settings(5); }
static LxEntity g_phys_cube, g_player;
static float g_cube_y0;
static void s_physics_objects(void) {
    g_phys_cube = ed_create("Physics Cube");
    g_player = ed_create("Character");
    g_cube_y0 = lx_entity_world_position(ED.scene, g_phys_cube).y;
    SMOKE_CHECK(lx_get_rigidbody(ED.scene, g_phys_cube) && lx_get_collider(ED.scene, g_phys_cube), "Physics Cube has a collider and a rigidbody");
    SMOKE_CHECK(lx_get_character(ED.scene, g_player) != NULL, "Character has a character controller");
    ed_select(g_phys_cube);   /* inspector shows the physics sections, viewport the wireframe */
}
/* the new project's sample script (Assets/Scripts/game.c) is compiled by the built-in compiler */
static LxEntity g_spinner;
static float g_spin0;
static void s_script_compiled(void) {
    SMOKE_CHECK(!ed_scripts_supported() || lx_behaviour_find("Spinner") != NULL, "the sample script compiled and registered Spinner");
    SMOKE_CHECK(ed_script_errors() == 0, "no script errors");
}
static void s_script_attach(void) {
    if (!lx_behaviour_find("Spinner")) return;
    g_spinner = ed_create("Cube");
    lx_add_behaviour(ED.scene, g_spinner, "Spinner");
    g_spin0 = lx_entity_euler(ED.scene, g_spinner).y;
}
static void s_script_ran(void) {
    if (!g_spinner) return;
    float y = lx_entity_euler(ED.scene, g_spinner).y;
    SMOKE_CHECK(fabsf(y - g_spin0) > 0.5f, "the script behaviour runs in play mode (%.1f -> %.1f)", g_spin0, y);
}
static void s_physics_fell(void) {
    float y = lx_entity_world_position(ED.scene, g_phys_cube).y;
    SMOKE_CHECK(y < g_cube_y0 - 0.01f, "the physics cube falls in play mode (%.2f -> %.2f)", g_cube_y0, y);
}
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

/* the State Graph editor on the sample's Patroller */
static void sg_shot(const char *name) {
    const char *shot = getenv("LUXORA_SMOKE_SHOT");
    if (!shot || !shot[0]) return;
    char dir[LX_PATH_MAX], path[LX_PATH_MAX];
    lx_path_parent(dir, sizeof dir, shot);
    lx_path_join(path, sizeof path, dir[0] ? dir : ".", name);
    lx_renderer_screenshot(path);
    g_big_skip = true;   /* a slow screenshot (read back at the end of the frame) isn't a slow editor frame */
}
static UiNode *find_hint(UiNode *n, const char *hint) {
    if (!n) return NULL;
    if (n->type == UI_TEXTFIELD && strstr(n->hint, hint)) return n;
    for (UiNode *c = n->first; c; c = c->next) { UiNode *r = find_hint(c, hint); if (r) return r; }
    return NULL;
}
static int g_sg_actions;
static int sg_action_count(void) {
    LxGraph *g = lx_asset_graph("Graphs/Patroller.lxgraph");
    LxGraphState *st = g ? lx_graph_find_state(g, "Go to B") : NULL;
    return st ? st->action_count : -1;
}
static void s_sg_open(void) {
    LxEntity e = lx_entity_find(ED.scene, "Patroller");
    SMOKE_CHECK(e && lx_get_state_graph(ED.scene, e), "the sample has a Patroller with a State Graph");
    if (!e) return;
    ed_graph_edit(e, 0);
    SMOKE_CHECK(ED.view_tabs && ED.view_tabs->tab_index == 1 && ge_graph() && ge_graph()->state_count == 5, "State Graph editor shows its graph");
    SMOKE_CHECK(ed_graph_select_state("Go to B"), "select a state");
    g_sg_actions = sg_action_count();
}
static void s_sg_browser(void) {
    UiNode *b = find_node(ui_root(ED.ui), NULL, "Action Browser");
    SMOKE_CHECK(b != NULL, "Action Browser button in the bottom bar");
    if (b) ui_test_click(ED.ui, b, false);
    UiNode *w[8];
    SMOKE_CHECK(ui_window_nodes(ED.ui, w, 8) == 1 && GE.ab_win, "the Action Browser opens as a window");
}
static void s_sg_search(void) {
    UiNode *w[8];
    int n = ui_window_nodes(ED.ui, w, 8);
    UiNode *f = n ? find_hint(w[0], "Search actions") : NULL;
    SMOKE_CHECK(f != NULL, "Action Browser search field");
    if (f) ui_test_type(ED.ui, f, "rotate");
}
static void s_sg_clear(void) {
    SMOKE_CHECK(!strcmp(GE.ab_sel, "Rotate"), "searching picks the first match (%s)", GE.ab_sel);
    UiNode *w[8];
    int n = ui_window_nodes(ED.ui, w, 8);
    UiNode *f = n ? find_hint(w[0], "Search actions") : NULL;
    if (f) ui_test_type(ED.ui, f, "");   /* leaving the field must not add anything */
    SMOKE_CHECK(sg_action_count() == g_sg_actions, "clearing the search adds nothing");
}
static void s_sg_shot(void) { sg_shot("state_graph.ppm"); }
static void s_sg_pick(void) {
    UiNode *w[8];
    int n = ui_window_nodes(ED.ui, w, 8);
    UiNode *it = n ? find_node(w[0], NULL, "Translate") : NULL;
    SMOKE_CHECK(it != NULL, "Translate is listed");
    if (it) ui_test_click(ED.ui, it, false);
    SMOKE_CHECK(!strcmp(GE.ab_sel, "Translate") && sg_action_count() == g_sg_actions, "clicking an action selects it (%s), nothing added", GE.ab_sel);
}
static void s_sg_picked(void) {
    UiNode *w[8];
    int n = ui_window_nodes(ED.ui, w, 8);
    UiNode *it = n ? find_node(w[0], NULL, "Translate") : NULL;
    SMOKE_CHECK(it && it->bg == UI_HEX(0x7a2412) && it->bg_hover != 0, "the clicked action is highlighted, also under the mouse");
}
static void s_sg_add(void) {
    UiNode *w[8];
    int n = ui_window_nodes(ED.ui, w, 8);
    UiNode *b = n ? find_node(w[0], NULL, "Add Action To State") : NULL;
    SMOKE_CHECK(b && !b->disabled, "Add Action To State is enabled");
    if (b) ui_test_click(ED.ui, b, false);
    SMOKE_CHECK(sg_action_count() == g_sg_actions + 1, "the action was added (%d)", sg_action_count());
}
static void s_sg_saved(void) {
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, "Graphs/Patroller.lxgraph");
    char *t = lx_file_read(path, NULL);
    SMOKE_CHECK(t && strstr(t, "type = Translate"), "the clicked action was added and saved");
    lx_free(t);
    ge_undo();
    SMOKE_CHECK(sg_action_count() == g_sg_actions, "graph undo removes it again");
    ge_action_browser_toggle();
    UiNode *w[8];
    SMOKE_CHECK(ui_window_nodes(ED.ui, w, 8) == 0, "the Action Browser closes");
}
static void s_sg_play(void) { ed_play(); }
static void s_sg_playing(void) {
    LxEntity e = lx_entity_find(ED.scene, "Patroller");
    const char *st = e ? lx_fsm_current_state(ED.scene, e, 0) : "";
    SMOKE_CHECK(st[0] != 0, "the graph runs in play mode (%s)", st);
    SMOKE_CHECK(ge_run() != NULL, "the editor shows the running graph");
    sg_shot("state_graph_play.ppm");
}
static uint32_t g_sg_gen;
static void s_sg_load(void) { g_sg_gen = ED.scene_gen; lx_load_scene("Scenes/Main.lxscene"); }   /* what the Load Scene action does */
static void s_sg_loaded(void) {
    SMOKE_CHECK(ED.play != ED_EDIT && ED.scene_gen != g_sg_gen && lx_entity_find(ED.scene, "Patroller"), "Load Scene works in Play mode");
}
static void s_sg_stop(void) { ed_stop(); }
static void s_sg_stopped(void) {
    char err[200] = "";
    SMOKE_CHECK(ED.play == ED_EDIT && ed_graph_check(err, sizeof err), "edit mode again, graph still fine %s", err);
    if (ED.view_tabs) ED.view_tabs->tab_index = 0;
}

/* the game templates: the New Project dialog, then each template made, played and pictured */
static char g_tpl_dir[LX_PATH_MAX];
static void s_tpl_close(void) {
    char parent[LX_PATH_MAX];   /* the template projects go next to the smoke project */
    lx_path_parent(parent, sizeof parent, ED.project.root);
    lx_path_join(g_tpl_dir, sizeof g_tpl_dir, parent, "luxora_smoke_templates");
    lx_path_remove(g_tpl_dir);
    lx_mkdir_p(g_tpl_dir);
    ED.dirty = false;
    ED.in_frame = true;
    ed_request_close_project();
    ED.in_frame = false;
}
static void s_tpl_dialog(void) {
    UiNode *b = find_node(ui_root(ED.ui), NULL, "New Project");
    SMOKE_CHECK(ED.mode != ED_MODE_EDITOR && b && b->on_click, "the Project Manager has New Project");
    if (b && b->on_click) b->on_click(b);
}
static void s_tpl_pick(void) {
    UiNode *dlg = ui_modal_node(ED.ui);   /* the dialog floats over the root */
    UiNode *fps = find_label(dlg, "First Person Shooter"), *tps = find_label(dlg, "Third Person Action");
    SMOKE_CHECK(fps && tps, "the dialog offers the game templates");
    UiNode *card = fps ? fps->parent->parent : NULL;   /* label > top row > card */
    if (card && card->on_click) card->on_click(card);
    SMOKE_CHECK(card && card->selected, "the FPS card is selected");
}
static void s_tpl_dialog_shot(void) { sg_shot("new_project.ppm"); }
static void tpl_open(LxProjectTemplate tpl, const char *name) {
    ui_close_modal(ED.ui);
    LxProject p;
    bool ok = lx_project_create(&p, g_tpl_dir, name, tpl);
    SMOKE_CHECK(ok, "%s project created", name);
    if (!ok) return;
    ED.in_frame = true;
    ed_open_project(p.root);
    ED.in_frame = false;
}
static void s_tpl_fps(void) { tpl_open(LX_TEMPLATE_FPS, "Shooter"); }
static void s_tpl_tps(void) { tpl_open(LX_TEMPLATE_THIRD_PERSON, "Adventure"); }
static void s_tpl_play(void) {
    SMOKE_CHECK(ED.has_project && ED.scene && lx_entity_find(ED.scene, "Player"), "the template scene opens");
    ed_play();
}
static void tpl_playing(const char *shot, const char *first_state) {
    LxEntity p = lx_entity_find(ED.scene, "Player"), gm = lx_entity_find(ED.scene, "Game Manager");
    const char *ps = p ? lx_fsm_current_state(ED.scene, p, 0) : "", *gs = gm ? lx_fsm_current_state(ED.scene, gm, 0) : "";
    SMOKE_CHECK(!strcmp(ps, first_state) && !strcmp(gs, "Playing"), "the template plays (%s, %s)", ps, gs);
    SMOKE_CHECK(lx_mouse_lock_wanted(), "the game asks for the mouse");
    sg_shot(shot);
}
static void s_tpl_fps_playing(void) { tpl_playing("fps_template.ppm", "Playing"); }
static void s_tpl_tps_playing(void) { tpl_playing("tps_template.ppm", "Playing"); }
static void s_tpl_stop(void) {
    ED.in_frame = true;
    ed_stop();
    ED.in_frame = false;
}
static void s_tpl_stopped(void) {
    SMOKE_CHECK(ED.play == ED_EDIT && !lx_mouse_locked() && lx_mouse_shown(), "stopping gives the mouse back");
}

static const struct { int frame; SmokeFn fn; const char *what; } STEPS[] = {
    {2, s_mat_select, "select a material asset"},
    {3, s_mat_capture, "find material widgets"},
    {6, s_mat_edit, "edit roughness through the slider"},
    {9, s_mat_verify, "verify the material file"},
    {10, s_make_editable, "select the cube"},
    {12, s_make_editable_click, "make its built-in material editable"},
    {15, s_make_editable_verify, "verify the new material"},
    {16, s_asset_none, "back to entities"},
    {17, s_create_all, "create every object type"},
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
    {63, s_set5, "project settings: physics"},
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
    {129, s_script_compiled, "scripts compiled"},
    {130, s_physics_objects, "create physics objects"},
    {131, s_script_attach, "attach the sample script behaviour"},
    {132, s_play, "play"},
    {149, s_physics_fell, "physics runs in play mode"},
    {149, s_script_ran, "the script runs in play mode"},
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
    {204, s_game_icon, "set a custom game icon"},
    {205, s_close, "close build settings"},
    {206, s_game_edit, "game view in edit mode"},
    {209, s_game_back, "scene view"},
    {211, s_game_play, "play switches to game view"},
    {214, s_game_stop, "stop restores scene view"},
    {214, s_collapse, "select the cube"},
    {215, s_collapse_click, "collapse and expand a component"},
    {216, s_close_project, "close project"},
    {219, s_closed, "back at the project manager"},
    {222, s_reopen_project, "reopen the project"},
    {226, s_reopened, "project is open again"},
    {228, s_quit_prompt, "close the window with unsaved changes"},
    {229, s_prompt_shot, "screenshot of the save prompt"},
    {231, s_quit_cancel, "cancel the save prompt"},
    {233, s_parallax_view, "look at the cobblestone path"},
    {238, s_parallax_shot, "screenshot of parallax mapping"},
    {239, s_material_view, "inspect the cobblestone material"},
    {243, s_material_shot, "screenshot of the material inspector"},
    {245, s_prefab_make, "save an object as a prefab"},
    {248, s_prefab_place, "place prefab copies, edit one and apply"},
    {251, s_prefab_applied, "every copy updated"},
    {255, s_prefab_shot, "screenshot of prefabs"},
    {256, s_model_import, "import two Mixamo characters as humanoids"},
    {260, s_model_inspector_shot, "screenshot of the model inspector, open the Avatar window"},
    {264, s_avatar_shot, "screenshot of the Avatar window"},
    {266, s_model_place, "place a character and preview another model's dance on it"},
    {270, s_model_anim_shot, "screenshot of the retargeted dance"},
    {272, s_model_reset, "stop the preview: back to the rest pose"},
    {273, s_model_play, "play mode with an animated character"},
    {283, s_model_playing, "the character animates in play mode"},
    {285, s_model_stop, "press Stop (mid-frame, like the toolbar button)"},
    {290, s_model_stopped, "edit mode again after Stop"},
    {292, s_big_make, "make a folder of 4K textures"},
    {294, s_big_open, "open it in the Content Browser"},
    {300, s_sg_open, "open the State Graph editor"},
    {303, s_sg_browser, "open the Action Browser"},
    {305, s_sg_search, "search for an action"},
    {307, s_sg_clear, "clear the search"},
    {309, s_sg_shot, "screenshot of the State Graph editor"},
    {310, s_sg_pick, "click an action in the list"},
    {311, s_sg_picked, "it's highlighted"},
    {311, s_sg_add, "add the action to the selected state"},
    {313, s_sg_saved, "saved; undo; close the browser"},
    {314, s_sg_play, "play"},
    {330, s_sg_playing, "the State Graph runs"},
    {331, s_sg_load, "the game loads a scene"},
    {333, s_sg_loaded, "the new scene plays"},
    {335, s_sg_stop, "stop"},
    {337, s_sg_stopped, "back in edit mode"},
};

/* the template steps start once everything above is finished (the 4K thumbnails can take a while) */
static const struct { int frame; SmokeFn fn; const char *what; } TPL_STEPS[] = {
    {0, s_tpl_close, "close the project"},
    {4, s_tpl_dialog, "New Project"},
    {8, s_tpl_pick, "pick the First Person Shooter template"},
    {12, s_tpl_dialog_shot, "screenshot of the New Project dialog"},
    {14, s_tpl_fps, "create and open the FPS template"},
    {20, s_tpl_play, "play the FPS template"},
    {84, s_tpl_fps_playing, "the FPS template runs"},
    {86, s_tpl_stop, "stop"},
    {90, s_tpl_stopped, "edit mode, mouse free"},
    {92, s_tpl_tps, "create and open the Third Person template"},
    {98, s_tpl_play, "play the Third Person template"},
    {164, s_tpl_tps_playing, "the Third Person template runs"},
    {166, s_tpl_stop, "stop"},
    {170, s_tpl_stopped, "edit mode, mouse free"},
};
static int g_tpl_base = -1;

bool ed_smoke_step(int frame) {
    double now = lx_time();
    if (g_frame_prev > 0 && frame > 5) g_frame_times[g_frame_n++ % 32] = now - g_frame_prev;
    g_frame_prev = now;
    big_watch();
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++)
        if (STEPS[i].frame == frame) {
            LX_INFO("[smoke] %s", STEPS[i].what);
            if (ED.mode == ED_MODE_EDITOR || frame >= 216) STEPS[i].fn(); /* the last steps run in the Project Manager */
        }
    if (frame < 340 || (g_big_last && !g_big_done)) return true;
    if (g_tpl_base < 0) g_tpl_base = frame + 1;
    for (size_t i = 0; i < sizeof TPL_STEPS / sizeof TPL_STEPS[0]; i++)
        if (TPL_STEPS[i].frame == frame - g_tpl_base) {
            LX_INFO("[smoke] %s", TPL_STEPS[i].what);
            TPL_STEPS[i].fn();
        }
    return frame - g_tpl_base < 172;
}
