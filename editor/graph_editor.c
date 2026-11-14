/* State Graph editor, laid out like PlayMaker: a big graph view with an object/graph toolbar, the
 * FSM / State / Events / Variables inspector on the right, a bottom bar (Debug, Hints, Errors,
 * Action Browser) and the Action Browser as a floating window.
 *
 * This file is the graph view: drawing states, links, global transitions, comments and the minimap;
 * selecting, moving, linking, context menus, copy/paste, auto layout and undo. Graphs are assets
 * (.lxgraph): every edit is saved straight away and running copies re-bind to it. */
#include "graph_editor.h"
#include <math.h>

GraphEd GE = {.zoom = 1, .sel_trans = -1, .sel_comment = -1, .minimap = true, .hints = true, .link_state = -1};

static const uint32_t STATE_COLORS[GE_COLORS] = {0x34363c, 0x7a2412, 0x1d3a5f, 0x4a2350, 0x24502c, 0x1d4a4a, 0x5a4a14, 0x2a2b30};
static const char *const COLOR_NAMES[GE_COLORS] = {"Default", "Red", "Blue", "Purple", "Green", "Teal", "Yellow", "Grey"};
uint32_t ge_state_color(int c) { return UI_HEX(STATE_COLORS[c >= 0 && c < GE_COLORS ? c : 0]); }

#define COL_LINK UI_HEX(0xb1b5bd)
#define COL_HOT UI_HEX(0x5a92ff)
#define COL_ACCENT UI_HEX(0x3d7cf5)
#define COL_START UI_HEX(0x7cc24e)
#define COL_TEXT UI_HEX(0xdfe1e6)
#define COL_DIM UI_HEX(0x8d919a)
#define COL_BOX UI_HEX(0x28292e)
#define COL_EDGE UI_HEX(0x34363c)

/* ======================================================================
 *  the edited graph
 * ====================================================================== */
LxGraph *ge_graph(void) { return GE.ref[0] ? lx_asset_graph(GE.ref) : NULL; }

int ge_sel_index(void) {
    LxGraph *g = ge_graph();
    return g && GE.sel_count && GE.sel[0] < g->state_count ? GE.sel[0] : -1;
}
LxGraphState *ge_sel_state(void) {
    int i = ge_sel_index();
    return i >= 0 ? &ge_graph()->states[i] : NULL;
}
static bool is_selected(int i) {
    for (int k = 0; k < GE.sel_count; k++) if (GE.sel[k] == i) return true;
    return false;
}
void ge_select_state(int idx, bool add) {
    if (!add) GE.sel_count = 0;
    GE.sel_comment = -1;
    if (idx < 0) { GE.sel_trans = -1; GE.insp_dirty = true; return; }
    if (add && is_selected(idx)) {   /* shift-click again removes it */
        for (int k = 0; k < GE.sel_count; k++)
            if (GE.sel[k] == idx) { memmove(GE.sel + k, GE.sel + k + 1, sizeof(int) * (size_t)(GE.sel_count - k - 1)); GE.sel_count--; break; }
    } else if (GE.sel_count < GE_MAX_SEL) {
        if (add) GE.sel[GE.sel_count++] = idx;
        else { GE.sel[0] = idx; GE.sel_count = 1; }
    }
    GE.sel_trans = -1;
    GE.insp_dirty = true;
    if (GE.insp_tabs && GE.sel_count) GE.insp_tabs->tab_index = 1;   /* State tab */
}

LxFsmRun *ge_run(void) {
    if (ED.play == ED_EDIT || !GE.obj || !ED.scene) return NULL;
    LxFsmRun *r = lx_fsm_run_of(ED.scene, GE.obj, GE.slot);
    return r && r->graph && r->graph == ge_graph() ? r : NULL;
}

static void clear_history(void) {
    for (int i = 0; i < GE.undo_count; i++) lx_free(GE.undo[i]);
    for (int i = 0; i < GE.redo_count; i++) lx_free(GE.redo[i]);
    GE.undo_count = GE.redo_count = 0;
    lx_free(GE.last_text);
    GE.last_text = NULL;
    GE.last_key = NULL;
}

static bool g_save_pending;
static void save_graph(LxGraph *g);
static void flush_save(void) {
    if (!g_save_pending) return;
    g_save_pending = false;
    LxGraph *g = ge_graph();
    if (g) save_graph(g);
}

static void set_target(const char *ref, LxEntity obj, int slot) {
    GE.obj = obj;
    GE.slot = slot;
    if (!strcmp(ref, GE.ref)) return;
    flush_save();
    lx_strcpy(GE.ref, sizeof GE.ref, ref);
    clear_history();
    LxGraph *g = ge_graph();
    if (g) { GE.last_text = lx_graph_to_text(g); GE.seen_version = g->version; }
    GE.sel_count = 0;
    GE.sel_trans = GE.sel_comment = -1;
    GE.drag = GE_DRAG_NONE;
    GE.framed = false;
    GE.insp_dirty = true;
    GE.ab_dirty = true;
}

static void save_graph(LxGraph *g) {
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, GE.ref);
    if (!lx_graph_save(g, path)) ui_notify(ED.ui, "Couldn't save %s", GE.ref);
}

void ge_commit(bool structural, const void *key) {
    LxGraph *g = ge_graph();
    if (!g) return;
    char *t = lx_graph_to_text(g);
    if (GE.last_text && !strcmp(t, GE.last_text)) {
        lx_free(t);
        if (structural) GE.insp_dirty = true;
        return;
    }
    double now = lx_time();
    bool merge = key && key == GE.last_key && now - GE.last_commit_time < 1.0;   /* one undo step per drag / burst */
    if (!merge && GE.last_text) {
        if (GE.undo_count == GE_UNDO) { lx_free(GE.undo[0]); memmove(GE.undo, GE.undo + 1, sizeof(char *) * (GE_UNDO - 1)); GE.undo_count--; }
        GE.undo[GE.undo_count++] = GE.last_text;
    } else lx_free(GE.last_text);
    GE.last_text = t;
    GE.last_key = key;
    GE.last_commit_time = now;
    for (int i = 0; i < GE.redo_count; i++) lx_free(GE.redo[i]);
    GE.redo_count = 0;
    lx_graph_touch(g);
    g_save_pending = true;   /* written once the mouse is up: not every frame of a drag */
    GE.seen_version = g->version;
    if (structural) GE.insp_dirty = true;
    GE.ab_dirty = true;
}

static void restore(char *text) {
    LxGraph *ng = lx_graph_from_text(text);
    if (!ng) { lx_free(text); return; }
    lx_graph_replace(GE.ref, ng);   /* same object, new contents: running graphs re-bind */
    lx_free(GE.last_text);
    GE.last_text = text;
    GE.last_key = NULL;
    LxGraph *g = ge_graph();
    if (g) {
        save_graph(g);
        GE.seen_version = g->version;
        int n = 0;
        for (int k = 0; k < GE.sel_count; k++) if (GE.sel[k] < g->state_count) GE.sel[n++] = GE.sel[k];
        GE.sel_count = n;
        if (GE.sel_comment >= g->comment_count) GE.sel_comment = -1;
    }
    GE.sel_trans = -1;
    GE.insp_dirty = true;
}
void ge_undo(void) {
    if (!GE.undo_count || !GE.last_text) { ui_notify(ED.ui, "Nothing to undo in this graph"); return; }
    if (GE.redo_count < GE_UNDO) GE.redo[GE.redo_count++] = lx_strdup(GE.last_text);
    restore(GE.undo[--GE.undo_count]);
}
void ge_redo(void) {
    if (!GE.redo_count) return;
    if (GE.last_text && GE.undo_count < GE_UNDO) GE.undo[GE.undo_count++] = lx_strdup(GE.last_text);
    restore(GE.redo[--GE.redo_count]);
}

/* ======================================================================
 *  geometry (graph units are logical pixels at 100%)
 * ====================================================================== */
static float SC(void) { return ui_scale(ED.ui) * GE.zoom; }
static float sx(float gx) { return GE.canvas->x + (gx - GE.pan_x) * SC(); }
static float sy(float gy) { return GE.canvas->y + (gy - GE.pan_y) * SC(); }
static float gx_of(float x) { return GE.pan_x + (x - GE.canvas->x) / SC(); }
static float gy_of(float y) { return GE.pan_y + (y - GE.canvas->y) / SC(); }

static float state_w(const LxGraphState *st) {
    float s = ui_scale(ED.ui);
    float w = ui_text_width(ED.ui, UI_FONT_BOLD, st->name) / s + (st->sequence ? 52 : 36);
    for (int t = 0; t < st->trans_count; t++) w = lx_maxf(w, ui_text_width(ED.ui, UI_FONT_BODY, st->trans[t].event) / s + 28);
    return lx_maxf(GE_STATE_W, ceilf(w / 8) * 8);
}
static float state_h(const LxGraphState *st) { return GE_TITLE_H + GE_ROW_H * (float)st->trans_count; }

/* global transition tabs above a state: right-aligned, side by side */
static int global_tabs(const LxGraph *g, int si, float *xs, float *ws, int *idx, int max) {
    const LxGraphState *st = &g->states[si];
    float s = ui_scale(ED.ui), x = st->x + state_w(st);
    int n = 0;
    for (int i = 0; i < g->global_count && n < max; i++) {
        if (strcmp(g->globals[i].to, st->name)) continue;
        float w = (ui_text_width(ED.ui, UI_FONT_SMALL, g->globals[i].event) + 14 * s) / (s * GE.zoom);   /* a fixed size on screen */
        x -= w;
        xs[n] = x;
        ws[n] = w;
        idx[n] = i;
        x -= 4;
        n++;
    }
    return n;
}

/* a link: cubic from the event row to the target's title (graph units) */
static bool link_curve(const LxGraph *g, int si, int ti, float p[8]) {
    const LxGraphState *s = &g->states[si];
    if (ti >= s->trans_count) return false;
    const LxGraphState *t = lx_graph_find_state(g, s->trans[ti].to);
    if (!t) return false;
    float sw = state_w(s), tw = state_w(t);
    float ey = s->y + GE_TITLE_H + GE_ROW_H * (float)ti + GE_ROW_H * 0.5f, ty = t->y + GE_TITLE_H * 0.5f;
    if (t != s && t->x >= s->x + sw - 10) {   /* to the right: leave right, enter left */
        float x1 = s->x + sw, x2 = t->x, c = lx_maxf(40, (x2 - x1) * 0.5f);
        float q[8] = {x1, ey, x1 + c, ey, x2 - c, ty, x2 - 2, ty};
        memcpy(p, q, sizeof q);
    } else if (t != s && t->x + tw <= s->x + 10) {   /* to the left */
        float x1 = s->x, x2 = t->x + tw, c = lx_maxf(40, (x1 - x2) * 0.5f);
        float q[8] = {x1, ey, x1 - c, ey, x2 + c, ty, x2 + 2, ty};
        memcpy(p, q, sizeof q);
    } else {   /* stacked, or itself: out on the right and round */
        float x1 = s->x + sw, x2 = t->x + tw;
        float q[8] = {x1, ey, x1 + 60, ey, x2 + 60, ty, x2 + 2, ty};
        memcpy(p, q, sizeof q);
    }
    return true;
}
static LxVec2 bez(const float p[8], float t) {
    float u = 1 - t, a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
    return lx_v2(a * p[0] + b * p[2] + c * p[4] + d * p[6], a * p[1] + b * p[3] + c * p[5] + d * p[7]);
}
static void draw_curve(UiCtx *ui, const float p[8], uint32_t col, float thick, bool arrow) {
    LxVec2 prev = bez(p, 0);
    for (int i = 1; i <= 24; i++) {
        LxVec2 q = bez(p, (float)i / 24);
        ui_draw_line(ui, sx(prev.x), sy(prev.y), sx(q.x), sy(q.y), thick, col);
        prev = q;
    }
    if (arrow) {
        LxVec2 a = bez(p, 0.94f), b = bez(p, 1);
        float dx = sx(b.x) - sx(a.x), dy = sy(b.y) - sy(a.y), l = sqrtf(dx * dx + dy * dy);
        if (l < 1e-3f) { dx = 1; dy = 0; l = 1; }
        dx /= l; dy /= l;
        float s = 7 * ui_scale(ui) * lx_clampf(GE.zoom, 0.6f, 1.4f);
        float tx = sx(b.x), ty = sy(b.y);
        ui_draw_triangle(ui, tx, ty, tx - dx * s - dy * s * 0.55f, ty - dy * s + dx * s * 0.55f, tx - dx * s + dy * s * 0.55f, ty - dy * s - dx * s * 0.55f, col);
    }
}

