/* Luxora UI toolkit implementation (retained mode). See ui.h. */
#include "ui.h"
#include <stdarg.h>
#include <ctype.h>

#define MAX_CLIP 64
#define MAX_POPUPS 8
#define MAX_TOASTS 6

typedef struct GlyphRef { float u0, v0, u1, v1, w, h, xoff, yoff, adv; } GlyphRef;
typedef struct FontData {
    float ascent, line;
    GlyphRef ascii[128];
    uint32_t extra_cp[32];
    GlyphRef extra[32];
    int extra_count;
} FontData;

typedef struct Toast { char text[160]; float time; } Toast;

struct UiCtx {
    UiNode *root;
    void (*after_input)(UiCtx *ui);
    uint32_t app_icon;
    UiNode *popups[MAX_POPUPS];
    int popup_count;
    UiNode *modal;
    UiTheme theme;
    float scale;           /* logical -> physical */
    float atlas_scale;     /* 1 or 2 */
    FontData fonts[UI_FONT_COUNT];
    float atlas_w, atlas_h;
    float white_u, white_v;
    /* draw list */
    LxUiVertex *verts; int vcount, vcap;
    uint32_t *idx; int icount, icap;
    LxUiCmd *cmds; int ccount, ccap;
    float clip[MAX_CLIP][4];
    int clip_depth;
    uint32_t cur_tex;
    /* input */
    LxVec2 mouse;
    UiNode *hot, *active, *focus;
    LxVec2 press_pos;
    float press_value;
    bool dragged;
    double last_click_time;
    UiNode *last_click_node;
    float hover_time;
    LxVec2 hover_pos;
    UiNode *tooltip_node;
    bool split_drag;
    /* drag & drop */
    bool dnd_active;
    const char *dnd_type;
    char dnd_payload[LX_REF_MAX];
    char dnd_label[128];
    UiNode *dnd_source;
    /* misc */
    float time;
    int cursor;
    Toast toasts[MAX_TOASTS];
    int toast_count;
    UiNode *modal_backdrop;
};

/* ======================================================================
 *  colours / theme
 * ====================================================================== */
uint32_t ui_alpha(uint32_t c, float a) {
    uint32_t al = (uint32_t)((float)(c >> 24) * lx_clampf(a, 0, 1));
    return (c & 0x00ffffffu) | (al << 24);
}
uint32_t ui_lerp_color(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255);
        r |= (uint32_t)(x + (y - x) * t) << s;
    }
    return r;
}

/* Hell theme: obsidian surfaces with a faint warmth, ember accents, and soft vertical gradients. */
static void default_theme(UiTheme *t) {
    t->window = UI_HEX(0x0d0909);
    t->window_top = UI_HEX(0x150d0c);
    t->panel = UI_HEX(0x161011);
    t->panel_alt = UI_HEX(0x1c1415);
    t->panel_alt_top = UI_HEX(0x261a19);
    t->header = UI_HEX(0x110c0c);
    t->header_top = UI_HEX(0x1b1110);
    t->field = UI_HEX(0x0b0808);
    t->field_hover = UI_HEX(0x120d0d);
    t->border = UI_HEX(0x2c1e1d);
    t->border_light = UI_HEX(0x4a2c27);
    t->text = UI_HEX(0xf1e6e1);
    t->text_dim = UI_HEX(0xa48f89);
    t->text_disabled = UI_HEX(0x64504c);
    t->accent = UI_HEX(0xd8391b);
    t->accent_top = UI_HEX(0xff7a2e);
    t->accent_hover = UI_HEX(0xec4a20);
    t->accent_hover_top = UI_HEX(0xff9a45);
    t->accent_text = UI_HEX(0xfff6ef);
    t->selection = UI_HEX(0x4e1610);
    t->selection_top = UI_HEX(0x7a2412);
    t->hover = UI_HEX(0x241717);
    t->active = UI_HEX(0x301d1b);
    t->danger = UI_HEX(0xff3d3d);
    t->success = UI_HEX(0x7cc24e);
    t->warning = UI_HEX(0xffb03a);
    t->axis_x = UI_HEX(0xf0503c);
    t->axis_y = UI_HEX(0x86c84a);
    t->axis_z = UI_HEX(0x4f8cf0);
    t->glow = UI_HEX(0xff5a1f);
}

const UiTheme *ui_theme(UiCtx *ui) { return &ui->theme; }
float ui_scale(UiCtx *ui) { return ui->scale; }

/* ======================================================================
 *  fonts
 * ====================================================================== */
static void load_fonts(UiCtx *ui) {
    bool hi = ui->scale > 1.3f;
    LxFontAtlas fa;
    lx_font_atlas_get(hi, &fa);
    const unsigned char *png = fa.png;
    size_t png_size = fa.png_size;
    const LxBakedGlyph *glyphs = fa.glyphs;
    int count = fa.glyph_count;
    const LxBakedFont *fonts = fa.fonts;
    ui->atlas_scale = fa.scale;
    lx_game_ui_use_atlas(hi);   /* the game UI (shown in the viewport while playing) draws with this atlas too */
    int w, h;
    uint8_t *rgba = lx_png_decode(png, png_size, &w, &h);
    if (!rgba) { LX_ERROR("Font atlas decode failed"); return; }
    uint8_t *alpha = lx_alloc((size_t)w * (size_t)h);
    for (int i = 0; i < w * h; i++) alpha[i] = rgba[i * 4];
    lx_free(rgba);
    lx_renderer_set_font_atlas(alpha, w, h);
    lx_free(alpha);
    ui->atlas_w = (float)w;
    ui->atlas_h = (float)h;
    ui->white_u = 1.5f / (float)w;
    ui->white_v = 1.5f / (float)h;
    float k = ui->scale / ui->atlas_scale; /* atlas px -> physical px */
    for (int f = 0; f < UI_FONT_COUNT; f++) {
        ui->fonts[f].ascent = fonts[f].ascent * k;
        ui->fonts[f].line = fonts[f].line_height * k;
    }
    for (int i = 0; i < count; i++) {
        const LxBakedGlyph *g = &glyphs[i];
        GlyphRef r;
        r.u0 = (float)g->x / (float)w;
        r.v0 = (float)g->y / (float)h;
        r.u1 = (float)(g->x + g->w) / (float)w;
        r.v1 = (float)(g->y + g->h) / (float)h;
        r.w = (float)g->w * k;
        r.h = (float)g->h * k;
        r.xoff = (float)g->xoff * k;
        r.yoff = (float)g->yoff * k;
        r.adv = g->advance * k;
        FontData *fd = &ui->fonts[g->font];
        if (g->cp < 128) fd->ascii[g->cp] = r;
        else if (fd->extra_count < 32) {
            fd->extra_cp[fd->extra_count] = g->cp;
            fd->extra[fd->extra_count++] = r;
        }
    }
}

static const GlyphRef *glyph(UiCtx *ui, UiFont f, uint32_t cp) {
    FontData *fd = &ui->fonts[f];
    if (cp < 128) return &fd->ascii[cp];
    for (int i = 0; i < fd->extra_count; i++)
        if (fd->extra_cp[i] == cp) return &fd->extra[i];
    return &fd->ascii['?'];
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

float ui_text_width(UiCtx *ui, UiFont f, const char *t) {
    float w = 0;
    while (t && *t) w += glyph(ui, f, utf8_next(&t))->adv;
    return w;
}
float ui_line_height(UiCtx *ui, UiFont f) { return ui->fonts[f].line; }

/* ======================================================================
 *  draw list
 * ====================================================================== */
static void ensure(UiCtx *ui, int nv, int ni) {
    if (ui->vcount + nv > ui->vcap) {
        while (ui->vcount + nv > ui->vcap) ui->vcap = ui->vcap ? ui->vcap * 2 : 8192;
        ui->verts = lx_realloc(ui->verts, sizeof(LxUiVertex) * (size_t)ui->vcap);
    }
    if (ui->icount + ni > ui->icap) {
        while (ui->icount + ni > ui->icap) ui->icap = ui->icap ? ui->icap * 2 : 16384;
        ui->idx = lx_realloc(ui->idx, sizeof(uint32_t) * (size_t)ui->icap);
    }
}

static void begin_cmd(UiCtx *ui, uint32_t tex) {
    const float *c = ui->clip[ui->clip_depth];
    LxUiCmd *last = ui->ccount ? &ui->cmds[ui->ccount - 1] : NULL;
    if (last && last->texture == tex && !memcmp(last->clip, c, sizeof last->clip)) return;
    if (last && last->index_count == 0) { last->texture = tex; memcpy(last->clip, c, sizeof last->clip); return; }
    LxUiCmd cmd;
    cmd.texture = tex;
    cmd.index_offset = (uint32_t)ui->icount;
    cmd.index_count = 0;
    memcpy(cmd.clip, c, sizeof cmd.clip);
    LX_VEC_PUSH(ui->cmds, ui->ccount, ui->ccap, cmd);
}

static void quad_uv(UiCtx *ui, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col, uint32_t tex) {
    begin_cmd(ui, tex);
    ensure(ui, 4, 6);
    uint32_t b = (uint32_t)ui->vcount;
    LxUiVertex *v = ui->verts + ui->vcount;
    v[0] = (LxUiVertex){x0, y0, u0, v0, col};
    v[1] = (LxUiVertex){x1, y0, u1, v0, col};
    v[2] = (LxUiVertex){x1, y1, u1, v1, col};
    v[3] = (LxUiVertex){x0, y1, u0, v1, col};
    ui->vcount += 4;
    uint32_t *i = ui->idx + ui->icount;
    i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b; i[4] = b + 2; i[5] = b + 3;
    ui->icount += 6;
    ui->cmds[ui->ccount - 1].index_count += 6;
}

static void tri_raw(UiCtx *ui, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col) {
    begin_cmd(ui, 0);
    ensure(ui, 3, 3);
    uint32_t b = (uint32_t)ui->vcount;
    LxUiVertex *v = ui->verts + ui->vcount;
    v[0] = (LxUiVertex){x0, y0, ui->white_u, ui->white_v, col};
    v[1] = (LxUiVertex){x1, y1, ui->white_u, ui->white_v, col};
    v[2] = (LxUiVertex){x2, y2, ui->white_u, ui->white_v, col};
    ui->vcount += 3;
    ui->idx[ui->icount++] = b;
    ui->idx[ui->icount++] = b + 1;
    ui->idx[ui->icount++] = b + 2;
    ui->cmds[ui->ccount - 1].index_count += 3;
}

void ui_draw_triangle(UiCtx *ui, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col) {
    if (!(col >> 24)) return;
    tri_raw(ui, x0, y0, x1, y1, x2, y2, col);
}

/* colour of a gradient rectangle at a point */
typedef struct Grad { float x, y, w, h; uint32_t a, b; bool horiz; } Grad;
static uint32_t grad_at(const Grad *g, float px, float py) {
    float t = g->horiz ? (px - g->x) / g->w : (py - g->y) / g->h;
    return ui_lerp_color(g->a, g->b, lx_clampf(t, 0, 1));
}
static void grad_quad(UiCtx *ui, const Grad *g, float x0, float y0, float x1, float y1) {
    begin_cmd(ui, 0);
    ensure(ui, 4, 6);
    uint32_t b = (uint32_t)ui->vcount;
    LxUiVertex *v = ui->verts + ui->vcount;
    float u = ui->white_u, w = ui->white_v;
    v[0] = (LxUiVertex){x0, y0, u, w, grad_at(g, x0, y0)};
    v[1] = (LxUiVertex){x1, y0, u, w, grad_at(g, x1, y0)};
    v[2] = (LxUiVertex){x1, y1, u, w, grad_at(g, x1, y1)};
    v[3] = (LxUiVertex){x0, y1, u, w, grad_at(g, x0, y1)};
    ui->vcount += 4;
    uint32_t *i = ui->idx + ui->icount;
    i[0] = b; i[1] = b + 1; i[2] = b + 2; i[3] = b; i[4] = b + 2; i[5] = b + 3;
    ui->icount += 6;
    ui->cmds[ui->ccount - 1].index_count += 6;
}
static void grad_tri(UiCtx *ui, const Grad *g, float x0, float y0, float x1, float y1, float x2, float y2) {
    begin_cmd(ui, 0);
    ensure(ui, 3, 3);
    uint32_t b = (uint32_t)ui->vcount;
    LxUiVertex *v = ui->verts + ui->vcount;
    v[0] = (LxUiVertex){x0, y0, ui->white_u, ui->white_v, grad_at(g, x0, y0)};
    v[1] = (LxUiVertex){x1, y1, ui->white_u, ui->white_v, grad_at(g, x1, y1)};
    v[2] = (LxUiVertex){x2, y2, ui->white_u, ui->white_v, grad_at(g, x2, y2)};
    ui->vcount += 3;
    ui->idx[ui->icount++] = b;
    ui->idx[ui->icount++] = b + 1;
    ui->idx[ui->icount++] = b + 2;
    ui->cmds[ui->ccount - 1].index_count += 3;
}

static void rect_impl(UiCtx *ui, const Grad *g, float r) {
    float x = g->x, y = g->y, w = g->w, h = g->h;
    r = lx_minf(r, lx_minf(w, h) * 0.5f);
    if (r < 0.75f) { grad_quad(ui, g, x, y, x + w, y + h); return; }
    grad_quad(ui, g, x + r, y, x + w - r, y + h);
    grad_quad(ui, g, x, y + r, x + r, y + h - r);
    grad_quad(ui, g, x + w - r, y + r, x + w, y + h - r);
    const int seg = 5;
    float cx[4] = {x + r, x + w - r, x + w - r, x + r}, cy[4] = {y + r, y + r, y + h - r, y + h - r};
    float start[4] = {LX_PI, 1.5f * LX_PI, 0, 0.5f * LX_PI};
    for (int c = 0; c < 4; c++)
        for (int s = 0; s < seg; s++) {
            float a0 = start[c] + (float)s / seg * 0.5f * LX_PI, a1 = start[c] + (float)(s + 1) / seg * 0.5f * LX_PI;
            grad_tri(ui, g, cx[c], cy[c], cx[c] + cosf(a0) * r, cy[c] + sinf(a0) * r, cx[c] + cosf(a1) * r, cy[c] + sinf(a1) * r);
        }
}

void ui_draw_rect_gradient(UiCtx *ui, float x, float y, float w, float h, uint32_t a, uint32_t b, float r, bool horizontal) {
    if (w <= 0 || h <= 0 || (!(a >> 24) && !(b >> 24))) return;
    Grad g = {x, y, w, h, a, b, horizontal};
    rect_impl(ui, &g, r);
}

/* Theme surfaces get their gradient automatically: anything filled with one of the theme's surface
 * colours (taller than a hairline) blends down from its *_top companion. */
static uint32_t gradient_top(UiCtx *ui, uint32_t col) {
    const UiTheme *t = &ui->theme;
    if (col == t->accent) return t->accent_top;
    if (col == t->accent_hover) return t->accent_hover_top;
    if (col == t->selection) return t->selection_top;
    if (col == t->panel_alt) return t->panel_alt_top;
    if (col == t->header) return t->header_top;
    if (col == t->window) return t->window_top;
    return col;
}

void ui_draw_rect(UiCtx *ui, float x, float y, float w, float h, uint32_t col, float r) {
    if (w <= 0 || h <= 0 || !(col >> 24)) return;
    uint32_t top = h > 6.0f ? gradient_top(ui, col) : col;
    Grad g = {x, y, w, h, top, col, false};
    rect_impl(ui, &g, r);
}

void ui_draw_line(UiCtx *ui, float x0, float y0, float x1, float y1, float th, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) return;
    float nx = -dy / l * th * 0.5f, ny = dx / l * th * 0.5f;
    tri_raw(ui, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, col);
    tri_raw(ui, x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, col);
}

void ui_draw_border(UiCtx *ui, float x, float y, float w, float h, uint32_t col, float r, float t) {
    if (!(col >> 24) || w <= 0 || h <= 0) return;
    if (r < 1.0f) {
        ui_draw_rect(ui, x, y, w, t, col, 0);
        ui_draw_rect(ui, x, y + h - t, w, t, col, 0);
        ui_draw_rect(ui, x, y + t, t, h - 2 * t, col, 0);
        ui_draw_rect(ui, x + w - t, y + t, t, h - 2 * t, col, 0);
        return;
    }
    r = lx_minf(r, lx_minf(w, h) * 0.5f);
    ui_draw_rect(ui, x + r, y, w - 2 * r, t, col, 0);
    ui_draw_rect(ui, x + r, y + h - t, w - 2 * r, t, col, 0);
    ui_draw_rect(ui, x, y + r, t, h - 2 * r, col, 0);
    ui_draw_rect(ui, x + w - t, y + r, t, h - 2 * r, col, 0);
    float cx[4] = {x + r, x + w - r, x + w - r, x + r}, cy[4] = {y + r, y + r, y + h - r, y + h - r};
    float start[4] = {LX_PI, 1.5f * LX_PI, 0, 0.5f * LX_PI};
    for (int c = 0; c < 4; c++)
        for (int s = 0; s < 5; s++) {
            float a0 = start[c] + (float)s / 5 * 0.5f * LX_PI, a1 = start[c] + (float)(s + 1) / 5 * 0.5f * LX_PI;
            ui_draw_line(ui, cx[c] + cosf(a0) * (r - t * 0.5f), cy[c] + sinf(a0) * (r - t * 0.5f), cx[c] + cosf(a1) * (r - t * 0.5f),
                         cy[c] + sinf(a1) * (r - t * 0.5f), t, col);
        }
}

