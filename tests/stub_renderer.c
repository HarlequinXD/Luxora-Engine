/* Stub renderer for running the editor's UI and logic without a GPU (local smoke tests).
 * Implements the renderer API as no-ops while still exercising scene culling inputs. */
#include "lx_internal.h"
#include <stdio.h>
#include <math.h>

static LxGraphicsSettings g_settings;
static LxRenderStats g_stats;
static uint32_t g_next_tex = 2;   /* 1 = white */
struct LxRenderTarget { int w, h; uint32_t tex; };

bool lx_renderer_init(void) { g_settings = lx_graphics_defaults(2); lx_gi_init(); return true; }
void lx_renderer_shutdown(void) { lx_gi_shutdown(); }
void lx_renderer_wait_idle(void) {}
LxGraphicsSettings *lx_graphics_settings(void) { if (!g_settings.shadow_resolution) g_settings = lx_graphics_defaults(2); return &g_settings; }
const LxRenderStats *lx_render_stats(void) { return &g_stats; }
void lx_renderer_set_vsync(bool on) { LX_UNUSED(on); }
bool lx_renderer_begin_frame(void) { int w, h; lx_window_size(&w, &h); return w > 0 && h > 0; }
void lx_renderer_end_frame(void) {}
int lx_renderer_validation_errors(void) { return 0; }
void lx_renderer_mesh_release(LxMesh *m) { LX_UNUSED(m); }
void lx_renderer_texture_release(LxTexture *t) { LX_UNUSED(t); }
uint32_t lx_ui_white_texture(void) { return 1; }
/* ---- a small software rasterizer for the UI, so --screenshot works without a GPU ---- */
typedef struct SwTex { uint32_t id; int w, h, ch; uint8_t *px; } SwTex;
static SwTex g_tex[512];
static int g_tex_count;
static SwTex g_font;
static LxUiVertex *g_v; static int g_vn, g_vcap;
static uint32_t *g_i; static int g_in, g_icap;
static LxUiCmd *g_c; static int g_cn, g_ccap;
static LxVec4 g_clear;