/* ======================================================================
 *  hit testing
 * ====================================================================== */
enum { H_NONE, H_STATE, H_EVENT, H_GLOBAL, H_START, H_COMMENT, H_COMMENT_SIZE, H_MINIMAP };
typedef struct Hit { int kind, state, trans, comment; } Hit;

static void minimap_rect(float *x, float *y, float *w, float *h) {
    float s = ui_scale(ED.ui);
    *w = 160 * s;
    *h = 100 * s;
    *x = GE.canvas->x + GE.canvas->cw - *w - 10 * s;
    *y = GE.canvas->y + GE.canvas->ch - *h - 10 * s;
}
static bool minimap_shown(void) {
    LxGraph *g = ge_graph();
    float s = ui_scale(ED.ui);
    return GE.minimap && g && GE.canvas->cw > 640 * s && GE.canvas->ch > 360 * s;
}

static Hit hit_test(float mx, float my) {
    Hit h = {H_NONE, -1, -1, -1};
    LxGraph *g = ge_graph();
    if (!g) return h;
    if (minimap_shown()) {
        float x, y, w, hh;
        minimap_rect(&x, &y, &w, &hh);
        if (mx >= x && my >= y && mx < x + w && my < y + hh) { h.kind = H_MINIMAP; return h; }
    }
    float gx = gx_of(mx), gy = gy_of(my);
    /* states: topmost first (selected ones are drawn on top) */
    for (int pass = 0; pass < 2; pass++)
        for (int i = g->state_count - 1; i >= 0; i--) {
            if ((pass == 0) != is_selected(i)) continue;
            const LxGraphState *st = &g->states[i];
            float w = state_w(st);
            if (gx >= st->x && gx < st->x + w && gy >= st->y && gy < st->y + state_h(st)) {
                h.state = i;
                if (gy < st->y + GE_TITLE_H) h.kind = H_STATE;
                else { h.kind = H_EVENT; h.trans = (int)((gy - st->y - GE_TITLE_H) / GE_ROW_H); }
                return h;
            }
            /* tabs above it */
            if (gy >= st->y - 26 && gy < st->y - 8) {
                float stw = (ui_text_width(ED.ui, UI_FONT_SMALL, "START") + 12 * ui_scale(ED.ui)) / SC();
                if (!strcmp(g->start, st->name) && gx >= st->x && gx < st->x + stw) { h.kind = H_START; h.state = i; return h; }
                float xs[8], ws[8];
                int idx[8], n = global_tabs(g, i, xs, ws, idx, 8);
                for (int k = 0; k < n; k++)
                    if (gx >= xs[k] && gx < xs[k] + ws[k]) { h.kind = H_GLOBAL; h.state = i; h.trans = idx[k]; return h; }
            }
        }
    for (int i = g->comment_count - 1; i >= 0; i--) {
        const LxGraphComment *c = &g->comments[i];
        if (gx >= c->x && gx < c->x + c->w && gy >= c->y && gy < c->y + c->h) {
            h.comment = i;
            h.kind = gx > c->x + c->w - 12 && gy > c->y + c->h - 12 ? H_COMMENT_SIZE : H_COMMENT;
            return h;
        }
    }
    return h;
}

/* ======================================================================
 *  drawing
 * ====================================================================== */
static void draw_wrapped(UiCtx *ui, UiFont f, float x, float y, float w, float maxy, const char *text, uint32_t col) {
    char line[256];
    float lh = ui_line_height(ui, f);
    const char *p = text;
    while (*p && y + lh <= maxy) {
        size_t n = 0, last_space = 0;
        line[0] = 0;
        while (p[n] && p[n] != '\n' && n < sizeof line - 1) {
            line[n] = p[n];
            line[n + 1] = 0;
            if (p[n] == ' ') last_space = n;
            if (ui_text_width(ui, f, line) > w && n > 0) {
                if (last_space) n = last_space;
                break;
            }
            n++;
        }
        line[n] = 0;
        ui_draw_text(ui, f, x, y, line, col);
        y += lh;
        p += n;
        while (*p == ' ' || *p == '\n') p++;
    }
}

static void draw_grid(UiCtx *ui, UiNode *n) {
    float step = 24 * SC();
    if (step < 7) step *= 4;
    float ox = fmodf(-GE.pan_x * SC(), step), oy = fmodf(-GE.pan_y * SC(), step);
    if (ox < 0) ox += step;
    if (oy < 0) oy += step;
    uint32_t c = UI_RGBA(74, 44, 39, 64);
    for (float x = n->x + ox; x < n->x + n->cw; x += step) ui_draw_rect(ui, x, n->y, 1, n->ch, c, 0);
    for (float y = n->y + oy; y < n->y + n->ch; y += step) ui_draw_rect(ui, n->x, y, n->cw, 1, c, 0);
}

static void draw_state(UiCtx *ui, const LxGraph *g, int i, const LxFsmRun *run) {
    const LxGraphState *st = &g->states[i];
    float s = SC(), x = sx(st->x), y = sy(st->y), w = state_w(st) * s, h = state_h(st) * s, th = GE_TITLE_H * s, rh = GE_ROW_H * s;
    bool sel = is_selected(i), active = run && run->state == i;
    bool err = active && run->error;
    float r = 5 * s;
    /* shadow, glow */
    ui_draw_rect(ui, x + 2 * s, y + 3 * s, w, h, UI_RGBA(0, 0, 0, 110), r);
    if (active) ui_draw_rect(ui, x - 5 * s, y - 5 * s, w + 10 * s, h + 10 * s, ui_alpha(err ? UI_HEX(0xe5534b) : COL_ACCENT, 0.22f), r + 4 * s);
    ui_draw_rect(ui, x, y, w, h, COL_BOX, r);
    /* title */
    if (active) ui_draw_rect_gradient(ui, x, y, w, th, UI_HEX(0x5a92ff), UI_HEX(0x3d7cf5), r, false);
    else ui_draw_rect(ui, x, y, w, th, ge_state_color(st->color), r);
    if (st->trans_count) ui_draw_rect(ui, x, y + th - r, w, r, active ? UI_HEX(0x3d7cf5) : ge_state_color(st->color), 0);
    ui_push_clip(ui, x, y, w, h);
    UiFont tf = GE.zoom >= 0.8f ? UI_FONT_BOLD : UI_FONT_SMALL;
    if (GE.zoom >= 0.4f) {
        float tw = ui_text_width(ui, tf, st->name);
        ui_draw_text(ui, tf, x + (w - tw) * 0.5f, y + (th - ui_line_height(ui, tf)) * 0.5f, st->name, UI_HEX(0xffffff));
        if (st->sequence) ui_draw_icon(ui, UI_ICON_LIST, x + w - 18 * s, y + (th - 12 * s) * 0.5f, 12 * s, ui_alpha(UI_HEX(0xffffff), 0.7f));
    }
    /* event rows */
    UiFont ef = GE.zoom >= 0.8f ? UI_FONT_BODY : UI_FONT_SMALL;
    for (int t = 0; t < st->trans_count; t++) {
        float ry = y + th + rh * (float)t;
        ui_draw_rect(ui, x, ry, w, 1, UI_HEX(0x34363c), 0);
        bool hot = sel && GE.sel_trans == t;
        bool fired = run && run->prev_state == i && run->state_time < 1.0f && !strcmp(run->last_event, st->trans[t].event);
        if (hot || fired) ui_draw_rect(ui, x + 1, ry + 1, w - 2, rh - 1, UI_HEX(0x1f3050), 0);
        if (GE.zoom >= 0.45f) {
            float tw = ui_text_width(ui, ef, st->trans[t].event);
            uint32_t c = hot || fired ? UI_HEX(0xffb03a) : !st->trans[t].to[0] ? COL_DIM : COL_TEXT;
            ui_draw_text(ui, ef, x + (w - tw) * 0.5f, ry + (rh - ui_line_height(ui, ef)) * 0.5f, st->trans[t].event, c);
        }
    }
    ui_pop_clip(ui);
    /* border */
    if (sel) ui_draw_border(ui, x - 1, y - 1, w + 2, h + 2, COL_ACCENT, r, 2 * ui_scale(ui));
    else ui_draw_border(ui, x, y, w, h, active ? COL_ACCENT : COL_EDGE, r, 1);
    /* unlinked transitions: a stub */
    for (int t = 0; t < st->trans_count; t++)
        if (!st->trans[t].to[0] || !lx_graph_find_state(g, st->trans[t].to))
            ui_draw_circle(ui, x + w, y + th + rh * ((float)t + 0.5f), 3 * s, COL_DIM, 10);
    /* START and global transition tabs */
    UiFont sf = UI_FONT_SMALL;
    float tab_h = lx_maxf(16 * s, ui_line_height(ui, sf) + 2), tab_y = y - 8 * s - tab_h;
    if (!strcmp(g->start, st->name) && GE.zoom >= 0.35f) {
        float tw = ui_text_width(ui, sf, "START") + 12 * ui_scale(ui);
        ui_draw_rect(ui, x, tab_y, tw, tab_h, COL_START, 3 * s);
        ui_draw_text(ui, sf, x + (tw - ui_text_width(ui, sf, "START")) * 0.5f, tab_y + (tab_h - ui_line_height(ui, sf)) * 0.5f, "START", UI_HEX(0x1b1c1f));
        ui_draw_line(ui, x + tw * 0.5f, tab_y + tab_h, x + tw * 0.5f, y - 3 * s, 2 * ui_scale(ui), COL_START);
        ui_draw_triangle(ui, x + tw * 0.5f, y - 1 * s, x + tw * 0.5f - 4 * s, y - 6 * s, x + tw * 0.5f + 4 * s, y - 6 * s, COL_START);
    }
    float xs[8], ws[8];
    int idx[8], n = global_tabs(g, i, xs, ws, idx, 8);
    for (int k = 0; k < n && GE.zoom >= 0.35f; k++) {
        const char *ev = g->globals[idx[k]].event;
        bool fired = run && run->state == i && run->state_time < 1.0f && !strcmp(run->last_event, ev);
        float tx = sx(xs[k]), tw = ws[k] * s;
        ui_draw_rect(ui, tx, tab_y, tw, tab_h, UI_HEX(0x111214), 3 * s);
        ui_draw_border(ui, tx, tab_y, tw, tab_h, fired ? COL_HOT : UI_HEX(0x45474e), 3 * s, 1);
        ui_draw_text(ui, sf, tx + 7 * ui_scale(ui), tab_y + (tab_h - ui_line_height(ui, sf)) * 0.5f, ev, fired ? UI_HEX(0xffb03a) : COL_TEXT);
        uint32_t lc = fired ? COL_HOT : COL_LINK;
        ui_draw_line(ui, tx + tw * 0.5f, tab_y + tab_h, tx + tw * 0.5f, y - 3 * s, 1.6f * ui_scale(ui), lc);
        ui_draw_triangle(ui, tx + tw * 0.5f, y - 1 * s, tx + tw * 0.5f - 4 * s, y - 6 * s, tx + tw * 0.5f + 4 * s, y - 6 * s, lc);
    }
    /* debug: time in the active state */
    if (active && GE.debug) {
        char b[32];
        snprintf(b, sizeof b, "%.1fs", (double)run->state_time);
        ui_draw_text(ui, UI_FONT_SMALL, x + w - ui_text_width(ui, UI_FONT_SMALL, b), y + h + 3 * s, b, UI_HEX(0xffb03a));
    }
}