void ui_draw_circle(UiCtx *ui, float cx, float cy, float r, uint32_t col, int seg) {
    if (seg < 6) seg = 6;
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / seg * 2 * LX_PI, a1 = (float)(i + 1) / seg * 2 * LX_PI;
        tri_raw(ui, cx, cy, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, col);
    }
}

void ui_draw_ring(UiCtx *ui, float cx, float cy, float r, float th, uint32_t col, int seg) {
    if (seg < 6) seg = 6;
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / seg * 2 * LX_PI, a1 = (float)(i + 1) / seg * 2 * LX_PI;
        ui_draw_line(ui, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, th, col);
    }
}

static void arc(UiCtx *ui, float cx, float cy, float r, float a0, float a1, float th, uint32_t col, int seg) {
    for (int i = 0; i < seg; i++) {
        float b0 = a0 + (a1 - a0) * (float)i / seg, b1 = a0 + (a1 - a0) * (float)(i + 1) / seg;
        ui_draw_line(ui, cx + cosf(b0) * r, cy + sinf(b0) * r, cx + cosf(b1) * r, cy + sinf(b1) * r, th, col);
    }
}

void ui_draw_text(UiCtx *ui, UiFont f, float x, float y, const char *t, uint32_t col) {
    float pen = floorf(x + 0.5f);
    float base = floorf(y + ui->fonts[f].ascent + 0.5f);
    while (t && *t) {
        const GlyphRef *g = glyph(ui, f, utf8_next(&t));
        if (g->w > 0) {
            float gx = pen + g->xoff, gy = base - ui->fonts[f].ascent + g->yoff;
            quad_uv(ui, gx, gy, gx + g->w, gy + g->h, g->u0, g->v0, g->u1, g->v1, col, 0);
        }
        pen += g->adv;
    }
}

/* draws text clipped with an ellipsis when it exceeds max_w */
/* the width draw_text_fit ends up drawing (the text, or its start plus "…") */
static float fit_width(UiCtx *ui, UiFont f, const char *t, float max_w) {
    if (!t || !t[0]) return 0;
    float full = ui_text_width(ui, f, t);
    if (full <= max_w + 1.0f) return full;
    float ell = ui_text_width(ui, f, "…"), w = 0;
    const char *p = t;
    int n = 0;
    while (*p && n < 250) {
        const char *q = p;
        float a = glyph(ui, f, utf8_next(&q))->adv;
        if (w + a + ell > max_w) break;
        n += (int)(q - p);
        w += a;
        p = q;
    }
    return w + ell;
}

static void draw_text_fit(UiCtx *ui, UiFont f, float x, float y, const char *t, uint32_t col, float max_w) {
    if (!t || !t[0]) return;
    if (ui_text_width(ui, f, t) <= max_w + 1.0f) { ui_draw_text(ui, f, x, y, t, col); return; }
    char buf[256];
    float ell = ui_text_width(ui, f, "…");
    size_t n = 0;
    float w = 0;
    const char *p = t;
    while (*p && n < sizeof buf - 4) {
        const char *q = p;
        float a = glyph(ui, f, utf8_next(&q))->adv;
        if (w + a + ell > max_w) break;
        memcpy(buf + n, p, (size_t)(q - p));
        n += (size_t)(q - p);
        w += a;
        p = q;
    }
    memcpy(buf + n, "…", 4);
    ui_draw_text(ui, f, x, y, buf, col);
}

void ui_draw_mesh(UiCtx *ui, const LxUiVertex *v, int vc, const uint32_t *idx, int ic, const LxUiCmd *cmds, int cc) {
    if (!vc || !ic) return;
    ensure(ui, vc, ic);
    uint32_t base = (uint32_t)ui->vcount;
    memcpy(ui->verts + ui->vcount, v, sizeof(LxUiVertex) * (size_t)vc);
    ui->vcount += vc;
    for (int c = 0; c < cc; c++) {
        begin_cmd(ui, cmds[c].texture);
        for (uint32_t k = 0; k < cmds[c].index_count; k++) ui->idx[ui->icount++] = base + idx[cmds[c].index_offset + k];
        ui->cmds[ui->ccount - 1].index_count += cmds[c].index_count;
    }
}

void ui_draw_image(UiCtx *ui, uint32_t tex, float x, float y, float w, float h) {
    quad_uv(ui, x, y, x + w, y + h, 0, 0, 1, 1, 0xffffffffu, tex);
}

void ui_push_clip(UiCtx *ui, float x, float y, float w, float h) {
    if (ui->clip_depth + 1 >= MAX_CLIP) return;
    const float *c = ui->clip[ui->clip_depth];
    float x0 = lx_maxf(x, c[0]), y0 = lx_maxf(y, c[1]);
    float x1 = lx_minf(x + w, c[0] + c[2]), y1 = lx_minf(y + h, c[1] + c[3]);
    float *n = ui->clip[++ui->clip_depth];
    n[0] = x0; n[1] = y0; n[2] = lx_maxf(0, x1 - x0); n[3] = lx_maxf(0, y1 - y0);
}
void ui_pop_clip(UiCtx *ui) { if (ui->clip_depth > 0) ui->clip_depth--; }

/* ======================================================================
 *  icons: a vector icon set drawn in a size x size box
 * ====================================================================== */
