/* Game UI: an immediate-mode HUD/menu layer for games.
 *
 * Calls made during a frame append textured triangles in canvas units (the screen scaled to a
 * reference height, 1080 by default). The host maps the canvas onto a viewport: the player onto
 * the whole window, the editor onto the viewport while Play is running. Text uses the same baked
 * Inter / DejaVu atlas as the editor (UI texture 0); its white texel draws solid shapes, so most
 * frames are a single draw call.
 */
#include "../lx_internal.h"
#include "../gfx/font_atlas.h"
#include <stdarg.h>

void lx_font_atlas_get(bool hi, LxFontAtlas *o) {
    o->png = hi ? lx_font_png_2x : lx_font_png_1x;
    o->png_size = hi ? sizeof lx_font_png_2x : sizeof lx_font_png_1x;
    o->glyphs = hi ? lx_glyphs_2x : lx_glyphs_1x;
    o->glyph_count = hi ? LX_GLYPH_COUNT_2X : LX_GLYPH_COUNT_1X;
    o->fonts = hi ? lx_fonts_2x : lx_fonts_1x;
    o->scale = hi ? 2.0f : 1.0f;
}

enum { F_BODY, F_BOLD, F_SMALL, F_TITLE, F_MONO };
static const float FONT_PX[LX_FONT_COUNT] = {14, 14, 12, 22, 13};   /* sizes the atlas was baked at (1x) */

typedef struct Glyph { float u0, v0, u1, v1, w, h, xoff, yoff, adv; bool valid; } Glyph;

static struct {
    bool atlas_known, editor_atlas, player_atlas_loaded;
    float atlas_w, atlas_h, atlas_scale, white_u, white_v;
    Glyph ascii[LX_FONT_COUNT][128];
    uint32_t extra_cp[LX_FONT_COUNT][32];
    Glyph extra[LX_FONT_COUNT][32];
    int extra_count[LX_FONT_COUNT];
    float ascent[LX_FONT_COUNT];   /* atlas px */
    float line[LX_FONT_COUNT];
    /* frame state */
    float ref_h;
    LxUiAnchor anchor;
    LxUiFont font;
    float vx, vy, vw, vh, scale;
    LxUiVertex *v;
    int vc, vcap;
    uint32_t *idx;
    int ic, icap;
    LxUiCmd *cmds;
    int cc, ccap;
    LxUiVertex *wv;
    int wvcap;
    LxUiCmd *wcmds;
    int wccap;
    /* pointer */
    LxVec2 pointer;
    bool down, prev_down, pressed, released;
    bool over_button, over_button_prev;
    bool cursor_drawn;
    uint64_t active_id;
    /* image textures */
    struct { char ref[LX_REF_MAX]; uint32_t tex; } tex[64];
    int ntex;
} G;

/* ---------------- atlas ---------------- */
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void load_glyphs(bool hi) {
    LxFontAtlas fa;
    lx_font_atlas_get(hi, &fa);
    G.atlas_w = (float)be32(fa.png + 16);   /* PNG IHDR width/height */
    G.atlas_h = (float)be32(fa.png + 20);
    G.atlas_scale = fa.scale;
    G.white_u = 1.5f / G.atlas_w;
    G.white_v = 1.5f / G.atlas_h;
    memset(G.ascii, 0, sizeof G.ascii);
    memset(G.extra_count, 0, sizeof G.extra_count);
    for (int f = 0; f < LX_FONT_COUNT; f++) { G.ascent[f] = fa.fonts[f].ascent; G.line[f] = fa.fonts[f].line_height; }
    for (int i = 0; i < fa.glyph_count; i++) {
        const LxBakedGlyph *g = &fa.glyphs[i];
        Glyph r = {(float)g->x / G.atlas_w, (float)g->y / G.atlas_h, (float)(g->x + g->w) / G.atlas_w, (float)(g->y + g->h) / G.atlas_h,
                   (float)g->w, (float)g->h, (float)g->xoff, (float)g->yoff, g->advance, true};
        if (g->font >= LX_FONT_COUNT) continue;
        if (g->cp < 128) G.ascii[g->font][g->cp] = r;
        else if (G.extra_count[g->font] < 32) {
            int k = G.extra_count[g->font]++;
            G.extra_cp[g->font][k] = g->cp;
            G.extra[g->font][k] = r;
        }
    }
    G.atlas_known = true;
}