static void draw_comment(UiCtx *ui, const LxGraph *g, int i) {
    const LxGraphComment *c = &g->comments[i];
    static const uint32_t bgs[GE_COLORS] = {0x2a2414, 0x3a1612, 0x13283f, 0x2a1630, 0x16301a, 0x123232, 0x2a2414, 0x221d1d};
    static const uint32_t bds[GE_COLORS] = {0x5a4a20, 0x7a2a1e, 0x2a4a7a, 0x5a2c6a, 0x2c6a34, 0x246a6a, 0x5a4a20, 0x4a3c3a};
    int k = c->color >= 0 && c->color < GE_COLORS ? c->color : 0;
    float s = SC(), x = sx(c->x), y = sy(c->y), w = c->w * s, h = c->h * s;
    ui_draw_rect(ui, x, y, w, h, ui_alpha(UI_HEX(bgs[k]), 0.92f), 4 * s);
    ui_draw_border(ui, x, y, w, h, GE.sel_comment == i ? COL_ACCENT : UI_HEX(bds[k]), 4 * s, GE.sel_comment == i ? 2 : 1);
    if (GE.zoom >= 0.45f) {
        ui_push_clip(ui, x, y, w, h);
        draw_wrapped(ui, GE.zoom >= 0.8f ? UI_FONT_BODY : UI_FONT_SMALL, x + 10 * s, y + 8 * s, w - 20 * s, y + h - 4 * s, c->text, COL_TEXT);
        ui_pop_clip(ui);
    }
    ui_draw_line(ui, x + w - 9 * s, y + h - 3 * s, x + w - 3 * s, y + h - 9 * s, 1, COL_DIM);   /* resize grip */
}

static void graph_bounds(const LxGraph *g, float *x0, float *y0, float *x1, float *y1, bool sel_only) {
    *x0 = *y0 = 1e9f;
    *x1 = *y1 = -1e9f;
    for (int i = 0; i < g->state_count; i++) {
        if (sel_only && !is_selected(i)) continue;
        const LxGraphState *st = &g->states[i];
        *x0 = lx_minf(*x0, st->x);
        *y0 = lx_minf(*y0, st->y - 30);
        *x1 = lx_maxf(*x1, st->x + state_w(st));
        *y1 = lx_maxf(*y1, st->y + state_h(st));
    }
    for (int i = 0; i < g->comment_count && !sel_only; i++) {
        const LxGraphComment *c = &g->comments[i];
        *x0 = lx_minf(*x0, c->x);
        *y0 = lx_minf(*y0, c->y);
        *x1 = lx_maxf(*x1, c->x + c->w);
        *y1 = lx_maxf(*y1, c->y + c->h);
    }
    if (*x0 > *x1) { *x0 = *y0 = 0; *x1 = *y1 = 100; }
}

static void draw_minimap(UiCtx *ui, const LxGraph *g, const LxFsmRun *run) {
    float mx, my, mw, mh;
    minimap_rect(&mx, &my, &mw, &mh);
    float x0, y0, x1, y1;
    graph_bounds(g, &x0, &y0, &x1, &y1, false);
    /* include the visible area */
    float vx0 = gx_of(GE.canvas->x), vy0 = gy_of(GE.canvas->y), vx1 = gx_of(GE.canvas->x + GE.canvas->cw), vy1 = gy_of(GE.canvas->y + GE.canvas->ch);
    x0 = lx_minf(x0, vx0); y0 = lx_minf(y0, vy0); x1 = lx_maxf(x1, vx1); y1 = lx_maxf(y1, vy1);
    float k = lx_minf((mw - 12) / lx_maxf(1, x1 - x0), (mh - 12) / lx_maxf(1, y1 - y0));
    float ox = mx + (mw - (x1 - x0) * k) * 0.5f, oy = my + (mh - (y1 - y0) * k) * 0.5f;
    ui_draw_rect(ui, mx, my, mw, mh, UI_RGBA(10, 7, 7, 220), 5);
    ui_draw_border(ui, mx, my, mw, mh, UI_HEX(0x34363c), 5, 1);
    for (int i = 0; i < g->comment_count; i++) {
        const LxGraphComment *c = &g->comments[i];
        ui_draw_rect(ui, ox + (c->x - x0) * k, oy + (c->y - y0) * k, c->w * k, c->h * k, UI_RGBA(90, 74, 32, 120), 1);
    }
    for (int i = 0; i < g->state_count; i++) {
        const LxGraphState *st = &g->states[i];
        uint32_t c = run && run->state == i ? COL_HOT : is_selected(i) ? COL_ACCENT : ge_state_color(st->color == 0 ? 7 : st->color);
        ui_draw_rect(ui, ox + (st->x - x0) * k, oy + (st->y - y0) * k, lx_maxf(2, state_w(st) * k), lx_maxf(2, state_h(st) * k), c, 1);
    }
    ui_draw_border(ui, ox + (vx0 - x0) * k, oy + (vy0 - y0) * k, (vx1 - vx0) * k, (vy1 - vy0) * k, COL_TEXT, 2, 1);
}
static void minimap_pan(float m_x, float m_y) {
    LxGraph *g = ge_graph();
    if (!g) return;
    float mx, my, mw, mh;
    minimap_rect(&mx, &my, &mw, &mh);
    float x0, y0, x1, y1;
    graph_bounds(g, &x0, &y0, &x1, &y1, false);
    float vw = GE.canvas->cw / SC(), vh = GE.canvas->ch / SC();
    float vx0 = GE.pan_x, vy0 = GE.pan_y;
    x0 = lx_minf(x0, vx0); y0 = lx_minf(y0, vy0); x1 = lx_maxf(x1, vx0 + vw); y1 = lx_maxf(y1, vy0 + vh);
    float k = lx_minf((mw - 12) / lx_maxf(1, x1 - x0), (mh - 12) / lx_maxf(1, y1 - y0));
    float ox = mx + (mw - (x1 - x0) * k) * 0.5f, oy = my + (mh - (y1 - y0) * k) * 0.5f;
    GE.pan_x = x0 + (m_x - ox) / k - vw * 0.5f;
    GE.pan_y = y0 + (m_y - oy) / k - vh * 0.5f;
}

static void draw_center_message(UiCtx *ui, UiNode *n, const char *title, const char *sub) {
    float s = ui_scale(ui);
    float tw = ui_text_width(ui, UI_FONT_TITLE, title);
    ui_draw_icon(ui, UI_ICON_GRAPH, n->x + (n->cw - 48 * s) * 0.5f, n->y + n->ch * 0.5f - 90 * s, 48 * s, UI_HEX(0x45474e));
    ui_draw_text(ui, UI_FONT_TITLE, n->x + (n->cw - tw) * 0.5f, n->y + n->ch * 0.5f - 30 * s, title, COL_TEXT);
    if (sub) {
        float sw = ui_text_width(ui, UI_FONT_BODY, sub);
        ui_draw_text(ui, UI_FONT_BODY, n->x + (n->cw - sw) * 0.5f, n->y + n->ch * 0.5f + 6 * s, sub, COL_DIM);
    }
}

static void canvas_draw(UiNode *n, UiCtx *ui) {
    float s = ui_scale(ui);
    ui_draw_rect(ui, n->x, n->y, n->cw, n->ch, UI_HEX(0x16171a), 0);
    ui_push_clip(ui, n->x, n->y, n->cw, n->ch);
    draw_grid(ui, n);
    LxGraph *g = ge_graph();
    if (!g) {
        if (GE.ref[0]) draw_center_message(ui, n, "This graph can't be opened", GE.ref);
        else if (GE.obj && ED.scene && lx_entity_valid(ED.scene, GE.obj)) {
            char b[160];
            snprintf(b, sizeof b, "%s has no State Graph", lx_entity_name(ED.scene, GE.obj));
            draw_center_message(ui, n, b, "Click here (or right-click) to add one, or drop a .lxgraph asset here.");
        } else draw_center_message(ui, n, "State Graph", "Select an object to see its State Graph, or a .lxgraph asset in the Content Browser.");
        ui_pop_clip(ui);
        return;
    }
    LxFsmRun *run = ge_run();
    /* watermark: graph name and description */
    ui_draw_text(ui, UI_FONT_TITLE, n->x + 16 * s, n->y + 10 * s, g->name, UI_RGBA(241, 230, 225, 34));
    if (g->description[0]) draw_wrapped(ui, UI_FONT_SMALL, n->x + 18 * s, n->y + 44 * s, 420 * s, n->y + 120 * s, g->description, UI_RGBA(241, 230, 225, 56));
    for (int i = 0; i < g->comment_count; i++) draw_comment(ui, g, i);
    /* links under the states */
    for (int i = 0; i < g->state_count; i++) {
        const LxGraphState *st = &g->states[i];
        for (int t = 0; t < st->trans_count; t++) {
            float p[8];
            if (!link_curve(g, i, t, p)) continue;
            bool hot = (is_selected(i) && (GE.sel_trans < 0 || GE.sel_trans == t)) ||
                       (run && run->prev_state == i && run->state_time < 1.0f && !strcmp(run->last_event, st->trans[t].event));
            draw_curve(ui, p, hot ? COL_HOT : COL_LINK, (hot ? 2.4f : 1.6f) * s * lx_clampf(GE.zoom, 0.7f, 1.3f), true);
            ui_draw_circle(ui, sx(p[0]), sy(p[1]), 3 * SC(), hot ? COL_HOT : COL_LINK, 10);
        }
    }
    for (int i = 0; i < g->state_count; i++) if (!is_selected(i)) draw_state(ui, g, i, run);
    for (int i = 0; i < g->state_count; i++) if (is_selected(i)) draw_state(ui, g, i, run);
    /* gesture feedback */
    LxVec2 m = lx_mouse_pos();
    if (GE.drag == GE_DRAG_LINK && GE.drag_moved && GE.link_state >= 0 && GE.link_state < g->state_count) {
        const LxGraphState *st = &g->states[GE.link_state];
        float ax, ay;
        if (GE.link_trans >= 0) { ax = sx(st->x + state_w(st)); ay = sy(st->y + GE_TITLE_H + GE_ROW_H * ((float)GE.link_trans + 0.5f)); }
        else { ax = sx(st->x + state_w(st) * 0.5f); ay = sy(st->y); }
        ui_draw_line(ui, ax, ay, m.x, m.y, 2 * s, COL_HOT);
        ui_draw_circle(ui, m.x, m.y, 4 * s, COL_HOT, 12);
    }
    if (GE.drag == GE_DRAG_START && GE.drag_moved) {
        ui_draw_line(ui, GE.press.x, GE.press.y, m.x, m.y, 2 * s, COL_START);
        ui_draw_circle(ui, m.x, m.y, 4 * s, COL_START, 12);
    }
    if (GE.drag == GE_DRAG_BOX && GE.drag_moved) {
        float x0 = lx_minf(GE.press.x, m.x), y0 = lx_minf(GE.press.y, m.y);
        float w = fabsf(m.x - GE.press.x), h = fabsf(m.y - GE.press.y);
        ui_draw_rect(ui, x0, y0, w, h, ui_alpha(COL_ACCENT, 0.12f), 0);
        ui_draw_border(ui, x0, y0, w, h, COL_ACCENT, 0, 1);
    }
    if (run && run->error) {
        const char *msg = "Stopped: too many state changes in one frame (an endless loop?)";
        float tw = ui_text_width(ui, UI_FONT_BOLD, msg) + 24 * s;
        ui_draw_rect(ui, n->x + (n->cw - tw) * 0.5f, n->y + 10 * s, tw, 28 * s, UI_HEX(0x5a1414), 5 * s);
        ui_draw_text(ui, UI_FONT_BOLD, n->x + (n->cw - tw) * 0.5f + 12 * s, n->y + 10 * s + (28 * s - ui_line_height(ui, UI_FONT_BOLD)) * 0.5f, msg, COL_TEXT);
    }
    bool map = minimap_shown();
    if (GE.hints) {
        static const char *const lines[] = {"Right-click: add states, comments", "Drag an event onto a state: link it", "Double-click: rename · Space: actions", "Drag a comment: moves the states in it",
                                            "Middle/Alt-drag: pan · Scroll: zoom · F: frame", "Del · Ctrl+C/V/D · Ctrl+Z: undo"};
        float lh = ui_line_height(ui, UI_FONT_SMALL), room = n->cw - (map ? 180 * s : 0) - 24 * s;
        int count = (int)(sizeof lines / sizeof lines[0]);
        float y = n->y + n->ch - lh * (float)count - 10 * s;
        for (int i = 0; i < count; i++)
            if (ui_text_width(ui, UI_FONT_SMALL, lines[i]) < room) ui_draw_text(ui, UI_FONT_SMALL, n->x + 12 * s, y + lh * (float)i, lines[i], UI_RGBA(241, 230, 225, 80));
    }
    if (map) draw_minimap(ui, g, run);
    if (GE.focused) ui_draw_border(ui, n->x, n->y, n->cw, n->ch, ui_alpha(COL_ACCENT, 0.35f), 0, 1);
    ui_pop_clip(ui);
}