#define P(u, v) (x + (u) * s), (y + (v) * s)
void ui_draw_icon(UiCtx *ui, UiIcon icon, float x, float y, float s, uint32_t c) {
    float t = lx_maxf(1.2f, s * 0.085f); /* stroke */
    uint32_t c2 = ui_alpha(c, 0.55f);
    switch (icon) {
    case UI_ICON_NONE: case UI_ICON_COUNT: break;
    case UI_ICON_FOLDER:
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.2f, s * 0.38f, s * 0.15f, c, s * 0.04f);
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.3f, s * 0.88f, s * 0.55f, c, s * 0.06f);
        break;
    case UI_ICON_FOLDER_OPEN:
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.2f, s * 0.38f, s * 0.15f, c2, s * 0.04f);
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.3f, s * 0.8f, s * 0.55f, c2, s * 0.06f);
        ui_draw_triangle(ui, P(0.18f, 0.45f), P(0.98f, 0.45f), P(0.86f, 0.85f), c);
        ui_draw_triangle(ui, P(0.18f, 0.45f), P(0.86f, 0.85f), P(0.06f, 0.85f), c);
        break;
    case UI_ICON_FILE: case UI_ICON_SCRIPT: case UI_ICON_PREFAB:
        ui_draw_rect(ui, x + s * 0.2f, y + s * 0.08f, s * 0.6f, s * 0.84f, c2, s * 0.06f);
        ui_draw_triangle(ui, P(0.6f, 0.08f), P(0.8f, 0.08f), P(0.8f, 0.28f), ui_alpha(c, 0.2f));
        if (icon == UI_ICON_SCRIPT) {
            ui_draw_line(ui, P(0.42f, 0.42f), P(0.32f, 0.55f), t, c);
            ui_draw_line(ui, P(0.32f, 0.55f), P(0.42f, 0.68f), t, c);
            ui_draw_line(ui, P(0.58f, 0.42f), P(0.68f, 0.55f), t, c);
            ui_draw_line(ui, P(0.68f, 0.55f), P(0.58f, 0.68f), t, c);
        } else if (icon == UI_ICON_PREFAB) {
            ui_draw_rect(ui, x + s * 0.36f, y + s * 0.42f, s * 0.28f, s * 0.28f, c, s * 0.03f);
        } else {
            for (int i = 0; i < 3; i++) ui_draw_rect(ui, x + s * 0.3f, y + s * (0.4f + i * 0.14f), s * 0.4f, t, c, 0);
        }
        break;
    case UI_ICON_SCENE:
        ui_draw_rect(ui, x + s * 0.08f, y + s * 0.16f, s * 0.84f, s * 0.68f, c2, s * 0.08f);
        ui_draw_triangle(ui, P(0.14f, 0.78f), P(0.42f, 0.42f), P(0.62f, 0.78f), c);
        ui_draw_triangle(ui, P(0.48f, 0.78f), P(0.68f, 0.52f), P(0.86f, 0.78f), c);
        ui_draw_circle(ui, x + s * 0.72f, y + s * 0.32f, s * 0.07f, c, 10);
        break;
    case UI_ICON_MESH: case UI_ICON_CUBE: {
        /* isometric cube */
        ui_draw_triangle(ui, P(0.5f, 0.1f), P(0.88f, 0.3f), P(0.5f, 0.5f), c);
        ui_draw_triangle(ui, P(0.5f, 0.1f), P(0.5f, 0.5f), P(0.12f, 0.3f), c);
        ui_draw_triangle(ui, P(0.12f, 0.3f), P(0.5f, 0.5f), P(0.5f, 0.92f), ui_alpha(c, 0.75f));
        ui_draw_triangle(ui, P(0.12f, 0.3f), P(0.5f, 0.92f), P(0.12f, 0.72f), ui_alpha(c, 0.75f));
        ui_draw_triangle(ui, P(0.5f, 0.5f), P(0.88f, 0.3f), P(0.88f, 0.72f), ui_alpha(c, 0.5f));
        ui_draw_triangle(ui, P(0.5f, 0.5f), P(0.88f, 0.72f), P(0.5f, 0.92f), ui_alpha(c, 0.5f));
        if (icon == UI_ICON_MESH) {
            ui_draw_line(ui, P(0.5f, 0.5f), P(0.5f, 0.92f), t * 0.6f, UI_RGBA(0, 0, 0, 90));
            ui_draw_line(ui, P(0.12f, 0.3f), P(0.88f, 0.72f), t * 0.6f, UI_RGBA(0, 0, 0, 60));
        }
        break;
    }
    case UI_ICON_MATERIAL: case UI_ICON_SPHERE:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.4f, ui_alpha(c, 0.75f), 24);
        ui_draw_circle(ui, x + s * 0.42f, y + s * 0.42f, s * 0.26f, c, 20);
        ui_draw_circle(ui, x + s * 0.36f, y + s * 0.34f, s * 0.08f, UI_RGBA(255, 255, 255, 170), 10);
        break;
    case UI_ICON_TEXTURE:
        ui_draw_rect(ui, x + s * 0.1f, y + s * 0.1f, s * 0.8f, s * 0.8f, c2, s * 0.06f);
        for (int r = 0; r < 4; r++)
            for (int q = 0; q < 4; q++)
                if ((r + q) & 1) ui_draw_rect(ui, x + s * (0.1f + q * 0.2f), y + s * (0.1f + r * 0.2f), s * 0.2f, s * 0.2f, c, 0);
        break;
    case UI_ICON_SOUND: case UI_ICON_SPEAKER:
        ui_draw_rect(ui, x + s * 0.1f, y + s * 0.36f, s * 0.18f, s * 0.28f, c, 0);
        ui_draw_triangle(ui, P(0.28f, 0.36f), P(0.5f, 0.14f), P(0.5f, 0.86f), c);
        ui_draw_triangle(ui, P(0.28f, 0.36f), P(0.5f, 0.86f), P(0.28f, 0.64f), c);
        arc(ui, x + s * 0.5f, y + s * 0.5f, s * 0.18f, -0.9f, 0.9f, t, c, 6);
        arc(ui, x + s * 0.5f, y + s * 0.5f, s * 0.34f, -0.9f, 0.9f, t, c, 8);
        break;
    case UI_ICON_LISTENER:
        arc(ui, x + s * 0.5f, y + s * 0.55f, s * 0.32f, LX_PI, 2 * LX_PI, t * 1.3f, c, 10);
        ui_draw_rect(ui, x + s * 0.12f, y + s * 0.52f, s * 0.16f, s * 0.3f, c, s * 0.05f);
        ui_draw_rect(ui, x + s * 0.72f, y + s * 0.52f, s * 0.16f, s * 0.3f, c, s * 0.05f);
        break;
    case UI_ICON_APP:
        if (ui->app_icon) { ui_draw_image(ui, ui->app_icon, x, y, s, s); break; }
        /* fall through - no texture yet: draw the vector project mark */
    case UI_ICON_PROJECT:
        ui_draw_rect(ui, x + s * 0.1f, y + s * 0.1f, s * 0.8f, s * 0.8f, c2, s * 0.14f);
        ui_draw_triangle(ui, P(0.5f, 0.24f), P(0.76f, 0.5f), P(0.5f, 0.76f), c);
        ui_draw_triangle(ui, P(0.5f, 0.24f), P(0.5f, 0.76f), P(0.24f, 0.5f), c);
        break;
    case UI_ICON_PLAY:
        ui_draw_triangle(ui, P(0.25f, 0.15f), P(0.85f, 0.5f), P(0.25f, 0.85f), c);
        break;
    case UI_ICON_PAUSE:
        ui_draw_rect(ui, x + s * 0.24f, y + s * 0.18f, s * 0.17f, s * 0.64f, c, s * 0.03f);
        ui_draw_rect(ui, x + s * 0.59f, y + s * 0.18f, s * 0.17f, s * 0.64f, c, s * 0.03f);
        break;
    case UI_ICON_STOP:
        ui_draw_rect(ui, x + s * 0.2f, y + s * 0.2f, s * 0.6f, s * 0.6f, c, s * 0.06f);
        break;
    case UI_ICON_STEP:
        ui_draw_triangle(ui, P(0.18f, 0.18f), P(0.62f, 0.5f), P(0.18f, 0.82f), c);
        ui_draw_rect(ui, x + s * 0.66f, y + s * 0.18f, s * 0.15f, s * 0.64f, c, 0);
        break;
    case UI_ICON_CAMERA:
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.3f, s * 0.6f, s * 0.44f, c, s * 0.06f);
        ui_draw_triangle(ui, P(0.66f, 0.52f), P(0.94f, 0.3f), P(0.94f, 0.74f), c);
        ui_draw_circle(ui, x + s * 0.25f, y + s * 0.2f, s * 0.1f, c2, 10);
        ui_draw_circle(ui, x + s * 0.48f, y + s * 0.2f, s * 0.1f, c2, 10);
        break;
    case UI_ICON_LIGHT: case UI_ICON_BULB:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.4f, s * 0.26f, c, 18);
        ui_draw_rect(ui, x + s * 0.38f, y + s * 0.62f, s * 0.24f, s * 0.12f, c, 0);
        ui_draw_rect(ui, x + s * 0.4f, y + s * 0.78f, s * 0.2f, s * 0.08f, c2, s * 0.03f);
        break;
    case UI_ICON_SUN:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.2f, c, 16);
        for (int i = 0; i < 8; i++) {
            float a = (float)i / 8 * 2 * LX_PI;
            ui_draw_line(ui, x + s * (0.5f + cosf(a) * 0.3f), y + s * (0.5f + sinf(a) * 0.3f), x + s * (0.5f + cosf(a) * 0.44f),
                         y + s * (0.5f + sinf(a) * 0.44f), t, c);
        }
        break;
    case UI_ICON_SPOT:
        ui_draw_rect(ui, x + s * 0.36f, y + s * 0.08f, s * 0.28f, s * 0.2f, c, s * 0.04f);
        ui_draw_triangle(ui, P(0.4f, 0.28f), P(0.6f, 0.28f), P(0.9f, 0.9f), c2);
        ui_draw_triangle(ui, P(0.4f, 0.28f), P(0.9f, 0.9f), P(0.1f, 0.9f), c2);
        break;
    case UI_ICON_PLANE:
        ui_draw_triangle(ui, P(0.3f, 0.3f), P(0.95f, 0.3f), P(0.7f, 0.75f), c);
        ui_draw_triangle(ui, P(0.3f, 0.3f), P(0.7f, 0.75f), P(0.05f, 0.75f), c);
        break;
    case UI_ICON_CYLINDER: case UI_ICON_CAPSULE: {
        bool cap = icon == UI_ICON_CAPSULE;
        ui_draw_rect(ui, x + s * 0.24f, y + s * 0.2f, s * 0.52f, s * 0.6f, c, cap ? s * 0.26f : s * 0.04f);
        if (!cap) ui_draw_ring(ui, x + s * 0.5f, y + s * 0.2f, s * 0.26f * 0.98f, t * 0.6f, c2, 16);
        break;
    }
    case UI_ICON_ENTITY:
        ui_draw_border(ui, x + s * 0.18f, y + s * 0.18f, s * 0.64f, s * 0.64f, c, s * 0.1f, t);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.1f, c, 10);
        break;
    case UI_ICON_CROWD:
        for (int i = 0; i < 3; i++) {
            float cx = 0.22f + i * 0.28f, sc = i == 1 ? 1.0f : 0.85f;
            ui_draw_circle(ui, x + s * cx, y + s * 0.34f, s * 0.1f * sc, i == 1 ? c : c2, 10);
            ui_draw_rect(ui, x + s * (cx - 0.12f * sc), y + s * 0.5f, s * 0.24f * sc, s * 0.34f, i == 1 ? c : c2, s * 0.1f);
        }
        break;
    /* window controls: thin, crisp strokes like a native caption */
    case UI_ICON_WIN_MIN:
        ui_draw_line(ui, P(0.24f, 0.52f), P(0.76f, 0.52f), s * 0.07f, c);
        break;
    case UI_ICON_WIN_MAX:
        ui_draw_border(ui, x + s * 0.26f, y + s * 0.26f, s * 0.48f, s * 0.48f, c, s * 0.04f, s * 0.07f);
        break;
    case UI_ICON_WIN_RESTORE:
        ui_draw_line(ui, P(0.36f, 0.24f), P(0.78f, 0.24f), s * 0.07f, c);
        ui_draw_line(ui, P(0.78f, 0.21f), P(0.78f, 0.64f), s * 0.07f, c);
        ui_draw_border(ui, x + s * 0.22f, y + s * 0.36f, s * 0.42f, s * 0.42f, c, s * 0.04f, s * 0.07f);
        break;
    case UI_ICON_WIN_CLOSE:
        ui_draw_line(ui, P(0.27f, 0.27f), P(0.73f, 0.73f), s * 0.075f, c);
        ui_draw_line(ui, P(0.73f, 0.27f), P(0.27f, 0.73f), s * 0.075f, c);
        break;
    case UI_ICON_COLLIDER: {
        /* wireframe cube */
        float a = 0.22f, b = 0.62f, o = 0.16f;
        ui_draw_border(ui, x + s * a, y + s * (a + o), s * (b - a + 0.0f), s * (b - a), c, 0, t);
        ui_draw_border(ui, x + s * (a + o), y + s * a, s * (b - a), s * (b - a), c2, 0, t);
        ui_draw_line(ui, P(a, a + o), P(a + o, a), t, c2);
        ui_draw_line(ui, P(b, a + o), P(b + o, a), t, c2);
        ui_draw_line(ui, P(a, b + o), P(a + o, b), t, c2);
        ui_draw_line(ui, P(b, b + o), P(b + o, b), t, c2);
        break;
    }
    case UI_ICON_RIGIDBODY:
        /* a falling ball with motion lines */
        ui_draw_circle(ui, x + s * 0.56f, y + s * 0.62f, s * 0.25f, c, 20);
        ui_draw_circle(ui, x + s * 0.49f, y + s * 0.55f, s * 0.07f, UI_RGBA(255, 255, 255, 150), 10);
        ui_draw_line(ui, P(0.22f, 0.12f), P(0.22f, 0.4f), t, c2);
        ui_draw_line(ui, P(0.4f, 0.08f), P(0.4f, 0.3f), t, c2);
        ui_draw_line(ui, P(0.58f, 0.1f), P(0.58f, 0.28f), t, c2);
        break;
    case UI_ICON_CHARACTER:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.22f, s * 0.13f, c, 16);
        ui_draw_rect(ui, x + s * 0.34f, y + s * 0.4f, s * 0.32f, s * 0.48f, c, s * 0.15f);
        break;
    case UI_ICON_ANIMATOR: {
        /* a figure mid-stride */
        ui_draw_circle(ui, x + s * 0.56f, y + s * 0.16f, s * 0.11f, c, 14);
        ui_draw_line(ui, P(0.52f, 0.3f), P(0.44f, 0.58f), t * 1.4f, c);        /* body */
        ui_draw_line(ui, P(0.44f, 0.58f), P(0.62f, 0.72f), t * 1.2f, c);       /* front leg */
        ui_draw_line(ui, P(0.62f, 0.72f), P(0.58f, 0.9f), t * 1.2f, c);
        ui_draw_line(ui, P(0.44f, 0.58f), P(0.3f, 0.76f), t * 1.2f, c);        /* back leg */
        ui_draw_line(ui, P(0.3f, 0.76f), P(0.16f, 0.82f), t * 1.2f, c);
        ui_draw_line(ui, P(0.5f, 0.36f), P(0.72f, 0.5f), t * 1.1f, c);         /* arms */
        ui_draw_line(ui, P(0.5f, 0.36f), P(0.3f, 0.46f), t * 1.1f, c);
        break;
    }
    case UI_ICON_AVATAR:
        /* a standing figure with its joints marked */
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.15f, s * 0.11f, c, 14);
        ui_draw_line(ui, P(0.5f, 0.28f), P(0.5f, 0.58f), t * 1.4f, c);
        ui_draw_line(ui, P(0.24f, 0.34f), P(0.76f, 0.34f), t * 1.2f, c);
        ui_draw_line(ui, P(0.5f, 0.58f), P(0.36f, 0.9f), t * 1.2f, c);
        ui_draw_line(ui, P(0.5f, 0.58f), P(0.64f, 0.9f), t * 1.2f, c);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.58f, s * 0.06f, c, 10);
        ui_draw_circle(ui, x + s * 0.24f, y + s * 0.34f, s * 0.05f, c, 10);
        ui_draw_circle(ui, x + s * 0.76f, y + s * 0.34f, s * 0.05f, c, 10);
        break;
    case UI_ICON_MODEL:
        /* a cube with a figure's head on top: an imported model */
        ui_draw_border(ui, x + s * 0.18f, y + s * 0.42f, s * 0.64f, s * 0.46f, c, s * 0.06f, t);
        ui_draw_line(ui, P(0.18f, 0.58f), P(0.82f, 0.58f), t, c);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.22f, s * 0.13f, c, 14);
        break;
    case UI_ICON_PROBE: {
        /* a mirror ball: dark sphere reflecting a bright horizon, with a highlight */
        float cx = x + s * 0.5f, cy = y + s * 0.5f, r = s * 0.4f;
        ui_draw_circle(ui, cx, cy, r, ui_alpha(c, 0.45f), 24);
        ui_draw_rect(ui, cx - r * 0.86f, cy - r * 0.08f, r * 1.72f, r * 0.2f, c, r * 0.1f);
        ui_draw_circle(ui, cx - r * 0.35f, cy - r * 0.42f, r * 0.2f, UI_RGBA(255, 255, 255, 230), 12);
        ui_draw_ring(ui, cx, cy, r, t, c, 24);
        break;
    }
    case UI_ICON_BEHAVIOUR:
        ui_draw_ring(ui, x + s * 0.5f, y + s * 0.5f, s * 0.36f, t, c, 20);
        ui_draw_line(ui, P(0.42f, 0.3f), P(0.3f, 0.5f), t, c);
        ui_draw_line(ui, P(0.3f, 0.5f), P(0.42f, 0.7f), t, c);
        ui_draw_line(ui, P(0.58f, 0.3f), P(0.7f, 0.5f), t, c);
        ui_draw_line(ui, P(0.7f, 0.5f), P(0.58f, 0.7f), t, c);
        break;
    case UI_ICON_SELECT:
        ui_draw_triangle(ui, P(0.25f, 0.1f), P(0.78f, 0.6f), P(0.5f, 0.62f), c);
        ui_draw_triangle(ui, P(0.25f, 0.1f), P(0.5f, 0.62f), P(0.3f, 0.82f), c);
        ui_draw_line(ui, P(0.5f, 0.6f), P(0.66f, 0.9f), t * 1.4f, c);
        break;
    case UI_ICON_MOVE:
        ui_draw_line(ui, P(0.5f, 0.12f), P(0.5f, 0.88f), t, c);
        ui_draw_line(ui, P(0.12f, 0.5f), P(0.88f, 0.5f), t, c);
        ui_draw_triangle(ui, P(0.5f, 0.04f), P(0.62f, 0.2f), P(0.38f, 0.2f), c);
        ui_draw_triangle(ui, P(0.5f, 0.96f), P(0.38f, 0.8f), P(0.62f, 0.8f), c);
        ui_draw_triangle(ui, P(0.04f, 0.5f), P(0.2f, 0.38f), P(0.2f, 0.62f), c);
        ui_draw_triangle(ui, P(0.96f, 0.5f), P(0.8f, 0.62f), P(0.8f, 0.38f), c);
        break;
    case UI_ICON_ROTATE: case UI_ICON_REFRESH: case UI_ICON_REDO: case UI_ICON_UNDO: {
        bool undo = icon == UI_ICON_UNDO;
        float a0 = undo ? -0.6f * LX_PI : 0.15f * LX_PI, a1 = undo ? 0.85f * LX_PI : 1.6f * LX_PI;
        if (icon == UI_ICON_REDO) { a0 = 1.6f * LX_PI; a1 = 3.05f * LX_PI; }
        arc(ui, x + s * 0.5f, y + s * 0.52f, s * 0.32f, a0, a1, t, c, 14);
        float ea = undo ? a0 : a1;
        float ex = x + s * 0.5f + cosf(ea) * s * 0.32f, ey = y + s * 0.52f + sinf(ea) * s * 0.32f;
        float tx = -sinf(ea) * (undo ? -1.0f : 1.0f), ty = cosf(ea) * (undo ? -1.0f : 1.0f);
        float as = s * 0.16f;
        ui_draw_triangle(ui, ex + tx * as, ey + ty * as, ex - ty * as * 0.8f, ey + tx * as * 0.8f, ex + ty * as * 0.8f, ey - tx * as * 0.8f, c);
        break;
    }
    case UI_ICON_SCALE:
        ui_draw_border(ui, x + s * 0.12f, y + s * 0.38f, s * 0.5f, s * 0.5f, c2, 0, t);
        ui_draw_line(ui, P(0.4f, 0.6f), P(0.84f, 0.16f), t, c);
        ui_draw_triangle(ui, P(0.88f, 0.12f), P(0.88f, 0.42f), P(0.58f, 0.12f), c);
        break;
    case UI_ICON_FOCUS:
        ui_draw_ring(ui, x + s * 0.5f, y + s * 0.5f, s * 0.14f, t, c, 12);
        for (int i = 0; i < 4; i++) {
            float sx = (i & 1) ? 0.9f : 0.1f, sy = (i & 2) ? 0.9f : 0.1f, dx = (i & 1) ? -0.22f : 0.22f, dy = (i & 2) ? -0.22f : 0.22f;
            ui_draw_line(ui, P(sx, sy), P(sx + dx, sy), t, c);
            ui_draw_line(ui, P(sx, sy), P(sx, sy + dy), t, c);
        }
        break;
    case UI_ICON_GRID:
        for (int i = 0; i < 4; i++) {
            float o = 0.14f + i * 0.24f;
            ui_draw_line(ui, P(o, 0.1f), P(o, 0.9f), t * 0.8f, c);
            ui_draw_line(ui, P(0.1f, o), P(0.9f, o), t * 0.8f, c);
        }
        break;
    case UI_ICON_GLOBE:
        ui_draw_ring(ui, x + s * 0.5f, y + s * 0.5f, s * 0.38f, t, c, 20);
        ui_draw_line(ui, P(0.12f, 0.5f), P(0.88f, 0.5f), t, c);
        for (int i = 0; i < 12; i++) {
            float a0 = -LX_PI * 0.5f + (float)i / 12 * LX_PI, a1 = -LX_PI * 0.5f + (float)(i + 1) / 12 * LX_PI;
            ui_draw_line(ui, x + s * (0.5f + cosf(a0) * 0.16f), y + s * (0.5f + sinf(a0) * 0.38f), x + s * (0.5f + cosf(a1) * 0.16f),
                         y + s * (0.5f + sinf(a1) * 0.38f), t, c);
            ui_draw_line(ui, x + s * (0.5f - cosf(a0) * 0.16f), y + s * (0.5f + sinf(a0) * 0.38f), x + s * (0.5f - cosf(a1) * 0.16f),
                         y + s * (0.5f + sinf(a1) * 0.38f), t, c);
        }
        break;
    case UI_ICON_LOCAL:
        ui_draw_line(ui, P(0.2f, 0.8f), P(0.2f, 0.15f), t, UI_HEX(0x6cc04a));
        ui_draw_line(ui, P(0.2f, 0.8f), P(0.85f, 0.8f), t, UI_HEX(0xe0524d));
        ui_draw_line(ui, P(0.2f, 0.8f), P(0.6f, 0.45f), t, UI_HEX(0x4a8cf0));
        break;
    case UI_ICON_CHEVRON_RIGHT: case UI_ICON_CHEVRON_DOWN: case UI_ICON_CHEVRON_LEFT: case UI_ICON_CHEVRON_UP: {
        float pts[4][6] = {{0.38f, 0.22f, 0.66f, 0.5f, 0.38f, 0.78f}, {0.22f, 0.38f, 0.5f, 0.66f, 0.78f, 0.38f},
                           {0.62f, 0.22f, 0.34f, 0.5f, 0.62f, 0.78f}, {0.22f, 0.62f, 0.5f, 0.34f, 0.78f, 0.62f}};
        float *p = pts[icon - UI_ICON_CHEVRON_RIGHT];
        ui_draw_line(ui, P(p[0], p[1]), P(p[2], p[3]), t * 1.3f, c);
        ui_draw_line(ui, P(p[2], p[3]), P(p[4], p[5]), t * 1.3f, c);
        break;
    }
    case UI_ICON_PLUS:
        ui_draw_line(ui, P(0.5f, 0.18f), P(0.5f, 0.82f), t * 1.3f, c);
        ui_draw_line(ui, P(0.18f, 0.5f), P(0.82f, 0.5f), t * 1.3f, c);
        break;
    case UI_ICON_MINUS:
        ui_draw_line(ui, P(0.18f, 0.5f), P(0.82f, 0.5f), t * 1.3f, c);
        break;
    case UI_ICON_CLOSE:
        ui_draw_line(ui, P(0.24f, 0.24f), P(0.76f, 0.76f), t * 1.3f, c);
        ui_draw_line(ui, P(0.76f, 0.24f), P(0.24f, 0.76f), t * 1.3f, c);
        break;
    case UI_ICON_CHECK: case UI_ICON_SUCCESS:
        if (icon == UI_ICON_SUCCESS) { ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.42f, c, 20); c = UI_HEX(0x1b1c1f); }
        ui_draw_line(ui, P(0.24f, 0.52f), P(0.42f, 0.7f), t * 1.5f, c);
        ui_draw_line(ui, P(0.42f, 0.7f), P(0.78f, 0.3f), t * 1.5f, c);
        break;
    case UI_ICON_SEARCH:
        ui_draw_ring(ui, x + s * 0.42f, y + s * 0.42f, s * 0.24f, t * 1.2f, c, 16);
        ui_draw_line(ui, P(0.6f, 0.6f), P(0.86f, 0.86f), t * 1.6f, c);
        break;
    case UI_ICON_EYE: case UI_ICON_EYE_OFF:
        arc(ui, x + s * 0.5f, y + s * 0.95f, s * 0.6f, -0.78f * LX_PI, -0.22f * LX_PI, t, c, 10);
        arc(ui, x + s * 0.5f, y + s * 0.05f, s * 0.6f, 0.22f * LX_PI, 0.78f * LX_PI, t, c, 10);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.12f, c, 12);
        if (icon == UI_ICON_EYE_OFF) ui_draw_line(ui, P(0.15f, 0.85f), P(0.85f, 0.15f), t * 1.3f, c);
        break;
    case UI_ICON_LOCK:
        arc(ui, x + s * 0.5f, y + s * 0.42f, s * 0.18f, LX_PI, 2 * LX_PI, t * 1.2f, c, 10);
        ui_draw_line(ui, P(0.32f, 0.42f), P(0.32f, 0.5f), t * 1.2f, c);
        ui_draw_line(ui, P(0.68f, 0.42f), P(0.68f, 0.5f), t * 1.2f, c);
        ui_draw_rect(ui, x + s * 0.22f, y + s * 0.48f, s * 0.56f, s * 0.4f, c, s * 0.06f);
        break;
    case UI_ICON_GEAR:
        for (int i = 0; i < 8; i++) {
            float a = (float)i / 8 * 2 * LX_PI;
            ui_draw_line(ui, x + s * (0.5f + cosf(a) * 0.26f), y + s * (0.5f + sinf(a) * 0.26f), x + s * (0.5f + cosf(a) * 0.43f),
                         y + s * (0.5f + sinf(a) * 0.43f), s * 0.14f, c);
        }
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.3f, c, 20);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.12f, UI_HEX(0x1b1c1f), 12);
        break;
    case UI_ICON_BUILD:
        ui_draw_line(ui, P(0.2f, 0.82f), P(0.62f, 0.4f), s * 0.14f, c);
        ui_draw_rect(ui, x + s * 0.5f, y + s * 0.12f, s * 0.38f, s * 0.22f, c, s * 0.04f);
        ui_draw_triangle(ui, P(0.5f, 0.34f), P(0.7f, 0.34f), P(0.62f, 0.46f), c);
        break;
    case UI_ICON_BACK:
        ui_draw_line(ui, P(0.2f, 0.5f), P(0.82f, 0.5f), t * 1.3f, c);
        ui_draw_line(ui, P(0.2f, 0.5f), P(0.44f, 0.26f), t * 1.3f, c);
        ui_draw_line(ui, P(0.2f, 0.5f), P(0.44f, 0.74f), t * 1.3f, c);
        break;
    case UI_ICON_UP:
        ui_draw_line(ui, P(0.5f, 0.18f), P(0.5f, 0.82f), t * 1.3f, c);
        ui_draw_line(ui, P(0.5f, 0.18f), P(0.26f, 0.42f), t * 1.3f, c);
        ui_draw_line(ui, P(0.5f, 0.18f), P(0.74f, 0.42f), t * 1.3f, c);
        break;
    case UI_ICON_TRASH:
        ui_draw_rect(ui, x + s * 0.16f, y + s * 0.2f, s * 0.68f, s * 0.08f, c, 0);
        ui_draw_rect(ui, x + s * 0.4f, y + s * 0.1f, s * 0.2f, s * 0.1f, c, 0);
        ui_draw_rect(ui, x + s * 0.24f, y + s * 0.32f, s * 0.52f, s * 0.58f, c2, s * 0.05f);
        ui_draw_line(ui, P(0.4f, 0.42f), P(0.4f, 0.8f), t, c);
        ui_draw_line(ui, P(0.6f, 0.42f), P(0.6f, 0.8f), t, c);
        break;
    case UI_ICON_DOTS:
        for (int i = 0; i < 3; i++) ui_draw_circle(ui, x + s * (0.22f + i * 0.28f), y + s * 0.5f, s * 0.08f, c, 8);
        break;
    case UI_ICON_SAVE:
        ui_draw_rect(ui, x + s * 0.12f, y + s * 0.12f, s * 0.76f, s * 0.76f, c, s * 0.08f);
        ui_draw_rect(ui, x + s * 0.28f, y + s * 0.12f, s * 0.4f, s * 0.24f, UI_HEX(0x1b1c1f), 0);
        ui_draw_rect(ui, x + s * 0.26f, y + s * 0.54f, s * 0.48f, s * 0.26f, UI_HEX(0x1b1c1f), s * 0.03f);
        break;
    case UI_ICON_OPEN: case UI_ICON_EXTERNAL:
        ui_draw_border(ui, x + s * 0.12f, y + s * 0.24f, s * 0.62f, s * 0.64f, c2, s * 0.06f, t);
        ui_draw_line(ui, P(0.42f, 0.58f), P(0.86f, 0.14f), t * 1.2f, c);
        ui_draw_triangle(ui, P(0.9f, 0.1f), P(0.9f, 0.42f), P(0.58f, 0.1f), c);
        break;
    case UI_ICON_COPY:
        ui_draw_border(ui, x + s * 0.12f, y + s * 0.12f, s * 0.5f, s * 0.56f, c2, s * 0.06f, t);
        ui_draw_rect(ui, x + s * 0.36f, y + s * 0.32f, s * 0.5f, s * 0.56f, c, s * 0.06f);
        break;
    case UI_ICON_FILTER:
        ui_draw_triangle(ui, P(0.12f, 0.18f), P(0.88f, 0.18f), P(0.58f, 0.52f), c);
        ui_draw_triangle(ui, P(0.12f, 0.18f), P(0.58f, 0.52f), P(0.42f, 0.52f), c);
        ui_draw_rect(ui, x + s * 0.42f, y + s * 0.5f, s * 0.16f, s * 0.34f, c, 0);
        break;
    case UI_ICON_LIST:
        for (int i = 0; i < 3; i++) {
            ui_draw_rect(ui, x + s * 0.12f, y + s * (0.22f + i * 0.24f), s * 0.12f, s * 0.12f, c, 0);
            ui_draw_rect(ui, x + s * 0.32f, y + s * (0.24f + i * 0.24f), s * 0.56f, s * 0.08f, c, 0);
        }
        break;
    case UI_ICON_TILES:
        for (int i = 0; i < 4; i++) ui_draw_rect(ui, x + s * (0.14f + (i & 1) * 0.38f), y + s * (0.14f + (i >> 1) * 0.38f), s * 0.32f, s * 0.32f, c, s * 0.05f);
        break;
    case UI_ICON_LAYERS:
        for (int i = 2; i >= 0; i--) {
            float o = i * 0.16f;
            uint32_t cc = i ? c2 : c;
            ui_draw_triangle(ui, P(0.5f, 0.12f + o), P(0.9f, 0.34f + o), P(0.5f, 0.56f + o), cc);
            ui_draw_triangle(ui, P(0.5f, 0.12f + o), P(0.5f, 0.56f + o), P(0.1f, 0.34f + o), cc);
        }
        break;
    case UI_ICON_HOME:
        ui_draw_triangle(ui, P(0.5f, 0.1f), P(0.92f, 0.48f), P(0.08f, 0.48f), c);
        ui_draw_rect(ui, x + s * 0.2f, y + s * 0.46f, s * 0.6f, s * 0.42f, c, 0);
        ui_draw_rect(ui, x + s * 0.42f, y + s * 0.62f, s * 0.16f, s * 0.26f, UI_HEX(0x1b1c1f), 0);
        break;
    case UI_ICON_DRAG:
        for (int i = 0; i < 6; i++) ui_draw_circle(ui, x + s * (0.36f + (i & 1) * 0.28f), y + s * (0.24f + (i >> 1) * 0.26f), s * 0.07f, c, 8);
        break;
    case UI_ICON_HIERARCHY:
        ui_draw_rect(ui, x + s * 0.1f, y + s * 0.1f, s * 0.36f, s * 0.2f, c, s * 0.04f);
        ui_draw_rect(ui, x + s * 0.48f, y + s * 0.42f, s * 0.42f, s * 0.18f, c, s * 0.04f);
        ui_draw_rect(ui, x + s * 0.48f, y + s * 0.72f, s * 0.42f, s * 0.18f, c, s * 0.04f);
        ui_draw_line(ui, P(0.24f, 0.3f), P(0.24f, 0.81f), t, c2);
        ui_draw_line(ui, P(0.24f, 0.51f), P(0.48f, 0.51f), t, c2);
        ui_draw_line(ui, P(0.24f, 0.81f), P(0.48f, 0.81f), t, c2);
        break;
    case UI_ICON_INSPECTOR:
        ui_draw_rect(ui, x + s * 0.14f, y + s * 0.1f, s * 0.72f, s * 0.8f, c2, s * 0.06f);
        for (int i = 0; i < 3; i++) {
            ui_draw_rect(ui, x + s * 0.24f, y + s * (0.24f + i * 0.2f), s * 0.18f, s * 0.08f, c, 0);
            ui_draw_rect(ui, x + s * 0.46f, y + s * (0.24f + i * 0.2f), s * 0.3f, s * 0.08f, c, 0);
        }
        break;
    case UI_ICON_CONSOLE:
        ui_draw_rect(ui, x + s * 0.08f, y + s * 0.16f, s * 0.84f, s * 0.68f, c2, s * 0.08f);
        ui_draw_line(ui, P(0.22f, 0.36f), P(0.36f, 0.5f), t * 1.2f, c);
        ui_draw_line(ui, P(0.36f, 0.5f), P(0.22f, 0.64f), t * 1.2f, c);
        ui_draw_rect(ui, x + s * 0.44f, y + s * 0.6f, s * 0.3f, t * 1.2f, c, 0);
        break;
    case UI_ICON_VIEWPORT: case UI_ICON_MONITOR:
        ui_draw_rect(ui, x + s * 0.08f, y + s * 0.14f, s * 0.84f, s * 0.56f, c, s * 0.06f);
        ui_draw_rect(ui, x + s * 0.14f, y + s * 0.2f, s * 0.72f, s * 0.44f, UI_HEX(0x1b1c1f), s * 0.02f);
        ui_draw_rect(ui, x + s * 0.42f, y + s * 0.7f, s * 0.16f, s * 0.12f, c, 0);
        ui_draw_rect(ui, x + s * 0.28f, y + s * 0.82f, s * 0.44f, s * 0.06f, c, s * 0.03f);
        if (icon == UI_ICON_VIEWPORT) ui_draw_triangle(ui, P(0.24f, 0.6f), P(0.42f, 0.34f), P(0.58f, 0.6f), c);
        break;
    case UI_ICON_STATS:
        for (int i = 0; i < 4; i++) {
            float hgt = (float[]){0.3f, 0.55f, 0.4f, 0.72f}[i];
            ui_draw_rect(ui, x + s * (0.12f + i * 0.2f), y + s * (0.86f - hgt), s * 0.14f, s * hgt, c, s * 0.02f);
        }
        break;
    case UI_ICON_INFO:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.42f, c, 20);
        ui_draw_rect(ui, x + s * 0.44f, y + s * 0.42f, s * 0.12f, s * 0.34f, UI_HEX(0x1b1c1f), 0);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.28f, s * 0.07f, UI_HEX(0x1b1c1f), 8);
        break;
    case UI_ICON_WARNING:
        ui_draw_triangle(ui, P(0.5f, 0.08f), P(0.94f, 0.88f), P(0.06f, 0.88f), c);
        ui_draw_rect(ui, x + s * 0.45f, y + s * 0.34f, s * 0.1f, s * 0.3f, UI_HEX(0x1b1c1f), 0);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.75f, s * 0.06f, UI_HEX(0x1b1c1f), 8);
        break;
    case UI_ICON_ERROR:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.5f, s * 0.42f, c, 20);
        ui_draw_line(ui, P(0.34f, 0.34f), P(0.66f, 0.66f), t * 1.5f, UI_HEX(0x1b1c1f));
        ui_draw_line(ui, P(0.66f, 0.34f), P(0.34f, 0.66f), t * 1.5f, UI_HEX(0x1b1c1f));
        break;
    case UI_ICON_KEYBOARD:
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.24f, s * 0.88f, s * 0.52f, c2, s * 0.08f);
        for (int r = 0; r < 2; r++)
            for (int q = 0; q < 5; q++) ui_draw_rect(ui, x + s * (0.14f + q * 0.15f), y + s * (0.32f + r * 0.14f), s * 0.1f, s * 0.09f, c, 0);
        ui_draw_rect(ui, x + s * 0.26f, y + s * 0.6f, s * 0.48f, s * 0.08f, c, 0);
        break;
    case UI_ICON_MOUSE:
        ui_draw_rect(ui, x + s * 0.26f, y + s * 0.1f, s * 0.48f, s * 0.8f, c2, s * 0.24f);
        ui_draw_line(ui, P(0.5f, 0.16f), P(0.5f, 0.4f), t, c);
        ui_draw_line(ui, P(0.28f, 0.42f), P(0.72f, 0.42f), t, c);
        break;
    case UI_ICON_GAMEPAD: case UI_ICON_JOYSTICK:
        ui_draw_rect(ui, x + s * 0.06f, y + s * 0.3f, s * 0.88f, s * 0.42f, c, s * 0.2f);
        ui_draw_circle(ui, x + s * 0.2f, y + s * 0.66f, s * 0.14f, c, 12);
        ui_draw_circle(ui, x + s * 0.8f, y + s * 0.66f, s * 0.14f, c, 12);
        ui_draw_rect(ui, x + s * 0.18f, y + s * 0.47f, s * 0.16f, s * 0.05f, UI_HEX(0x1b1c1f), 0);
        ui_draw_rect(ui, x + s * 0.235f, y + s * 0.415f, s * 0.05f, s * 0.16f, UI_HEX(0x1b1c1f), 0);
        ui_draw_circle(ui, x + s * 0.7f, y + s * 0.44f, s * 0.04f, UI_HEX(0x1b1c1f), 8);
        ui_draw_circle(ui, x + s * 0.78f, y + s * 0.52f, s * 0.04f, UI_HEX(0x1b1c1f), 8);
        break;
    case UI_ICON_TOUCH:
        ui_draw_ring(ui, x + s * 0.44f, y + s * 0.3f, s * 0.18f, t * 0.8f, c2, 14);
        ui_draw_rect(ui, x + s * 0.38f, y + s * 0.26f, s * 0.12f, s * 0.42f, c, s * 0.06f);
        ui_draw_rect(ui, x + s * 0.38f, y + s * 0.5f, s * 0.38f, s * 0.38f, c, s * 0.12f);
        break;
    case UI_ICON_WINDOWS:
        ui_draw_rect(ui, x + s * 0.12f, y + s * 0.14f, s * 0.35f, s * 0.34f, c, 0);
        ui_draw_rect(ui, x + s * 0.53f, y + s * 0.1f, s * 0.36f, s * 0.38f, c, 0);
        ui_draw_rect(ui, x + s * 0.12f, y + s * 0.52f, s * 0.35f, s * 0.34f, c, 0);
        ui_draw_rect(ui, x + s * 0.53f, y + s * 0.52f, s * 0.36f, s * 0.38f, c, 0);
        break;
    case UI_ICON_LINUX:
        ui_draw_rect(ui, x + s * 0.26f, y + s * 0.1f, s * 0.48f, s * 0.78f, c, s * 0.24f);
        ui_draw_rect(ui, x + s * 0.34f, y + s * 0.4f, s * 0.32f, s * 0.42f, UI_HEX(0xe8e8e8), s * 0.15f);
        ui_draw_circle(ui, x + s * 0.42f, y + s * 0.26f, s * 0.04f, UI_HEX(0xffffff), 8);
        ui_draw_circle(ui, x + s * 0.58f, y + s * 0.26f, s * 0.04f, UI_HEX(0xffffff), 8);
        ui_draw_triangle(ui, P(0.44f, 0.32f), P(0.56f, 0.32f), P(0.5f, 0.38f), UI_HEX(0xf2b640));
        ui_draw_rect(ui, x + s * 0.2f, y + s * 0.84f, s * 0.22f, s * 0.08f, UI_HEX(0xf2b640), s * 0.04f);
        ui_draw_rect(ui, x + s * 0.58f, y + s * 0.84f, s * 0.22f, s * 0.08f, UI_HEX(0xf2b640), s * 0.04f);
        break;
    case UI_ICON_ANDROID:
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.44f, s * 0.24f, c, 20);
        ui_draw_rect(ui, x + s * 0.2f, y + s * 0.44f, s * 0.6f, s * 0.36f, c, s * 0.04f);
        ui_draw_rect(ui, x + s * 0.2f, y + s * 0.44f, s * 0.6f, s * 0.04f, UI_HEX(0x1b1c1f), 0);
        ui_draw_circle(ui, x + s * 0.4f, y + s * 0.32f, s * 0.035f, UI_HEX(0x1b1c1f), 8);
        ui_draw_circle(ui, x + s * 0.6f, y + s * 0.32f, s * 0.035f, UI_HEX(0x1b1c1f), 8);
        ui_draw_line(ui, P(0.34f, 0.18f), P(0.28f, 0.08f), t, c);
        ui_draw_line(ui, P(0.66f, 0.18f), P(0.72f, 0.08f), t, c);
        break;
    case UI_ICON_PHONE:
        ui_draw_rect(ui, x + s * 0.28f, y + s * 0.06f, s * 0.44f, s * 0.88f, c, s * 0.08f);
        ui_draw_rect(ui, x + s * 0.32f, y + s * 0.14f, s * 0.36f, s * 0.66f, UI_HEX(0x1b1c1f), 0);
        ui_draw_circle(ui, x + s * 0.5f, y + s * 0.87f, s * 0.035f, UI_HEX(0x1b1c1f), 8);
        break;
    }
}
#undef P