void lx_renderer_set_font_atlas(const uint8_t *a, int w, int h) {
    lx_free(g_font.px);
    g_font.px = lx_alloc((size_t)w * (size_t)h);
    memcpy(g_font.px, a, (size_t)w * (size_t)h);
    g_font.w = w; g_font.h = h; g_font.ch = 1;
}
uint32_t lx_ui_texture_from_rgba(const uint8_t *p, int w, int h) {
    uint32_t id = g_next_tex++;
    if (g_tex_count < 512 && p && w > 0 && h > 0 && (size_t)w * (size_t)h <= 4096u * 4096u) {
        SwTex *t = &g_tex[g_tex_count++];
        t->id = id; t->w = w; t->h = h; t->ch = 4;
        t->px = lx_alloc((size_t)w * (size_t)h * 4);
        memcpy(t->px, p, (size_t)w * (size_t)h * 4);
    }
    return id;
}
void lx_ui_texture_free(uint32_t id) {
    for (int i = 0; i < g_tex_count; i++)
        if (g_tex[i].id == id) { lx_free(g_tex[i].px); g_tex[i] = g_tex[--g_tex_count]; return; }
}
void lx_renderer_draw_ui(const LxUiDrawList *l) {
    /* validate the draw list the toolkit produced */
    for (int i = 0; i < l->cmd_count; i++)
        if (l->cmds[i].index_offset + l->cmds[i].index_count > (uint32_t)l->index_count) { fprintf(stderr, "bad UI draw command\n"); abort(); }
    for (int i = 0; i < l->index_count; i++)
        if (l->indices[i] >= (uint32_t)l->vertex_count) { fprintf(stderr, "bad UI index\n"); abort(); }
    /* keep it for a screenshot */
    if (l->vertex_count > g_vcap) { g_vcap = l->vertex_count; g_v = lx_realloc(g_v, sizeof *g_v * (size_t)g_vcap); }
    if (l->index_count > g_icap) { g_icap = l->index_count; g_i = lx_realloc(g_i, sizeof *g_i * (size_t)g_icap); }
    if (l->cmd_count > g_ccap) { g_ccap = l->cmd_count; g_c = lx_realloc(g_c, sizeof *g_c * (size_t)g_ccap); }
    if (l->vertex_count) memcpy(g_v, l->vertices, sizeof *g_v * (size_t)l->vertex_count);
    if (l->index_count) memcpy(g_i, l->indices, sizeof *g_i * (size_t)l->index_count);
    if (l->cmd_count) memcpy(g_c, l->cmds, sizeof *g_c * (size_t)l->cmd_count);
    g_vn = l->vertex_count; g_in = l->index_count; g_cn = l->cmd_count;
    g_clear = l->clear_color;
}
static void sample(const SwTex *t, float u, float v, float out[4]) {
    if (!t || !t->px) { out[0] = out[1] = out[2] = out[3] = 1; return; }
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    float acc[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; k++) {
        int xx = x0 + (k & 1), yy = y0 + (k >> 1);
        xx = xx < 0 ? 0 : xx >= t->w ? t->w - 1 : xx;
        yy = yy < 0 ? 0 : yy >= t->h ? t->h - 1 : yy;
        float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
        const uint8_t *p = t->px + ((size_t)yy * (size_t)t->w + (size_t)xx) * (size_t)t->ch;
        for (int c = 0; c < 4; c++) acc[c] += wgt * (t->ch == 1 ? (float)p[0] : (float)p[c]) / 255.0f;
    }
    memcpy(out, acc, sizeof acc);
}
float *lx_renderer_gi_readback(int res[3]) { (void)res; return NULL; }   /* photon GI runs on the CPU here */
float *lx_renderer_gi_readback_level(int level, int res[3], LxVec3 *origin, float *cell) { (void)level; (void)res; (void)origin; (void)cell; return NULL; }
bool lx_renderer_screenshot(const char *path) {
    int W, H;
    lx_window_size(&W, &H);
    if (W <= 0 || H <= 0) return false;
    float *fb = lx_alloc(sizeof(float) * 3 * (size_t)W * (size_t)H);
    for (int i = 0; i < W * H; i++) { fb[i * 3] = g_clear.x; fb[i * 3 + 1] = g_clear.y; fb[i * 3 + 2] = g_clear.z; }
    for (int c = 0; c < g_cn; c++) {
        const LxUiCmd *cmd = &g_c[c];
        const SwTex *tex = NULL;
        bool font = cmd->texture == 0;
        if (font) tex = &g_font;
        else for (int k = 0; k < g_tex_count; k++) if (g_tex[k].id == cmd->texture) tex = &g_tex[k];
        int cx0 = (int)lx_maxf(0, cmd->clip[0]), cy0 = (int)lx_maxf(0, cmd->clip[1]);
        int cx1 = (int)lx_minf((float)W, cmd->clip[0] + cmd->clip[2]), cy1 = (int)lx_minf((float)H, cmd->clip[1] + cmd->clip[3]);
        for (uint32_t t = 0; t + 2 < cmd->index_count; t += 3) {
            const LxUiVertex *a = &g_v[g_i[cmd->index_offset + t]], *b = &g_v[g_i[cmd->index_offset + t + 1]], *d = &g_v[g_i[cmd->index_offset + t + 2]];
            float area = (b->x - a->x) * (d->y - a->y) - (b->y - a->y) * (d->x - a->x);
            if (fabsf(area) < 1e-6f) continue;
            int x0 = (int)floorf(lx_minf(a->x, lx_minf(b->x, d->x))), x1 = (int)ceilf(lx_maxf(a->x, lx_maxf(b->x, d->x)));
            int y0 = (int)floorf(lx_minf(a->y, lx_minf(b->y, d->y))), y1 = (int)ceilf(lx_maxf(a->y, lx_maxf(b->y, d->y)));
            x0 = x0 < cx0 ? cx0 : x0;
            y0 = y0 < cy0 ? cy0 : y0;
            x1 = x1 > cx1 ? cx1 : x1;
            y1 = y1 > cy1 ? cy1 : y1;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    float px = (float)x + 0.5f, py = (float)y + 0.5f;
                    float w0 = ((b->x - px) * (d->y - py) - (b->y - py) * (d->x - px)) / area;
                    float w1 = ((d->x - px) * (a->y - py) - (d->y - py) * (a->x - px)) / area;
                    float w2 = 1 - w0 - w1;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                    float u = w0 * a->u + w1 * b->u + w2 * d->u, v = w0 * a->v + w1 * b->v + w2 * d->v;
                    float col[4];
                    const LxUiVertex *vs[3] = {a, b, d};
                    float ws[3] = {w0, w1, w2};
                    for (int k = 0; k < 4; k++) {
                        col[k] = 0;
                        for (int q = 0; q < 3; q++) col[k] += ws[q] * (float)((vs[q]->color >> (8 * k)) & 255) / 255.0f;
                    }
                    float tc[4];
                    sample(tex, u, v, tc);
                    float alpha = font ? col[3] * tc[0] : col[3] * tc[3];
                    float rgb[3] = {font ? col[0] : col[0] * tc[0], font ? col[1] : col[1] * tc[1], font ? col[2] : col[2] * tc[2]};
                    float *dst = &fb[((size_t)y * (size_t)W + (size_t)x) * 3];
                    for (int k = 0; k < 3; k++) dst[k] = dst[k] * (1 - alpha) + rgb[k] * alpha;
                }
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { lx_free(fb); return false; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    uint8_t *row = lx_alloc((size_t)W * 3);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W * 3; x++) row[x] = (uint8_t)(lx_clampf(fb[(size_t)y * (size_t)W * 3 + (size_t)x], 0, 1) * 255.0f + 0.5f);
        fwrite(row, 1, (size_t)W * 3, f);
    }
    fclose(f);
    lx_free(row);
    lx_free(fb);
    return true;
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

void lx_reflections_refresh(void) {}