/* ======================================================================
 *  view
 * ====================================================================== */
static void frame_rect(float x0, float y0, float x1, float y1) {
    if (!GE.canvas || GE.canvas->cw < 10) return;
    float s = ui_scale(ED.ui);
    float vw = GE.canvas->cw / s, vh = GE.canvas->ch / s;
    float pad = 60;
    GE.zoom = lx_clampf(lx_minf(vw / (x1 - x0 + pad * 2), vh / (y1 - y0 + pad * 2)), 0.3f, 1.0f);
    GE.pan_x = (x0 + x1) * 0.5f - vw / GE.zoom * 0.5f;
    GE.pan_y = (y0 + y1) * 0.5f - vh / GE.zoom * 0.5f;
}
void ge_frame_all(void) {
    LxGraph *g = ge_graph();
    if (!g) return;
    float x0, y0, x1, y1;
    graph_bounds(g, &x0, &y0, &x1, &y1, GE.sel_count > 0);
    frame_rect(x0, y0, x1, y1);
}
static void zoom_at(float factor, float mx, float my) {
    float gx = gx_of(mx), gy = gy_of(my);
    GE.zoom = lx_clampf(GE.zoom * factor, 0.25f, 2.0f);
    GE.pan_x = gx - (mx - GE.canvas->x) / SC();
    GE.pan_y = gy - (my - GE.canvas->y) / SC();
}

/* ======================================================================
 *  editing helpers
 * ====================================================================== */
static void add_state_at(float gx, float gy) {
    LxGraph *g = ge_graph();
    if (!g) return;
    char name[LX_FSM_NAME];
    lx_graph_unique_name(g, "State 1", name, sizeof name, 0);
    lx_graph_add_state(g, name, roundf(gx / 8) * 8, roundf(gy / 8) * 8);
    ge_select_state(g->state_count - 1, false);
    ge_commit(true, NULL);
}

static void delete_selection(void) {
    LxGraph *g = ge_graph();
    if (!g) return;
    if (GE.sel_comment >= 0 && GE.sel_comment < g->comment_count) {
        memmove(&g->comments[GE.sel_comment], &g->comments[GE.sel_comment + 1], sizeof *g->comments * (size_t)(g->comment_count - GE.sel_comment - 1));
        g->comment_count--;
        GE.sel_comment = -1;
        ge_commit(true, NULL);
        return;
    }
    LxGraphState *st = ge_sel_state();
    if (st && GE.sel_count == 1 && GE.sel_trans >= 0 && GE.sel_trans < st->trans_count) {
        lx_graph_remove_transition(st, GE.sel_trans);
        GE.sel_trans = -1;
        ge_commit(true, NULL);
        return;
    }
    if (!GE.sel_count) return;
    if (GE.sel_count >= g->state_count) { ui_notify(ED.ui, "A graph keeps at least one state"); return; }
    /* highest index first so the others stay valid */
    int idx[GE_MAX_SEL];
    int n = GE.sel_count;
    memcpy(idx, GE.sel, sizeof(int) * (size_t)n);
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++) if (idx[b] > idx[a]) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
    for (int a = 0; a < n; a++) lx_graph_remove_state(g, idx[a]);
    GE.sel_count = 0;
    GE.sel_trans = -1;
    ge_commit(true, NULL);
}

/* clipboard: graph text of the selected states (plus the events and variables they use) */
static char *g_clip;

static void copy_selection(void) {
    LxGraph *g = ge_graph();
    if (!g || !GE.sel_count) return;
    LxGraph *c = lx_graph_clone(g);
    /* keep only selected states */
    int n = 0;
    for (int i = 0; i < c->state_count; i++) {
        if (is_selected(i)) c->states[n++] = c->states[i];
        else { lx_free(c->states[i].actions); lx_free(c->states[i].trans); }
    }
    c->state_count = n;
    c->global_count = 0;
    c->comment_count = 0;
    c->start[0] = 0;
    lx_free(g_clip);
    g_clip = lx_graph_to_text(c);
    lx_graph_free(c);
    ui_notify(ED.ui, "Copied %d state%s", n, n == 1 ? "" : "s");
}

static void paste_at(float gx, float gy) {
    LxGraph *g = ge_graph();
    if (!g || !g_clip) return;
    LxGraph *c = lx_graph_from_text(g_clip);
    if (!c || !c->state_count) { lx_graph_free(c); return; }
    float x0 = 1e9f, y0 = 1e9f;
    for (int i = 0; i < c->state_count; i++) { x0 = lx_minf(x0, c->states[i].x); y0 = lx_minf(y0, c->states[i].y); }
    /* new names for clashes; links between pasted states follow */
    char olds[GE_MAX_SEL][LX_FSM_NAME], news[GE_MAX_SEL][LX_FSM_NAME];
    int count = c->state_count < GE_MAX_SEL ? c->state_count : GE_MAX_SEL;
    for (int i = 0; i < count; i++) {
        lx_strcpy(olds[i], LX_FSM_NAME, c->states[i].name);
        lx_graph_unique_name(g, c->states[i].name, news[i], LX_FSM_NAME, 0);
        for (int k = 0; k < i; k++)   /* also unique among the pasted ones */
            if (!strcmp(news[k], news[i])) { char b[LX_FSM_NAME]; snprintf(b, sizeof b, "%.50s %d", news[i], i + 2); lx_strcpy(news[i], LX_FSM_NAME, b); }
    }
    for (int i = 0; i < c->event_count; i++)
        if (!lx_graph_find_event(g, c->events[i].name) && !lx_fsm_is_system_event(c->events[i].name)) lx_graph_add_event(g, c->events[i].name)->global = c->events[i].global;
    for (int i = 0; i < c->var_count; i++)
        if (!lx_graph_find_var(g, c->vars[i].name)) { LxGraphVar *v = lx_graph_add_var(g, c->vars[i].name, c->vars[i].type); *v = c->vars[i]; }
    GE.sel_count = 0;
    for (int i = 0; i < count; i++) {
        const LxGraphState *src = &c->states[i];
        LxGraphState *st = lx_graph_add_state(g, news[i], roundf((src->x - x0 + gx) / 8) * 8, roundf((src->y - y0 + gy) / 8) * 8);
        int si = g->state_count - 1;
        st->color = src->color;
        st->sequence = src->sequence;
        lx_strcpy(st->description, sizeof st->description, src->description);
        for (int a = 0; a < src->action_count; a++) {
            const LxFsmActionDesc *d = lx_fsm_action_find(src->actions[a].type);
            if (!d) continue;
            LxGraphAction *na = lx_graph_add_action(&g->states[si], d, -1);
            *na = src->actions[a];
        }
        for (int t = 0; t < src->trans_count; t++) {
            LxGraphTransition *nt = lx_graph_add_transition(g, &g->states[si], src->trans[t].event);
            const char *to = src->trans[t].to;
            for (int k = 0; k < count; k++) if (!strcmp(olds[k], to)) to = news[k];
            if (nt) lx_strcpy(nt->to, sizeof nt->to, to);
        }
        if (GE.sel_count < GE_MAX_SEL) GE.sel[GE.sel_count++] = si;
    }
    lx_graph_free(c);
    GE.sel_trans = -1;
    ge_commit(true, NULL);
}

/* simple layered layout: depth from the start state along transitions */
static void auto_layout(void) {
    LxGraph *g = ge_graph();
    if (!g || !g->state_count) return;
    int n = g->state_count;
    int *depth = lx_calloc((size_t)n, sizeof(int)), *queue = lx_calloc((size_t)n, sizeof(int)), *row = lx_calloc((size_t)n + 1, sizeof(int));
    for (int i = 0; i < n; i++) depth[i] = -1;
    int qh = 0, qt = 0, s0 = lx_graph_state_index(g, g->start);
    if (s0 < 0) s0 = 0;
    depth[s0] = 0;
    queue[qt++] = s0;
    while (qh < qt) {
        int i = queue[qh++];
        const LxGraphState *st = &g->states[i];
        for (int t = 0; t < st->trans_count; t++) {
            int j = lx_graph_state_index(g, st->trans[t].to);
            if (j >= 0 && depth[j] < 0) { depth[j] = depth[i] + 1; queue[qt++] = j; }
        }
    }
    /* states reached only by global transitions or not at all: their own column at the end */
    int maxd = 0;
    for (int i = 0; i < n; i++) if (depth[i] > maxd) maxd = depth[i];
    for (int i = 0; i < n; i++) if (depth[i] < 0) depth[i] = maxd + 1;
    float colw[64] = {0};
    for (int i = 0; i < n; i++) { int d = depth[i] < 63 ? depth[i] : 63; colw[d] = lx_maxf(colw[d], state_w(&g->states[i])); }
    float colx[64];
    colx[0] = 80;
    for (int d = 1; d < 64; d++) colx[d] = colx[d - 1] + colw[d - 1] + 110;
    float rowy[64];
    for (int d = 0; d < 64; d++) rowy[d] = 80;
    for (int qi = 0; qi < n; qi++) {
        int i = qi < qt ? queue[qi] : -1;
        if (i < 0) break;
        int d = depth[i] < 63 ? depth[i] : 63;
        g->states[i].x = colx[d];
        g->states[i].y = rowy[d];
        rowy[d] += state_h(&g->states[i]) + 70;
    }
    for (int i = 0; i < n; i++) {
        bool placed = false;
        for (int q = 0; q < qt; q++) if (queue[q] == i) placed = true;
        if (placed) continue;
        int d = depth[i] < 63 ? depth[i] : 63;
        g->states[i].x = colx[d];
        g->states[i].y = rowy[d];
        rowy[d] += state_h(&g->states[i]) + 70;
    }
    lx_free(depth);
    lx_free(queue);
    lx_free(row);
    ge_commit(false, NULL);
    ge_frame_all();
}

/* ======================================================================
 *  context menus
 * ====================================================================== */
static int g_menu_state = -1, g_menu_trans = -1, g_menu_comment = -1;
static float g_menu_gx, g_menu_gy;

static UiNode *mitem(UiNode *p, const char *text, UiIcon icon, void (*cb)(UiNode *)) {
    UiNode *it = ui_menu_item(ED.ui, p, text, NULL, cb);
    if (icon) { it->icon = icon; it->icon_color = COL_DIM; }
    return it;
}
/* an item that opens another menu: it must not close the popups after its handler */
static UiNode *subitem(UiNode *p, const char *text, void (*cb)(UiNode *)) {
    UiNode *it = ui_menu_item(ED.ui, p, text, "›", cb);
    it->menu_item = false;
    return it;
}

static void m_add_state(UiNode *n) { LX_UNUSED(n); add_state_at(g_menu_gx, g_menu_gy); }
static void m_add_comment(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g) return;
    LxGraphComment c;
    memset(&c, 0, sizeof c);
    c.x = roundf(g_menu_gx / 8) * 8;
    c.y = roundf(g_menu_gy / 8) * 8;
    c.w = 220;
    c.h = 90;
    lx_strcpy(c.text, sizeof c.text, "Note");
    LX_VEC_PUSH(g->comments, g->comment_count, g->comment_cap, c);
    GE.sel_count = 0;
    GE.sel_comment = g->comment_count - 1;
    ge_commit(true, NULL);
}
static void m_paste(UiNode *n) { LX_UNUSED(n); paste_at(g_menu_gx, g_menu_gy); }
static void m_select_all(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g) return;
    GE.sel_count = 0;
    for (int i = 0; i < g->state_count && i < GE_MAX_SEL; i++) GE.sel[GE.sel_count++] = i;
    GE.insp_dirty = true;
}
static void m_auto_layout(UiNode *n) { LX_UNUSED(n); auto_layout(); }
static void m_frame(UiNode *n) { LX_UNUSED(n); GE.sel_count = 0; ge_frame_all(); }
static void m_add_graph(UiNode *n) { LX_UNUSED(n); if (GE.obj) ge_new_graph_for(GE.obj); }