/* ======================================================================
 *  nodes
 * ====================================================================== */
static UiNode *alloc_node(UiCtx *ui, UiType type) {
    UiNode *n = lx_calloc(1, sizeof *n);
    n->type = type;
    n->ctx = ui;
    n->dir = UI_COL;
    n->font = UI_FONT_BODY;
    n->fg = ui->theme.text;
    n->split = 0.5f;
    n->split_min = 80;
    n->step = 0.1f;
    n->precision = 3;
    n->maxv = 1.0f;
    return n;
}

UiNode *ui_new(UiCtx *ui, UiNode *parent, UiType type) {
    UiNode *n = alloc_node(ui, type);
    if (parent) {
        n->parent = parent;
        n->prev = parent->last;
        if (parent->last) parent->last->next = n;
        else parent->first = n;
        parent->last = n;
    }
    return n;
}

static void free_tree(UiCtx *ui, UiNode *n);
static void forget(UiCtx *ui, UiNode *n) {
    if (ui->hot == n) ui->hot = NULL;
    if (ui->active == n) ui->active = NULL;
    if (ui->focus == n) ui->focus = NULL;
    if (ui->last_click_node == n) ui->last_click_node = NULL;
    if (ui->tooltip_node == n) ui->tooltip_node = NULL;
    if (ui->dnd_source == n) ui->dnd_source = NULL;
    /* a dropdown list or colour picker whose field is going away goes with it: its items point at the field */
    for (int i = 0; i < ui->popup_count; i++)
        if (ui->popups[i] != n && ui->popups[i]->owner == n) {
            UiNode *p = ui->popups[i];
            ui->popups[i] = ui->popups[--ui->popup_count];
            p->owner = NULL;
            free_tree(ui, p);
            i = -1;   /* the list changed */
        }
}