void lx_game_ui_use_atlas(bool hi) {
    G.editor_atlas = true;
    load_glyphs(hi);
}

static const Glyph *glyph(int f, uint32_t cp) {
    if (cp < 128 && G.ascii[f][cp].valid) return &G.ascii[f][cp];
    for (int i = 0; i < G.extra_count[f]; i++) if (G.extra_cp[f][i] == cp) return &G.extra[f][i];
    return &G.ascii[f]['?'];
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p;
    int len = 1;
    if (c >= 0xf0) { c &= 0x07; len = 4; }
    else if (c >= 0xe0) { c &= 0x0f; len = 3; }
    else if (c >= 0xc0) { c &= 0x1f; len = 2; }
    for (int i = 1; i < len && p[i]; i++) c = (c << 6) | (p[i] & 0x3f);
    *s += len;
    return c;
}

/* ---------------- geometry ---------------- */
static void begin_cmd(uint32_t tex) {
    LxUiCmd *last = G.cc ? &G.cmds[G.cc - 1] : NULL;
    if (last && last->texture == tex) return;
    if (last && last->index_count == 0) { last->texture = tex; return; }
    LxUiCmd c = {tex, (uint32_t)G.ic, 0, {0, 0, 0, 0}};
    LX_VEC_PUSH(G.cmds, G.cc, G.ccap, c);
}

static uint32_t vert(float x, float y, float u, float v, uint32_t col) {
    LxUiVertex q = {x, y, u, v, col};
    LX_VEC_PUSH(G.v, G.vc, G.vcap, q);
    return (uint32_t)(G.vc - 1);
}
static void tri(uint32_t a, uint32_t b, uint32_t c) {
    LX_VEC_PUSH(G.idx, G.ic, G.icap, a);
    LX_VEC_PUSH(G.idx, G.ic, G.icap, b);
    LX_VEC_PUSH(G.idx, G.ic, G.icap, c);
    G.cmds[G.cc - 1].index_count += 3;
}
static void quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col, uint32_t tex) {
    begin_cmd(tex);
    uint32_t a = vert(x0, y0, u0, v0, col), b = vert(x1, y0, u1, v0, col), c = vert(x1, y1, u1, v1, col), d = vert(x0, y1, u0, v1, col);
    tri(a, b, c);
    tri(a, c, d);
}
static void solid_quad(float x0, float y0, float x1, float y1, uint32_t col) { quad_uv(x0, y0, x1, y1, G.white_u, G.white_v, G.white_u, G.white_v, col, 0); }

/* outline points of a rounded rectangle, clockwise from the top-left corner */
static int rounded_points(float x, float y, float w, float h, float r, LxVec2 *out) {
    r = lx_clampf(r, 0, lx_minf(w, h) * 0.5f);
    const int seg = 6;
    float cx[4] = {x + r, x + w - r, x + w - r, x + r}, cy[4] = {y + r, y + r, y + h - r, y + h - r};
    float start[4] = {LX_PI, 1.5f * LX_PI, 0, 0.5f * LX_PI};
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= seg; i++) {
            float a = start[c] + (float)i / seg * 0.5f * LX_PI;
            out[n++] = lx_v2(cx[c] + cosf(a) * r, cy[c] + sinf(a) * r);
        }
    return n;
}

static void anchor_point(float w, float h, float x, float y, float *ox, float *oy) {
    float ax = (float)((int)G.anchor % 3) * 0.5f, ay = (float)((int)G.anchor / 3) * 0.5f;
    LxVec2 cs = lx_ui_canvas_size();
    *ox = ax * cs.x + x - ax * w;
    *oy = ay * cs.y + y - ay * h;
}
static void anchor_origin(float x, float y, float *ox, float *oy) {
    float ax = (float)((int)G.anchor % 3) * 0.5f, ay = (float)((int)G.anchor / 3) * 0.5f;
    LxVec2 cs = lx_ui_canvas_size();
    *ox = ax * cs.x + x;
    *oy = ay * cs.y + y;
}