static void m_set_start(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    lx_strcpy(g->start, sizeof g->start, g->states[g_menu_state].name);
    ge_commit(true, NULL);
}
static void m_toggle_sequence(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    g->states[g_menu_state].sequence = !g->states[g_menu_state].sequence;
    ge_commit(true, NULL);
}
static void m_color_pick(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g) return;
    int c = (int)n->tag;
    if (g_menu_comment >= 0 && g_menu_comment < g->comment_count) g->comments[g_menu_comment].color = c;
    else
        for (int k = 0; k < GE.sel_count; k++)
            if (GE.sel[k] < g->state_count) g->states[GE.sel[k]].color = c;
    ge_commit(true, NULL);
}
static void m_colors(UiNode *n) {
    float x = n->x + n->cw, y = n->y;
    ui_close_popups(ED.ui);
    UiNode *p = ui_popup(ED.ui, x, y, 150);
    if (!p) return;
    for (int i = 0; i < GE_COLORS; i++) {
        UiNode *it = ui_menu_item(ED.ui, p, COLOR_NAMES[i], NULL, m_color_pick);
        it->icon = UI_ICON_CUBE;
        it->icon_color = i == 0 ? COL_DIM : ge_state_color(i);
        it->tag = i;
    }
}
static void rename_done(const char *text) {
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    if (!lx_graph_rename_state(g, g_menu_state, text)) { ui_notify(ED.ui, "There's already a state called %s", text); return; }
    ge_commit(true, NULL);
}
static void m_rename(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    ed_prompt("Rename State", "Name", g->states[g_menu_state].name, rename_done);
}
static void m_copy(UiNode *n) { LX_UNUSED(n); copy_selection(); }
static void m_duplicate(UiNode *n) {
    LX_UNUSED(n);
    LxGraphState *st = ge_sel_state();
    if (!st) return;
    float x = st->x + 40, y = st->y + 40;
    copy_selection();
    paste_at(x, y);
}
static void m_delete(UiNode *n) { LX_UNUSED(n); GE.sel_trans = -1; delete_selection(); }
static void m_add_action(UiNode *n) { LX_UNUSED(n); ge_open_action_browser(); }

/* events: pick one for a new transition, a new global transition, or to change a transition's event */
enum { EV_ADD_TRANS, EV_ADD_GLOBAL, EV_CHANGE };
static int g_ev_mode;
static void event_new_done(const char *text);
static void apply_event(const char *ev) {
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count || !ev[0]) return;
    if (!lx_fsm_is_system_event(ev) && !lx_graph_find_event(g, ev)) lx_graph_add_event(g, ev);
    LxGraphState *st = &g->states[g_menu_state];
    if (g_ev_mode == EV_ADD_TRANS) {
        for (int t = 0; t < st->trans_count; t++)
            if (!strcmp(st->trans[t].event, ev)) { ui_notify(ED.ui, "%s already has %s", st->name, ev); return; }
        lx_graph_add_transition(g, st, ev);
        GE.sel_trans = st->trans_count - 1;
    } else if (g_ev_mode == EV_ADD_GLOBAL) {
        for (int i = 0; i < g->global_count; i++)
            if (!strcmp(g->globals[i].event, ev)) { ui_notify(ED.ui, "%s is already a global transition (to %s)", ev, g->globals[i].to); return; }
        LxGraphTransition t;
        memset(&t, 0, sizeof t);
        lx_strcpy(t.event, sizeof t.event, ev);
        lx_strcpy(t.to, sizeof t.to, st->name);
        LX_VEC_PUSH(g->globals, g->global_count, g->global_cap, t);
    } else if (g_menu_trans >= 0 && g_menu_trans < st->trans_count) {
        lx_strcpy(st->trans[g_menu_trans].event, sizeof st->trans[g_menu_trans].event, ev);
    }
    ge_commit(true, NULL);
}
static void event_pick(UiNode *n) {
    if (!strcmp(n->path, "+new")) { ed_prompt("New Event", "Event name (e.g. OPEN_DOOR)", "", event_new_done); return; }
    char ev[LX_FSM_NAME];
    lx_strcpy(ev, sizeof ev, n->path);
    apply_event(ev);
}
static void event_new_done(const char *text) {
    char ev[LX_FSM_NAME];
    size_t k = 0;
    for (const char *p = text; *p && k + 1 < sizeof ev; p++) ev[k++] = *p == ' ' ? '_' : (char)toupper((unsigned char)*p);
    ev[k] = 0;
    apply_event(ev);
}
static void event_menu(UiNode *from, int mode) {
    float x = from->x + from->cw, y = from->y;
    ui_close_popups(ED.ui);
    g_ev_mode = mode;
    LxGraph *g = ge_graph();
    UiNode *p = ui_popup(ED.ui, x, y, 200);
    if (!p || !g) return;
    UiNode *it = ui_menu_item(ED.ui, p, "New Event...", NULL, event_pick);
    it->icon = UI_ICON_PLUS;
    lx_strcpy(it->path, sizeof it->path, "+new");
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "FINISHED", NULL, event_pick);
    lx_strcpy(it->path, sizeof it->path, "FINISHED");
    for (int i = 0; i < g->event_count; i++) {
        it = ui_menu_item(ED.ui, p, g->events[i].name, NULL, event_pick);
        lx_strcpy(it->path, sizeof it->path, g->events[i].name);
    }
    LxGlobals *gl = lx_fsm_globals();
    for (int i = 0; gl && i < gl->event_count; i++) {
        if (lx_graph_find_event(g, gl->events[i].name)) continue;
        it = ui_menu_item(ED.ui, p, gl->events[i].name, "global", event_pick);
        lx_strcpy(it->path, sizeof it->path, gl->events[i].name);
    }
    ui_menu_separator(ED.ui, p);
    for (int i = 0; i < LX_FSM_SYSTEM_EVENT_COUNT; i++) {
        if (!strcmp(LX_FSM_SYSTEM_EVENTS[i], "FINISHED")) continue;
        it = ui_menu_item(ED.ui, p, LX_FSM_SYSTEM_EVENTS[i], "system", event_pick);
        it->fg = COL_DIM;
        lx_strcpy(it->path, sizeof it->path, LX_FSM_SYSTEM_EVENTS[i]);
    }
}
static void m_add_trans(UiNode *n) { event_menu(n, EV_ADD_TRANS); }
static void m_add_global(UiNode *n) { event_menu(n, EV_ADD_GLOBAL); }
static void m_change_event(UiNode *n) { event_menu(n, EV_CHANGE); }
static void m_clear_link(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    LxGraphState *st = &g->states[g_menu_state];
    if (g_menu_trans >= 0 && g_menu_trans < st->trans_count) { st->trans[g_menu_trans].to[0] = 0; ge_commit(true, NULL); }
}
static void m_move_trans(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    LxGraphState *st = &g->states[g_menu_state];
    int a = g_menu_trans, b = a + (int)n->tag;
    if (a < 0 || b < 0 || a >= st->trans_count || b >= st->trans_count) return;
    LxGraphTransition t = st->trans[a];
    st->trans[a] = st->trans[b];
    st->trans[b] = t;
    GE.sel_trans = b;
    ge_commit(true, NULL);
}
static void m_remove_trans(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    lx_graph_remove_transition(&g->states[g_menu_state], g_menu_trans);
    GE.sel_trans = -1;
    ge_commit(true, NULL);
}
static void m_remove_global(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_trans < 0 || g_menu_trans >= g->global_count) return;
    memmove(&g->globals[g_menu_trans], &g->globals[g_menu_trans + 1], sizeof *g->globals * (size_t)(g->global_count - g_menu_trans - 1));
    g->global_count--;
    ge_commit(true, NULL);
}
static void comment_text_done(const char *text) {
    LxGraph *g = ge_graph();
    if (!g || g_menu_comment < 0 || g_menu_comment >= g->comment_count) return;
    lx_strcpy(g->comments[g_menu_comment].text, sizeof g->comments[g_menu_comment].text, text);
    ge_commit(true, NULL);
}
static void m_comment_text(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_comment < 0 || g_menu_comment >= g->comment_count) return;
    ed_prompt("Comment", "Text", g->comments[g_menu_comment].text, comment_text_done);
}

static void open_context_menu(Hit h, float mx, float my) {
    LxGraph *g = ge_graph();
    UiNode *p = ui_popup(ED.ui, mx, my, 210);
    if (!p) return;
    g_menu_gx = gx_of(mx);
    g_menu_gy = gy_of(my);
    g_menu_state = h.state;
    g_menu_trans = h.trans;
    g_menu_comment = h.comment;
    if (!g) {
        if (GE.obj && !GE.ref[0]) mitem(p, "Add State Graph", UI_ICON_GRAPH, m_add_graph);
        else { UiNode *it = mitem(p, "Select an object first", UI_ICON_INFO, NULL); it->disabled = true; }
        return;
    }
    if (h.kind == H_STATE || h.kind == H_EVENT || h.kind == H_START) {
        if (!is_selected(h.state)) ge_select_state(h.state, false);
        if (h.kind == H_EVENT) {
            GE.sel_trans = h.trans;
            subitem(p, "Change Event", m_change_event);
            mitem(p, "Clear Link", UI_ICON_CLOSE, m_clear_link);
            UiNode *u = mitem(p, "Move Up", UI_ICON_CHEVRON_UP, m_move_trans); u->tag = -1;
            UiNode *d = mitem(p, "Move Down", UI_ICON_CHEVRON_DOWN, m_move_trans); d->tag = 1;
            mitem(p, "Remove Transition", UI_ICON_TRASH, m_remove_trans);
            ui_menu_separator(ED.ui, p);
        }
        mitem(p, "Set as Start State", UI_ICON_PLAY, m_set_start);
        subitem(p, "Add Transition", m_add_trans);
        subitem(p, "Add Global Transition", m_add_global);
        mitem(p, "Add Action...", UI_ICON_PLUS, m_add_action);
        UiNode *sq = mitem(p, "Sequence (one action at a time)", UI_ICON_LIST, m_toggle_sequence);
        if (g->states[h.state].sequence) { sq->icon = UI_ICON_CHECK; sq->icon_color = COL_START; }
        subitem(p, "Color", m_colors);
        ui_menu_separator(ED.ui, p);
        mitem(p, "Rename...", UI_ICON_NONE, m_rename)->hint[0] = 0;
        mitem(p, "Copy", UI_ICON_COPY, m_copy);
        mitem(p, "Duplicate", UI_ICON_COPY, m_duplicate);
        UiNode *del = mitem(p, GE.sel_count > 1 ? "Delete States" : "Delete State", UI_ICON_TRASH, m_delete);
        if (g->state_count <= GE.sel_count) del->disabled = true;
        return;
    }
    if (h.kind == H_GLOBAL) {
        mitem(p, "Remove Global Transition", UI_ICON_TRASH, m_remove_global);
        return;
    }
    if (h.kind == H_COMMENT || h.kind == H_COMMENT_SIZE) {
        GE.sel_count = 0;
        GE.sel_comment = h.comment;
        mitem(p, "Edit Text...", UI_ICON_NONE, m_comment_text);
        subitem(p, "Color", m_colors);
        mitem(p, "Delete Comment", UI_ICON_TRASH, m_delete);
        return;
    }
    mitem(p, "Add State", UI_ICON_PLUS, m_add_state);
    mitem(p, "Add Comment", UI_ICON_INFO, m_add_comment);
    UiNode *ps = mitem(p, "Paste", UI_ICON_COPY, m_paste);
    ps->disabled = !g_clip;
    ui_menu_separator(ED.ui, p);
    mitem(p, "Select All", UI_ICON_SELECT, m_select_all);
    mitem(p, "Auto Layout", UI_ICON_GRID, m_auto_layout);
    mitem(p, "Frame All", UI_ICON_FOCUS, m_frame);
    mitem(p, "Action Browser", UI_ICON_LIST, m_add_action);
}

static void canvas_right_click(UiNode *n) {
    LxVec2 m = lx_mouse_pos();
    LX_UNUSED(n);
    if (GE.drag != GE_DRAG_NONE) return;
    open_context_menu(hit_test(m.x, m.y), m.x, m.y);
}

/* ======================================================================
 *  input
 * ====================================================================== */
static int g_group[GE_MAX_SEL], g_group_count;   /* states inside the comment being dragged */