static void free_tree(UiCtx *ui, UiNode *n) {
    UiNode *c = n->first;
    while (c) {
        UiNode *next = c->next;
        free_tree(ui, c);
        c = next;
    }
    forget(ui, n);
    lx_free(n);
}

void ui_remove(UiCtx *ui, UiNode *n) {
    if (!n) return;
    if (n->parent) {
        if (n->prev) n->prev->next = n->next;
        else n->parent->first = n->next;
        if (n->next) n->next->prev = n->prev;
        else n->parent->last = n->prev;
    }
    for (int i = 0; i < ui->popup_count; i++)
        if (ui->popups[i] == n) { ui->popups[i] = ui->popups[--ui->popup_count]; break; }
    if (ui->modal == n) ui->modal = NULL;
    free_tree(ui, n);
}

void ui_clear(UiCtx *ui, UiNode *n) {
    while (n->first) ui_remove(ui, n->first);
}

void ui_set_text(UiNode *n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->text, sizeof n->text, fmt, ap);
    va_end(ap);
}

void ui_pad(UiNode *n, float l, float t, float r, float b) { n->pad[0] = l; n->pad[1] = t; n->pad[2] = r; n->pad[3] = b; }

UiNode *ui_box(UiCtx *ui, UiNode *parent, int dir) {
    UiNode *n = ui_new(ui, parent, UI_BOX);
    n->dir = dir;
    return n;
}
UiNode *ui_spacer(UiCtx *ui, UiNode *parent) {
    UiNode *n = ui_new(ui, parent, UI_BOX);
    n->grow = 1;
    return n;
}
UiNode *ui_label(UiCtx *ui, UiNode *parent, const char *text) {
    UiNode *n = ui_new(ui, parent, UI_LABEL);
    lx_strcpy(n->text, sizeof n->text, text);
    return n;
}
UiNode *ui_button(UiCtx *ui, UiNode *parent, const char *text, void (*cb)(UiNode *)) {
    UiNode *n = ui_new(ui, parent, UI_BUTTON);
    lx_strcpy(n->text, sizeof n->text, text);
    n->on_click = cb;
    n->bg = ui->theme.panel_alt;
    n->radius = 4;
    n->center_text = true;
    return n;
}
UiNode *ui_icon_button(UiCtx *ui, UiNode *parent, UiIcon icon, const char *tooltip, void (*cb)(UiNode *)) {
    UiNode *n = ui_new(ui, parent, UI_BUTTON);
    n->icon = icon;
    n->on_click = cb;
    n->radius = 4;
    n->w = 26;
    n->h = 26;
    n->center_text = true;
    if (tooltip) lx_strcpy(n->tooltip, sizeof n->tooltip, tooltip);
    return n;
}
UiNode *ui_checkbox(UiCtx *ui, UiNode *parent, const char *label, bool *v) {
    UiNode *n = ui_new(ui, parent, UI_CHECKBOX);
    lx_strcpy(n->text, sizeof n->text, label ? label : "");
    n->b = v;
    return n;
}
UiNode *ui_textfield(UiCtx *ui, UiNode *parent, char *buf, int cap) {
    UiNode *n = ui_new(ui, parent, UI_TEXTFIELD);
    n->s = buf;
    n->s_cap = cap;
    n->radius = 3;
    return n;
}
UiNode *ui_number(UiCtx *ui, UiNode *parent, float *v, float step) {
    UiNode *n = ui_new(ui, parent, UI_NUMBER);
    n->f = v;
    n->step = step;
    n->minv = -1e30f;
    n->maxv = 1e30f;
    n->radius = 3;
    return n;
}
UiNode *ui_int(UiCtx *ui, UiNode *parent, int *v, int minv, int maxv) {
    UiNode *n = ui_new(ui, parent, UI_NUMBER);
    n->i = v;
    n->step = 0.25f;
    n->minv = (float)minv;
    n->maxv = (float)maxv;
    n->radius = 3;
    return n;
}
UiNode *ui_slider(UiCtx *ui, UiNode *parent, float *v, float minv, float maxv) {
    UiNode *n = ui_new(ui, parent, UI_SLIDER);
    n->f = v;
    n->minv = minv;
    n->maxv = maxv;
    n->radius = 3;
    n->precision = 2;
    return n;
}
UiNode *ui_dropdown(UiCtx *ui, UiNode *parent, int *v, const char *const *opts, int count) {
    UiNode *n = ui_new(ui, parent, UI_DROPDOWN);
    n->i = v;
    n->options = opts;
    n->option_count = count;
    n->radius = 3;
    return n;
}
UiNode *ui_image(UiCtx *ui, UiNode *parent, uint32_t tex) {
    UiNode *n = ui_new(ui, parent, UI_IMAGE);
    n->texture = tex;
    return n;
}
UiNode *ui_separator(UiCtx *ui, UiNode *parent) { return ui_new(ui, parent, UI_SEPARATOR); }
UiNode *ui_color(UiCtx *ui, UiNode *parent, float *rgb, bool alpha) {
    UiNode *n = ui_new(ui, parent, UI_COLOR);
    n->f = rgb;
    n->tag = alpha ? 4 : 3;
    n->radius = 3;
    return n;
}
UiNode *ui_split(UiCtx *ui, UiNode *parent, int dir, float ratio) {
    UiNode *n = ui_new(ui, parent, UI_SPLIT);
    n->dir = dir;
    n->split = ratio;
    n->grow = 1;
    return n;
}
UiNode *ui_tabs(UiCtx *ui, UiNode *parent) {
    UiNode *n = ui_new(ui, parent, UI_TABS);
    n->grow = 1;
    return n;
}

/* ======================================================================
 *  layout
 * ====================================================================== */
static float S(UiCtx *ui, float v) { return v * ui->scale; }
#define ROW_H 24.0f

static bool is_field(UiType t) { return t == UI_TEXTFIELD || t == UI_NUMBER || t == UI_SLIDER || t == UI_DROPDOWN || t == UI_COLOR; }

/* intrinsic size in physical pixels */
static void measure(UiCtx *ui, UiNode *n, float *ow, float *oh) {
    float pw = S(ui, n->pad[0] + n->pad[2]), ph = S(ui, n->pad[1] + n->pad[3]);
    float w = 0, h = 0;
    float icon = n->icon ? S(ui, n->icon_size > 0 ? n->icon_size : 16) : 0;
    switch (n->type) {
    case UI_LABEL: {
        float tw = n->text[0] ? ceilf(ui_text_width(ui, n->font, n->text)) + 1.0f : 0;
        w = tw + icon + (icon > 0 && tw > 0 ? S(ui, 6) : 0);
        h = lx_maxf(ui_line_height(ui, n->font), icon);
        h = lx_maxf(h, n->font == UI_FONT_TITLE ? 0 : S(ui, 20));
        break;
    }
    case UI_BUTTON: {
        float tw = n->text[0] ? ui_text_width(ui, n->font, n->text) : 0;
        w = tw + icon + (icon > 0 && tw > 0 ? S(ui, 6) : 0) + S(ui, n->text[0] ? 20 : 8);
        h = S(ui, ROW_H);
        break;
    }
    case UI_CHECKBOX:
        w = S(ui, 16) + (n->text[0] ? S(ui, 8) + ui_text_width(ui, n->font, n->text) : 0);
        h = S(ui, 22);
        break;
    case UI_TEXTFIELD: case UI_NUMBER: case UI_SLIDER: case UI_DROPDOWN: case UI_COLOR:
        w = S(ui, n->type == UI_COLOR ? 44 : 60);
        h = S(ui, 22);
        break;
    case UI_IMAGE: w = S(ui, 64); h = S(ui, 64); break;
    case UI_SEPARATOR: w = S(ui, 9); h = S(ui, 9); break;
    case UI_CUSTOM: w = S(ui, 32); h = S(ui, 32); break;
    case UI_SPLIT: case UI_TABS: w = S(ui, 100); h = S(ui, 100); break;
    case UI_BOX: {
        int count = 0;
        for (UiNode *c = n->first; c; c = c->next) {
            if (c->hidden) continue;
            float cw, chh;
            measure(ui, c, &cw, &chh);
            if (c->w > 0) cw = S(ui, c->w);
            if (c->h > 0) chh = S(ui, c->h);
            if (n->dir == UI_ROW && !n->wrap) { w += cw; h = lx_maxf(h, chh); }
            else { h += chh; w = lx_maxf(w, cw); }
            count++;
        }
        if (count > 1) {
            if (n->dir == UI_ROW && !n->wrap) w += S(ui, n->gap) * (float)(count - 1);
            else h += S(ui, n->gap) * (float)(count - 1);
        }
        if (n->scroll || n->wrap) { if (n->dir == UI_COL || n->wrap) h = lx_minf(h, S(ui, 60)); }
        break;
    }
    }
    w += pw;
    h += ph;
    if (n->min_w > 0) w = lx_maxf(w, S(ui, n->min_w));
    *ow = w;
    *oh = h;
}

static void layout(UiCtx *ui, UiNode *n, float x, float y, float w, float h);

static void layout_children(UiCtx *ui, UiNode *n) {
    float ix = n->x + S(ui, n->pad[0]), iy = n->y + S(ui, n->pad[1]);
    float iw = lx_maxf(0, n->cw - S(ui, n->pad[0] + n->pad[2])), ih = lx_maxf(0, n->ch - S(ui, n->pad[1] + n->pad[3]));
    float gap = S(ui, n->gap);
    if (n->type == UI_SPLIT) {
        UiNode *a = n->first, *b = a ? a->next : NULL;
        if (!a) return;
        float div = S(ui, 4);
        float total = n->dir == UI_ROW ? iw : ih;
        float mn = lx_minf(S(ui, n->split_min), total * 0.45f);
        float first = lx_clampf(total * n->split, mn, total - mn - div);
        if (!b) { layout(ui, a, ix, iy, iw, ih); return; }
        if (n->dir == UI_ROW) {
            layout(ui, a, ix, iy, first, ih);
            layout(ui, b, ix + first + div, iy, iw - first - div, ih);
        } else {
            layout(ui, a, ix, iy, iw, first);
            layout(ui, b, ix, iy + first + div, iw, ih - first - div);
        }
        return;
    }
    if (n->type == UI_TABS) {
        float th = S(ui, 30);
        int i = 0;
        for (UiNode *c = n->first; c; c = c->next, i++) {
            c->hidden = i != n->tab_index;
            if (!c->hidden) layout(ui, c, ix, iy + th, iw, ih - th);
        }
        return;
    }
    if (n->wrap) {
        float cx = ix, cy = iy - n->scroll_pos, row = 0;
        for (UiNode *c = n->first; c; c = c->next) {
            if (c->hidden) continue;
            float cw, chh;
            measure(ui, c, &cw, &chh);
            if (c->w > 0) cw = S(ui, c->w);
            if (c->h > 0) chh = S(ui, c->h);
            if (cx > ix && cx + cw > ix + iw) { cx = ix; cy += row + gap; row = 0; }
            layout(ui, c, cx, cy, cw, chh);
            cx += cw + gap;
            row = lx_maxf(row, chh);
        }
        n->content = (cy + row + n->scroll_pos) - iy;
        n->scroll_max = lx_maxf(0, n->content - ih);
        return;
    }
    bool row_dir = n->dir == UI_ROW;
    float avail = row_dir ? iw : ih;
    float used = 0, grow_total = 0;
    int count = 0;
    float sizes_static[256];
    float *sizes = sizes_static;
    int nchild = 0;
    for (UiNode *c = n->first; c; c = c->next) nchild++;
    if (nchild > 256) sizes = lx_alloc(sizeof(float) * (size_t)nchild);
    int k = 0;
    for (UiNode *c = n->first; c; c = c->next, k++) {
        if (c->hidden) { sizes[k] = 0; continue; }
        float cw, chh;
        measure(ui, c, &cw, &chh);
        float fixed = row_dir ? c->w : c->h;
        float main = fixed > 0 ? S(ui, fixed) : (c->grow > 0 ? 0 : (row_dir ? cw : chh));
        if (c->grow > 0 && fixed <= 0) {
            /* growing fields still need their minimum */
            if (row_dir && is_field(c->type)) main = 0;
        }
        sizes[k] = main;
        used += main;
        grow_total += (fixed > 0) ? 0 : c->grow;
        count++;
    }
    if (count > 1) used += gap * (float)(count - 1);
    float left = n->scroll ? 0 : lx_maxf(0, avail - used);
    float pos = (row_dir ? ix : iy) - (n->scroll ? n->scroll_pos : 0);
    if (grow_total <= 0 && left > 0 && n->align != UI_ALIGN_START) pos += n->align == UI_ALIGN_CENTER ? left * 0.5f : left;
    k = 0;
    for (UiNode *c = n->first; c; c = c->next, k++) {
        if (c->hidden) continue;
        float main = sizes[k];
        float fixed = row_dir ? c->w : c->h;
        if (c->grow > 0 && fixed <= 0 && grow_total > 0) main += left * c->grow / grow_total;
        float cross_fixed = row_dir ? c->h : c->w;
        float cross_avail = row_dir ? ih : iw;
        float cross = cross_fixed > 0 ? lx_minf(S(ui, cross_fixed), cross_avail) : cross_avail;
        float cross_off = 0;
        if (cross_fixed > 0 && n->cross_align != UI_ALIGN_START)
            cross_off = (cross_avail - cross) * (n->cross_align == UI_ALIGN_CENTER ? 0.5f : 1.0f);
        if (row_dir) layout(ui, c, pos, iy + cross_off, main, cross);
        else layout(ui, c, ix + cross_off, pos, cross, main);
        pos += main + gap;
    }
    if (sizes != sizes_static) lx_free(sizes);
    n->content = used;
    n->scroll_max = n->scroll ? lx_maxf(0, used - avail) : 0;
}

static void layout(UiCtx *ui, UiNode *n, float x, float y, float w, float h) {
    n->x = x;
    n->y = y;
    n->cw = lx_maxf(0, w);
    n->ch = lx_maxf(0, h);
    n->scroll_pos = lx_clampf(n->scroll_pos, 0, n->scroll_max);
    if (n->first) layout_children(ui, n);
    if (n->scroll) n->scroll_pos = lx_clampf(n->scroll_pos, 0, n->scroll_max);
}

/* ======================================================================
 *  hit testing
 * ====================================================================== */
static bool inside(UiNode *n, LxVec2 p) { return p.x >= n->x && p.y >= n->y && p.x < n->x + n->cw && p.y < n->y + n->ch; }

static bool interactive(UiNode *n) {
    if (n->disabled) return false;
    switch (n->type) {
    case UI_BUTTON: case UI_CHECKBOX: case UI_TEXTFIELD: case UI_NUMBER: case UI_SLIDER: case UI_DROPDOWN:
    case UI_COLOR: case UI_SPLIT: case UI_TABS: case UI_CUSTOM: return true;
    default: return n->on_click || n->on_double_click || n->on_right_click || n->drag_type || n->accept || n->tooltip[0] || n->scroll || n->wrap;
    }
}

static UiNode *hit(UiNode *n, LxVec2 p) {
    if (n->hidden || !inside(n, p)) return NULL;
    for (UiNode *c = n->last; c; c = c->prev) {
        UiNode *h = hit(c, p);
        if (h) return h;
    }
    return interactive(n) ? n : NULL;
}

static bool split_divider_hit(UiCtx *ui, UiNode *n, LxVec2 p) {
    if (n->type != UI_SPLIT || !n->first || !n->first->next) return false;
    UiNode *a = n->first;
    float grab = S(ui, 4);
    if (n->dir == UI_ROW) return p.x >= a->x + a->cw - grab && p.x <= a->x + a->cw + S(ui, 4) + grab && p.y >= n->y && p.y < n->y + n->ch;
    return p.y >= a->y + a->ch - grab && p.y <= a->y + a->ch + S(ui, 4) + grab && p.x >= n->x && p.x < n->x + n->cw;
}

/* splits are found even when a child covers the divider */
static UiNode *find_split(UiCtx *ui, UiNode *n, LxVec2 p) {
    if (n->hidden || !inside(n, p)) return NULL;
    for (UiNode *c = n->last; c; c = c->prev) {
        UiNode *s = find_split(ui, c, p);
        if (s) return s;
    }
    return split_divider_hit(ui, n, p) ? n : NULL;
}

/* ======================================================================
 *  popups, modals, toasts
 * ====================================================================== */
UiNode *ui_popup(UiCtx *ui, float x, float y, float min_w) {
    if (ui->popup_count >= MAX_POPUPS) return NULL;
    UiNode *p = alloc_node(ui, UI_BOX);
    p->dir = UI_COL;
    p->bg = ui->theme.panel_alt;
    p->border = ui->theme.border_light;
    p->radius = 6;
    ui_pad(p, 4, 4, 4, 4);
    p->gap = 1;
    p->x = x;
    p->y = y;
    p->min_w = min_w;
    p->tag = 0x706f70; /* popup marker */
    ui->popups[ui->popup_count++] = p;
    return p;
}