static bool ready(void) { return G.atlas_known && G.vw > 0 && G.vh > 0; }

/* ---------------- public API ---------------- */
LxVec2 lx_ui_canvas_size(void) {
    float rh = G.ref_h > 1 ? G.ref_h : 1080.0f;
    float aspect = G.vh > 0 ? G.vw / G.vh : 16.0f / 9.0f;
    return lx_v2(rh * aspect, rh);
}
void lx_ui_set_reference_height(float units) { G.ref_h = units > 1 ? units : 1080.0f; }
void lx_ui_anchor(LxUiAnchor a) { G.anchor = a; }
void lx_ui_font(LxUiFont f) { G.font = f; }

static void fill_rounded(float x, float y, float w, float h, uint32_t color, float radius) {
    if (radius <= 0.5f) { solid_quad(x, y, x + w, y + h, color); return; }
    LxVec2 pts[32];
    int n = rounded_points(x, y, w, h, radius, pts);
    begin_cmd(0);
    uint32_t c = vert(x + w * 0.5f, y + h * 0.5f, G.white_u, G.white_v, color);
    uint32_t first = (uint32_t)G.vc;
    for (int i = 0; i < n; i++) vert(pts[i].x, pts[i].y, G.white_u, G.white_v, color);
    for (int i = 0; i < n; i++) tri(c, first + (uint32_t)i, first + (uint32_t)((i + 1) % n));
}

void lx_ui_rect(float x, float y, float w, float h, uint32_t color, float radius) {
    if (!ready() || w <= 0 || h <= 0) return;
    float px, py;
    anchor_point(w, h, x, y, &px, &py);
    fill_rounded(px, py, w, h, color, radius);
}

void lx_ui_rect_outline(float x, float y, float w, float h, uint32_t color, float radius, float t) {
    if (!ready() || w <= 0 || h <= 0 || t <= 0) return;
    float px, py;
    anchor_point(w, h, x, y, &px, &py);
    LxVec2 outer[32], inner[32];
    int n = rounded_points(px, py, w, h, radius, outer);
    rounded_points(px + t, py + t, w - 2 * t, h - 2 * t, lx_maxf(radius - t, 0), inner);
    begin_cmd(0);
    uint32_t base = (uint32_t)G.vc;
    for (int i = 0; i < n; i++) { vert(outer[i].x, outer[i].y, G.white_u, G.white_v, color); vert(inner[i].x, inner[i].y, G.white_u, G.white_v, color); }
    for (int i = 0; i < n; i++) {
        uint32_t o0 = base + (uint32_t)(2 * i), i0 = o0 + 1, o1 = base + (uint32_t)(2 * ((i + 1) % n)), i1 = o1 + 1;
        tri(o0, o1, i1);
        tri(o0, i1, i0);
    }
}

void lx_ui_circle(float x, float y, float r, uint32_t color) {
    if (!ready() || r <= 0) return;
    float cx, cy;
    anchor_origin(x, y, &cx, &cy);
    begin_cmd(0);
    uint32_t c = vert(cx, cy, G.white_u, G.white_v, color), first = (uint32_t)G.vc;
    const int seg = 40;
    for (int i = 0; i < seg; i++) {
        float a = (float)i / seg * 2 * LX_PI;
        vert(cx + cosf(a) * r, cy + sinf(a) * r, G.white_u, G.white_v, color);
    }
    for (int i = 0; i < seg; i++) tri(c, first + (uint32_t)i, first + (uint32_t)((i + 1) % seg));
}