static void finish_link(LxGraph *g, float mx, float my) {
    Hit h = hit_test(mx, my);
    int target = h.kind == H_STATE || h.kind == H_EVENT || h.kind == H_START || h.kind == H_GLOBAL ? h.state : -1;
    if (GE.link_state < 0 || GE.link_state >= g->state_count) return;
    if (GE.link_trans == -2) {   /* START dragged onto a state */
        if (target >= 0) { lx_strcpy(g->start, sizeof g->start, g->states[target].name); ge_commit(true, NULL); }
        return;
    }
    if (GE.link_trans == -1) {   /* a global transition re-aimed */
        if (target >= 0 && GE.link_global >= 0 && GE.link_global < g->global_count) {
            lx_strcpy(g->globals[GE.link_global].to, sizeof g->globals[GE.link_global].to, g->states[target].name);
            ge_commit(true, NULL);
        }
        return;
    }
    LxGraphState *st = &g->states[GE.link_state];
    if (GE.link_trans >= st->trans_count) return;
    LxGraphTransition *t = &st->trans[GE.link_trans];
    if (target >= 0) lx_strcpy(t->to, sizeof t->to, g->states[target].name);
    else t->to[0] = 0;   /* dropped on nothing: unlinked */
    ge_commit(true, NULL);
}

static void canvas_frame(UiNode *n) {
    UiCtx *ui = ED.ui;
    LxGraph *g = ge_graph();
    if (g && !GE.framed && n->cw > 50) {   /* first look at a graph: fit all of it */
        GE.framed = true;
        float x0, y0, x1, y1;
        graph_bounds(g, &x0, &y0, &x1, &y1, false);
        frame_rect(x0, y0, x1, y1);
    }
    bool blocked = ui_has_modal(ui) || ui_popup_open(ui);
    bool hot = !blocked && ui_is_inside(ui, n) && (ui_is_hot(ui, n) || ui_active(ui) == n);
    LxVec2 m = lx_mouse_pos();
    bool pressed_l = lx_mouse_pressed(LX_MOUSE_LEFT), pressed_m = lx_mouse_pressed(LX_MOUSE_MIDDLE);
    if ((pressed_l || pressed_m || lx_mouse_pressed(LX_MOUSE_RIGHT)) && !blocked) GE.focused = hot;
    if (!g) {
        if (hot && pressed_l && GE.obj && !GE.ref[0]) open_context_menu((Hit){H_NONE, -1, -1, -1}, m.x, m.y);
        return;
    }
    /* zoom */
    float wheel = lx_mouse_wheel();
    if (hot && wheel != 0 && GE.drag == GE_DRAG_NONE) zoom_at(powf(1.12f, wheel), m.x, m.y);
    /* start a gesture */
    if (hot && GE.drag == GE_DRAG_NONE && (pressed_l || pressed_m)) {
        GE.press = GE.last = m;
        GE.press_gx = gx_of(m.x);
        GE.press_gy = gy_of(m.y);
        GE.drag_moved = false;
        bool shift = lx_key_down(LX_KEY_SHIFT), alt = lx_key_down(LX_KEY_ALT), space = lx_key_down(LX_KEY_SPACE);
        double now = lx_time();
        bool dbl = now - GE.last_click < 0.35 && fabsf(m.x - GE.last_click_pos.x) < 6 && fabsf(m.y - GE.last_click_pos.y) < 6;
        GE.last_click = dbl ? 0 : now;
        GE.last_click_pos = m;
        if (pressed_m || alt || space) GE.drag = GE_DRAG_PAN;
        else {
            Hit h = hit_test(m.x, m.y);
            switch (h.kind) {
            case H_MINIMAP: GE.drag = GE_DRAG_MINIMAP; minimap_pan(m.x, m.y); break;
            case H_EVENT:
                if (!is_selected(h.state) || GE.sel_count != 1) ge_select_state(h.state, false);
                GE.sel_trans = h.trans;
                GE.insp_dirty = true;
                GE.drag = GE_DRAG_LINK;
                GE.link_state = h.state;
                GE.link_trans = h.trans;
                if (dbl) { g_menu_state = h.state; g_menu_trans = h.trans; GE.drag = GE_DRAG_NONE;
                           UiNode tmp; memset(&tmp, 0, sizeof tmp); tmp.x = m.x; tmp.y = m.y; event_menu(&tmp, EV_CHANGE); }
                break;
            case H_STATE:
                if (dbl) { g_menu_state = h.state; GE.drag = GE_DRAG_NONE; m_rename(NULL); break; }
                if (shift) ge_select_state(h.state, true);
                else if (!is_selected(h.state)) ge_select_state(h.state, false);
                else { GE.sel_trans = -1; if (GE.insp_tabs) GE.insp_tabs->tab_index = 1; GE.insp_dirty = true; }
                GE.drag = GE_DRAG_MOVE;
                break;
            case H_START:
                GE.drag = GE_DRAG_START;
                GE.link_state = h.state;
                GE.link_trans = -2;
                break;
            case H_GLOBAL:
                ge_select_state(h.state, false);
                GE.drag = GE_DRAG_LINK;
                GE.link_state = h.state;
                GE.link_trans = -1;
                GE.link_global = h.trans;
                break;
            case H_COMMENT: case H_COMMENT_SIZE:
                GE.sel_count = 0;
                GE.sel_trans = -1;
                GE.sel_comment = h.comment;
                GE.insp_dirty = true;
                if (dbl) { g_menu_comment = h.comment; m_comment_text(NULL); break; }
                GE.drag = h.kind == H_COMMENT ? GE_DRAG_COMMENT : GE_DRAG_COMMENT_SIZE;
                /* a comment is a frame: the states inside it move with it (hold Ctrl to move it alone) */
                g_group_count = 0;
                if (GE.drag == GE_DRAG_COMMENT && !lx_key_down(LX_KEY_CTRL)) {
                    const LxGraphComment *c = &g->comments[h.comment];
                    for (int i = 0; i < g->state_count && g_group_count < GE_MAX_SEL; i++) {
                        const LxGraphState *st = &g->states[i];
                        if (st->x >= c->x && st->y >= c->y && st->x + state_w(st) <= c->x + c->w && st->y + state_h(st) <= c->y + c->h) g_group[g_group_count++] = i;
                    }
                }
                break;
            default:
                if (dbl) { add_state_at(GE.press_gx - 75, GE.press_gy - 11); break; }
                if (!shift) { GE.sel_count = 0; GE.sel_trans = -1; GE.sel_comment = -1; GE.insp_dirty = true; }
                GE.drag = GE_DRAG_BOX;
                break;
            }
        }
    }
    if (GE.drag == GE_DRAG_NONE) return;
    /* continue it */
    float dx = m.x - GE.last.x, dy = m.y - GE.last.y;
    if (!GE.drag_moved && (fabsf(m.x - GE.press.x) > 4 || fabsf(m.y - GE.press.y) > 4)) GE.drag_moved = true;
    bool down = lx_mouse_down(LX_MOUSE_LEFT) || (GE.drag == GE_DRAG_PAN && lx_mouse_down(LX_MOUSE_MIDDLE));
    switch (GE.drag) {
    case GE_DRAG_PAN: GE.pan_x -= dx / SC(); GE.pan_y -= dy / SC(); break;
    case GE_DRAG_MINIMAP: minimap_pan(m.x, m.y); break;
    case GE_DRAG_MOVE:
        if (GE.drag_moved) {
            float ddx = gx_of(m.x) - gx_of(GE.last.x), ddy = gy_of(m.y) - gy_of(GE.last.y);
            for (int k = 0; k < GE.sel_count; k++)
                if (GE.sel[k] < g->state_count) { g->states[GE.sel[k]].x += ddx; g->states[GE.sel[k]].y += ddy; }
        }
        break;
    case GE_DRAG_COMMENT: case GE_DRAG_COMMENT_SIZE:
        if (GE.drag_moved && GE.sel_comment >= 0 && GE.sel_comment < g->comment_count) {
            LxGraphComment *c = &g->comments[GE.sel_comment];
            float ddx = dx / SC(), ddy = dy / SC();
            if (GE.drag == GE_DRAG_COMMENT) {
                c->x += ddx;
                c->y += ddy;
                for (int k = 0; k < g_group_count; k++)
                    if (g_group[k] < g->state_count) { g->states[g_group[k]].x += ddx; g->states[g_group[k]].y += ddy; }
            }
            else { c->w = lx_maxf(80, c->w + ddx); c->h = lx_maxf(40, c->h + ddy); }
        }
        break;
    default: break;
    }
    GE.last = m;
    if (down) return;
    /* release */
    switch (GE.drag) {
    case GE_DRAG_MOVE:
        if (GE.drag_moved) {
            bool snap = !lx_key_down(LX_KEY_CTRL);   /* Ctrl: free placement */
            for (int k = 0; k < GE.sel_count && snap; k++)
                if (GE.sel[k] < g->state_count) { g->states[GE.sel[k]].x = roundf(g->states[GE.sel[k]].x / 8) * 8; g->states[GE.sel[k]].y = roundf(g->states[GE.sel[k]].y / 8) * 8; }
            ge_commit(false, NULL);
        }
        break;
    case GE_DRAG_COMMENT: case GE_DRAG_COMMENT_SIZE:
        if (GE.drag_moved) ge_commit(false, NULL);
        break;
    case GE_DRAG_LINK: case GE_DRAG_START:
        if (GE.drag_moved) finish_link(g, m.x, m.y);
        break;
    case GE_DRAG_BOX:
        if (GE.drag_moved) {
            float x0 = lx_minf(gx_of(GE.press.x), gx_of(m.x)), x1 = lx_maxf(gx_of(GE.press.x), gx_of(m.x));
            float y0 = lx_minf(gy_of(GE.press.y), gy_of(m.y)), y1 = lx_maxf(gy_of(GE.press.y), gy_of(m.y));
            for (int i = 0; i < g->state_count; i++) {
                const LxGraphState *st = &g->states[i];
                if (st->x < x1 && st->x + state_w(st) > x0 && st->y < y1 && st->y + state_h(st) > y0 && !is_selected(i) && GE.sel_count < GE_MAX_SEL)
                    GE.sel[GE.sel_count++] = i;
            }
            GE.insp_dirty = true;
        }
        break;
    default: break;
    }
    GE.drag = GE_DRAG_NONE;
    GE.link_state = -1;
}

static void canvas_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(n);
    if (strcmp(type, "graph")) return;
    if (GE.obj && ED.scene && lx_entity_valid(ED.scene, GE.obj)) {
        LxStateGraph *sg = lx_get_state_graph(ED.scene, GE.obj);
        if (sg && sg->count && GE.slot < sg->count && !sg->slots[GE.slot].graph[0]) lx_strcpy(sg->slots[GE.slot].graph, LX_REF_MAX, payload);
        else { lx_add_state_graph(ED.scene, GE.obj, payload); sg = lx_get_state_graph(ED.scene, GE.obj); GE.slot = sg ? sg->count - 1 : 0; }
        ed_mark_changed();
        ed_commit();
        ED.inspector_dirty = true;
        GE.ref[0] = 0;   /* picked up again by the sync */
    } else set_target(payload, 0, 0);
}

/* ======================================================================
 *  keyboard (the editor's shortcuts hand over while the graph view has focus)
 * ====================================================================== */
bool ed_graph_has_focus(void) {
    return GE.focused && GE.panel && !GE.panel->hidden && ED.view_tabs && ED.view_tabs->tab_index == 1 && !ui_wants_keyboard(ED.ui);
}
bool ed_graph_keys(void) {
    if (!ed_graph_has_focus()) return false;
    LxGraph *g = ge_graph();
    bool ctrl = lx_key_down(LX_KEY_CTRL), shift = lx_key_down(LX_KEY_SHIFT);
    if (ctrl) {
        if (lx_key_pressed(LX_KEY_Z)) { if (shift) ge_redo(); else ge_undo(); return true; }
        if (lx_key_pressed(LX_KEY_Y)) { ge_redo(); return true; }
        if (!g) return false;
        if (lx_key_pressed(LX_KEY_C)) { copy_selection(); return true; }
        if (lx_key_pressed(LX_KEY_V)) {
            LxVec2 m = lx_mouse_pos();
            bool in = GE.canvas && m.x >= GE.canvas->x && m.y >= GE.canvas->y && m.x < GE.canvas->x + GE.canvas->cw && m.y < GE.canvas->y + GE.canvas->ch;
            paste_at(in ? gx_of(m.x) : GE.pan_x + 60, in ? gy_of(m.y) : GE.pan_y + 60);
            return true;
        }
        if (lx_key_pressed(LX_KEY_D)) { m_duplicate(NULL); return true; }
        if (lx_key_pressed(LX_KEY_A)) { m_select_all(NULL); return true; }
        return false;   /* Ctrl+S, Ctrl+P... stay with the editor */
    }
    if (!g) return true;
    if (lx_key_pressed(LX_KEY_DELETE) || lx_key_pressed(LX_KEY_BACKSPACE)) delete_selection();
    if (lx_key_pressed(LX_KEY_F)) ge_frame_all();
    if (lx_key_pressed(LX_KEY_HOME)) { GE.sel_count = 0; ge_frame_all(); }
    if (lx_key_pressed(LX_KEY_F2) && GE.sel_count) { g_menu_state = GE.sel[0]; m_rename(NULL); }
    if (lx_key_pressed(LX_KEY_ESCAPE)) { GE.sel_count = 0; GE.sel_trans = -1; GE.sel_comment = -1; GE.insp_dirty = true; }
    if (lx_key_pressed(LX_KEY_SPACE) && !lx_mouse_down(LX_MOUSE_LEFT)) ge_open_action_browser();
    return true;
}