UiNode *ui_menu_item(UiCtx *ui, UiNode *popup, const char *text, const char *shortcut, void (*cb)(UiNode *)) {
    UiNode *b = ui_new(ui, popup, UI_BUTTON);
    lx_strcpy(b->text, sizeof b->text, text);
    if (shortcut) lx_strcpy(b->hint, sizeof b->hint, shortcut);
    b->on_click = cb;
    b->radius = 4;
    b->h = 26;
    b->menu_item = true; /* closes popups when clicked (tag stays free for the caller) */
    return b;
}

UiNode *ui_menu_separator(UiCtx *ui, UiNode *popup) {
    UiNode *s = ui_new(ui, popup, UI_SEPARATOR);
    return s;
}

void ui_close_popups(UiCtx *ui) {
    while (ui->popup_count) ui_remove(ui, ui->popups[ui->popup_count - 1]);
}
bool ui_popup_open(UiCtx *ui) { return ui->popup_count > 0; }

UiNode *ui_modal(UiCtx *ui, const char *title, float w, float h) {
    ui_close_modal(ui);
    UiNode *m = alloc_node(ui, UI_BOX);
    m->dir = UI_COL;
    m->bg = ui->theme.panel;
    m->border = ui->theme.border_light;
    m->radius = 8;
    m->w = w;
    m->h = h;
    UiNode *header = ui_box(ui, m, UI_ROW);
    header->h = 40;
    ui_pad(header, 16, 0, 8, 0);
    header->cross_align = UI_ALIGN_CENTER;
    UiNode *t = ui_label(ui, header, title);
    t->font = UI_FONT_BOLD;
    t->grow = 1;
    ui->modal = m;
    UiNode *body = ui_box(ui, m, UI_COL);
    body->grow = 1;
    ui_pad(body, 16, 8, 16, 16);
    body->gap = 8;
    return body;
}
void ui_close_modal(UiCtx *ui) {
    if (ui->modal) {
        UiNode *m = ui->modal;
        ui->modal = NULL;
        free_tree(ui, m);
    }
}
bool ui_has_modal(UiCtx *ui) { return ui->modal != NULL; }
UiNode *ui_modal_node(UiCtx *ui) { return ui->modal; }

void ui_notify(UiCtx *ui, const char *fmt, ...) {
    if (ui->toast_count == MAX_TOASTS) {
        memmove(ui->toasts, ui->toasts + 1, sizeof(Toast) * (MAX_TOASTS - 1));
        ui->toast_count--;
    }
    Toast *t = &ui->toasts[ui->toast_count++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->text, sizeof t->text, fmt, ap);
    va_end(ap);
    t->time = 0;
}

/* ======================================================================
 *  queries
 * ====================================================================== */
bool ui_wants_keyboard(UiCtx *ui) { return ui->focus && (ui->focus->type == UI_TEXTFIELD || ui->focus->editing); }
bool ui_wants_mouse(UiCtx *ui) { return ui->popup_count > 0 || ui->modal || (ui->hot && ui->hot->type != UI_CUSTOM); }
bool ui_is_hot(UiCtx *ui, UiNode *n) { return ui->hot == n; }
UiNode *ui_hot_node(UiCtx *ui) { return ui->hot; }
void ui_set_app_icon(UiCtx *ui, uint32_t texture) { ui->app_icon = texture; }
bool ui_overlay_open(UiCtx *ui) { return ui->popup_count > 0 || ui->modal != NULL; }
bool ui_is_inside(UiCtx *ui, UiNode *n) { return inside(n, ui->mouse); }
UiNode *ui_active(UiCtx *ui) { return ui->active; }
const char *ui_dragging(UiCtx *ui, const char **payload) {
    if (!ui->dnd_active) return NULL;
    if (payload) *payload = ui->dnd_payload;
    return ui->dnd_type;
}

/* ======================================================================
 *  text editing
 * ====================================================================== */
static char *edit_buf(UiNode *n, int *cap) {
    if (n->type == UI_TEXTFIELD) { *cap = n->s_cap; return n->s; }
    *cap = (int)sizeof n->edit_backup;
    return n->edit_backup; /* numbers edit a temporary string */
}

static void value_to_text(UiNode *n, char *out, size_t cap) {
    if (n->i) snprintf(out, cap, "%d", *n->i);
    else if (n->f) {
        snprintf(out, cap, "%.*f", n->precision, *n->f);
        /* trim trailing zeros */
        char *dot = strchr(out, '.');
        if (dot) {
            char *e = out + strlen(out) - 1;
            while (e > dot + 1 && *e == '0') *e-- = 0;
        }
    } else out[0] = 0;
}

static void begin_edit(UiCtx *ui, UiNode *n) {
    ui->focus = n;
    n->editing = true;
    if (n->type == UI_NUMBER) value_to_text(n, n->edit_backup, sizeof n->edit_backup);
    else if (n->s) lx_strcpy(n->edit_backup, sizeof n->edit_backup, n->s);
    int cap;
    char *b = edit_buf(n, &cap);
    n->caret = b ? (int)strlen(b) : 0;
    n->sel_anchor = 0; /* select all */
}

static void commit_number(UiNode *n) {
    char *end;
    float v = strtof(n->edit_backup, &end);
    if (end == n->edit_backup) return;
    if (!isfinite(v)) return;   /* "nan" / "inf" would break whatever the field drives */
    v = lx_clampf(v, n->minv, n->maxv);
    if (n->i) *n->i = (int)lroundf(v);
    else if (n->f) *n->f = v;
    if (n->on_change) n->on_change(n);
}

static void end_edit(UiCtx *ui, UiNode *n, bool commit) {
    if (!n->editing) return;
    n->editing = false;
    if (n->type == UI_NUMBER) { if (commit) commit_number(n); }
    else if (n->type == UI_TEXTFIELD) {
        if (!commit && n->s) lx_strcpy(n->s, (size_t)n->s_cap, n->edit_backup);
        else if (commit && n->on_change) n->on_change(n);
    }
    if (ui->focus == n) ui->focus = NULL;
}

void ui_focus(UiCtx *ui, UiNode *n) {
    if (ui->focus && ui->focus != n) end_edit(ui, ui->focus, true);
    if (n && (n->type == UI_TEXTFIELD || n->type == UI_NUMBER)) begin_edit(ui, n);
    else ui->focus = n;
}

static void delete_selection(char *b, UiNode *n) {
    int a = n->sel_anchor < n->caret ? n->sel_anchor : n->caret, z = n->sel_anchor < n->caret ? n->caret : n->sel_anchor;
    if (a == z) return;
    memmove(b + a, b + z, strlen(b + z) + 1);
    n->caret = n->sel_anchor = a;
}

static void handle_text_keys(UiCtx *ui, UiNode *n) {
    int cap;
    char *b = edit_buf(n, &cap);
    if (!b) return;
    int len = (int)strlen(b);
    if (n->caret > len) n->caret = len;
    if (n->sel_anchor > len) n->sel_anchor = len;
    bool shift = lx_key_down(LX_KEY_SHIFT), ctrl = lx_key_down(LX_KEY_CTRL);
    int count;
    const uint32_t *chars = lx_text_input(&count);
    if (ctrl && lx_key_pressed(LX_KEY_A)) { n->sel_anchor = 0; n->caret = len; return; }
    if (ctrl && lx_key_pressed(LX_KEY_C)) {
        int a = n->sel_anchor < n->caret ? n->sel_anchor : n->caret, z = n->sel_anchor < n->caret ? n->caret : n->sel_anchor;
        char tmp[512];
        int l = z - a < 511 ? z - a : 511;
        memcpy(tmp, b + a, (size_t)l);
        tmp[l] = 0;
        lx_platform_clipboard_set(tmp);
        return;
    }
    if (ctrl && (lx_key_pressed(LX_KEY_V) || lx_key_pressed(LX_KEY_X))) {
        if (lx_key_pressed(LX_KEY_X)) {
            int a = n->sel_anchor < n->caret ? n->sel_anchor : n->caret, z = n->sel_anchor < n->caret ? n->caret : n->sel_anchor;
            char tmp[512];
            int l = z - a < 511 ? z - a : 511;
            memcpy(tmp, b + a, (size_t)l);
            tmp[l] = 0;
            lx_platform_clipboard_set(tmp);
            delete_selection(b, n);
            return;
        }
        delete_selection(b, n);
        const char *clip = lx_platform_clipboard_get();
        len = (int)strlen(b);
        int add = (int)strlen(clip);
        if (len + add >= cap) add = cap - 1 - len;
        if (add > 0) {
            memmove(b + n->caret + add, b + n->caret, (size_t)(len - n->caret + 1));
            memcpy(b + n->caret, clip, (size_t)add);
            n->caret += add;
            n->sel_anchor = n->caret;
        }
        return;
    }
    for (int i = 0; i < count && !ctrl; i++) {
        uint32_t cp = chars[i];
        char enc[4];
        int el = 0;
        if (cp < 0x80) enc[el++] = (char)cp;
        else if (cp < 0x800) { enc[el++] = (char)(0xc0 | (cp >> 6)); enc[el++] = (char)(0x80 | (cp & 0x3f)); }
        else { enc[el++] = (char)(0xe0 | (cp >> 12)); enc[el++] = (char)(0x80 | ((cp >> 6) & 0x3f)); enc[el++] = (char)(0x80 | (cp & 0x3f)); }
        delete_selection(b, n);
        len = (int)strlen(b);
        if (len + el >= cap) break;
        memmove(b + n->caret + el, b + n->caret, (size_t)(len - n->caret + 1));
        memcpy(b + n->caret, enc, (size_t)el);
        n->caret += el;
        n->sel_anchor = n->caret;
    }
    len = (int)strlen(b);
    if (lx_key_pressed(LX_KEY_BACKSPACE)) {
        if (n->sel_anchor != n->caret) delete_selection(b, n);
        else if (n->caret > 0) {
            int p = n->caret - 1;
            while (p > 0 && ((unsigned char)b[p] & 0xc0) == 0x80) p--;
            memmove(b + p, b + n->caret, (size_t)(len - n->caret + 1));
            n->caret = n->sel_anchor = p;
        }
    }
    if (lx_key_pressed(LX_KEY_DELETE)) {
        if (n->sel_anchor != n->caret) delete_selection(b, n);
        else if (n->caret < len) {
            int p = n->caret + 1;
            while (p < len && ((unsigned char)b[p] & 0xc0) == 0x80) p++;
            memmove(b + n->caret, b + p, (size_t)(len - p + 1));
        }
    }
    if (lx_key_pressed(LX_KEY_LEFT) && n->caret > 0) {
        n->caret--;
        while (n->caret > 0 && ((unsigned char)b[n->caret] & 0xc0) == 0x80) n->caret--;
        if (!shift) n->sel_anchor = n->caret;
    }
    if (lx_key_pressed(LX_KEY_RIGHT) && n->caret < len) {
        n->caret++;
        while (n->caret < len && ((unsigned char)b[n->caret] & 0xc0) == 0x80) n->caret++;
        if (!shift) n->sel_anchor = n->caret;
    }
    if (lx_key_pressed(LX_KEY_HOME)) { n->caret = 0; if (!shift) n->sel_anchor = 0; }
    if (lx_key_pressed(LX_KEY_END)) { n->caret = (int)strlen(b); if (!shift) n->sel_anchor = n->caret; }
    if (lx_key_pressed(LX_KEY_ENTER) || lx_key_pressed(LX_KEY_TAB)) end_edit(ui, n, true);
    else if (lx_key_pressed(LX_KEY_ESCAPE)) end_edit(ui, n, false);
    else if (n->type == UI_TEXTFIELD && count && n->on_change && n->tag == 0x6c697665) n->on_change(n); /* live filter fields */
}

/* ======================================================================
 *  input dispatch
 * ====================================================================== */
static void open_dropdown(UiCtx *ui, UiNode *n);
static void open_color(UiCtx *ui, UiNode *n);

static UiNode *find_handler(UiNode *n, int which) {
    for (UiNode *p = n; p; p = p->parent) {
        if (which == 0 && p->on_right_click) return p;
        if (which == 1 && p->accept) return p;
        if (which == 2 && (p->scroll || p->wrap) && p->scroll_max > 0) return p;
    }
    return NULL;
}

static bool accepts(const char *list, const char *type) {
    if (!list || !type) return false;
    size_t tl = strlen(type);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == tl && !strncmp(p, type, l)) return true;
        if (l == 1 && *p == '*') return true;
        p += l + (e ? 1 : 0);
        if (!e) break;
    }
    return false;
}

static void set_value_from_slider(UiCtx *ui, UiNode *n) {
    float t = lx_clampf((ui->mouse.x - n->x) / lx_maxf(1, n->cw), 0, 1);
    float v = n->minv + (n->maxv - n->minv) * t;
    if (n->i) *n->i = (int)lroundf(v);
    else if (n->f) *n->f = v;
    if (n->on_change) n->on_change(n);
}

static void process_input(UiCtx *ui, float dt) {
    ui->mouse = lx_mouse_pos();
    LxVec2 m = ui->mouse;
    /* hot node: popups > modal > tree */
    UiNode *h = NULL;
    for (int i = ui->popup_count - 1; i >= 0 && !h; i--) h = hit(ui->popups[i], m);
    bool over_popup = false;
    for (int i = 0; i < ui->popup_count; i++) over_popup |= inside(ui->popups[i], m);
    UiNode *split = NULL;
    if (!h && !over_popup) {
        if (ui->modal) h = hit(ui->modal, m);
        else {
            split = find_split(ui, ui->root, m);
            h = split ? split : hit(ui->root, m);
        }
    }
    if (ui->active) h = (ui->active == h || ui->dnd_active) ? h : h;
    if (ui->hot) ui->hot->hover = false;
    ui->hot = h;
    if (h) h->hover = true;

    /* tooltips */
    if (h != ui->tooltip_node || fabsf(m.x - ui->hover_pos.x) > 3 || fabsf(m.y - ui->hover_pos.y) > 3) {
        ui->tooltip_node = h;
        ui->hover_time = 0;
        ui->hover_pos = m;
    } else ui->hover_time += dt;

    int cursor = 0;
    if (h && (h->type == UI_TEXTFIELD || (h->type == UI_NUMBER && h->editing))) cursor = 1;
    if (h && h->type == UI_NUMBER && !h->editing) cursor = 2;
    if (split || (ui->active && ui->active->type == UI_SPLIT)) {
        UiNode *s = split ? split : ui->active;
        cursor = s->dir == UI_ROW ? 2 : 3;
    }
    if (h && h->type == UI_BUTTON) cursor = 0;
    if (cursor != ui->cursor) { ui->cursor = cursor; lx_platform_set_cursor(cursor); }

    /* wheel scrolling */
    float wheel = lx_mouse_wheel();
    if (wheel != 0 && h) {
        UiNode *sc = find_handler(h, 2);
        if (sc) sc->scroll_pos = lx_clampf(sc->scroll_pos - wheel * S(ui, 48), 0, sc->scroll_max);
    }

    /* press */
    if (lx_mouse_pressed(LX_MOUSE_LEFT)) {
        if (ui->popup_count && !over_popup) ui_close_popups(ui);
        if (ui->focus && ui->focus != h) end_edit(ui, ui->focus, true);
        ui->active = h;
        ui->press_pos = m;
        ui->dragged = false;
        if (h) {
            h->active = true;
            if (h->type == UI_NUMBER && !h->editing) ui->press_value = h->i ? (float)*h->i : (h->f ? *h->f : 0);
            if (h->type == UI_SLIDER) set_value_from_slider(ui, h);
            if (h->type == UI_TEXTFIELD && !h->editing) begin_edit(ui, h);
            if (h->type == UI_TABS) {
                /* tab header hit */
                float th = S(ui, 30), tx = h->x + S(ui, 4);
                int i = 0;
                if (m.y < h->y + th)
                    for (UiNode *c = h->first; c; c = c->next, i++) {
                        float tw = ui_text_width(ui, UI_FONT_BOLD, c->text) + S(ui, c->icon ? 46 : 28);
                        if (m.x >= tx && m.x < tx + tw) { h->tab_index = i; break; }
                        tx += tw + S(ui, 2);
                    }
            }
        }
    }
    if (lx_mouse_pressed(LX_MOUSE_RIGHT) && h) {
        if (ui->popup_count && !over_popup) ui_close_popups(ui);
        UiNode *rc = find_handler(h, 0);
        if (rc) rc->on_right_click(rc);
    } else if (lx_mouse_pressed(LX_MOUSE_RIGHT) && ui->popup_count && !over_popup) ui_close_popups(ui);

    /* drag */
    UiNode *a = ui->active;
    if (a && lx_mouse_down(LX_MOUSE_LEFT)) {
        float dx = m.x - ui->press_pos.x, dy = m.y - ui->press_pos.y;
        if (!ui->dragged && dx * dx + dy * dy > S(ui, 4) * S(ui, 4)) {
            ui->dragged = true;
            if (a->drag_type) {
                ui->dnd_active = true;
                ui->dnd_type = a->drag_type;
                lx_strcpy(ui->dnd_payload, sizeof ui->dnd_payload, a->drag_payload);
                lx_strcpy(ui->dnd_label, sizeof ui->dnd_label, a->text[0] ? a->text : a->drag_payload);
                ui->dnd_source = a;
            }
        }
        if (a->type == UI_SPLIT) {
            float total = a->dir == UI_ROW ? a->cw : a->ch;
            float pos = a->dir == UI_ROW ? m.x - a->x : m.y - a->y;
            if (total > 0) a->split = lx_clampf(pos / total, 0.02f, 0.98f);
        } else if (a->type == UI_NUMBER && !a->editing && ui->dragged) {
            float v = ui->press_value + (m.x - ui->press_pos.x) / ui->scale * a->step;
            if (lx_key_down(LX_KEY_SHIFT)) v = ui->press_value + (m.x - ui->press_pos.x) / ui->scale * a->step * 0.1f;
            v = lx_clampf(v, a->minv, a->maxv);
            if (a->i) *a->i = (int)lroundf(v);
            else if (a->f) *a->f = v;
            if (a->on_change) a->on_change(a);
        } else if (a->type == UI_SLIDER) set_value_from_slider(ui, a);
        else if (a->type == UI_TEXTFIELD && a->editing && a->s) {
            /* drag-select text */
            float tx = a->x + S(ui, 6) - 0;
            int best = 0;
            float bd = 1e9f;
            for (int i = 0; i <= (int)strlen(a->s); i++) {
                char tmp[256];
                lx_strcpy(tmp, (size_t)(i + 1 < 256 ? i + 1 : 256), a->s);
                float d = fabsf(tx + ui_text_width(ui, a->font, tmp) - m.x);
                if (d < bd) { bd = d; best = i; }
            }
            a->caret = best;
        }
    }

    /* release */
    if (lx_mouse_released(LX_MOUSE_LEFT)) {
        if (ui->dnd_active) {
            UiNode *target = h ? find_handler(h, 1) : NULL;
            if (target && accepts(target->accept, ui->dnd_type) && target->on_drop) target->on_drop(target, ui->dnd_type, ui->dnd_payload);
            ui->dnd_active = false;
            ui->dnd_type = NULL;
        } else if (a && a == h && !ui->dragged) {
            double now = lx_time();
            bool dbl = ui->last_click_node == a && now - ui->last_click_time < 0.35;
            ui->last_click_time = now;
            ui->last_click_node = a;
            bool menu = a->menu_item;   /* read now: handlers below may free a */
            switch (a->type) {
            case UI_CHECKBOX:
                if (a->b) *a->b = !*a->b;
                if (a->on_change) { a->on_change(a); a = NULL; }   /* the change may rebuild (free) the checkbox */
                break;
            case UI_NUMBER: begin_edit(ui, a); break;
            case UI_DROPDOWN: open_dropdown(ui, a); break;
            case UI_COLOR: open_color(ui, a); break;
            default: break;
            }
            if (a && dbl && a->on_double_click) a->on_double_click(a);
            else if (a && a->on_click) a->on_click(a);
            if (menu) ui_close_popups(ui);
        }
        if (ui->active) ui->active->active = false;
        ui->active = NULL;
    }

    /* keyboard */
    if (ui->focus && ui->focus->editing) handle_text_keys(ui, ui->focus);
    else if (ui->focus && ui->focus->on_key) ui->focus->on_key(ui->focus);
    if (lx_key_pressed(LX_KEY_ESCAPE) && !ui_wants_keyboard(ui)) {
        if (ui->popup_count) ui_close_popups(ui);
    }
}