void lx_ui_line(float x0, float y0, float x1, float y1, float t, uint32_t color) {
    if (!ready()) return;
    float ax, ay, bx, by;
    anchor_origin(x0, y0, &ax, &ay);
    anchor_origin(x1, y1, &bx, &by);
    float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    begin_cmd(0);
    uint32_t a = vert(ax + nx, ay + ny, G.white_u, G.white_v, color), b = vert(bx + nx, by + ny, G.white_u, G.white_v, color);
    uint32_t c = vert(bx - nx, by - ny, G.white_u, G.white_v, color), d = vert(ax - nx, ay - ny, G.white_u, G.white_v, color);
    tri(a, b, c);
    tri(a, c, d);
}

static int pick_font(float size) {
    if (G.font == LX_FONT_MONO) return F_MONO;
    if (size > 18.0f) return F_TITLE;   /* the large baked font keeps big text sharp */
    return G.font == LX_FONT_BOLD ? F_BOLD : F_BODY;
}
static float font_k(int f, float size) { return size / (FONT_PX[f] * G.atlas_scale); }   /* atlas px -> canvas units */

float lx_ui_text_width(float size, const char *t) {
    if (!G.atlas_known || !t) return 0;
    int f = pick_font(size);
    float k = font_k(f, size), w = 0;
    while (*t) w += glyph(f, utf8_next(&t))->adv * k;
    return w;
}

static void text_at(float x, float y, float size, uint32_t color, const char *t) {
    int f = pick_font(size);
    float k = font_k(f, size), pen = x;
    while (*t) {
        uint32_t cp = utf8_next(&t);
        if (cp == '\n') { pen = x; y += G.line[f] * k; continue; }
        const Glyph *g = glyph(f, cp);
        if (g->w > 0) {
            float gx = pen + g->xoff * k, gy = y + g->yoff * k;
            quad_uv(gx, gy, gx + g->w * k, gy + g->h * k, g->u0, g->v0, g->u1, g->v1, color, 0);
        }
        pen += g->adv * k;
    }
}

void lx_ui_text(float x, float y, float size, uint32_t color, const char *fmt, ...) {
    if (!ready() || !fmt) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int f = pick_font(size);
    /* measure the widest line and the block height so anchors align the whole block */
    float w = 0, lw = 0, k = font_k(f, size);
    int lines = 1;
    for (const char *p = buf; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp == '\n') { w = lx_maxf(w, lw); lw = 0; lines++; continue; }
        lw += glyph(f, cp)->adv * k;
    }
    w = lx_maxf(w, lw);
    float h = G.line[f] * k * (float)lines, px, py;
    anchor_point(w, h, x, y, &px, &py);
    text_at(px, py, size, color, buf);
}

static uint32_t image_texture(const char *ref) {
    for (int i = 0; i < G.ntex; i++) if (!strcmp(G.tex[i].ref, ref)) return G.tex[i].tex;
    if (G.ntex >= 64) return 0;
    LxTexture *t = lx_asset_texture(ref);
    uint32_t id = (t && t->pixels) ? lx_ui_texture_from_rgba(t->pixels, t->width, t->height) : 0;
    lx_strcpy(G.tex[G.ntex].ref, LX_REF_MAX, ref);
    G.tex[G.ntex++].tex = id;
    return id;
}

void lx_ui_image(const char *ref, float x, float y, float w, float h, uint32_t tint) {
    if (!ready() || !ref || !ref[0]) return;
    uint32_t tex = image_texture(ref);
    if (!tex) return;
    float px, py;
    anchor_point(w, h, x, y, &px, &py);
    quad_uv(px, py, px + w, py + h, 0, 0, 1, 1, tint, tex);
}

void lx_ui_bar(float x, float y, float w, float h, float value, uint32_t fill, uint32_t background) {
    if (!ready()) return;
    float px, py;
    anchor_point(w, h, x, y, &px, &py);
    float r = lx_minf(h * 0.5f, 6.0f);
    fill_rounded(px, py, w, h, background, r);
    float fw = w * lx_clampf(value, 0, 1);
    if (fw > 0.5f) fill_rounded(px, py, fw, h, fill, lx_minf(r, fw * 0.5f));
}

