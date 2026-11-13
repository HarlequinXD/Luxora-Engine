/* Stub renderer for running the editor's UI and logic without a GPU (local smoke tests).
 * Implements the renderer API as no-ops while still exercising scene culling inputs. */
#include "lx_internal.h"

static LxGraphicsSettings g_settings;
static LxRenderStats g_stats;
static uint32_t g_next_tex = 1;
struct LxRenderTarget { int w, h; uint32_t tex; };

bool lx_renderer_init(void) { g_settings = lx_graphics_defaults(2); lx_gi_init(); return true; }
void lx_renderer_shutdown(void) { lx_gi_shutdown(); }
void lx_renderer_wait_idle(void) {}
LxGraphicsSettings *lx_graphics_settings(void) { if (!g_settings.shadow_resolution) g_settings = lx_graphics_defaults(2); return &g_settings; }
const LxRenderStats *lx_render_stats(void) { return &g_stats; }
void lx_renderer_set_vsync(bool on) { LX_UNUSED(on); }
bool lx_renderer_begin_frame(void) { int w, h; lx_window_size(&w, &h); return w > 0 && h > 0; }
void lx_renderer_end_frame(void) {}
bool lx_renderer_screenshot(const char *p) { LX_UNUSED(p); return false; }
int lx_renderer_validation_errors(void) { return 0; }
void lx_renderer_mesh_release(LxMesh *m) { LX_UNUSED(m); }
void lx_renderer_texture_release(LxTexture *t) { LX_UNUSED(t); }
void lx_renderer_set_font_atlas(const uint8_t *a, int w, int h) { LX_UNUSED(a); LX_UNUSED(w); LX_UNUSED(h); }
uint32_t lx_ui_texture_from_rgba(const uint8_t *p, int w, int h) { LX_UNUSED(p); LX_UNUSED(w); LX_UNUSED(h); return g_next_tex++; }
void lx_ui_texture_free(uint32_t id) { LX_UNUSED(id); }
uint32_t lx_ui_white_texture(void) { return 1; }
void lx_renderer_draw_ui(const LxUiDrawList *l) {
    /* validate the draw list the toolkit produced */
    for (int i = 0; i < l->cmd_count; i++)
        if (l->cmds[i].index_offset + l->cmds[i].index_count > (uint32_t)l->index_count) { fprintf(stderr, "bad UI draw command\n"); abort(); }
    for (int i = 0; i < l->index_count; i++)
        if (l->indices[i] >= (uint32_t)l->vertex_count) { fprintf(stderr, "bad UI index\n"); abort(); }
}
LxRenderTarget *lx_render_target_create(int w, int h) { LxRenderTarget *r = lx_calloc(1, sizeof *r); r->w = w; r->h = h; r->tex = g_next_tex++; return r; }
void lx_render_target_resize(LxRenderTarget *rt, int w, int h) { rt->w = w; rt->h = h; }
void lx_render_target_destroy(LxRenderTarget *rt) { lx_free(rt); }
uint32_t lx_render_target_texture(const LxRenderTarget *rt) { return rt ? rt->tex : 0; }
void lx_render_scene(LxScene *s, const LxRenderView *v, LxRenderTarget *t) {
    LX_UNUSED(v); LX_UNUSED(t);
    lx_scene_update_transforms(s);
    lx_gi_update(s, &g_settings); /* exercise the photon tracer with the real scene */
    g_stats.gi_photons_traced = lx_gi_last_photon_count();
    g_stats.gi_ms = lx_gi_last_ms();
}
bool lx_camera_view(LxScene *s, LxEntity cam, int w, int h, LxRenderView *out) {
    LxEntityData *d = lx_entity_data(s, cam);
    if (!d || !(d->comps & LX_COMP_CAMERA)) return false;
    memset(out, 0, sizeof *out);
    LxMat4 world = lx_entity_world_matrix(s, cam);
    out->view = lx_mat4_inverse(world);
    out->proj = lx_mat4_perspective(d->camera.fov_deg * LX_DEG2RAD, h > 0 ? (float)w / (float)h : 1, d->camera.near_clip, d->camera.far_clip);
    out->eye = lx_v3(world.m[12], world.m[13], world.m[14]);
    out->width = w;
    out->height = h;
    return true;
}