/* ======================================================================
 *  dropdown & colour popups
 * ====================================================================== */
static void dropdown_pick(UiNode *item) {
    UiNode *src = item->user;
    if (!src) return;
    if (src->i) *src->i = item->tab_index;
    if (src->on_change) src->on_change(src);
}

static void open_dropdown(UiCtx *ui, UiNode *n) {
    UiNode *p = ui_popup(ui, n->x, n->y + n->ch + S(ui, 2), n->cw / ui->scale);
    if (!p) return;
    p->owner = n;
    for (int i = 0; i < n->option_count; i++) {
        UiNode *it = ui_menu_item(ui, p, n->options[i], NULL, dropdown_pick);
        it->user = n;
        it->tab_index = i;
        it->selected = n->i && *n->i == i;
        if (it->selected) it->icon = UI_ICON_CHECK;
    }
    if (n->option_count > 16) {   /* long lists scroll, starting at the current choice */
        p->h = 16 * 27 + 8;
        p->scroll = true;
        if (n->i && *n->i > 8) p->scroll_pos = S(ui, 27) * (float)(*n->i - 6);
    }
}

static void color_changed(UiNode *s) {
    UiNode *src = s->user;
    if (src && src->on_change) src->on_change(src);
}

static void open_color(UiCtx *ui, UiNode *n) {
    UiNode *p = ui_popup(ui, n->x, n->y + n->ch + S(ui, 2), 220);
    if (!p || !n->f) return;
    p->owner = n;
    ui_pad(p, 10, 10, 10, 10);
    p->gap = 6;
    static const char *labels[4] = {"R", "G", "B", "A"};
    for (int c = 0; c < (int)n->tag; c++) {
        UiNode *row = ui_box(ui, p, UI_ROW);
        row->gap = 8;
        row->h = 22;
        UiNode *l = ui_label(ui, row, labels[c]);
        l->w = 14;
        l->fg = c == 0 ? ui->theme.axis_x : c == 1 ? ui->theme.axis_y : c == 2 ? ui->theme.axis_z : ui->theme.text_dim;
        UiNode *sl = ui_slider(ui, row, &n->f[c], 0, c < 3 && n->maxv > 1.0f ? n->maxv : 1.0f);
        sl->grow = 1;
        sl->user = n;
        sl->on_change = color_changed;
    }
}

/* ======================================================================
 *  rendering
 * ====================================================================== */
static void draw_node(UiCtx *ui, UiNode *n);

static void draw_children(UiCtx *ui, UiNode *n) {
    bool clip = n->clip || n->scroll || n->wrap;
    if (clip) ui_push_clip(ui, n->x, n->y, n->cw, n->ch);
    for (UiNode *c = n->first; c; c = c->next) {
        if (c->hidden) continue;
        /* skip children fully outside the current clip */
        const float *cl = ui->clip[ui->clip_depth];
        if (c->x > cl[0] + cl[2] || c->y > cl[1] + cl[3] || c->x + c->cw < cl[0] || c->y + c->ch < cl[1]) continue;
        draw_node(ui, c);
    }
    if (clip) ui_pop_clip(ui);
    if (n->scroll_max > 0) {
        /* scrollbar */
        float track = n->ch, thumb = lx_maxf(S(ui, 24), track * track / (track + n->scroll_max));
        float ty = n->y + (track - thumb) * (n->scroll_pos / n->scroll_max);
        ui_draw_rect(ui, n->x + n->cw - S(ui, 6), ty + S(ui, 2), S(ui, 4), thumb - S(ui, 4), ui_alpha(ui->theme.text_dim, 0.45f), S(ui, 2));
    }
}

static void draw_field_bg(UiCtx *ui, UiNode *n) {
    const UiTheme *t = &ui->theme;
    uint32_t bg = n->bg ? n->bg : (n->hover || n->editing ? t->field_hover : t->field);
    ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, bg, S(ui, n->radius));
    uint32_t border = n->editing ? t->accent : (n->hover ? t->border_light : t->border);
    ui_draw_border(ui, n->x, n->y, n->cw, n->ch, border, S(ui, n->radius), lx_maxf(1, S(ui, 1)));
}

static float text_y(UiCtx *ui, UiNode *n, UiFont f) { return n->y + (n->ch - ui_line_height(ui, f)) * 0.5f; }

static void draw_edit_text(UiCtx *ui, UiNode *n, const char *b, float tx, float max_w) {
    const UiTheme *t = &ui->theme;
    float ty = text_y(ui, n, n->font);
    ui_push_clip(ui, n->x + S(ui, 2), n->y, n->cw - S(ui, 4), n->ch);
    if (n->editing) {
        int a = n->sel_anchor < n->caret ? n->sel_anchor : n->caret, z = n->sel_anchor < n->caret ? n->caret : n->sel_anchor;
        char tmp[256];
        lx_strcpy(tmp, (size_t)(n->caret + 1 < 256 ? n->caret + 1 : 256), b);
        float cx = ui_text_width(ui, n->font, tmp);
        float shift = lx_maxf(0, cx - max_w + S(ui, 4));
        tx -= shift;
        if (a != z) {
            lx_strcpy(tmp, (size_t)(a + 1 < 256 ? a + 1 : 256), b);
            float x0 = ui_text_width(ui, n->font, tmp);
            lx_strcpy(tmp, (size_t)(z + 1 < 256 ? z + 1 : 256), b);
            float x1 = ui_text_width(ui, n->font, tmp);
            ui_draw_rect(ui, tx + x0, n->y + S(ui, 3), x1 - x0, n->ch - S(ui, 6), t->selection, 0);
        }
        ui_draw_text(ui, n->font, tx, ty, b, t->text);
        if (fmodf(ui->time, 1.0f) < 0.6f) ui_draw_rect(ui, tx + cx, n->y + S(ui, 4), lx_maxf(1, S(ui, 1)), n->ch - S(ui, 8), t->text, 0);
    } else {
        if (!b[0] && n->hint[0]) ui_draw_text(ui, n->font, tx, ty, n->hint, t->text_disabled);
        else draw_text_fit(ui, n->font, tx, ty, b, n->disabled ? t->text_disabled : t->text, max_w);
    }
    ui_pop_clip(ui);
}

static void draw_node(UiCtx *ui, UiNode *n) {
    const UiTheme *t = &ui->theme;
    float r = S(ui, n->radius);
    switch (n->type) {
    case UI_BOX: case UI_CUSTOM:
        if (n->bg) {
            uint32_t bg = n->bg;
            if (n->selected) bg = t->selection;
            else if ((n->hover || n->accept) && n->bg_hover && (n->hover || (ui->dnd_active && ui_is_inside(ui, n)))) bg = n->bg_hover;
            ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, bg, r);
        } else if (n->selected) ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, t->selection, r);
        else if (n->bg_hover && n->hover) ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, n->bg_hover, r);
        if (n->accept && ui->dnd_active && accepts(n->accept, ui->dnd_type) && ui_is_inside(ui, n))
            ui_draw_border(ui, n->x, n->y, n->cw, n->ch, t->accent, r, S(ui, 1.5f));
        if (n->border) ui_draw_border(ui, n->x, n->y, n->cw, n->ch, n->border, r, lx_maxf(1, S(ui, 1)));
        if (n->on_draw) n->on_draw(n, ui);
        break;
    case UI_LABEL: {
        if (n->bg || n->selected) ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, n->selected ? t->selection : n->bg, r);
        float x = n->x + S(ui, n->pad[0]);
        float is = S(ui, n->icon_size > 0 ? n->icon_size : 16);
        float tw = ui_text_width(ui, n->font, n->text);
        float content = tw + (n->icon ? is + S(ui, 6) : 0);
        float room = n->cw - S(ui, n->pad[0]) - S(ui, n->pad[2]);
        if (n->center_text) {
            /* a text too long for the box is shortened to "…" first, then the shortened text is centred */
            float fit = content > room ? room : content;
            if (content > room) {
                float tw_fit = fit_width(ui, n->font, n->text, room - (n->icon ? is + S(ui, 6) : 0));
                fit = tw_fit + (n->icon ? is + S(ui, 6) : 0);
            }
            x = n->x + (n->cw - fit) * 0.5f;
        }
        if (n->icon) {
            ui_draw_icon(ui, n->icon, x, n->y + (n->ch - is) * 0.5f, is, n->icon_color ? n->icon_color : n->fg);
            x += is + S(ui, 6);
        }
        draw_text_fit(ui, n->font, x, text_y(ui, n, n->font), n->text, n->disabled ? t->text_disabled : n->fg,
                      n->x + n->cw - x - S(ui, n->pad[2]));
        break;
    }
    case UI_BUTTON: {
        bool menu = n->menu_item;
        uint32_t bg = n->bg;
        if (n->selected && !menu) bg = n->bg_active ? n->bg_active : t->accent;
        if (!n->disabled) {
            if (n->active && n->hover) bg = n->bg_active ? n->bg_active : t->active;
            else if (n->hover) bg = n->bg_hover ? n->bg_hover : (menu ? t->accent : t->hover);
        }
        if (bg) ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, bg, r);
        if (n->border) ui_draw_border(ui, n->x, n->y, n->cw, n->ch, n->border, r, lx_maxf(1, S(ui, 1)));
        uint32_t fg = n->disabled ? t->text_disabled : (menu && n->hover ? t->accent_text : n->fg);
        if (n->selected && !menu && !n->bg_active) fg = t->accent_text;
        float is = S(ui, n->icon_size > 0 ? n->icon_size : 16);
        float tw = n->text[0] ? ui_text_width(ui, n->font, n->text) : 0;
        float content = tw + (n->icon ? is : 0) + (n->icon && tw > 0 ? S(ui, 6) : 0);
        float x = (n->center_text && !menu) ? n->x + (n->cw - content) * 0.5f : n->x + S(ui, menu ? 8 : 10) + S(ui, n->pad[0]);
        if (menu && !n->icon) x += is + S(ui, 6); /* align menu text */
        if (n->icon) {
            uint32_t ic = n->icon_color && !(menu && n->hover) ? n->icon_color : fg;
            if (n->disabled) ic = t->text_disabled;
            ui_draw_icon(ui, n->icon, x, n->y + (n->ch - is) * 0.5f, is, ic);
            x += is + (tw > 0 ? S(ui, 6) : 0);
        }
        if (tw > 0) draw_text_fit(ui, n->font, x, text_y(ui, n, n->font), n->text, fg, n->x + n->cw - x - S(ui, 6));
        if (n->hint[0]) {
            float hw = ui_text_width(ui, UI_FONT_SMALL, n->hint);
            ui_draw_text(ui, UI_FONT_SMALL, n->x + n->cw - hw - S(ui, 10), text_y(ui, n, UI_FONT_SMALL), n->hint,
                         menu && n->hover ? ui_alpha(t->accent_text, 0.8f) : t->text_dim);
        }
        break;
    }
    case UI_CHECKBOX: {
        float bs = S(ui, 16), by = n->y + (n->ch - bs) * 0.5f;
        bool on = n->b && *n->b;
        ui_draw_rect(ui, n->x, by, bs, bs, on ? (n->hover ? t->accent_hover : t->accent) : (n->hover ? t->field_hover : t->field), S(ui, 3));
        if (!on) ui_draw_border(ui, n->x, by, bs, bs, n->hover ? t->border_light : t->border, S(ui, 3), lx_maxf(1, S(ui, 1)));
        if (on) ui_draw_icon(ui, UI_ICON_CHECK, n->x, by, bs, t->accent_text);
        if (n->text[0]) ui_draw_text(ui, n->font, n->x + bs + S(ui, 8), text_y(ui, n, n->font), n->text, n->disabled ? t->text_disabled : n->fg);
        break;
    }
    case UI_TEXTFIELD: {
        draw_field_bg(ui, n);
        float ix = n->icon ? S(ui, 22) : 0;
        if (n->icon) ui_draw_icon(ui, n->icon, n->x + S(ui, 5), n->y + (n->ch - S(ui, 14)) * 0.5f, S(ui, 14), t->text_dim);
        draw_edit_text(ui, n, n->s ? n->s : "", n->x + S(ui, 7) + ix, n->cw - S(ui, 14) - ix);
        break;
    }
    case UI_NUMBER: {
        draw_field_bg(ui, n);
        float tag_w = 0;
        if (n->icon_color) {
            /* coloured axis tag (X/Y/Z) */
            tag_w = S(ui, 4);
            ui_draw_rect(ui, n->x, n->y, tag_w, n->ch, n->icon_color, S(ui, n->radius));
            ui_draw_rect(ui, n->x + tag_w * 0.5f, n->y, tag_w * 0.5f, n->ch, n->icon_color, 0);
        }
        if (n->editing) draw_edit_text(ui, n, n->edit_backup, n->x + S(ui, 7) + tag_w, n->cw - S(ui, 14) - tag_w);
        else {
            char buf[64];
            value_to_text(n, buf, sizeof buf);
            float tw = ui_text_width(ui, n->font, buf);
            float tx = n->x + tag_w + lx_maxf(S(ui, 6), (n->cw - tag_w - tw) * 0.5f);
            if (n->text[0]) {
                ui_draw_text(ui, UI_FONT_SMALL, n->x + tag_w + S(ui, 6), text_y(ui, n, UI_FONT_SMALL), n->text, t->text_dim);
                tx = n->x + n->cw - tw - S(ui, 7);
            }
            ui_push_clip(ui, n->x, n->y, n->cw, n->ch);
            ui_draw_text(ui, n->font, tx, text_y(ui, n, n->font), buf, n->disabled ? t->text_disabled : t->text);
            ui_pop_clip(ui);
        }
        break;
    }
    case UI_SLIDER: {
        draw_field_bg(ui, n);
        float v = n->i ? (float)*n->i : (n->f ? *n->f : 0);
        float tt = lx_clampf((v - n->minv) / lx_maxf(1e-6f, n->maxv - n->minv), 0, 1);
        float fa = n->hover ? 0.78f : 0.62f;   /* ember fill: deep red rising to orange */
        ui_draw_rect_gradient(ui, n->x + S(ui, 1), n->y + S(ui, 1), (n->cw - S(ui, 2)) * tt, n->ch - S(ui, 2), ui_alpha(UI_HEX(0x8a1f12), fa),
                              ui_alpha(t->accent_top, fa), S(ui, n->radius), true);
        char buf[64];
        value_to_text(n, buf, sizeof buf);
        float tw = ui_text_width(ui, n->font, buf);
        ui_draw_text(ui, n->font, n->x + (n->cw - tw) * 0.5f, text_y(ui, n, n->font), buf, t->text);
        break;
    }
    case UI_DROPDOWN: {
        draw_field_bg(ui, n);
        const char *cur = (n->i && *n->i >= 0 && *n->i < n->option_count) ? n->options[*n->i] : "";
        draw_text_fit(ui, n->font, n->x + S(ui, 8), text_y(ui, n, n->font), cur, t->text, n->cw - S(ui, 30));
        ui_draw_icon(ui, UI_ICON_CHEVRON_DOWN, n->x + n->cw - S(ui, 20), n->y + (n->ch - S(ui, 14)) * 0.5f, S(ui, 14), t->text_dim);
        break;
    }
    case UI_COLOR: {
        draw_field_bg(ui, n);
        if (n->f) {
            float k = n->maxv > 1.0f ? 1.0f / n->maxv : 1.0f;
            uint32_t col = UI_RGBA((int)(lx_saturate(powf(n->f[0] * k, 1 / 2.2f)) * 255), (int)(lx_saturate(powf(n->f[1] * k, 1 / 2.2f)) * 255),
                                   (int)(lx_saturate(powf(n->f[2] * k, 1 / 2.2f)) * 255), 255);
            ui_draw_rect(ui, n->x + S(ui, 3), n->y + S(ui, 3), n->cw - S(ui, 6), n->ch - S(ui, 6), col, S(ui, 2));
        }
        break;
    }
    case UI_IMAGE: {
        if (n->bg) ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, n->bg, r);
        float w = n->cw, h = n->ch, x = n->x, y = n->y;
        if (n->tex_aspect > 0) {
            if (w / h > n->tex_aspect) { float nw = h * n->tex_aspect; x += (w - nw) * 0.5f; w = nw; }
            else { float nh = w / n->tex_aspect; y += (h - nh) * 0.5f; h = nh; }
        }
        if (n->texture) ui_draw_image(ui, n->texture, x, y, w, h);
        if (n->on_draw) n->on_draw(n, ui);
        break;
    }
    case UI_SEPARATOR: {
        bool vertical = n->parent && n->parent->dir == UI_ROW;
        if (vertical) ui_draw_rect(ui, n->x + n->cw * 0.5f, n->y + S(ui, 4), lx_maxf(1, S(ui, 1)), n->ch - S(ui, 8), t->border, 0);
        else ui_draw_rect(ui, n->x + S(ui, 4), n->y + n->ch * 0.5f, n->cw - S(ui, 8), lx_maxf(1, S(ui, 1)), t->border, 0);
        break;
    }
    case UI_SPLIT: {
        if (n->first && n->first->next) {
            UiNode *a = n->first;
            bool hot = ui->hot == n || ui->active == n;
            uint32_t c = hot ? t->accent : t->window;
            if (n->dir == UI_ROW) ui_draw_rect(ui, a->x + a->cw, n->y, S(ui, 4), n->ch, c, 0);
            else ui_draw_rect(ui, n->x, a->y + a->ch, n->cw, S(ui, 4), c, 0);
        }
        break;
    }
    case UI_TABS: {
        float th = S(ui, 30);
        ui_draw_rect(ui, n->x, n->y, n->cw, th, t->header, 0);
        float tx = n->x + S(ui, 4);
        int i = 0;
        for (UiNode *c = n->first; c; c = c->next, i++) {
            float tw = ui_text_width(ui, UI_FONT_BOLD, c->text) + S(ui, c->icon ? 46 : 28);
            bool sel = i == n->tab_index;
            bool hov = ui->hot == n && ui->mouse.y < n->y + th && ui->mouse.x >= tx && ui->mouse.x < tx + tw;
            if (sel) {
                ui_draw_rect(ui, tx, n->y + S(ui, 4), tw, th - S(ui, 4), t->panel, S(ui, 5));
                ui_draw_rect(ui, tx, n->y + th - S(ui, 5), tw, S(ui, 5), t->panel, 0);
                ui_draw_rect(ui, tx + S(ui, 8), n->y + S(ui, 4), tw - S(ui, 16), S(ui, 2), t->accent, S(ui, 1));
            } else if (hov) ui_draw_rect(ui, tx, n->y + S(ui, 6), tw, th - S(ui, 10), t->hover, S(ui, 4));
            float x = tx + S(ui, 14);
            if (c->icon) {
                ui_draw_icon(ui, c->icon, x, n->y + (th - S(ui, 14)) * 0.5f + S(ui, 2), S(ui, 14), sel ? t->accent : t->text_dim);
                x += S(ui, 20);
            }
            ui_draw_text(ui, UI_FONT_BOLD, x, n->y + S(ui, 2) + (th - ui_line_height(ui, UI_FONT_BOLD)) * 0.5f, c->text, sel ? t->text : t->text_dim);
            tx += tw + S(ui, 2);
        }
        break;
    }
    }
    if (n->first) draw_children(ui, n);
}