static uint64_t hash_id(const char *label, float x, float y) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = label; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ull; }
    int32_t ix = (int32_t)(x * 4), iy = (int32_t)(y * 4);
    h ^= (uint64_t)(uint32_t)ix * 0x9e3779b97f4a7c15ull;
    h ^= (uint64_t)(uint32_t)iy * 0xc2b2ae3d27d4eb4full;
    return h | 1;
}

bool lx_ui_button(const char *label, float x, float y, float w, float h) {
    if (!ready() || !label) return false;
    float px, py;
    anchor_point(w, h, x, y, &px, &py);
    uint64_t id = hash_id(label, px, py);
    bool hover = G.pointer.x >= px && G.pointer.y >= py && G.pointer.x < px + w && G.pointer.y < py + h;
    if (hover) G.over_button = true;
    if (hover && G.pressed) G.active_id = id;
    bool held = G.down && G.active_id == id;
    bool clicked = G.released && G.active_id == id && hover;
    uint32_t bg = held ? LX_RGBA(22, 24, 30, 240) : hover ? LX_RGBA(58, 64, 78, 235) : LX_RGBA(32, 35, 43, 220);
    float r = lx_minf(10.0f, h * 0.3f);
    fill_rounded(px, py, w, h, bg, r);
    LxUiAnchor keep = G.anchor;
    G.anchor = LX_ANCHOR_TOP_LEFT;
    lx_ui_rect_outline(px, py, w, h, hover ? LX_RGBA(120, 160, 255, 200) : LX_RGBA(255, 255, 255, 40), r, 2.0f);
    float size = lx_clampf(h * 0.42f, 12, 64);
    LxUiFont kf = G.font;
    if (G.font == LX_FONT_REGULAR) G.font = LX_FONT_BOLD;
    float tw = lx_ui_text_width(size, label);
    int f = pick_font(size);
    float th = G.line[f] * font_k(f, size);
    text_at(px + (w - tw) * 0.5f, py + (h - th) * 0.5f + (held ? 1.5f : 0), size, LX_COLOR_WHITE, label);
    G.font = kf;
    G.anchor = keep;
    return clicked;
}

LxVec2 lx_ui_pointer(void) { return G.pointer; }
bool lx_ui_pointer_over(void) { return G.over_button || G.over_button_prev; }

bool lx_ui_world_to_canvas(LxScene *s, LxVec3 world, LxVec2 *out) {
    LxEntity cam = s ? lx_scene_main_camera(s) : 0;
    if (!cam || G.vw <= 0 || G.vh <= 0) return false;
    LxRenderView v;
    if (!lx_camera_view(s, cam, (int)G.vw, (int)G.vh, &v)) return false;
    LxMat4 vp = lx_mat4_mul(v.proj, v.view);
    float cx = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8] * world.z + vp.m[12];
    float cy = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9] * world.z + vp.m[13];
    float cw = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];
    if (cw <= 1e-4f) return false;   /* behind the camera */
    LxVec2 cs = lx_ui_canvas_size();
    if (out) *out = lx_v2((cx / cw * 0.5f + 0.5f) * cs.x, (0.5f - cy / cw * 0.5f) * cs.y);
    return true;
}

/* ---------------- host side ---------------- */
void lx_game_ui_begin_frame(float vx, float vy, float vw, float vh, bool clear) {
    G.vx = vx;
    G.vy = vy;
    G.vw = vw;
    G.vh = vh;
    float rh = G.ref_h > 1 ? G.ref_h : 1080.0f;
    G.scale = vh > 0 ? vh / rh : 1;
    if (!G.atlas_known && !G.editor_atlas) load_glyphs(true);   /* games use the 2x atlas */
    if (!clear) return;   /* paused in the editor: keep showing the last frame */
    G.vc = G.ic = G.cc = 0;
    G.cursor_drawn = false;
    G.anchor = LX_ANCHOR_TOP_LEFT;
    G.font = LX_FONT_REGULAR;
    G.over_button_prev = G.over_button;
    G.over_button = false;
    /* pointer: the first touch, else the mouse */
    LxVec2 p;
    bool down;
    if (lx_touch_count() > 0) { p = lx_touch_pos(0); down = true; }
    else { p = lx_mouse_pos(); down = lx_mouse_down(LX_MOUSE_LEFT); }
    if (G.released) G.active_id = 0;
    G.pressed = down && !G.prev_down;
    G.released = !down && G.prev_down;
    G.prev_down = G.down = down;
    G.pointer = lx_v2((p.x - vx) / G.scale, (p.y - vy) / G.scale);
}