/* ======================================================================
 *  new graphs
 * ====================================================================== */
void ge_new_graph_for(LxEntity e) {
    if (!ED.scene || !lx_entity_valid(ED.scene, e)) return;
    char dir[LX_PATH_MAX], name[LX_NAME_MAX], ref[LX_REF_MAX], path[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, ED.project.assets, "Graphs");
    lx_mkdir_p(dir);
    lx_strcpy(name, sizeof name, lx_entity_name(ED.scene, e));
    for (char *c = name; *c; c++) if (strchr("/\\:*?\"<>|", *c)) *c = '_';
    if (!name[0]) lx_strcpy(name, sizeof name, "Graph");
    for (int i = 1; i < 1000; i++) {
        if (i == 1) snprintf(ref, sizeof ref, "Graphs/%s.lxgraph", name);
        else snprintf(ref, sizeof ref, "Graphs/%s %d.lxgraph", name, i);
        lx_asset_path(path, sizeof path, ref);
        if (!lx_file_exists(path)) break;
    }
    LxGraph *g = lx_graph_new(lx_entity_name(ED.scene, e));
    if (!lx_graph_save(g, path)) { lx_graph_free(g); ui_notify(ED.ui, "Couldn't create %s", ref); return; }
    lx_graph_replace(ref, g);   /* takes it */
    lx_add_state_graph(ED.scene, e, ref);
    LxStateGraph *sg = lx_get_state_graph(ED.scene, e);
    int slot = 0;
    for (int i = 0; sg && i < sg->count; i++) if (!strcmp(sg->slots[i].graph, ref)) slot = i;
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
    ED.content_dirty = true;
    set_target(ref, e, slot);
    if (ED.view_tabs) ED.view_tabs->tab_index = 1;
    ui_notify(ED.ui, "Created %s", ref);
}

/* ======================================================================
 *  toolbar & bottom bar
 * ====================================================================== */
static void pick_object(UiNode *n) {
    LxEntity e = (LxEntity)n->tag;
    if (ED.scene && lx_entity_valid(ED.scene, e)) { GE.locked = false; ed_select(e); }
}
static void tb_object(UiNode *b) {
    UiNode *p = ui_popup(ED.ui, b->x, b->y + b->ch, 220);
    if (!p || !ED.scene) return;
    int n = 0;
    for (int i = 0; i < lx_scene_entity_count(ED.scene) && n < 60; i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        if (!(lx_entity_components(ED.scene, e) & LX_COMP_STATE_GRAPH)) continue;
        UiNode *it = ui_menu_item(ED.ui, p, lx_entity_name(ED.scene, e), NULL, pick_object);
        it->tag = (intptr_t)e;
        it->icon = UI_ICON_GRAPH;
        it->icon_color = ed_color_for_icon(UI_ICON_GRAPH);
        n++;
    }
    if (!n) { UiNode *it = ui_menu_item(ED.ui, p, "No objects have a State Graph yet", NULL, NULL); it->disabled = true; }
}
static void pick_slot(UiNode *n) {
    int slot = (int)n->tag;
    LxStateGraph *sg = GE.obj && ED.scene ? lx_get_state_graph(ED.scene, GE.obj) : NULL;
    if (!sg || slot >= sg->count) return;
    GE.slot = slot;
    set_target(sg->slots[slot].graph, GE.obj, slot);
}
static void m_new_graph_slot(UiNode *n) { LX_UNUSED(n); if (GE.obj) ge_new_graph_for(GE.obj); }
static void m_select_asset(UiNode *n) {
    LX_UNUSED(n);
    if (!GE.ref[0]) return;
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, GE.ref);
    GE.locked = true;   /* keep showing it while the asset is selected */
    ed_select_asset(path);
}
static void tb_graph(UiNode *b) {
    UiNode *p = ui_popup(ED.ui, b->x, b->y + b->ch, 240);
    if (!p) return;
    LxStateGraph *sg = GE.obj && ED.scene ? lx_get_state_graph(ED.scene, GE.obj) : NULL;
    for (int i = 0; sg && i < sg->count; i++) {
        LxGraph *g = sg->slots[i].graph[0] ? lx_asset_graph(sg->slots[i].graph) : NULL;
        char t[160];
        snprintf(t, sizeof t, "%d  %s", i + 1, g ? g->name : sg->slots[i].graph[0] ? sg->slots[i].graph : "(empty)");
        UiNode *it = ui_menu_item(ED.ui, p, t, NULL, pick_slot);
        it->tag = i;
        if (i == GE.slot) { it->icon = UI_ICON_CHECK; it->icon_color = COL_START; }
    }
    if (GE.obj && (!sg || sg->count < LX_FSM_GRAPHS_PER_OBJECT)) {
        if (sg && sg->count) ui_menu_separator(ED.ui, p);
        mitem(p, "New Graph on this Object", UI_ICON_PLUS, m_new_graph_slot);
    }
    if (GE.ref[0]) mitem(p, "Show Graph Asset", UI_ICON_FOLDER, m_select_asset);
}
static void tb_lock(UiNode *b) {
    GE.locked = !GE.locked;
    b->selected = GE.locked;
}
static void tb_frame(UiNode *b) { LX_UNUSED(b); GE.sel_count = 0; ge_frame_all(); }
static void tb_zoom(UiNode *b) { LX_UNUSED(b); GE.zoom = 1; }
static void tb_minimap(UiNode *b) { GE.minimap = !GE.minimap; b->selected = GE.minimap; ge_prefs_save(); }
static void tb_debug(UiNode *b) { GE.debug = !GE.debug; b->selected = GE.debug; GE.insp_dirty = true; ge_prefs_save(); }
static void tb_hints(UiNode *b) { GE.hints = !GE.hints; b->selected = GE.hints; ge_prefs_save(); }
static void tb_errors(UiNode *b) { LX_UNUSED(b); if (GE.insp_tabs) GE.insp_tabs->tab_index = 0; }
static void tb_ab(UiNode *b) { LX_UNUSED(b); ge_action_browser_toggle(); }
static void tb_prefs(UiNode *b) {
    UiNode *p = ui_popup(ED.ui, b->x, b->y - 120 * ui_scale(ED.ui), 220);
    if (!p) return;
    UiNode *it = mitem(p, "Show Minimap", GE.minimap ? UI_ICON_CHECK : UI_ICON_NONE, tb_minimap);
    LX_UNUSED(it);
    mitem(p, "Show Hints", GE.hints ? UI_ICON_CHECK : UI_ICON_NONE, tb_hints);
    mitem(p, "Debug While Playing", GE.debug ? UI_ICON_CHECK : UI_ICON_NONE, tb_debug);
    mitem(p, "Auto Layout This Graph", UI_ICON_GRID, m_auto_layout);
}

static UiNode *tbtn(UiNode *parent, const char *text, UiIcon icon, const char *tip, void (*cb)(UiNode *)) {
    UiNode *b = ui_button(ED.ui, parent, text, cb);
    b->h = 24;
    b->icon = icon;
    b->bg = UI_HEX(0x28292e);
    b->border = UI_HEX(0x34363c);
    b->radius = 4;
    b->font = UI_FONT_SMALL;
    b->bg_active = UI_HEX(0x2b4a86);
    if (tip) lx_strcpy(b->tooltip, sizeof b->tooltip, tip);
    return b;
}

static void sync_target(void) {
    if (!ED.scene) { set_target("", 0, 0); return; }
    if (GE.locked) {
        if (!GE.obj || lx_entity_valid(ED.scene, GE.obj)) {
            if (GE.obj) {   /* the object's slot may have been changed in the Inspector */
                LxStateGraph *sg = lx_get_state_graph(ED.scene, GE.obj);
                if (sg && GE.slot < sg->count) set_target(sg->slots[GE.slot].graph, GE.obj, GE.slot);
            }
            return;
        }
        GE.locked = false;
        if (GE.lock_btn) GE.lock_btn->selected = false;
    }
    LxStateGraph *sg = ED.selected ? lx_get_state_graph(ED.scene, ED.selected) : NULL;
    if (sg) {
        int slot = GE.obj == ED.selected ? GE.slot : 0;
        if (slot >= sg->count) slot = sg->count - 1;
        if (slot < 0) slot = 0;
        set_target(sg->count ? sg->slots[slot].graph : "", ED.selected, slot);
        return;
    }
    if (ED.selected_asset[0] && ed_icon_for_file(ED.selected_asset, false) == UI_ICON_GRAPH) {
        char rel[LX_REF_MAX];
        ed_relative_asset(rel, sizeof rel, ED.selected_asset);
        set_target(rel, 0, 0);
        return;
    }
    if (ED.selected) { set_target("", ED.selected, 0); return; }
    if (GE.obj && !lx_entity_valid(ED.scene, GE.obj)) set_target("", 0, 0);
}

static void panel_frame(UiNode *n) {
    LX_UNUSED(n);
    if (g_save_pending && !lx_mouse_down(LX_MOUSE_LEFT)) flush_save();
    sync_target();
    LxGraph *g = ge_graph();
    if (g && g->version != GE.seen_version) {   /* changed elsewhere (Inspector, another editor, reload) */
        GE.seen_version = g->version;
        GE.insp_dirty = true;
        lx_free(GE.last_text);
        GE.last_text = lx_graph_to_text(g);
        int k = 0;
        for (int i = 0; i < GE.sel_count; i++) if (GE.sel[i] < g->state_count) GE.sel[k++] = GE.sel[i];
        GE.sel_count = k;
    }
    char b[160];
    if (GE.obj_btn) {
        if (GE.obj && ED.scene && lx_entity_valid(ED.scene, GE.obj)) snprintf(b, sizeof b, "%s", lx_entity_name(ED.scene, GE.obj));
        else snprintf(b, sizeof b, GE.ref[0] ? "(graph asset)" : "No object");
        ui_set_text(GE.obj_btn, "%s", b);
    }
    if (GE.graph_btn) ui_set_text(GE.graph_btn, "%s", g ? g->name : GE.ref[0] ? "(missing)" : "No graph");
    if (GE.state_lbl) {
        LxFsmRun *run = ge_run();
        const LxGraphState *st = run && run->state >= 0 && run->state < g->state_count ? &g->states[run->state] : ge_sel_state();
        ui_set_text(GE.state_lbl, "%s", st ? st->name : "");
        GE.state_lbl->fg = run ? UI_HEX(0xffb03a) : COL_TEXT;
    }
    if (GE.zoom_btn) ui_set_text(GE.zoom_btn, "%d%%", (int)lroundf(GE.zoom * 100));
    if (GE.errors_btn) {
        char probs[32][160];
        int np = g ? lx_graph_problems(g, probs, 32) : 0;
        ui_set_text(GE.errors_btn, "Errors: %d", np);
        GE.errors_btn->fg = np ? UI_HEX(0xff6b5a) : UI_HEX(0x7cc24e);
    }
    if (GE.lock_btn) GE.lock_btn->selected = GE.locked;
    ge_inspector_frame();
}