static void draw_tooltip(UiCtx *ui) {
    UiNode *n = ui->tooltip_node;
    if (!n || !n->tooltip[0] || ui->hover_time < 0.55f || ui->active || ui->dnd_active) return;
    const UiTheme *t = &ui->theme;
    float tw = ui_text_width(ui, UI_FONT_SMALL, n->tooltip) + S(ui, 16), th = ui_line_height(ui, UI_FONT_SMALL) + S(ui, 10);
    int sw, sh;
    lx_window_size(&sw, &sh);
    float x = lx_minf(ui->mouse.x + S(ui, 12), (float)sw - tw - 4), y = ui->mouse.y + S(ui, 20);
    if (y + th > sh) y = ui->mouse.y - th - S(ui, 6);
    ui_draw_rect(ui, x + 2, y + 3, tw, th, UI_RGBA(0, 0, 0, 90), S(ui, 5));
    ui_draw_rect(ui, x, y, tw, th, UI_HEX(0x101113), S(ui, 5));
    ui_draw_border(ui, x, y, tw, th, t->border_light, S(ui, 5), 1);
    ui_draw_text(ui, UI_FONT_SMALL, x + S(ui, 8), y + S(ui, 5), n->tooltip, t->text);
}

static void draw_toasts(UiCtx *ui, float dt) {
    int sw, sh;
    lx_window_size(&sw, &sh);
    float y = (float)sh - S(ui, 44);
    const UiTheme *t = &ui->theme;
    for (int i = ui->toast_count - 1; i >= 0; i--) {
        Toast *to = &ui->toasts[i];
        to->time += dt;
        float a = to->time < 0.15f ? to->time / 0.15f : (to->time > 3.2f ? 1.0f - (to->time - 3.2f) / 0.5f : 1.0f);
        if (a <= 0) continue;
        float tw = ui_text_width(ui, UI_FONT_BODY, to->text) + S(ui, 48), th = S(ui, 36);
        float x = (float)sw - tw - S(ui, 16);
        ui_draw_rect(ui, x + 2, y - th + 3, tw, th, UI_RGBA(0, 0, 0, (int)(100 * a)), S(ui, 6));
        ui_draw_rect(ui, x, y - th, tw, th, ui_alpha(UI_HEX(0x2a2c31), a), S(ui, 6));
        ui_draw_rect(ui, x, y - th, S(ui, 3), th, ui_alpha(t->accent, a), S(ui, 1));
        ui_draw_icon(ui, UI_ICON_INFO, x + S(ui, 12), y - th + S(ui, 10), S(ui, 16), ui_alpha(t->accent, a));
        ui_draw_text(ui, UI_FONT_BODY, x + S(ui, 36), y - th + (th - ui_line_height(ui, UI_FONT_BODY)) * 0.5f, to->text, ui_alpha(t->text, a));
        y -= th + S(ui, 8);
    }
    while (ui->toast_count && ui->toasts[0].time > 3.7f) {
        memmove(ui->toasts, ui->toasts + 1, sizeof(Toast) * (size_t)(ui->toast_count - 1));
        ui->toast_count--;
    }
}

/* ======================================================================
 *  frame
 * ====================================================================== */
static void run_frame_hooks(UiNode *n) {
    if (n->on_frame) n->on_frame(n);
    for (UiNode *c = n->first; c;) {
        UiNode *next = c->next; /* hooks may rebuild their own subtree */
        run_frame_hooks(c);
        c = next;
    }
}

static void layout_popup(UiCtx *ui, UiNode *p) {
    float w, h;
    measure(ui, p, &w, &h);
    if (p->min_w > 0) w = lx_maxf(w, S(ui, p->min_w));
    if (p->w > 0) w = S(ui, p->w);
    int sw, sh;
    lx_window_size(&sw, &sh);
    float x = lx_minf(p->x, (float)sw - w - 4), y = p->y;
    if (y + h > sh - 4) y = lx_maxf(4, (float)sh - h - 4);
    layout(ui, p, lx_maxf(0, x), y, w, h);
}

void ui_frame(UiCtx *ui, float dt, LxVec4 clear) {
    ui->time += dt;
    int sw, sh;
    lx_window_size(&sw, &sh);
    float want = lx_maxf(1.0f, lx_dpi_scale());
    if (fabsf(want - ui->scale) > 0.01f) { ui->scale = want; load_fonts(ui); }
    run_frame_hooks(ui->root);
    if (ui->modal) run_frame_hooks(ui->modal);
    for (int i = 0; i < ui->popup_count; i++) run_frame_hooks(ui->popups[i]);
    layout(ui, ui->root, 0, 0, (float)sw, (float)sh);
    if (ui->modal) {
        float w = S(ui, ui->modal->w), h = S(ui, ui->modal->h);
        layout(ui, ui->modal, ((float)sw - w) * 0.5f, ((float)sh - h) * 0.5f, w, h);
    }
    for (int i = 0; i < ui->popup_count; i++) layout_popup(ui, ui->popups[i]);
    process_input(ui, dt);
    if (ui->after_input) ui->after_input(ui);
    /* input handlers may have rebuilt parts of the tree: lay out again before drawing */
    layout(ui, ui->root, 0, 0, (float)sw, (float)sh);
    if (ui->modal) {
        float w = S(ui, ui->modal->w), h = S(ui, ui->modal->h);
        layout(ui, ui->modal, ((float)sw - w) * 0.5f, ((float)sh - h) * 0.5f, w, h);
    }
    for (int i = 0; i < ui->popup_count; i++) layout_popup(ui, ui->popups[i]);

    ui->vcount = ui->icount = ui->ccount = 0;
    ui->clip_depth = 0;
    ui->clip[0][0] = 0; ui->clip[0][1] = 0; ui->clip[0][2] = (float)sw; ui->clip[0][3] = (float)sh;
    if (ui->root->bg) ui_draw_rect(ui, 0, 0, (float)sw, (float)sh, ui->root->bg, 0);
    draw_children(ui, ui->root);
    if (ui->modal) {
        ui_draw_rect(ui, 0, 0, (float)sw, (float)sh, UI_RGBA(0, 0, 0, 140), 0);
        UiNode *m = ui->modal;
        ui_draw_rect(ui, m->x + 3, m->y + 6, m->cw, m->ch, UI_RGBA(0, 0, 0, 110), S(ui, 10));
        draw_node(ui, m);
    }
    for (int i = 0; i < ui->popup_count; i++) {
        UiNode *p = ui->popups[i];
        ui_draw_rect(ui, p->x + 2, p->y + 4, p->cw, p->ch, UI_RGBA(0, 0, 0, 100), S(ui, 7));
        draw_node(ui, p);
    }
    if (ui->dnd_active) {
        float tw = ui_text_width(ui, UI_FONT_BODY, ui->dnd_label) + S(ui, 20);
        ui_draw_rect(ui, ui->mouse.x + S(ui, 14), ui->mouse.y + S(ui, 8), tw, S(ui, 26), ui_alpha(ui->theme.accent, 0.9f), S(ui, 4));
        ui_draw_text(ui, UI_FONT_BODY, ui->mouse.x + S(ui, 24), ui->mouse.y + S(ui, 8) + (S(ui, 26) - ui_line_height(ui, UI_FONT_BODY)) * 0.5f,
                     ui->dnd_label, ui->theme.accent_text);
    }
    draw_tooltip(ui);
    draw_toasts(ui, dt);
    LxUiDrawList dl = {ui->verts, ui->vcount, ui->idx, ui->icount, ui->cmds, ui->ccount, clear};
    lx_renderer_draw_ui(&dl);
}

UiCtx *ui_create(void) {
    UiCtx *ui = lx_calloc(1, sizeof *ui);
    default_theme(&ui->theme);
    ui->scale = lx_maxf(1.0f, lx_dpi_scale());
    load_fonts(ui);
    ui->root = alloc_node(ui, UI_BOX);
    ui->root->bg = ui->theme.window;
    return ui;
}

UiNode *ui_root(UiCtx *ui) { return ui->root; }

void ui_destroy(UiCtx *ui) {
    if (!ui) return;
    ui_close_popups(ui);
    ui_close_modal(ui);
    free_tree(ui, ui->root);
    lx_free(ui->verts);
    lx_free(ui->idx);
    lx_free(ui->cmds);
    lx_free(ui);
}

/* ======================================================================
 *  test hooks: the stress tester drives widgets through the same code paths as the mouse
 * ====================================================================== */
int ui_popup_nodes(UiCtx *ui, UiNode **out, int max) {
    int n = 0;
    for (int i = 0; i < ui->popup_count && n < max; i++) out[n++] = ui->popups[i];
    return n;
}
void ui_test_click(UiCtx *ui, UiNode *a, bool dbl) {
    bool menu = a->menu_item;
    switch (a->type) {
    case UI_CHECKBOX:
        if (a->b) *a->b = !*a->b;
        if (a->on_change) { a->on_change(a); a = NULL; }
        break;
    case UI_NUMBER: begin_edit(ui, a); break;
    case UI_DROPDOWN: open_dropdown(ui, a); break;
    case UI_COLOR: open_color(ui, a); break;
    default: break;
    }
    if (a && dbl && a->on_double_click) a->on_double_click(a);
    else if (a && a->on_click) a->on_click(a);   /* may free a: not touched after this */
    if (menu) ui_close_popups(ui);
}
void ui_test_right_click(UiCtx *ui, UiNode *a) {
    LX_UNUSED(ui);
    if (a->on_right_click) a->on_right_click(a);
}
/* type text into a text or number field and press Enter */
void ui_test_type(UiCtx *ui, UiNode *n, const char *text) {
    if (n->type != UI_TEXTFIELD && n->type != UI_NUMBER) return;
    if (n->type == UI_TEXTFIELD && !n->s) return;
    if (!n->editing) begin_edit(ui, n);
    int cap;
    char *b = edit_buf(n, &cap);
    if (b && cap > 0) lx_strcpy(b, (size_t)cap, text);
    end_edit(ui, n, true);
}
/* drop a dragged item onto n (or the nearest ancestor that takes it) */
bool ui_test_drop(UiCtx *ui, UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(ui);
    UiNode *target = find_handler(n, 1);
    if (!target || !accepts(target->accept, type) || !target->on_drop) return false;
    target->on_drop(target, type, payload);
    return true;
}

/* switches off a subtree whose bound data is gone: nothing in it is drawn, clicked or written through */
static void unbind(UiNode *n) {
    n->f = NULL; n->i = NULL; n->b = NULL; n->s = NULL; n->s_cap = 0;
    n->on_change = NULL; n->on_click = NULL; n->on_double_click = NULL; n->on_right_click = NULL; n->on_drop = NULL; n->on_key = NULL;
    n->on_frame = NULL;
    n->editing = false;
    for (UiNode *c = n->first; c; c = c->next) unbind(c);
}
void ui_unbind_children(UiCtx *ui, UiNode *n) {
    if (!n) return;
    for (UiNode *c = n->first; c; c = c->next) { unbind(c); c->hidden = true; }
    if (ui->focus && ui->focus->editing) {
        for (UiNode *p = ui->focus; p; p = p->parent) if (p == n) { ui->focus->editing = false; ui->focus = NULL; break; }
    }
    /* popups opened from those fields (dropdowns, colour pickers) point into the same data */
    for (int i = 0; i < ui->popup_count; i++)
        for (UiNode *o = ui->popups[i]->owner; o; o = o->parent)
            if (o == n) { ui_close_popups(ui); return; }
}
void ui_set_after_input(UiCtx *ui, void (*fn)(UiCtx *ui)) { ui->after_input = fn; }