void lx_game_ui_geometry(LxGameUiGeometry *out) {
    memset(out, 0, sizeof *out);
    /* a custom mouse pointer (Set Mouse): drawn last, over everything, once a frame */
    if (!G.cursor_drawn && g_lx_input.mouse_image[0] && !g_lx_input.mouse_locked && !g_lx_input.mouse_lock_blocked && !g_lx_input.mouse_hide_want && ready()) {
        G.cursor_drawn = true;
        uint32_t tex = image_texture(g_lx_input.mouse_image);
        float sz = g_lx_input.mouse_size, x = G.pointer.x - g_lx_input.mouse_hot_x * sz, y = G.pointer.y - g_lx_input.mouse_hot_y * sz;
        if (tex) quad_uv(x, y, x + sz, y + sz, 0, 0, 1, 1, 0xffffffffu, tex);
    }
    if (!G.ic) return;
    if (G.wvcap < G.vc) { G.wvcap = G.vc; G.wv = lx_realloc(G.wv, sizeof(LxUiVertex) * (size_t)G.wvcap); }
    for (int i = 0; i < G.vc; i++) {
        G.wv[i] = G.v[i];
        G.wv[i].x = G.vx + G.v[i].x * G.scale;
        G.wv[i].y = G.vy + G.v[i].y * G.scale;
    }
    if (G.wccap < G.cc) { G.wccap = G.cc; G.wcmds = lx_realloc(G.wcmds, sizeof(LxUiCmd) * (size_t)G.wccap); }
    for (int i = 0; i < G.cc; i++) {
        G.wcmds[i] = G.cmds[i];
        G.wcmds[i].clip[0] = G.vx; G.wcmds[i].clip[1] = G.vy; G.wcmds[i].clip[2] = G.vw; G.wcmds[i].clip[3] = G.vh;
    }
    out->vertices = G.wv;
    out->vertex_count = G.vc;
    out->indices = G.idx;
    out->index_count = G.ic;
    out->cmds = G.wcmds;
    out->cmd_count = G.cc;
}

void lx_game_ui_render(void) {
    if (!G.editor_atlas && !G.player_atlas_loaded) {
        /* games: load the 2x atlas (crisp at any size) into UI texture 0 */
        LxFontAtlas fa;
        lx_font_atlas_get(true, &fa);
        int w, h;
        uint8_t *rgba = lx_png_decode(fa.png, fa.png_size, &w, &h);
        if (rgba) {
            uint8_t *a = lx_alloc((size_t)w * (size_t)h);
            for (int i = 0; i < w * h; i++) a[i] = rgba[i * 4];
            lx_renderer_set_font_atlas(a, w, h);
            lx_free(a);
            lx_free(rgba);
            load_glyphs(true);
        }
        G.player_atlas_loaded = true;
        return;   /* this frame's calls ran before the atlas existed */
    }
    LxGameUiGeometry g;
    lx_game_ui_geometry(&g);
    if (!g.index_count) return;
    LxUiDrawList dl = {g.vertices, g.vertex_count, g.indices, g.index_count, g.cmds, g.cmd_count, lx_v4(0, 0, 0, 1)};
    lx_renderer_draw_ui(&dl);
}

void lx_game_ui_shutdown(void) {
    lx_free(G.v); lx_free(G.idx); lx_free(G.cmds); lx_free(G.wv); lx_free(G.wcmds);
    bool editor = G.editor_atlas;
    memset(&G, 0, sizeof G);
    G.editor_atlas = editor;
}