UiNode *ed_graph_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    ge_prefs_load();
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "State Graph");
    p->icon = UI_ICON_GRAPH;
    p->bg = UI_HEX(0x1f2023);
    p->on_frame = panel_frame;
    GE.panel = p;
    ui_track(&GE.panel);
    /* toolbar: object, graph, lock, current state ... frame, minimap, zoom */
    UiNode *tb = ui_box(ui, p, UI_ROW);
    tb->h = 32;
    tb->gap = 6;
    tb->cross_align = UI_ALIGN_CENTER;
    ui_pad(tb, 6, 0, 6, 0);
    tb->bg = UI_HEX(0x232428);
    GE.obj_btn = tbtn(tb, "No object", UI_ICON_ENTITY, "Objects with a State Graph", tb_object);
    GE.obj_btn->w = 170;
    GE.graph_btn = tbtn(tb, "No graph", UI_ICON_GRAPH, "This object's graphs", tb_graph);
    GE.graph_btn->w = 150;
    GE.lock_btn = tbtn(tb, "", UI_ICON_LOCK, "Lock: keep showing this graph when you select something else", tb_lock);
    GE.lock_btn->w = 28;
    UiNode *sl = ui_label(ui, tb, "State:");
    sl->font = UI_FONT_SMALL;
    sl->fg = COL_DIM;
    GE.state_lbl = ui_label(ui, tb, "");
    GE.state_lbl->font = UI_FONT_BOLD;
    GE.state_lbl->grow = 1;
    tbtn(tb, "Frame", UI_ICON_FOCUS, "Fit the graph in the view (F)", tb_frame)->w = 70;
    UiNode *mm = tbtn(tb, "Minimap", UI_ICON_TILES, NULL, tb_minimap);
    mm->w = 84;
    mm->selected = GE.minimap;
    GE.map_btn = mm;
    GE.zoom_btn = tbtn(tb, "100%", UI_ICON_SEARCH, "Zoom (scroll in the graph); click for 100%", tb_zoom);
    GE.zoom_btn->w = 72;
    /* graph view | inspector */
    UiNode *split = ui_split(ui, p, UI_ROW, 0.70f);
    split->split_min = 280;
    split->grow = 1;
    UiNode *c = ui_new(ui, split, UI_CUSTOM);
    c->grow = 1;
    c->on_frame = canvas_frame;
    c->on_draw = canvas_draw;
    c->on_right_click = canvas_right_click;
    c->accept = "graph";
    c->on_drop = canvas_drop;
    GE.canvas = c;
    ge_inspector(split);
    /* bottom bar */
    UiNode *bb = ui_box(ui, p, UI_ROW);
    bb->h = 32;
    bb->gap = 6;
    bb->cross_align = UI_ALIGN_CENTER;
    ui_pad(bb, 6, 0, 6, 0);
    bb->bg = UI_HEX(0x232428);
    GE.debug_btn = tbtn(bb, "Debug", UI_ICON_PLAY, "While playing: show the time in the active state and live variable values", tb_debug);
    GE.debug_btn->w = 80;
    GE.debug_btn->selected = GE.debug;
    GE.hints_btn = tbtn(bb, "Hints", UI_ICON_INFO, "Show mouse and keyboard hints in the graph view", tb_hints);
    GE.hints_btn->w = 74;
    GE.hints_btn->selected = GE.hints;
    tbtn(bb, "Preferences", UI_ICON_GEAR, NULL, tb_prefs)->w = 110;
    GE.errors_btn = tbtn(bb, "Errors: 0", UI_ICON_WARNING, "Problems in this graph (listed in the FSM tab)", tb_errors);
    GE.errors_btn->w = 100;
    ui_spacer(ui, bb);
    UiNode *ab = tbtn(bb, "Action Browser", UI_ICON_LIST, "Find actions to add to the selected state (Space in the graph)", tb_ab);
    ab->w = 140;
    ab->bg = UI_HEX(0x3d7cf5);
    ab->bg_hover = UI_HEX(0x5a92ff);
    ab->fg = UI_HEX(0xffffff);
    ab->font = UI_FONT_BOLD;
    GE.ab_btn = ab;
    ui_track(&GE.canvas);
    ui_track(&GE.obj_btn);
    ui_track(&GE.graph_btn);
    ui_track(&GE.lock_btn);
    ui_track(&GE.state_lbl);
    ui_track(&GE.zoom_btn);
    ui_track(&GE.map_btn);
    ui_track(&GE.debug_btn);
    ui_track(&GE.hints_btn);
    ui_track(&GE.errors_btn);
    ui_track(&GE.ab_btn);
    GE.insp_dirty = true;
    return p;
}

/* the editor rebuilt its layout or closed the project */
void ed_graph_forget(void) {
    flush_save();
    GE.ref[0] = 0;
    GE.obj = 0;
    GE.locked = false;
    GE.focused = false;
    GE.drag = GE_DRAG_NONE;
    clear_history();
    lx_free(g_clip);
    g_clip = NULL;
}

/* open the graph editor on an object's graph (the Inspector's Edit button) */
void ed_graph_edit(LxEntity e, int slot) {
    if (!ED.scene || !lx_entity_valid(ED.scene, e)) return;
    LxStateGraph *sg = lx_get_state_graph(ED.scene, e);
    if (!sg || slot < 0 || slot >= sg->count) return;
    GE.locked = false;
    if (ED.selected != e) ed_select(e);
    GE.slot = slot;
    set_target(sg->slots[slot].graph, e, slot);
    if (ED.view_tabs) ED.view_tabs->tab_index = 1;
}
void ed_graph_open_asset(const char *ref) {
    GE.locked = true;
    set_target(ref, 0, 0);
    if (ED.view_tabs) ED.view_tabs->tab_index = 1;
}

/* select a state by name (--show-graph Object/State, tests) */
bool ed_graph_select_state(const char *name) {
    LxGraph *g = ge_graph();
    int i = g ? lx_graph_state_index(g, name) : -1;
    if (i < 0) return false;
    ge_select_state(i, false);
    return true;
}

/* ======================================================================
 *  stress tester hooks: graph view gestures without a mouse
 * ====================================================================== */
static uint32_t g_trng = 12345;
static uint32_t trnd(int n) { g_trng = g_trng * 1103515245u + 12345u; return n > 0 ? (g_trng >> 8) % (uint32_t)n : 0; }

const char *ed_graph_stress(uint32_t seed) {
    static char what[160];
    g_trng ^= seed;
    LxGraph *g = ge_graph();
    if (ED.view_tabs && trnd(10) == 0) { ED.view_tabs->tab_index = (int)trnd(2); return "switch Scene / State Graph tab"; }
    if (!g) {
        if (GE.obj && ED.scene && lx_entity_valid(ED.scene, GE.obj) && ED.play == ED_EDIT && trnd(6) == 0) { ge_new_graph_for(GE.obj); return "new graph for the object"; }
        /* pick an object that has one */
        LxEntity with[64];
        int n = 0;
        for (int i = 0; ED.scene && i < lx_scene_entity_count(ED.scene) && n < 64; i++) {
            LxEntity e = lx_scene_entity_at(ED.scene, i);
            if (lx_entity_components(ED.scene, e) & LX_COMP_STATE_GRAPH) with[n++] = e;
        }
        if (n) { GE.locked = false; ed_select(with[trnd(n)]); if (ED.view_tabs) ED.view_tabs->tab_index = 1; return "select an object with a State Graph"; }
        return "nothing (no graph)";
    }
    if (!GE.canvas || GE.canvas->cw < 20) { if (ED.view_tabs) ED.view_tabs->tab_index = 1; return "show the graph view"; }
    float cx = GE.canvas->x + (float)trnd((int)GE.canvas->cw), cy = GE.canvas->y + (float)trnd((int)GE.canvas->ch);
    int si = g->state_count ? (int)trnd(g->state_count) : -1;
    switch (trnd(20)) {
    case 0: add_state_at(gx_of(cx), gy_of(cy)); return "add a state";
    case 1: ge_select_state(si, trnd(3) == 0); return "select a state";
    case 2: delete_selection(); return "delete the selection";
    case 3: copy_selection(); paste_at(gx_of(cx), gy_of(cy)); return "copy + paste";
    case 4: auto_layout(); return "auto layout";
    case 5: ge_undo(); return "graph undo";
    case 6: ge_redo(); return "graph redo";
    case 7: {   /* link a transition somewhere */
        if (si < 0 || !g->states[si].trans_count) return "link (no transitions)";
        LxGraphState *st = &g->states[si];
        int t = (int)trnd(st->trans_count), to = (int)trnd(g->state_count + 1);
        if (to == g->state_count) st->trans[t].to[0] = 0;
        else lx_strcpy(st->trans[t].to, sizeof st->trans[t].to, g->states[to].name);
        ge_commit(true, NULL);
        return "link a transition";
    }
    case 8: {
        if (si < 0) return "transition (no state)";
        static const char *EV[] = {"FINISHED", "GO", "HIT", "ARRIVED", "TRIGGER_ENTER", "OPEN DOOR"};
        g_menu_state = si;
        g_ev_mode = (int)trnd(3);
        g_menu_trans = g->states[si].trans_count ? (int)trnd(g->states[si].trans_count) : -1;
        if (g_ev_mode == EV_CHANGE && g_menu_trans < 0) g_ev_mode = EV_ADD_TRANS;
        apply_event(EV[trnd(6)]);
        return "add / change a transition event";
    }
    case 9: open_context_menu(hit_test(cx, cy), cx, cy); return "right-click the graph";
    case 10: zoom_at(trnd(2) ? 1.3f : 0.7f, cx, cy); return "zoom";
    case 11: ge_frame_all(); return "frame";
    case 12: {
        if (si < 0) return "add action (no state)";
        ge_select_state(si, false);
        const LxFsmActionDesc *d = lx_fsm_action_at((int)trnd(lx_fsm_action_count()));
        if (d) ge_add_action(d->name);
        snprintf(what, sizeof what, "add action %s", d ? d->name : "?");
        return what;
    }
    case 13: ge_action_browser_toggle(); return "toggle the Action Browser";
    case 14: GE.debug = !GE.debug; GE.insp_dirty = true; return "toggle Debug";
    case 15: if (GE.insp_tabs) GE.insp_tabs->tab_index = (int)trnd(4); return "inspector tab";
    case 16: {   /* drag states around */
        if (si < 0) return "move (no state)";
        ge_select_state(si, false);
        g->states[si].x += (float)trnd(200) - 100;
        g->states[si].y += (float)trnd(200) - 100;
        ge_commit(false, NULL);
        return "move a state";
    }
    case 17: { g_menu_gx = gx_of(cx); g_menu_gy = gy_of(cy); m_add_comment(NULL); return "add a comment"; }
    case 18: GE.locked = !GE.locked; return "toggle lock";
    default: {
        if (si < 0) return "rename (no state)";
        g_menu_state = si;
        static const char *NM[] = {"Idle", "Run", "Attack", "", "State 1", "a/b", "Ünïcode"};
        rename_done(NM[trnd(7)]);
        return "rename a state";
    }
    }
}

/* the graph being edited still makes sense */
bool ed_graph_check(char *err, size_t cap) {
    LxGraph *g = ge_graph();
    if (!g) return true;
    if (g->state_count < 1) { snprintf(err, cap, "graph %s has no states", GE.ref); return false; }
    if (!lx_graph_find_state(g, g->start)) { snprintf(err, cap, "start state '%s' doesn't exist", g->start); return false; }
    for (int i = 0; i < g->state_count; i++)
        for (int k = i + 1; k < g->state_count; k++)
            if (!strcmp(g->states[i].name, g->states[k].name)) { snprintf(err, cap, "two states called '%s'", g->states[i].name); return false; }
    for (int k = 0; k < GE.sel_count; k++)
        if (GE.sel[k] < 0 || GE.sel[k] >= g->state_count) { snprintf(err, cap, "selection points past the states"); return false; }
    char *t = lx_graph_to_text(g);
    LxGraph *back = lx_graph_from_text(t);
    char *t2 = back ? lx_graph_to_text(back) : NULL;
    bool same = t2 && !strcmp(t, t2);
    if (!same) {
        size_t i = 0;
        while (t2 && t[i] && t[i] == t2[i]) i++;
        size_t st = i > 50 ? i - 50 : 0;
        snprintf(err, cap, "graph text doesn't survive a save/load near: [%.100s] vs [%.100s]", t + st, t2 ? t2 + st : "(unreadable)");
    }
    lx_free(t);
    lx_free(t2);
    lx_graph_free(back);
    if (!isfinite(GE.zoom) || !isfinite(GE.pan_x) || !isfinite(GE.pan_y)) { snprintf(err, cap, "view is NaN"); return false; }
    return same;
}

void ed_graph_show_tab(int tab) {
    if (tab == 4) { ge_open_action_browser(); return; }
    if (GE.insp_tabs) GE.insp_tabs->tab_index = tab;
}
