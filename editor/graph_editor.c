/* State Graph editor: a big graph view with an object/graph toolbar, the
 * FSM / State / Events / Variables inspector on the right, a bottom bar (Debug, Hints, Errors,
 * Action Browser) and the Action Browser as a floating window.
 *
 * This file is the graph view: drawing states, links, global transitions, comments and the minimap;
 * selecting, moving, linking, context menus, copy/paste, auto layout and undo. Graphs are assets
 * (.lxgraph): every edit is saved straight away and running copies re-bind to it. */
#include "graph_editor.h"
#include <math.h>

GraphEd GE = {.zoom = 1, .sel_trans = -1, .sel_comment = -1, .minimap = true, .hints = true, .link_state = -1, .scrub = -1};

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

/* ---------------- the level being viewed: the graph, or inside a Sub State ---------------- */
int ge_scope(void) {
    LxGraph *g = ge_graph();
    if (!g || !GE.scope[0]) return -1;
    int i = lx_graph_state_index(g, GE.scope);
    if (i < 0 || g->states[i].kind != LX_GS_SUB) { GE.scope[0] = 0; return -1; }
    return i;
}
static bool vis(const LxGraph *g, int i) { return i >= 0 && i < g->state_count && lx_graph_in_scope(g, i, ge_scope()); }
static bool comment_vis(const LxGraph *g, int i) {
    const char *p = g->comments[i].parent;
    return GE.scope[0] ? !strcmp(p, GE.scope) : !p[0];
}
void ge_enter_scope(int state) {
    LxGraph *g = ge_graph();
    if (!g) return;
    if (state >= 0 && state < g->state_count && g->states[state].kind == LX_GS_SUB) lx_strcpy(GE.scope, sizeof GE.scope, g->states[state].name);
    else GE.scope[0] = 0;
    GE.sel_count = 0;
    GE.sel_trans = -1;
    GE.sel_comment = -1;
    GE.framed = false;
    GE.insp_dirty = true;
}
static void scope_up(void) {
    LxGraph *g = ge_graph();
    int sc = ge_scope();
    if (!g || sc < 0) return;
    ge_enter_scope(lx_graph_parent_index(g, sc));
    ge_select_state(sc, false);
}
/* is this state where its region starts (the START tab)? */
static bool is_start(const LxGraph *g, int i) { return i == lx_graph_region_start(g, lx_graph_parent_index(g, i), g->states[i].region); }

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
    GE.scope[0] = 0;
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
static float bottom_pad(void);
static float g_forced_zoom;   /* screenshots: the zoom to show the graph at once it is framed */
static bool g_qa_on_open;     /* screenshots: open the quick-add box once the graph is framed */
static float SC(void) { return ui_scale(ED.ui) * GE.zoom; }
static float sx(float gx) { return GE.canvas->x + (gx - GE.pan_x) * SC(); }
static float sy(float gy) { return GE.canvas->y + (gy - GE.pan_y) * SC(); }
static float gx_of(float x) { return GE.pan_x + (x - GE.canvas->x) / SC(); }
static float gy_of(float y) { return GE.pan_y + (y - GE.canvas->y) / SC(); }

/* what a Sub State card says under its transitions: what is inside, or (while playing) where it is */
static void sub_info(const LxGraph *g, const LxGraphState *st, const LxFsmRun *run, char *out, size_t cap) {
    int me = (int)(st - g->states), inside = 0;
    for (int i = 0; i < g->state_count; i++) inside += lx_graph_parent_index(g, i) == me;
    int regions = lx_graph_region_count(g, me);
    out[0] = 0;
    if (run) {
        size_t n = 0;
        for (int r = 0; r < regions && n + 2 < cap; r++) {
            LxFsmRun *k = lx_fsm_run_for_scope((LxFsmRun *)run, me, r);
            if (!k || k->state < 0) continue;
            n += (size_t)snprintf(out + n, cap - n, "%s%s", n ? " | " : "", g->states[k->state].name);
        }
        if (n) return;
    }
    if (regions > 1) snprintf(out, cap, "%d state%s, %d regions", inside, inside == 1 ? "" : "s", regions);
    else snprintf(out, cap, "%d state%s inside", inside, inside == 1 ? "" : "s");
}
static float state_w(const LxGraphState *st) {
    float s = ui_scale(ED.ui);
    float w = ui_text_width(ED.ui, UI_FONT_BOLD, st->name) / s + (st->sequence ? 52 : 36) + (st->kind != LX_GS_STATE ? 22 : 0);
    if (st->collapsed) return lx_maxf(GE_STATE_W * 0.7f, ceilf(w / 8) * 8);
    for (int t = 0; t < st->trans_count; t++) w = lx_maxf(w, ui_text_width(ED.ui, UI_FONT_BODY, st->trans[t].event) / s + 28);
    if (st->kind == LX_GS_EXIT) w = lx_maxf(w, ui_text_width(ED.ui, UI_FONT_BODY, lx_graph_exit_event(st)) / s + 52);
    return lx_maxf(GE_STATE_W, ceilf(w / 8) * 8);
}
static int extra_rows(const LxGraphState *st) { return st->collapsed ? 0 : st->kind != LX_GS_STATE ? 1 : 0; }
static float state_h(const LxGraphState *st) {
    if (st->collapsed) return GE_TITLE_H;
    return GE_TITLE_H + GE_ROW_H * (float)(st->trans_count + extra_rows(st));
}

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

/* a link: a cubic from the event row to the target's title (graph units), or two through its reroute point */
static int link_path(const LxGraph *g, int si, int ti, float p[2][8]) {
    const LxGraphState *s = &g->states[si];
    if (ti >= s->trans_count) return 0;
    int tix = lx_graph_state_index(g, s->trans[ti].to);
    if (tix < 0 || !vis(g, tix)) return 0;
    const LxGraphState *t = &g->states[tix];
    float sw = state_w(s), tw = state_w(t);
    float ey = s->collapsed ? s->y + GE_TITLE_H * 0.5f : s->y + GE_TITLE_H + GE_ROW_H * (float)ti + GE_ROW_H * 0.5f, ty = t->y + GE_TITLE_H * 0.5f;
    const LxGraphTransition *tr = &s->trans[ti];
    if (tr->via) {   /* out of the side facing the point, through it, into the side facing it */
        float vx = tr->via_x, vy = tr->via_y;
        bool out_right = vx >= s->x + sw * 0.5f, in_left = vx <= t->x + tw * 0.5f;
        float x1 = out_right ? s->x + sw : s->x, x2 = in_left ? t->x - 2 : t->x + tw + 2;
        float c1 = lx_maxf(30, fabsf(vx - x1) * 0.5f), c2 = lx_maxf(30, fabsf(x2 - vx) * 0.5f);
        float d1 = out_right ? 1 : -1, d2 = in_left ? 1 : -1;
        float dir = (x2 - x1) >= 0 ? 1 : -1;
        float q0[8] = {x1, ey, x1 + d1 * c1, ey, vx - dir * c1 * 0.6f, vy, vx, vy};
        float q1[8] = {vx, vy, vx + dir * c2 * 0.6f, vy, x2 - d2 * c2, ty, x2, ty};
        memcpy(p[0], q0, sizeof q0);
        memcpy(p[1], q1, sizeof q1);
        return 2;
    }
    if (t != s && t->x >= s->x + sw - 10) {   /* to the right: leave right, enter left */
        float x1 = s->x + sw, x2 = t->x, c = lx_maxf(40, (x2 - x1) * 0.5f);
        float q[8] = {x1, ey, x1 + c, ey, x2 - c, ty, x2 - 2, ty};
        memcpy(p[0], q, sizeof q);
    } else if (t != s && t->x + tw <= s->x + 10) {   /* to the left */
        float x1 = s->x, x2 = t->x + tw, c = lx_maxf(40, (x1 - x2) * 0.5f);
        float q[8] = {x1, ey, x1 - c, ey, x2 + c, ty, x2 + 2, ty};
        memcpy(p[0], q, sizeof q);
    } else {   /* stacked, or itself: out on the right and round */
        float x1 = s->x + sw, x2 = t->x + tw;
        float q[8] = {x1, ey, x1 + 60, ey, x2 + 60, ty, x2 + 2, ty};
        memcpy(p[0], q, sizeof q);
    }
    return 1;
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
enum { H_NONE, H_STATE, H_EVENT, H_GLOBAL, H_START, H_COMMENT, H_COMMENT_SIZE, H_MINIMAP, H_CRUMB, H_REGION, H_LINK, H_VIA };

/* breadcrumbs (where you are: Graph > Combat > Reload), laid out when drawn */
#define GE_CRUMBS 10
static struct { float x, y, w, h; int state; } g_crumbs[GE_CRUMBS];
static int g_crumb_count;

/* a region's lane: around its states, with a header strip on top (graph units) */
static bool region_rect(const LxGraph *g, int reg, float r[4]) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < g->state_count; i++) {
        if (!vis(g, i) || g->states[i].region != reg) continue;
        const LxGraphState *st = &g->states[i];
        x0 = lx_minf(x0, st->x);
        y0 = lx_minf(y0, st->y - 30);
        x1 = lx_maxf(x1, st->x + state_w(st));
        y1 = lx_maxf(y1, st->y + state_h(st));
    }
    if (x0 > x1) return false;
    r[0] = x0 - 24;
    r[1] = y0 - 34;
    r[2] = x1 - x0 + 48;
    r[3] = y1 - y0 + 58;
    return true;
}
static bool regions_shown(const LxGraph *g) { return lx_graph_region_count(g, ge_scope()) > 1; }
static int region_at(const LxGraph *g, float gx, float gy) {
    if (!regions_shown(g)) return 0;
    int n = lx_graph_region_count(g, ge_scope());
    for (int r = 0; r < n; r++) {
        float q[4];
        if (region_rect(g, r, q) && gx >= q[0] && gy >= q[1] && gx < q[0] + q[2] && gy < q[1] + q[3]) return r;
    }
    return 0;
}
typedef struct Hit { int kind, state, trans, comment; } Hit;
static Hit g_hover = {0, -1, -1, -1};   /* what the mouse is over (links light up and show their event) */

static void minimap_rect(float *x, float *y, float *w, float *h) {
    float s = ui_scale(ED.ui);
    *w = 160 * s;
    *h = 100 * s;
    *x = GE.canvas->x + GE.canvas->cw - *w - 10 * s;
    *y = GE.canvas->y + GE.canvas->ch - *h - 10 * s - bottom_pad();
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
    for (int k = 0; k < g_crumb_count; k++)
        if (mx >= g_crumbs[k].x && my >= g_crumbs[k].y && mx < g_crumbs[k].x + g_crumbs[k].w && my < g_crumbs[k].y + g_crumbs[k].h) {
            h.kind = H_CRUMB;
            h.state = g_crumbs[k].state;
            return h;
        }
    float gx = gx_of(mx), gy = gy_of(my);
    /* states: topmost first (selected ones are drawn on top) */
    for (int pass = 0; pass < 2; pass++)
        for (int i = g->state_count - 1; i >= 0; i--) {
            if ((pass == 0) != is_selected(i) || !vis(g, i)) continue;
            const LxGraphState *st = &g->states[i];
            float w = state_w(st);
            if (gx >= st->x && gx < st->x + w && gy >= st->y && gy < st->y + state_h(st)) {
                h.state = i;
                int row = gy < st->y + GE_TITLE_H ? -1 : (int)((gy - st->y - GE_TITLE_H) / GE_ROW_H);
                if (row < 0 || row >= st->trans_count || st->collapsed) h.kind = H_STATE;
                else { h.kind = H_EVENT; h.trans = row; }
                return h;
            }
            /* tabs above it */
            if (gy >= st->y - 26 && gy < st->y - 8) {
                float stw = (ui_text_width(ED.ui, UI_FONT_SMALL, "START") + 12 * ui_scale(ED.ui)) / SC();
                if (is_start(g, i) && gx >= st->x && gx < st->x + stw) { h.kind = H_START; h.state = i; return h; }
                float xs[8], ws[8];
                int idx[8], n = global_tabs(g, i, xs, ws, idx, 8);
                for (int k = 0; k < n; k++)
                    if (gx >= xs[k] && gx < xs[k] + ws[k]) { h.kind = H_GLOBAL; h.state = i; h.trans = idx[k]; return h; }
            }
        }
    /* links: a reroute point, then near the line */
    float near_px = 7 * ui_scale(ED.ui);
    for (int i = g->state_count - 1; i >= 0; i--) {
        if (!vis(g, i)) continue;
        const LxGraphState *st = &g->states[i];
        for (int t = 0; t < st->trans_count; t++) {
            float p[2][8];
            int segs = link_path(g, i, t, p);
            if (!segs) continue;
            if (st->trans[t].via && fabsf(sx(st->trans[t].via_x) - mx) < near_px && fabsf(sy(st->trans[t].via_y) - my) < near_px) {
                h.kind = H_VIA; h.state = i; h.trans = t; return h;
            }
            for (int sg = 0; sg < segs; sg++)
                for (int k = 1; k < 24; k++) {
                    LxVec2 q = bez(p[sg], (float)k / 24);
                    if (fabsf(sx(q.x) - mx) < near_px && fabsf(sy(q.y) - my) < near_px) { h.kind = H_LINK; h.state = i; h.trans = t; return h; }
                }
        }
    }
    if (regions_shown(g)) {
        int n = lx_graph_region_count(g, ge_scope());
        for (int r = 0; r < n; r++) {
            float q[4];
            if (region_rect(g, r, q) && gx >= q[0] && gx < q[0] + q[2] && gy >= q[1] && gy < q[1] + 22) { h.kind = H_REGION; h.trans = r; return h; }
        }
    }
    for (int i = g->comment_count - 1; i >= 0; i--) {
        if (!comment_vis(g, i)) continue;
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

/* ---------------- badges ---------------- */
/* what a state mostly does, from its first action that isn't arithmetic: an icon on its title */
static UiIcon state_badge(const LxGraphState *st) {
    static const struct { const char *cat; UiIcon icon; } MAP[] = {
        {"Transform", UI_ICON_MOVE}, {"Tween", UI_ICON_MOVE}, {"Interpolation", UI_ICON_MOVE}, {"Rigidbody", UI_ICON_RIGIDBODY},
        {"Physics Queries", UI_ICON_COLLIDER}, {"Collision", UI_ICON_COLLIDER}, {"Collider", UI_ICON_COLLIDER},
        {"Character Controller", UI_ICON_CHARACTER}, {"Animator", UI_ICON_ANIMATOR}, {"Animator Graph", UI_ICON_ANIMATOR},
        {"Audio Source", UI_ICON_SOUND}, {"Audio Mixer", UI_ICON_SOUND}, {"Audio Listener", UI_ICON_LISTENER}, {"Game UI", UI_ICON_VIEWPORT},
        {"Screen", UI_ICON_VIEWPORT}, {"Camera", UI_ICON_CAMERA}, {"Light", UI_ICON_BULB}, {"Rendering", UI_ICON_MATERIAL},
        {"Renderer", UI_ICON_MATERIAL}, {"Material", UI_ICON_MATERIAL}, {"Input", UI_ICON_GAMEPAD}, {"Keyboard", UI_ICON_KEYBOARD},
        {"Mouse", UI_ICON_MOUSE}, {"Gamepad", UI_ICON_GAMEPAD}, {"Touch", UI_ICON_TOUCH}, {"Time", UI_ICON_PAUSE}, {"Timer", UI_ICON_PAUSE},
        {"Sequence", UI_ICON_PAUSE}, {"AI", UI_ICON_CROWD}, {"Combat", UI_ICON_CHARACTER}, {"Game Object", UI_ICON_ENTITY},
        {"Object", UI_ICON_ENTITY}, {"Scene", UI_ICON_SCENE}, {"Save Data", UI_ICON_SAVE}, {"Inventory", UI_ICON_PREFAB},
        {"Debug", UI_ICON_CONSOLE}, {"Events", UI_ICON_EXTERNAL}};
    for (int a = 0; a < st->action_count; a++) {
        if (!st->actions[a].enabled) continue;
        const LxFsmActionDesc *d = lx_fsm_action_find(st->actions[a].type);
        if (!d || !d->category) continue;
        char top[48];
        size_t l = strcspn(d->category, "/");
        if (l >= sizeof top) l = sizeof top - 1;
        memcpy(top, d->category, l);
        top[l] = 0;
        for (size_t k = 0; k < sizeof MAP / sizeof MAP[0]; k++) if (!strcmp(MAP[k].cat, top)) return MAP[k].icon;
    }
    return UI_ICON_NONE;
}
/* problems, per state (the first one's text), worked out again only when the graph changes */
static const LxGraph *g_prob_graph;
static uint32_t g_prob_version = (uint32_t)-1;
static char g_prob_text[64][160];
static int g_prob_count;
static const char *state_problem(const LxGraph *g, int si) {
    if (g != g_prob_graph || g->version != g_prob_version) {
        g_prob_graph = g;
        g_prob_version = g->version;
        g_prob_count = lx_graph_problems(g, g_prob_text, 64);
        if (g_prob_count > 64) g_prob_count = 64;
    }
    const char *nm = g->states[si].name;
    size_t l = strlen(nm);
    for (int k = 0; k < g_prob_count; k++)
        if (!strncmp(g_prob_text[k], nm, l) && (g_prob_text[k][l] == ':' || g_prob_text[k][l] == ' ')) return g_prob_text[k];
    return NULL;
}

/* ---------------- live play ---------------- */
/* how far a running action has got, 0..1; -1 when it isn't a timed one */
static float action_progress(const LxFsmRun *r, int st, int a) {
    if (!r || st < 0 || st >= r->bound_states || a < 0 || a >= r->bound_counts[st]) return -1;
    const LxFsmBound *b = &r->bound[st][a];
    if (!b->entered || !b->desc) return -1;
    if (b->finished) return 1;
    if (b->progress >= 0) return b->progress;
    static const char *const timed[] = {"Duration", "Time", "Delay", "Timeout", "Seconds"};
    for (int k = 0; k < LX_FSM_MAX_PARAMS && b->desc->params[k].name; k++) {
        if (b->desc->params[k].type != LX_FSM_FLOAT) continue;
        bool is_t = false;
        for (int q = 0; q < 5 && !is_t; q++) is_t = !strcmp(b->desc->params[k].name, timed[q]);
        if (!is_t) continue;
        float d = b->lit[k].f;
        if (b->var[k] >= 0 && b->var[k] < r->var_count) d = r->vars[b->var[k]].f;
        else if (b->var[k] <= -2) { const LxFsmValue *gv = lx_fsm_global_value(-2 - b->var[k]); d = gv ? gv->f : 0; }
        if (d > 0) return lx_clampf(r->state_time / d, 0, 1);
    }
    return -1;
}
/* the variables a state's actions use, the watched ones (or all of them in Debug), formatted */
static int state_values(const LxGraph *g, const LxFsmRun *run, int si, char *out, size_t cap) {
    out[0] = 0;
    if (!run) return 0;
    const LxGraphState *st = &g->states[si];
    int n = 0;
    size_t len = 0;
    char seen[8][LX_FSM_NAME];
    for (int a = 0; a < st->action_count && n < 4; a++)
        for (int k = 0; k < LX_FSM_MAX_PARAMS && n < 4; k++) {
            const LxGraphParam *p = &st->actions[a].p[k];
            if (!p->is_var) continue;
            int vi = -1;
            for (int v = 0; v < g->var_count && vi < 0; v++) if (!strcmp(g->vars[v].name, p->value)) vi = v;
            if (vi < 0 || vi >= run->var_count || (!g->vars[vi].watch && !GE.debug)) continue;
            bool dup = false;
            for (int q = 0; q < n; q++) dup |= !strcmp(seen[q], p->value);
            if (dup) continue;
            lx_strcpy(seen[n], LX_FSM_NAME, p->value);
            char val[64];
            lx_fsm_value_format(&run->vars[vi], val, sizeof val, NULL);
            len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "%s%s %s", n ? "   " : "", p->value, val);
            n++;
            if (len >= cap) return n;
        }
    return n;
}

/* the timeline strip under the graph while playing: one block per state the graph went through */
static float g_tl[4];   /* x, y, w, h on screen when shown */
static bool timeline_shown(void) {
    LxFsmRun *run = ge_run();
    return run && run->history_count > 0 && GE.canvas && GE.canvas->ch > 200 * ui_scale(ED.ui);
}
static float bottom_pad(void) { return timeline_shown() ? 40 * ui_scale(ED.ui) : 0; }
static int timeline_entry_at(const LxFsmRun *run, float mx) {
    if (!run || run->history_count <= 0 || g_tl[2] <= 0) return -1;
    double t0 = run->history[0].time, t1 = lx_game_time(ED.scene);
    if (t1 <= t0) t1 = t0 + 0.001;
    double t = t0 + (double)((mx - g_tl[0]) / g_tl[2]) * (t1 - t0);
    int k = 0;
    while (k + 1 < run->history_count && run->history[k + 1].time <= t) k++;
    return k;
}
static void draw_timeline(UiCtx *ui, UiNode *n, const LxGraph *g, const LxFsmRun *run) {
    float s = ui_scale(ui), x = n->x + 10 * s, w = n->cw - 20 * s, h = 22 * s, y = n->y + n->ch - h - 10 * s;
    g_tl[0] = x; g_tl[1] = y; g_tl[2] = w; g_tl[3] = h;
    ui_draw_rect(ui, x - 4 * s, y - 4 * s, w + 8 * s, h + 8 * s, UI_RGBA(10, 7, 7, 230), 6 * s);
    double t0 = run->history[0].time, t1 = lx_game_time(ED.scene);
    if (t1 <= t0) t1 = t0 + 0.001;
    for (int k = 0; k < run->history_count; k++) {
        double a = run->history[k].time, b = k + 1 < run->history_count ? run->history[k + 1].time : t1;
        float bx = x + (float)((a - t0) / (t1 - t0)) * w, bw = lx_maxf(2, (float)((b - a) / (t1 - t0)) * w);
        int si = run->history[k].state;
        uint32_t c = si >= 0 && si < g->state_count ? ge_state_color(g->states[si].color == 0 ? 7 : g->states[si].color) : COL_EDGE;
        bool on = k == GE.scrub, last = k == run->history_count - 1;
        ui_draw_rect(ui, bx, y, bw - 1, h, on ? COL_ACCENT : last ? UI_HEX(0x3d7cf5) : c, 3 * s);
        if (si >= 0 && si < g->state_count && bw > ui_text_width(ui, UI_FONT_SMALL, g->states[si].name) + 10 * s) {
            ui_push_clip(ui, bx, y, bw, h);
            ui_draw_text(ui, UI_FONT_SMALL, bx + 5 * s, y + (h - ui_line_height(ui, UI_FONT_SMALL)) * 0.5f, g->states[si].name, UI_HEX(0xffffff));
            ui_pop_clip(ui);
        }
    }
    /* the one under the mouse, or being looked at: what, when, why */
    LxVec2 m = lx_mouse_pos();
    int k = GE.scrub;
    if (m.x >= x && m.x < x + w && m.y >= y - 4 * s && m.y < y + h + 4 * s) k = timeline_entry_at(run, m.x);
    if (k >= 0 && k < run->history_count) {
        int si = run->history[k].state;
        char tip[200];
        snprintf(tip, sizeof tip, "%s   at %.2f s   via %s", si >= 0 && si < g->state_count ? g->states[si].name : "?",
                 run->history[k].time - t0, run->history[k].event[0] ? run->history[k].event : "START");
        float tw = ui_text_width(ui, UI_FONT_SMALL, tip) + 16 * s, tx = lx_clampf(m.x - tw * 0.5f, n->x + 4 * s, n->x + n->cw - tw - 4 * s);
        if (k == GE.scrub && !(m.x >= x && m.x < x + w)) tx = x;
        ui_draw_rect(ui, tx, y - 30 * s, tw, 22 * s, UI_HEX(0x111214), 4 * s);
        ui_draw_border(ui, tx, y - 30 * s, tw, 22 * s, COL_EDGE, 4 * s, 1);
        ui_draw_text(ui, UI_FONT_SMALL, tx + 8 * s, y - 30 * s + (22 * s - ui_line_height(ui, UI_FONT_SMALL)) * 0.5f, tip, COL_TEXT);
    }
}

/* watched variables, top right of the graph */
static void draw_watch(UiCtx *ui, UiNode *n, const LxGraph *g, const LxFsmRun *run) {
    int count = 0;
    for (int i = 0; i < g->var_count; i++) count += g->vars[i].watch;
    if (!count) return;
    float s = ui_scale(ui), lh = ui_line_height(ui, UI_FONT_SMALL) + 4 * s, w = 220 * s, h = (float)(count + 1) * lh + 10 * s;
    float x = n->x + n->cw - w - 10 * s, y = n->y + 10 * s;
    ui_draw_rect(ui, x, y, w, h, UI_RGBA(10, 7, 7, 220), 6 * s);
    ui_draw_border(ui, x, y, w, h, COL_EDGE, 6 * s, 1);
    ui_draw_text(ui, UI_FONT_SMALL, x + 10 * s, y + 6 * s, run ? "WATCH" : "WATCH  (live while playing)", COL_DIM);
    float ry = y + 6 * s + lh;
    for (int i = 0; i < g->var_count; i++) {
        if (!g->vars[i].watch) continue;
        char val[96];
        if (run && i < run->var_count) lx_fsm_value_format(&run->vars[i], val, sizeof val, NULL);
        else lx_strcpy(val, sizeof val, g->vars[i].value);
        ui_draw_text(ui, UI_FONT_SMALL, x + 10 * s, ry, g->vars[i].name, UI_HEX(0xc89cff));
        float vw = ui_text_width(ui, UI_FONT_MONO, val);
        ui_push_clip(ui, x, ry, w - 8 * s, lh);
        ui_draw_text(ui, UI_FONT_MONO, x + w - 10 * s - vw, ry, val, run ? UI_HEX(0xffb03a) : COL_DIM);
        ui_pop_clip(ui);
        ry += lh;
    }
}

static void draw_state(UiCtx *ui, const LxGraph *g, int i, const LxFsmRun *run) {
    const LxGraphState *st = &g->states[i];
    float s = SC(), x = sx(st->x), y = sy(st->y), w = state_w(st) * s, h = state_h(st) * s, th = GE_TITLE_H * s, rh = GE_ROW_H * s;
    /* while playing: the run that is in this state (it may be inside a Sub State), and the one playing its region */
    const LxFsmRun *ar = run ? lx_fsm_run_in_state((LxFsmRun *)run, i) : NULL;
    const LxFsmRun *sr = run ? lx_fsm_run_for_scope((LxFsmRun *)run, lx_graph_parent_index(g, i), st->region) : NULL;
    bool sel = is_selected(i), active = ar != NULL;
    bool err = active && run->error;
    bool is_sub = st->kind == LX_GS_SUB, is_exit = st->kind == LX_GS_EXIT;
    float r = 5 * s;
    uint32_t head = is_exit ? UI_HEX(0x6a1d1d) : ge_state_color(st->color);
    /* shadow (a Sub State: a second card behind, it holds more), glow */
    if (is_sub) {
        ui_draw_rect(ui, x + 6 * s, y + 6 * s, w, h, UI_RGBA(0, 0, 0, 90), r);
        ui_draw_rect(ui, x + 4 * s, y + 4 * s, w, h, UI_HEX(0x222327), r);
        ui_draw_border(ui, x + 4 * s, y + 4 * s, w, h, COL_EDGE, r, 1);
    } else ui_draw_rect(ui, x + 2 * s, y + 3 * s, w, h, UI_RGBA(0, 0, 0, 110), r);
    if (active) ui_draw_rect(ui, x - 5 * s, y - 5 * s, w + 10 * s, h + 10 * s, ui_alpha(err ? UI_HEX(0xe5534b) : COL_ACCENT, 0.22f), r + 4 * s);
    ui_draw_rect(ui, x, y, w, h, COL_BOX, r);
    /* title */
    if (active) ui_draw_rect_gradient(ui, x, y, w, th, UI_HEX(0x5a92ff), UI_HEX(0x3d7cf5), r, false);
    else ui_draw_rect(ui, x, y, w, th, head, r);
    if (h > th + 1) ui_draw_rect(ui, x, y + th - r, w, r, active ? UI_HEX(0x3d7cf5) : head, 0);
    ui_push_clip(ui, x, y, w, h);
    UiFont tf = GE.zoom >= 0.8f ? UI_FONT_BOLD : UI_FONT_SMALL;
    bool compact = GE.zoom < 0.55f;   /* zoomed out: each card a block of colour with its name, big enough to read */
    if (compact) {
        ui_draw_rect(ui, x, y, w, h, active ? UI_HEX(0x3d7cf5) : ui_alpha(head, 0.85f), r);
        UiFont cf = h >= ui_line_height(ui, UI_FONT_BOLD) + 2 ? UI_FONT_BOLD : UI_FONT_SMALL;   /* a short card: smaller */
        float tw = ui_text_width(ui, cf, st->name);
        if (h >= ui_line_height(ui, cf) * 0.8f)
            ui_draw_text(ui, cf, x + lx_maxf(4, (w - tw) * 0.5f), y + (h - ui_line_height(ui, cf)) * 0.5f, st->name, UI_HEX(0xffffff));
    }
    if (GE.zoom >= 0.4f && !compact) {
        float tw = ui_text_width(ui, tf, st->name), icon_w = st->kind != LX_GS_STATE ? 16 * s : 0;
        float tx = x + (w - tw - icon_w) * 0.5f + icon_w;
        if (is_sub) ui_draw_icon(ui, UI_ICON_GRAPH, tx - icon_w, y + (th - 12 * s) * 0.5f, 12 * s, ui_alpha(UI_HEX(0xffffff), 0.8f));
        if (is_exit) ui_draw_icon(ui, UI_ICON_CLOSE, tx - icon_w, y + (th - 12 * s) * 0.5f, 12 * s, ui_alpha(UI_HEX(0xffffff), 0.8f));
        ui_draw_text(ui, tf, tx, y + (th - ui_line_height(ui, tf)) * 0.5f, st->name, UI_HEX(0xffffff));
        if (st->sequence) ui_draw_icon(ui, UI_ICON_LIST, x + w - 18 * s, y + (th - 12 * s) * 0.5f, 12 * s, ui_alpha(UI_HEX(0xffffff), 0.7f));
        /* a red dot on the left of the title bar: entering this state pauses Play mode */
        if (st->breakpoint) ui_draw_circle(ui, x + 10 * s, y + th * 0.5f, 4.5f * s, UI_HEX(0xe5484d), 12);
        UiIcon badge = st->kind == LX_GS_STATE ? state_badge(st) : UI_ICON_NONE;   /* what it mostly does */
        if (badge && GE.zoom >= 0.6f) ui_draw_icon(ui, badge, x + (st->breakpoint ? 19 : 6) * s, y + (th - 12 * s) * 0.5f, 12 * s, ui_alpha(UI_HEX(0xffffff), 0.6f));
    }
    /* event rows */
    UiFont ef = GE.zoom >= 0.8f ? UI_FONT_BODY : UI_FONT_SMALL;
    for (int t = 0; t < st->trans_count && !st->collapsed && !compact; t++) {
        float ry = y + th + rh * (float)t;
        ui_draw_rect(ui, x, ry, w, 1, UI_HEX(0x34363c), 0);
        bool hot = sel && GE.sel_trans == t;
        bool fired = sr && sr->prev_state == i && sr->state_time < 1.0f && !strcmp(sr->last_event, st->trans[t].event);
        if (hot || fired) ui_draw_rect(ui, x + 1, ry + 1, w - 2, rh - 1, UI_HEX(0x1f3050), 0);
        if (GE.zoom >= 0.45f) {
            float tw = ui_text_width(ui, ef, st->trans[t].event);
            uint32_t c = hot || fired ? UI_HEX(0xffb03a) : !st->trans[t].to[0] ? COL_DIM : COL_TEXT;
            ui_draw_text(ui, ef, x + (w - tw) * 0.5f, ry + (rh - ui_line_height(ui, ef)) * 0.5f, st->trans[t].event, c);
        }
    }
    /* what a Sub State holds (or where it is), where an Exit leads */
    if (extra_rows(st) && !compact) {
        float ry = y + th + rh * (float)st->trans_count;
        ui_draw_rect(ui, x, ry, w, 1, UI_HEX(0x34363c), 0);
        char info[160];
        if (is_sub) sub_info(g, st, run, info, sizeof info);
        else snprintf(info, sizeof info, "sends %s", lx_graph_exit_event(st));
        uint32_t c = is_sub && run && info[0] ? UI_HEX(0xffb03a) : COL_DIM;
        float tw = ui_text_width(ui, UI_FONT_SMALL, info);
        ui_draw_text(ui, UI_FONT_SMALL, x + lx_maxf(6 * s, (w - tw) * 0.5f), ry + (rh - ui_line_height(ui, UI_FONT_SMALL)) * 0.5f, info, c);
    }
    ui_pop_clip(ui);
    /* border */
    if (sel) ui_draw_border(ui, x - 1, y - 1, w + 2, h + 2, COL_ACCENT, r, 2 * ui_scale(ui));
    else ui_draw_border(ui, x, y, w, h, active ? COL_ACCENT : COL_EDGE, r, 1);
    /* something's wrong in it: an amber dot on the corner (hover for what) */
    if (state_problem(g, i)) {
        float cx = x + w - 1 * s, cy = y + 1 * s, rr = lx_maxf(5 * s, 4 * ui_scale(ui));
        ui_draw_circle(ui, cx, cy, rr + 1.5f * ui_scale(ui), UI_HEX(0x16171a), 14);
        ui_draw_circle(ui, cx, cy, rr, UI_HEX(0xf0a030), 14);
        if (rr > 4.5f * ui_scale(ui)) ui_draw_text(ui, UI_FONT_SMALL, cx - ui_text_width(ui, UI_FONT_SMALL, "!") * 0.5f, cy - ui_line_height(ui, UI_FONT_SMALL) * 0.5f, "!", UI_HEX(0x16171a));
    }
    /* unlinked transitions: a stub */
    for (int t = 0; t < st->trans_count && !st->collapsed; t++)
        if (!st->trans[t].to[0] || !lx_graph_find_state(g, st->trans[t].to))
            ui_draw_circle(ui, x + w, y + th + rh * ((float)t + 0.5f), 3 * s, COL_DIM, 10);
    /* START and global transition tabs */
    UiFont sf = UI_FONT_SMALL;
    float tab_h = lx_maxf(16 * s, ui_line_height(ui, sf) + 2), tab_y = y - 8 * s - tab_h;
    if (is_start(g, i) && GE.zoom >= 0.35f) {
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
        bool fired = ar && ar->state_time < 1.0f && !strcmp(ar->last_event, ev);
        float tx = sx(xs[k]), tw = ws[k] * s;
        ui_draw_rect(ui, tx, tab_y, tw, tab_h, UI_HEX(0x111214), 3 * s);
        ui_draw_border(ui, tx, tab_y, tw, tab_h, fired ? COL_HOT : UI_HEX(0x45474e), 3 * s, 1);
        ui_draw_text(ui, sf, tx + 7 * ui_scale(ui), tab_y + (tab_h - ui_line_height(ui, sf)) * 0.5f, ev, fired ? UI_HEX(0xffb03a) : COL_TEXT);
        uint32_t lc = fired ? COL_HOT : COL_LINK;
        ui_draw_line(ui, tx + tw * 0.5f, tab_y + tab_h, tx + tw * 0.5f, y - 3 * s, 1.6f * ui_scale(ui), lc);
        ui_draw_triangle(ui, tx + tw * 0.5f, y - 1 * s, tx + tw * 0.5f - 4 * s, y - 6 * s, tx + tw * 0.5f + 4 * s, y - 6 * s, lc);
    }
    if (active) {
        /* the longest-running timed action's progress, as a bar along the title's foot */
        float prog = -1;
        int running = 0;
        for (int a = 0; a < ar->bound_counts[i]; a++) {
            const LxFsmBound *b = &ar->bound[i][a];
            if (b->entered && !b->finished && b->desc) running++;
            float p = b->finished ? -1 : action_progress(ar, i, a);
            if (p >= 0 && (prog < 0 || p < prog)) prog = p;
        }
        if (prog >= 0) {
            ui_draw_rect(ui, x + 3 * s, y + th - 3 * s, w - 6 * s, 2 * s, UI_RGBA(0, 0, 0, 90), 0);
            ui_draw_rect(ui, x + 3 * s, y + th - 3 * s, (w - 6 * s) * prog, 2 * s, UI_HEX(0xffb03a), 0);
        }
        float by = y + h + 3 * s;
        if (GE.debug) {   /* time in the state, and how many of its actions are still going */
            char b[48];
            if (running) snprintf(b, sizeof b, "%d running  %.1fs", running, (double)ar->state_time);
            else snprintf(b, sizeof b, "%.1fs", (double)ar->state_time);
            ui_draw_text(ui, UI_FONT_SMALL, x + w - ui_text_width(ui, UI_FONT_SMALL, b), by, b, UI_HEX(0xffb03a));
        }
        char vals[200];
        if (GE.zoom >= 0.5f && state_values(g, run, i, vals, sizeof vals)) {
            ui_push_clip(ui, x - 40 * s, by, w + 80 * s, 20 * s);
            ui_draw_text(ui, UI_FONT_SMALL, x, by + (GE.debug ? ui_line_height(ui, UI_FONT_SMALL) : 0), vals, UI_HEX(0xc89cff));
            ui_pop_clip(ui);
        }
    }
    /* the timeline points here */
    if (run && GE.scrub >= 0 && GE.scrub < run->history_count && run->history[GE.scrub].state == i) {
        ui_draw_border(ui, x - 7 * s, y - 7 * s, w + 14 * s, h + 14 * s, UI_HEX(0xffb03a), r + 6 * s, 2 * ui_scale(ui));
        ui_draw_rect(ui, x - 7 * s, y - 7 * s, w + 14 * s, h + 14 * s, UI_RGBA(255, 176, 58, 26), r + 6 * s);
    }
}

/* where it has just been: the last few states glow faintly, fading as they get older */
static void draw_trail(UiCtx *ui, const LxGraph *g, const LxFsmRun *run) {
    if (!run || run->history_count < 2 || !ED.scene) return;
    double now = lx_game_time(ED.scene);
    float s = SC();
    for (int k = run->history_count - 2; k >= 0 && k >= run->history_count - 6; k--) {
        int si = run->history[k].state;
        if (si < 0 || si >= g->state_count || !vis(g, si) || lx_fsm_run_in_state((LxFsmRun *)run, si)) continue;
        double age = now - run->history[k + 1].time;   /* since it left */
        if (age > 2.5 || age < 0) continue;
        float a = (float)(1.0 - age / 2.5) * 0.55f;
        const LxGraphState *st = &g->states[si];
        float x = sx(st->x), y = sy(st->y), w = state_w(st) * s, h = state_h(st) * s;
        ui_draw_rect(ui, x - 4 * s, y - 4 * s, w + 8 * s, h + 8 * s, ui_alpha(UI_HEX(0xffb03a), a * 0.35f), 8 * s);
        ui_draw_border(ui, x - 4 * s, y - 4 * s, w + 8 * s, h + 8 * s, ui_alpha(UI_HEX(0xffb03a), a), 8 * s, 1.5f * ui_scale(ui));
    }
}

/* region lanes (a graph or Sub State with more than one region): behind the states, a header strip each */
static void draw_regions(UiCtx *ui, const LxGraph *g, const LxFsmRun *run) {
    if (!regions_shown(g)) return;
    int sc = ge_scope(), n = lx_graph_region_count(g, sc);
    static const uint32_t tints[LX_FSM_REGIONS] = {0x3d7cf5, 0x7cc24e, 0xffb03a, 0xb16be0};
    float s = SC();
    for (int r = 0; r < n; r++) {
        float q[4];
        if (!region_rect(g, r, q)) continue;
        float x = sx(q[0]), y = sy(q[1]), w = q[2] * s, h = q[3] * s;
        bool playing = run && lx_fsm_run_for_scope((LxFsmRun *)run, sc, r);
        ui_draw_rect(ui, x, y, w, h, ui_alpha(UI_HEX(tints[r]), 0.05f), 8 * s);
        ui_draw_border(ui, x, y, w, h, ui_alpha(UI_HEX(tints[r]), playing ? 0.6f : 0.3f), 8 * s, 1);
        ui_draw_rect(ui, x, y, w, 22 * s, ui_alpha(UI_HEX(tints[r]), 0.16f), 8 * s);
        if (GE.zoom >= 0.4f) {
            char label[96];
            const char *nm = lx_graph_region_name(g, sc, r);
            snprintf(label, sizeof label, "%s%sRegion %d", nm, nm[0] ? "  ·  " : "", r + 1);
            ui_draw_text(ui, UI_FONT_SMALL, x + 10 * ui_scale(ui), y + (22 * s - ui_line_height(ui, UI_FONT_SMALL)) * 0.5f, label, ui_alpha(UI_HEX(tints[r]), 0.95f));
        }
    }
}

/* where you are: the graph, then each Sub State down to the one being viewed; click one to go back up */
static void draw_crumbs(UiCtx *ui, UiNode *n, const LxGraph *g) {
    float s = ui_scale(ui);
    int chain[GE_CRUMBS], cn = 0;
    for (int p = ge_scope(); p >= 0 && cn < GE_CRUMBS - 1; p = lx_graph_parent_index(g, p)) chain[cn++] = p;
    g_crumb_count = 0;
    float x = n->x + 12 * s, y = n->y + 10 * s, h = 24 * s;
    for (int k = cn; k >= 0; k--) {
        int st = k == cn ? -1 : chain[k];
        const char *label = st < 0 ? g->name : g->states[st].name;
        bool here = k == 0;
        float w = ui_text_width(ui, here ? UI_FONT_BOLD : UI_FONT_BODY, label) + (st < 0 ? 34 : 20) * s;
        ui_draw_rect(ui, x, y, w, h, here ? UI_RGBA(61, 124, 245, 60) : UI_RGBA(255, 255, 255, 14), 5 * s);
        if (st < 0) ui_draw_icon(ui, UI_ICON_GRAPH, x + 8 * s, y + (h - 12 * s) * 0.5f, 12 * s, COL_DIM);
        ui_draw_text(ui, here ? UI_FONT_BOLD : UI_FONT_BODY, x + (st < 0 ? 24 : 10) * s, y + (h - ui_line_height(ui, UI_FONT_BODY)) * 0.5f, label,
                     here ? COL_TEXT : COL_DIM);
        if (g_crumb_count < GE_CRUMBS) {
            g_crumbs[g_crumb_count].x = x; g_crumbs[g_crumb_count].y = y; g_crumbs[g_crumb_count].w = w; g_crumbs[g_crumb_count].h = h;
            g_crumbs[g_crumb_count].state = here ? -2 : st;   /* -2: where you are already */
            g_crumb_count++;
        }
        x += w;
        if (k > 0) {
            ui_draw_icon(ui, UI_ICON_CHEVRON_RIGHT, x + 5 * s, y + (h - 12 * s) * 0.5f, 12 * s, COL_DIM);
            x += 22 * s;
        }
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
        if ((sel_only && !is_selected(i)) || !vis(g, i)) continue;
        const LxGraphState *st = &g->states[i];
        *x0 = lx_minf(*x0, st->x);
        *y0 = lx_minf(*y0, st->y - 30);
        *x1 = lx_maxf(*x1, st->x + state_w(st));
        *y1 = lx_maxf(*y1, st->y + state_h(st));
    }
    for (int i = 0; i < g->comment_count && !sel_only; i++) {
        if (!comment_vis(g, i)) continue;
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
        if (!comment_vis(g, i)) continue;
        const LxGraphComment *c = &g->comments[i];
        ui_draw_rect(ui, ox + (c->x - x0) * k, oy + (c->y - y0) * k, c->w * k, c->h * k, UI_RGBA(90, 74, 32, 120), 1);
    }
    for (int i = 0; i < g->state_count; i++) {
        if (!vis(g, i)) continue;
        const LxGraphState *st = &g->states[i];
        uint32_t c = run && lx_fsm_run_in_state((LxFsmRun *)run, i) ? COL_HOT : is_selected(i) ? COL_ACCENT : ge_state_color(st->color == 0 ? 7 : st->color);
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
    /* the description under the breadcrumbs */
    if (g->description[0] && ge_scope() < 0) draw_wrapped(ui, UI_FONT_SMALL, n->x + 18 * s, n->y + 42 * s, 420 * s, n->y + 120 * s, g->description, UI_RGBA(241, 230, 225, 56));
    draw_regions(ui, g, run);
    draw_trail(ui, g, run);
    for (int i = 0; i < g->comment_count; i++) if (comment_vis(g, i)) draw_comment(ui, g, i);
    /* links under the states */
    for (int i = 0; i < g->state_count; i++) {
        if (!vis(g, i)) continue;
        const LxGraphState *st = &g->states[i];
        const LxFsmRun *sr = run ? lx_fsm_run_for_scope(run, lx_graph_parent_index(g, i), st->region) : NULL;
        for (int t = 0; t < st->trans_count; t++) {
            float p[2][8];
            int segs = link_path(g, i, t, p);
            if (!segs) continue;
            bool hover = g_hover.kind == H_LINK && g_hover.state == i && g_hover.trans == t;
            bool hot = (is_selected(i) && (GE.sel_trans < 0 || GE.sel_trans == t)) || hover ||
                       (sr && sr->prev_state == i && sr->state_time < 1.0f && !strcmp(sr->last_event, st->trans[t].event));
            float thick = (hot ? 2.4f : 1.6f) * s * lx_clampf(GE.zoom, 0.7f, 1.3f);
            for (int sg = 0; sg < segs; sg++) draw_curve(ui, p[sg], hot ? COL_HOT : COL_LINK, thick, sg == segs - 1);
            ui_draw_circle(ui, sx(p[0][0]), sy(p[0][1]), 3 * SC(), hot ? COL_HOT : COL_LINK, 10);
            /* it just fired: a pulse runs along it */
            if (sr && sr->prev_state == i && sr->state_time < 0.6f && !strcmp(sr->last_event, st->trans[t].event)) {
                float u = lx_clampf(sr->state_time / 0.45f, 0, 1) * (float)segs;
                int sg = (int)u < segs ? (int)u : segs - 1;
                LxVec2 q = bez(p[sg], u - (float)sg);
                float rr = 6 * SC() * (1.2f - 0.4f * u / (float)segs);
                ui_draw_circle(ui, sx(q.x), sy(q.y), rr * 1.9f, ui_alpha(UI_HEX(0xffb03a), 0.25f), 16);
                ui_draw_circle(ui, sx(q.x), sy(q.y), rr, UI_HEX(0xffb03a), 16);
            }
            /* a reroute point: a small diamond you can drag (double-click removes it) */
            if (st->trans[t].via) {
                float vx = sx(st->trans[t].via_x), vy = sy(st->trans[t].via_y), d = 5 * ui_scale(ui);
                bool on = g_hover.kind == H_VIA && g_hover.state == i && g_hover.trans == t;
                ui_draw_triangle(ui, vx - d, vy, vx, vy - d, vx + d, vy, on || hot ? COL_HOT : COL_LINK);
                ui_draw_triangle(ui, vx - d, vy, vx + d, vy, vx, vy + d, on || hot ? COL_HOT : COL_LINK);
            }
            /* hovered (or selected): its event's name on it, and where to grab it to bend it */
            if ((hover || (is_selected(i) && GE.sel_trans == t)) && GE.zoom >= 0.35f) {
                LxVec2 mid = segs == 2 ? lx_v2(p[0][6], p[0][7]) : bez(p[0], 0.5f);
                const char *ev = st->trans[t].event;
                float tw = ui_text_width(ui, UI_FONT_SMALL, ev) + 14 * ui_scale(ui), lh = ui_line_height(ui, UI_FONT_SMALL) + 6 * ui_scale(ui);
                float lx_ = sx(mid.x) - tw * 0.5f, ly = sy(mid.y) - lh - 8 * ui_scale(ui);
                ui_draw_rect(ui, lx_, ly, tw, lh, UI_HEX(0x111214), lh * 0.5f);
                ui_draw_border(ui, lx_, ly, tw, lh, COL_HOT, lh * 0.5f, 1);
                ui_draw_text(ui, UI_FONT_SMALL, lx_ + 7 * ui_scale(ui), ly + 3 * ui_scale(ui), ev, UI_HEX(0xffb03a));
                if (!st->trans[t].via) ui_draw_circle(ui, sx(mid.x), sy(mid.y), 3.5f * ui_scale(ui), COL_HOT, 12);
            }
        }
    }
    for (int i = 0; i < g->state_count; i++) if (vis(g, i) && !is_selected(i)) draw_state(ui, g, i, run);
    for (int i = 0; i < g->state_count; i++) if (vis(g, i) && is_selected(i)) draw_state(ui, g, i, run);
    draw_crumbs(ui, n, g);
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
    if (g_hover.kind == H_STATE || g_hover.kind == H_EVENT) {   /* hovering a state with a problem: what it is */
        const char *pr = g_hover.state >= 0 && g_hover.state < g->state_count ? state_problem(g, g_hover.state) : NULL;
        if (pr) {
            LxVec2 hm = lx_mouse_pos();
            float tw = ui_text_width(ui, UI_FONT_SMALL, pr) + 16 * s, th2 = ui_line_height(ui, UI_FONT_SMALL) + 8 * s;
            float tx = lx_minf(hm.x + 14 * s, n->x + n->cw - tw - 4 * s), ty = hm.y + 18 * s;
            ui_draw_rect(ui, tx, ty, tw, th2, UI_HEX(0x2a1f10), 4 * s);
            ui_draw_border(ui, tx, ty, tw, th2, UI_HEX(0xf0a030), 4 * s, 1);
            ui_draw_text(ui, UI_FONT_SMALL, tx + 8 * s, ty + 4 * s, pr, UI_HEX(0xffd9a0));
        }
    }
    bool map = minimap_shown();
    if (GE.hints) {
        static const char *const lines[] = {"Right-click: add states, Sub States, comments", "Drag an event onto a state: link it (or into space: a new one)", "Double-click: rename · a Sub State: open it",
                                            "Esc: back out of a Sub State · Tab or Space: quick add", "Drag a link: bend it · Drag a comment: move what's in it",
                                            "Middle/Alt-drag: pan · Scroll: zoom · F: frame", "Del · Ctrl+C/V/D · Ctrl+Z: undo"};
        float lh = ui_line_height(ui, UI_FONT_SMALL), room = n->cw - (map ? 180 * s : 0) - 24 * s;
        int count = (int)(sizeof lines / sizeof lines[0]);
        float y = n->y + n->ch - lh * (float)count - 10 * s - bottom_pad();
        for (int i = 0; i < count; i++) {
            float lw = ui_text_width(ui, UI_FONT_SMALL, lines[i]), ly = y + lh * (float)i;
            if (lw >= room) continue;
            bool covers = false;   /* hints never draw over the graph itself */
            for (int k = 0; k < g->state_count && !covers; k++) {
                if (!vis(g, k)) continue;
                const LxGraphState *hs = &g->states[k];
                float hx = sx(hs->x) - 30 * s, hy = sy(hs->y) - 40 * s, hw = state_w(hs) * SC() + 60 * s, hh = state_h(hs) * SC() + 60 * s;
                covers = n->x + 12 * s < hx + hw && n->x + 12 * s + lw > hx && ly < hy + hh && ly + lh > hy;
            }
            for (int k = 0; k < g->comment_count && !covers; k++) {
                if (!comment_vis(g, k)) continue;
                const LxGraphComment *c = &g->comments[k];
                float hx = sx(c->x), hy = sy(c->y), hw = c->w * SC(), hh = c->h * SC();
                covers = n->x + 12 * s < hx + hw && n->x + 12 * s + lw > hx && ly < hy + hh && ly + lh > hy;
            }
            if (!covers) ui_draw_text(ui, UI_FONT_SMALL, n->x + 12 * s, ly, lines[i], UI_RGBA(241, 230, 225, 80));
        }
    }
    if (map) draw_minimap(ui, g, run);
    draw_watch(ui, n, g, run);
    if (timeline_shown()) draw_timeline(ui, n, g, run);
    else { g_tl[2] = 0; GE.scrub = -1; }
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
    int reg = region_at(g, gx, gy);
    LxGraphState *st = lx_graph_add_state(g, name, roundf(gx / 8) * 8, roundf(gy / 8) * 8);
    lx_strcpy(st->parent, sizeof st->parent, GE.scope);
    st->region = reg;
    ge_select_state(g->state_count - 1, false);
    ge_commit(true, NULL);
}
/* a Sub State with one state inside it to start from, or an Exit */
static void add_kind_at(float gx, float gy, LxGraphStateKind kind) {
    LxGraph *g = ge_graph();
    if (!g) return;
    char name[LX_FSM_NAME];
    lx_graph_unique_name(g, kind == LX_GS_SUB ? "Sub State" : "Exit", name, sizeof name, 0);
    int reg = region_at(g, gx, gy);
    LxGraphState *st = lx_graph_add_state(g, name, roundf(gx / 8) * 8, roundf(gy / 8) * 8);
    st->kind = kind;
    lx_strcpy(st->parent, sizeof st->parent, GE.scope);
    st->region = reg;
    if (kind == LX_GS_EXIT) st->color = 1;
    int me = g->state_count - 1;
    if (kind == LX_GS_SUB) {
        char inner[LX_FSM_NAME];
        lx_graph_unique_name(g, "State 1", inner, sizeof inner, 0);
        LxGraphState *in = lx_graph_add_state(g, inner, 80, 80);
        lx_strcpy(in->parent, sizeof in->parent, g->states[me].name);
    }
    ge_select_state(me, false);
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
    /* keep the selected states, and everything inside the selected Sub States */
    bool *keep = lx_calloc((size_t)g->state_count + 1, sizeof(bool));
    for (int i = 0; i < g->state_count; i++) {
        keep[i] = is_selected(i);
        for (int k = 0; k < GE.sel_count && !keep[i]; k++) keep[i] = GE.sel[k] < g->state_count && lx_graph_is_inside(g, i, GE.sel[k]);
    }
    int n = 0;
    for (int i = 0; i < c->state_count; i++) {
        if (keep[i]) {
            c->states[n] = c->states[i];
            if (is_selected(i)) c->states[n].parent[0] = 0;   /* the copied tops are relative to wherever they are pasted */
            n++;
        } else { lx_free(c->states[i].actions); lx_free(c->states[i].trans); }
    }
    lx_free(keep);
    c->state_count = n;
    c->global_count = 0;
    c->comment_count = 0;
    c->start[0] = 0;
    /* the regions of copied Sub States come along (by their names) */
    int rn = 0;
    for (int i = 0; i < c->region_count; i++) {
        bool ok = c->regions[i].parent[0] && lx_graph_find_state(c, c->regions[i].parent);
        if (ok) c->regions[rn++] = c->regions[i];
    }
    c->region_count = rn;
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
    for (int i = 0; i < c->state_count; i++)
        if (!c->states[i].parent[0]) { x0 = lx_minf(x0, c->states[i].x); y0 = lx_minf(y0, c->states[i].y); }
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
        bool top = !src->parent[0];
        LxGraphState *st = lx_graph_add_state(g, news[i], top ? roundf((src->x - x0 + gx) / 8) * 8 : src->x, top ? roundf((src->y - y0 + gy) / 8) * 8 : src->y);
        int si = g->state_count - 1;
        st->color = src->color;
        st->sequence = src->sequence;
        st->kind = src->kind;
        st->history = src->history;
        st->collapsed = src->collapsed;
        lx_strcpy(st->exit_event, sizeof st->exit_event, src->exit_event);
        if (top) { lx_strcpy(st->parent, sizeof st->parent, GE.scope); st->region = region_at(g, st->x, st->y); }
        else {
            const char *pp = src->parent;
            for (int k = 0; k < count; k++) if (!strcmp(olds[k], pp)) pp = news[k];
            lx_strcpy(st->parent, sizeof st->parent, pp);
            st->region = src->region;
        }
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
            if (nt) {
                lx_strcpy(nt->to, sizeof nt->to, to);
                nt->via = src->trans[t].via;   /* a bend moves with the pasted states */
                nt->via_x = src->trans[t].via_x + (top ? st->x - src->x : 0);
                nt->via_y = src->trans[t].via_y + (top ? st->y - src->y : 0);
            }
        }
        if (top && GE.sel_count < GE_MAX_SEL) GE.sel[GE.sel_count++] = si;
    }
    for (int r = 0; r < c->region_count; r++) {   /* copied Sub States' regions, under their new names */
        const char *pp = c->regions[r].parent, *st0 = c->regions[r].start;
        for (int k = 0; k < count; k++) { if (!strcmp(olds[k], pp)) pp = news[k]; }
        for (int k = 0; k < count; k++) { if (!strcmp(olds[k], st0)) st0 = news[k]; }
        int pi = lx_graph_state_index(g, pp);
        if (pi < 0) continue;
        LxGraphRegion *rg = lx_graph_region(g, pi, c->regions[r].index, true);
        if (rg) { lx_strcpy(rg->name, sizeof rg->name, c->regions[r].name); lx_strcpy(rg->start, sizeof rg->start, st0); }
    }
    lx_graph_free(c);
    GE.sel_trans = -1;
    ge_commit(true, NULL);
}

/* simple layered layout of the level being viewed: each region in a band of its own, columns by depth from
 * its start state along transitions */
static void auto_layout(void) {
    LxGraph *g = ge_graph();
    if (!g || !g->state_count) return;
    int n = g->state_count, sc = ge_scope(), regions = lx_graph_region_count(g, sc);
    int *depth = lx_calloc((size_t)n, sizeof(int)), *queue = lx_calloc((size_t)n, sizeof(int));
    float band_y = 80;
    for (int reg = 0; reg < regions; reg++) {
        for (int i = 0; i < n; i++) depth[i] = -1;
        int qh = 0, qt = 0, s0 = lx_graph_region_start(g, sc, reg);
        if (s0 >= 0) { depth[s0] = 0; queue[qt++] = s0; }
        while (qh < qt) {
            int i = queue[qh++];
            const LxGraphState *st = &g->states[i];
            for (int t = 0; t < st->trans_count; t++) {
                int j = lx_graph_state_index(g, st->trans[t].to);
                if (j >= 0 && depth[j] < 0 && vis(g, j) && g->states[j].region == reg) { depth[j] = depth[i] + 1; queue[qt++] = j; }
            }
        }
        /* states reached only by global transitions or not at all: their own column at the end */
        int maxd = 0;
        for (int i = 0; i < n; i++) if (depth[i] > maxd) maxd = depth[i];
        for (int i = 0; i < n; i++)
            if (depth[i] < 0 && vis(g, i) && g->states[i].region == reg) { depth[i] = maxd + 1; queue[qt++] = i; }
        float colw[64] = {0};
        for (int q = 0; q < qt; q++) { int i = queue[q], d = depth[i] < 63 ? depth[i] : 63; colw[d] = lx_maxf(colw[d], state_w(&g->states[i])); }
        float colx[64];
        colx[0] = 80;
        for (int d = 1; d < 64; d++) colx[d] = colx[d - 1] + colw[d - 1] + 110;
        float rowy[64];
        for (int d = 0; d < 64; d++) rowy[d] = band_y;
        float bottom = band_y;
        for (int q = 0; q < qt; q++) {
            int i = queue[q], d = depth[i] < 63 ? depth[i] : 63;
            g->states[i].x = colx[d];
            g->states[i].y = rowy[d];
            rowy[d] += state_h(&g->states[i]) + 70;
            bottom = lx_maxf(bottom, rowy[d]);
        }
        band_y = bottom + 90;   /* the next region below, clear of this one's lane */
    }
    lx_free(depth);
    lx_free(queue);
    ge_commit(false, NULL);
    ge_frame_all();
}


/* ======================================================================
 *  quick add (Tab or Space at the mouse): type, Enter
 * ====================================================================== */
static char g_qa_text[64];
static float g_qa_gx, g_qa_gy;
static UiNode *g_qa_list;
static int g_qa_link_state = -1, g_qa_link_trans = -1;   /* a link dropped in empty space: what gets linked to the new state */
static bool g_qa_new_state;                              /* nothing selected: a new state takes the action */

static bool qa_match(const char *text, const char *q) {
    if (!q[0]) return true;
    size_t n = strlen(q);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)q[i])) i++;
        if (i == n) return true;
    }
    return false;
}
/* the new state is where the link was dropped, and the link leads to it */
static int qa_new_state(LxGraphStateKind kind) {
    LxGraph *g = ge_graph();
    if (!g) return -1;
    if (kind == LX_GS_STATE) add_state_at(g_qa_gx, g_qa_gy);
    else add_kind_at(g_qa_gx, g_qa_gy, kind);
    g = ge_graph();
    int si = ge_sel_index();
    if (si < 0) return -1;
    if (g_qa_link_state >= 0 && g_qa_link_state < g->state_count && g_qa_link_trans >= 0 && g_qa_link_trans < g->states[g_qa_link_state].trans_count) {
        LxGraphTransition *t = &g->states[g_qa_link_state].trans[g_qa_link_trans];
        lx_strcpy(t->to, sizeof t->to, g->states[si].name);
        t->via = false;
        ge_commit(true, NULL);
    }
    g_qa_link_state = g_qa_link_trans = -1;
    return si;
}
static void qa_pick(UiNode *n) {
    int tag = (int)n->tag;
    if (tag < 0) { qa_new_state(tag == -1 ? LX_GS_STATE : tag == -2 ? LX_GS_SUB : LX_GS_EXIT); return; }
    const LxFsmActionDesc *d = lx_fsm_action_at(tag);
    if (!d) return;
    if (g_qa_new_state || g_qa_link_state >= 0) {
        if (qa_new_state(LX_GS_STATE) < 0) return;
    }
    ge_add_action(d->name);
    ge_note_recent(d->name);
}
static void qa_fill(void) {
    if (!g_qa_list) return;
    ui_clear(ED.ui, g_qa_list);
    int shown = 0;
    static const struct { const char *label; int tag; } EXTRA[] = {{"New State", -1}, {"New Sub State", -2}, {"New Exit", -3}};
    for (int k = 0; k < 3; k++) {
        if (EXTRA[k].tag == -3 && ge_scope() < 0) continue;
        if (g_qa_text[0] && !qa_match(EXTRA[k].label, g_qa_text)) continue;
        UiNode *it = ui_menu_item(ED.ui, g_qa_list, EXTRA[k].label, NULL, qa_pick);
        it->icon = EXTRA[k].tag == -1 ? UI_ICON_PLUS : EXTRA[k].tag == -2 ? UI_ICON_GRAPH : UI_ICON_CLOSE;
        it->icon_color = COL_START;
        it->tag = EXTRA[k].tag;
        shown++;
    }
    /* nothing typed yet: the actions used lately, and the favourites */
    if (!g_qa_text[0]) {
        const char *picks[24];
        int np = 0;
        for (int k = 0; k < GE.recent_count && np < 24; k++) picks[np++] = GE.recent[k];
        for (int k = 0; k < GE.fav_count && np < 24; k++) {
            bool dup = false;
            for (int q = 0; q < np; q++) dup |= !strcmp(picks[q], GE.favs[k]);
            if (!dup) picks[np++] = GE.favs[k];
        }
        if (np) {
            for (int k = 0; k < np && shown < 14; k++)
                for (int i = 0; i < lx_fsm_action_count(); i++) {
                    const LxFsmActionDesc *d = lx_fsm_action_at(i);
                    if (!d || strcmp(d->name, picks[k])) continue;
                    UiNode *it = ui_menu_item(ED.ui, g_qa_list, d->name, k < GE.recent_count ? "recent" : "favourite", qa_pick);
                    it->tag = i;
                    shown++;
                    break;
                }
            return;
        }
    }
    /* names first, then categories and descriptions */
    for (int pass = 0; pass < 2 && shown < 14; pass++)
        for (int i = 0; i < lx_fsm_action_count() && shown < 14; i++) {
            const LxFsmActionDesc *d = lx_fsm_action_at(i);
            if (!d) continue;
            bool by_name = qa_match(d->name, g_qa_text);
            if (pass == 0 ? !by_name : by_name || !(qa_match(d->category ? d->category : "", g_qa_text))) continue;
            if (!g_qa_text[0] && pass == 0 && shown >= 3 + 8) break;
            UiNode *it = ui_menu_item(ED.ui, g_qa_list, d->name, d->category, qa_pick);
            it->tag = i;
            shown++;
        }
    if (!shown) { UiNode *it = ui_menu_item(ED.ui, g_qa_list, "Nothing matches", NULL, NULL); it->disabled = true; }
}
static void qa_changed(UiNode *n) {
    LX_UNUSED(n);
    if (lx_key_pressed(LX_KEY_ENTER)) {   /* Enter: the first thing listed */
        for (UiNode *c = g_qa_list ? g_qa_list->first : NULL; c; c = c->next)
            if (c->on_click && !c->disabled) { UiNode pick = *c; ui_close_popups(ED.ui); qa_pick(&pick); return; }
        return;
    }
    qa_fill();
}
static void quick_add_open(float mx, float my, bool from_link) {
    LxGraph *g = ge_graph();
    if (!g) return;
    ui_close_popups(ED.ui);
    g_qa_gx = roundf(gx_of(mx) / 8) * 8;
    g_qa_gy = roundf(gy_of(my) / 8) * 8;
    g_qa_new_state = from_link || !GE.sel_count;
    if (!from_link) g_qa_link_state = g_qa_link_trans = -1;
    UiNode *p = ui_popup(ED.ui, mx, my, 300);
    if (!p) return;
    UiNode *head = ui_label(ED.ui, p, from_link ? "New state for this link: pick an action, or" :
                                      g_qa_new_state ? "New state here, doing:" : "Add to the selected state:");
    head->font = UI_FONT_SMALL;
    head->fg = COL_DIM;
    head->h = 18;
    g_qa_text[0] = 0;
    UiNode *tf = ui_textfield(ED.ui, p, g_qa_text, sizeof g_qa_text);
    tf->h = 26;
    tf->icon = UI_ICON_SEARCH;
    tf->on_change = qa_changed;
    lx_strcpy(tf->hint, sizeof tf->hint, "Type an action's name, Enter");
    g_qa_list = ui_box(ED.ui, p, UI_COL);
    ui_track(&g_qa_list);
    qa_fill();
    ui_focus(ED.ui, tf);
}

/* ---------------- align and distribute (several states selected) ---------------- */
static void align_sel(int how) {
    LxGraph *g = ge_graph();
    if (!g || GE.sel_count < 2) return;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < GE.sel_count; k++) {
        if (GE.sel[k] >= g->state_count) continue;
        const LxGraphState *st = &g->states[GE.sel[k]];
        x0 = lx_minf(x0, st->x); y0 = lx_minf(y0, st->y);
        x1 = lx_maxf(x1, st->x + state_w(st)); y1 = lx_maxf(y1, st->y + state_h(st));
    }
    if (how >= 6) {   /* distribute: equal gaps between them, in their current order */
        int idx[GE_MAX_SEL], n = 0;
        for (int k = 0; k < GE.sel_count; k++) if (GE.sel[k] < g->state_count) idx[n++] = GE.sel[k];
        bool across = how == 6;
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++) {
                float pa = across ? g->states[idx[a]].x : g->states[idx[a]].y, pb = across ? g->states[idx[b]].x : g->states[idx[b]].y;
                if (pb < pa) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
            }
        float total = 0;
        for (int a = 0; a < n; a++) total += across ? state_w(&g->states[idx[a]]) : state_h(&g->states[idx[a]]);
        float gap = ((across ? x1 - x0 : y1 - y0) - total) / (float)(n - 1 > 0 ? n - 1 : 1), at = across ? x0 : y0;
        for (int a = 0; a < n; a++) {
            LxGraphState *st = &g->states[idx[a]];
            if (across) { st->x = roundf(at); at += state_w(st) + gap; }
            else { st->y = roundf(at); at += state_h(st) + gap; }
        }
    } else
        for (int k = 0; k < GE.sel_count; k++) {
            if (GE.sel[k] >= g->state_count) continue;
            LxGraphState *st = &g->states[GE.sel[k]];
            switch (how) {
            case 0: st->x = x0; break;
            case 1: st->x = roundf((x0 + x1) * 0.5f - state_w(st) * 0.5f); break;
            case 2: st->x = x1 - state_w(st); break;
            case 3: st->y = y0; break;
            case 4: st->y = roundf((y0 + y1) * 0.5f - state_h(st) * 0.5f); break;
            default: st->y = y1 - state_h(st); break;
            }
        }
    ge_commit(false, NULL);
}
static void m_align_pick(UiNode *n) { align_sel((int)n->tag); }
static void m_align(UiNode *n) {
    float x = n->x + n->cw, y = n->y;
    ui_close_popups(ED.ui);
    UiNode *p = ui_popup(ED.ui, x, y, 190);
    if (!p) return;
    static const char *const NAMES[] = {"Left Edges", "Centres (across)", "Right Edges", "Tops", "Middles (down)", "Bottoms", "Distribute Across", "Distribute Down"};
    for (int i = 0; i < 8; i++) {
        if (i == 6) ui_menu_separator(ED.ui, p);
        UiNode *it = ui_menu_item(ED.ui, p, NAMES[i], NULL, m_align_pick);
        it->tag = i;
    }
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
    lx_strcpy(c.parent, sizeof c.parent, GE.scope);
    LX_VEC_PUSH(g->comments, g->comment_count, g->comment_cap, c);
    GE.sel_count = 0;
    GE.sel_comment = g->comment_count - 1;
    ge_commit(true, NULL);
}
static void m_paste(UiNode *n) { LX_UNUSED(n); paste_at(g_menu_gx, g_menu_gy); }
static void m_select_all_level(void);
static void m_select_all(UiNode *n) { LX_UNUSED(n); m_select_all_level(); }
static void m_select_all_level(void) {
    LxGraph *g = ge_graph();
    if (!g) return;
    GE.sel_count = 0;
    for (int i = 0; i < g->state_count && GE.sel_count < GE_MAX_SEL; i++) if (vis(g, i)) GE.sel[GE.sel_count++] = i;
    GE.insp_dirty = true;
}
static void m_auto_layout(UiNode *n) { LX_UNUSED(n); auto_layout(); }
static void m_frame(UiNode *n) { LX_UNUSED(n); GE.sel_count = 0; ge_frame_all(); }
static void m_add_graph(UiNode *n) { LX_UNUSED(n); if (GE.obj) ge_new_graph_for(GE.obj); }

static void m_set_start(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    const LxGraphState *st = &g->states[g_menu_state];
    lx_graph_set_region_start(g, lx_graph_parent_index(g, g_menu_state), st->region, st->name);
    ge_commit(true, NULL);
}
static void m_add_sub(UiNode *n) { LX_UNUSED(n); add_kind_at(g_menu_gx, g_menu_gy, LX_GS_SUB); }
static void m_add_exit(UiNode *n) { LX_UNUSED(n); add_kind_at(g_menu_gx, g_menu_gy, LX_GS_EXIT); }
static void m_open_sub(UiNode *n) { LX_UNUSED(n); ge_enter_scope(g_menu_state); }
static void m_back_out(UiNode *n) { LX_UNUSED(n); scope_up(); }
static void m_toggle_history(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    g->states[g_menu_state].history = !g->states[g_menu_state].history;
    ge_commit(true, NULL);
}
static void m_toggle_collapse(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g) return;
    bool to = !(g_menu_state >= 0 && g_menu_state < g->state_count && g->states[g_menu_state].collapsed);
    for (int k = 0; k < GE.sel_count; k++) if (GE.sel[k] < g->state_count) g->states[GE.sel[k]].collapsed = to;
    ge_commit(true, NULL);
}
static void exit_event_done(const char *text) {
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    char ev[LX_FSM_NAME];
    size_t k = 0;
    for (const char *p = text; *p && k + 1 < sizeof ev; p++) ev[k++] = *p == ' ' ? '_' : (char)toupper((unsigned char)*p);
    ev[k] = 0;
    LxGraphState *st = &g->states[g_menu_state];
    lx_strcpy(st->exit_event, sizeof st->exit_event, strcmp(ev, "FINISHED") ? ev : "");
    if (ev[0] && strcmp(ev, "FINISHED") && !lx_graph_find_event(g, ev)) lx_graph_add_event(g, ev);
    /* the Sub State it leaves gets a transition for it, ready to link */
    int up = lx_graph_parent_index(g, g_menu_state);
    if (up >= 0 && ev[0]) lx_graph_add_transition(g, &g->states[up], ev[0] ? ev : "FINISHED");
    ge_commit(true, NULL);
}
static void m_exit_event(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    ed_prompt("Exit Event", "The event its Sub State receives (FINISHED, or a name like DIED)", lx_graph_exit_event(&g->states[g_menu_state]), exit_event_done);
}

static bool in_names(char (*names)[LX_FSM_NAME], int n, const char *nm) {
    for (int b = 0; b < n; b++) if (!strcmp(names[b], nm)) return true;
    return false;
}
/* the selected states become a new Sub State: links between them stay inside, links into them now lead to it,
 * links out of them become the Sub State's own (they leave from any state inside) */
static void group_into_sub(void) {
    LxGraph *g = ge_graph();
    if (!g || !GE.sel_count) return;
    char names[GE_MAX_SEL][LX_FSM_NAME];
    int n = 0;
    float x0 = 1e9f, y0 = 1e9f;
    int reg = -1;
    for (int k = 0; k < GE.sel_count; k++) {
        int i = GE.sel[k];
        if (i >= g->state_count || !vis(g, i)) continue;
        lx_strcpy(names[n++], LX_FSM_NAME, g->states[i].name);
        x0 = lx_minf(x0, g->states[i].x);
        y0 = lx_minf(y0, g->states[i].y);
        if (reg < 0) reg = g->states[i].region;
    }
    if (!n) return;
    char sub_name[LX_FSM_NAME];
    lx_graph_unique_name(g, "Sub State", sub_name, sizeof sub_name, 0);
    LxGraphState *sub = lx_graph_add_state(g, sub_name, roundf(x0 / 8) * 8, roundf(y0 / 8) * 8);
    sub->kind = LX_GS_SUB;
    lx_strcpy(sub->parent, sizeof sub->parent, GE.scope);
    sub->region = reg < 0 ? 0 : reg;
    int si = g->state_count - 1;
    bool was_start = false;
    for (int a = 0; a < n; a++) {
        int i = lx_graph_state_index(g, names[a]);
        was_start |= is_start(g, i);
    }
    for (int i = 0; i < g->state_count; i++) {
        if (i == si) continue;
        LxGraphState *st = &g->states[i];
        bool inside = in_names(names, n, st->name);
        for (int t = st->trans_count - 1; t >= 0; t--) {
            bool to_in = in_names(names, n, st->trans[t].to);
            if (!inside && to_in) lx_strcpy(st->trans[t].to, sizeof st->trans[t].to, sub_name);   /* into it: to the Sub State */
            else if (inside && !to_in && st->trans[t].to[0]) {   /* out of it: the Sub State's own */
                LxGraphTransition *nt = lx_graph_add_transition(g, &g->states[si], st->trans[t].event);
                if (nt && !nt->to[0]) lx_strcpy(nt->to, sizeof nt->to, st->trans[t].to);
                lx_graph_remove_transition(&g->states[i], t);
            }
        }
    }
    for (int a = 0; a < n; a++) {
        int i = lx_graph_state_index(g, names[a]);
        lx_strcpy(g->states[i].parent, sizeof g->states[i].parent, sub_name);
        g->states[i].region = 0;
        g->states[i].x -= x0 - 80;
        g->states[i].y -= y0 - 80;
    }
    lx_graph_set_region_start(g, si, 0, names[0]);
    if (was_start) lx_graph_set_region_start(g, ge_scope(), sub->region, sub_name);
    for (int a = 0; a < n; a++) {   /* the group's own start: the one the others' links lead from first */
        int i = lx_graph_state_index(g, names[a]);
        bool reached = false;
        for (int b = 0; b < n && !reached; b++) {
            const LxGraphState *o = lx_graph_find_state(g, names[b]);
            for (int t = 0; o && t < o->trans_count; t++) reached |= !strcmp(o->trans[t].to, names[a]);
        }
        if (!reached) { lx_graph_set_region_start(g, si, 0, g->states[i].name); break; }
    }
    ge_select_state(si, false);
    ge_commit(true, NULL);
    ui_notify(ED.ui, "%d state%s now inside %s (double-click it to open)", n, n == 1 ? "" : "s", sub_name);
}
static void m_group_sub(UiNode *n) { LX_UNUSED(n); group_into_sub(); }

/* regions */
static int g_menu_region = -1;
static void m_move_region_pick(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g) return;
    int to = (int)n->tag, sc = ge_scope();
    if (to >= lx_graph_region_count(g, sc)) { lx_graph_region(g, sc, to, true); }
    float q[4], top = -1e9f;
    if (to >= lx_graph_region_count(g, sc) - 1 && !region_rect(g, to, q)) {   /* a new lane goes below the others */
        for (int r = 0; r < lx_graph_region_count(g, sc); r++) if (region_rect(g, r, q)) top = lx_maxf(top, q[1] + q[3]);
    }
    for (int k = 0; k < GE.sel_count; k++) {
        int i = GE.sel[k];
        if (i >= g->state_count || !vis(g, i)) continue;
        if (top > -1e8f) g->states[i].y = roundf((top + 70) / 8) * 8;
        g->states[i].region = to;
    }
    ge_commit(true, NULL);
}
static void m_move_region(UiNode *n) {
    float x = n->x + n->cw, y = n->y;
    ui_close_popups(ED.ui);
    LxGraph *g = ge_graph();
    UiNode *p = ui_popup(ED.ui, x, y, 190);
    if (!p || !g) return;
    int sc = ge_scope(), count = lx_graph_region_count(g, sc);
    for (int r = 0; r < count; r++) {
        char label[96];
        const char *nm = lx_graph_region_name(g, sc, r);
        snprintf(label, sizeof label, "Region %d%s%s", r + 1, nm[0] ? " - " : "", nm);
        UiNode *it = ui_menu_item(ED.ui, p, label, NULL, m_move_region_pick);
        it->tag = r;
    }
    if (count < LX_FSM_REGIONS) {
        ui_menu_separator(ED.ui, p);
        UiNode *it = ui_menu_item(ED.ui, p, "New Region (runs alongside)", NULL, m_move_region_pick);
        it->icon = UI_ICON_PLUS;
        it->tag = count;
    }
}
static void m_add_region(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g) return;
    int sc = ge_scope(), count = lx_graph_region_count(g, sc);
    if (count >= LX_FSM_REGIONS) { ui_notify(ED.ui, "Up to %d regions", LX_FSM_REGIONS); return; }
    float q[4], below = 80;
    for (int r = 0; r < count; r++) if (region_rect(g, r, q)) below = lx_maxf(below, q[1] + q[3] + 70);
    float x = 80;
    for (int r = 0; r < count; r++) if (region_rect(g, r, q)) { x = q[0] + 24; break; }
    lx_graph_region(g, sc, count, true);
    char name[LX_FSM_NAME];
    lx_graph_unique_name(g, "State 1", name, sizeof name, 0);
    LxGraphState *st = lx_graph_add_state(g, name, roundf(x / 8) * 8, roundf(below / 8) * 8);
    lx_strcpy(st->parent, sizeof st->parent, GE.scope);
    st->region = count;
    lx_graph_set_region_start(g, sc, count, name);
    ge_select_state(g->state_count - 1, false);
    ge_commit(true, NULL);
    ui_notify(ED.ui, "Region %d plays at the same time as the others", count + 1);
}
static void region_name_done(const char *text) {
    LxGraph *g = ge_graph();
    if (!g || g_menu_region < 0) return;
    LxGraphRegion *rg = lx_graph_region(g, ge_scope(), g_menu_region, true);
    if (rg) lx_strcpy(rg->name, sizeof rg->name, text);
    ge_commit(true, NULL);
}
static void m_region_rename(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_region < 0) return;
    ed_prompt("Region Name", "Name (e.g. Movement, Weapon)", lx_graph_region_name(g, ge_scope(), g_menu_region), region_name_done);
}
static void m_region_remove(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_region <= 0) return;
    lx_graph_remove_region(g, ge_scope(), g_menu_region);
    ge_commit(true, NULL);
}
static void m_region_select(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g) return;
    GE.sel_count = 0;
    for (int i = 0; i < g->state_count && GE.sel_count < GE_MAX_SEL; i++)
        if (vis(g, i) && g->states[i].region == g_menu_region) GE.sel[GE.sel_count++] = i;
    GE.insp_dirty = true;
}
static void m_toggle_sequence(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    g->states[g_menu_state].sequence = !g->states[g_menu_state].sequence;
    ge_commit(true, NULL);
}
static void m_toggle_breakpoint(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count) return;
    g->states[g_menu_state].breakpoint = !g->states[g_menu_state].breakpoint;
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
static void m_straighten(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!g || g_menu_state < 0 || g_menu_state >= g->state_count || g_menu_trans < 0 || g_menu_trans >= g->states[g_menu_state].trans_count) return;
    g->states[g_menu_state].trans[g_menu_trans].via = false;
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
        const LxGraphState *ms = &g->states[h.state];
        if (ms->kind == LX_GS_SUB) {
            mitem(p, "Open Sub State", UI_ICON_GRAPH, m_open_sub)->hint[0] = 0;
            UiNode *hi = mitem(p, "Remember Where It Left Off", UI_ICON_NONE, m_toggle_history);
            if (ms->history) { hi->icon = UI_ICON_CHECK; hi->icon_color = COL_START; }
            ui_menu_separator(ED.ui, p);
        }
        if (ms->kind == LX_GS_EXIT) { mitem(p, "Exit Event...", UI_ICON_NONE, m_exit_event); ui_menu_separator(ED.ui, p); }
        mitem(p, regions_shown(g) ? "Set as Region Start" : "Set as Start State", UI_ICON_PLAY, m_set_start);
        subitem(p, "Add Transition", m_add_trans);
        subitem(p, "Add Global Transition", m_add_global);
        mitem(p, "Add Action...", UI_ICON_PLUS, m_add_action);
        UiNode *sq = mitem(p, "Sequence (one action at a time)", UI_ICON_LIST, m_toggle_sequence);
        if (g->states[h.state].sequence) { sq->icon = UI_ICON_CHECK; sq->icon_color = COL_START; }
        UiNode *bp = mitem(p, "Breakpoint (pause Play here)", UI_ICON_NONE, m_toggle_breakpoint);
        if (g->states[h.state].breakpoint) { bp->icon = UI_ICON_CHECK; bp->icon_color = UI_HEX(0xe5484d); }
        subitem(p, "Color", m_colors);
        UiNode *cl = mitem(p, ms->collapsed ? "Expand" : "Collapse", ms->collapsed ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_UP, m_toggle_collapse);
        LX_UNUSED(cl);
        subitem(p, "Move to Region", m_move_region);
        mitem(p, GE.sel_count > 1 ? "Group into Sub State" : "Put into a Sub State", UI_ICON_GRAPH, m_group_sub);
        if (GE.sel_count > 1) subitem(p, "Align", m_align);
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
    if (h.kind == H_REGION) {
        g_menu_region = h.trans;
        mitem(p, "Rename Region...", UI_ICON_NONE, m_region_rename);
        mitem(p, "Select Its States", UI_ICON_SELECT, m_region_select);
        UiNode *rr = mitem(p, "Remove Region (states join the one before)", UI_ICON_TRASH, m_region_remove);
        rr->disabled = h.trans == 0;
        return;
    }
    if (h.kind == H_CRUMB) { ui_close_popups(ED.ui); return; }
    if (h.kind == H_LINK || h.kind == H_VIA) {
        ge_select_state(h.state, false);
        GE.sel_trans = h.trans;
        subitem(p, "Change Event", m_change_event);
        if (g->states[h.state].trans[h.trans].via) mitem(p, "Straighten (remove the bend)", UI_ICON_UNDO, m_straighten);
        else mitem(p, "Drag the line to bend it", UI_ICON_INFO, NULL)->disabled = true;
        mitem(p, "Clear Link", UI_ICON_CLOSE, m_clear_link);
        mitem(p, "Remove Transition", UI_ICON_TRASH, m_remove_trans);
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
    mitem(p, "Add Sub State", UI_ICON_GRAPH, m_add_sub);
    if (ge_scope() >= 0) mitem(p, "Add Exit", UI_ICON_CLOSE, m_add_exit);
    mitem(p, "Add Region", UI_ICON_TILES, m_add_region);
    mitem(p, "Add Comment", UI_ICON_INFO, m_add_comment);
    UiNode *ps = mitem(p, "Paste", UI_ICON_COPY, m_paste);
    ps->disabled = !g_clip;
    ui_menu_separator(ED.ui, p);
    mitem(p, "Select All", UI_ICON_SELECT, m_select_all);
    mitem(p, "Auto Layout", UI_ICON_GRID, m_auto_layout);
    mitem(p, "Frame All", UI_ICON_FOCUS, m_frame);
    mitem(p, "Action Browser", UI_ICON_LIST, m_add_action);
    if (ge_scope() >= 0) { ui_menu_separator(ED.ui, p); mitem(p, "Back Out of Sub State (Esc)", UI_ICON_CHEVRON_UP, m_back_out); }
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
        if (target >= 0) { lx_graph_set_region_start(g, lx_graph_parent_index(g, target), g->states[target].region, g->states[target].name); ge_commit(true, NULL); }
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
    if (target >= 0) { lx_strcpy(t->to, sizeof t->to, g->states[target].name); ge_commit(true, NULL); return; }
    if (h.kind == H_MINIMAP || h.kind == H_CRUMB) return;
    /* dropped in empty space: make the state it should go to (Right-click > Clear Link unlinks) */
    g_qa_link_state = GE.link_state;
    g_qa_link_trans = GE.link_trans;
    quick_add_open(mx, my, true);
}

static void canvas_frame(UiNode *n) {
    UiCtx *ui = ED.ui;
    LxGraph *g = ge_graph();
    if (g && !GE.framed && n->cw > 50) {   /* first look at a graph: fit all of it */
        GE.framed = true;
        float x0, y0, x1, y1;
        graph_bounds(g, &x0, &y0, &x1, &y1, false);
        frame_rect(x0, y0, x1, y1);
        if (g_forced_zoom > 0) { zoom_at(g_forced_zoom / GE.zoom, n->x + n->cw * 0.5f, n->y + n->ch * 0.5f); g_forced_zoom = 0; }
    } else if (g && g_qa_on_open && GE.framed) {
        g_qa_on_open = false;
        GE.sel_count = 0;
        quick_add_open(n->x + n->cw * 0.45f, n->y + n->ch * 0.55f, false);
    }
    bool blocked = ui_has_modal(ui) || ui_popup_open(ui);
    bool hot = !blocked && ui_is_inside(ui, n) && (ui_is_hot(ui, n) || ui_active(ui) == n);
    LxVec2 m = lx_mouse_pos();
    bool pressed_l = lx_mouse_pressed(LX_MOUSE_LEFT), pressed_m = lx_mouse_pressed(LX_MOUSE_MIDDLE);
    if ((pressed_l || pressed_m || lx_mouse_pressed(LX_MOUSE_RIGHT)) && !blocked) GE.focused = hot;
    if (g && hot && GE.drag == GE_DRAG_NONE) g_hover = hit_test(m.x, m.y);
    else if (GE.drag != GE_DRAG_VIA) g_hover.kind = H_NONE;
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
        if (pressed_l && g_tl[2] > 0 && m.x >= g_tl[0] && m.x < g_tl[0] + g_tl[2] && m.y >= g_tl[1] - 4 && m.y < g_tl[1] + g_tl[3] + 4)
            GE.drag = GE_DRAG_TIMELINE;   /* the history strip: look back through where the graph went */
        else if (pressed_m || alt || space) GE.drag = GE_DRAG_PAN;
        else {
            Hit h = hit_test(m.x, m.y);
            switch (h.kind) {
            case H_MINIMAP: GE.drag = GE_DRAG_MINIMAP; minimap_pan(m.x, m.y); break;
            case H_CRUMB:
                if (h.state != -2) ge_enter_scope(h.state);
                break;
            case H_LINK: case H_VIA:   /* select the transition; drag to bend it through a point */
                ge_select_state(h.state, false);
                GE.sel_trans = h.trans;
                GE.insp_dirty = true;
                if (dbl && h.kind == H_VIA) { g->states[h.state].trans[h.trans].via = false; ge_commit(true, NULL); break; }
                GE.link_state = h.state;
                GE.link_trans = h.trans;
                GE.drag = GE_DRAG_VIA;
                break;
            case H_REGION:   /* its header: select the region's states and move them together */
                g_menu_region = h.trans;
                m_region_select(NULL);
                if (dbl) { m_region_rename(NULL); break; }
                GE.drag = GE_DRAG_MOVE;
                break;
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
                if (dbl) {
                    g_menu_state = h.state;
                    GE.drag = GE_DRAG_NONE;
                    if (g->states[h.state].kind == LX_GS_SUB) ge_enter_scope(h.state);   /* look inside it */
                    else m_rename(NULL);
                    break;
                }
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
                        if (!vis(g, i)) continue;
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
    case GE_DRAG_VIA:
        if (GE.drag_moved && GE.link_state >= 0 && GE.link_state < g->state_count && GE.link_trans >= 0 &&
            GE.link_trans < g->states[GE.link_state].trans_count) {
            LxGraphTransition *tr = &g->states[GE.link_state].trans[GE.link_trans];
            tr->via = true;
            tr->via_x = gx_of(m.x);
            tr->via_y = gy_of(m.y);
        }
        break;
    case GE_DRAG_TIMELINE: {
        LxFsmRun *run = ge_run();
        int k = timeline_entry_at(run, m.x);
        if (k >= 0 && k != GE.scrub) {
            GE.scrub = k;
            int si = run->history[k].state;
            if (si >= 0 && si < g->state_count) {   /* show that state: inside its Sub State if it is in one */
                if (!vis(g, si)) ge_enter_scope(lx_graph_parent_index(g, si));
                GE.framed = true;
                ge_select_state(si, false);
            }
        }
        break;
    }
    case GE_DRAG_MOVE:
        if (GE.drag_moved) {
            float ddx = gx_of(m.x) - gx_of(GE.last.x), ddy = gy_of(m.y) - gy_of(GE.last.y);
            for (int k = 0; k < GE.sel_count; k++)
                if (GE.sel[k] < g->state_count) {
                    LxGraphState *ms = &g->states[GE.sel[k]];
                    ms->x += ddx;
                    ms->y += ddy;
                    for (int t = 0; t < ms->trans_count; t++) {   /* a bend between two moving states comes along */
                        int to = lx_graph_state_index(g, ms->trans[t].to);
                        if (ms->trans[t].via && to >= 0 && is_selected(to)) { ms->trans[t].via_x += ddx; ms->trans[t].via_y += ddy; }
                    }
                }
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
    case GE_DRAG_VIA:
        if (GE.drag_moved && GE.link_state >= 0 && GE.link_state < g->state_count && GE.link_trans >= 0 &&
            GE.link_trans < g->states[GE.link_state].trans_count) {
            LxGraphTransition *tr = &g->states[GE.link_state].trans[GE.link_trans];
            if (!lx_key_down(LX_KEY_CTRL)) { tr->via_x = roundf(tr->via_x / 8) * 8; tr->via_y = roundf(tr->via_y / 8) * 8; }
            ge_commit(true, NULL);
        }
        break;
    case GE_DRAG_BOX:
        if (GE.drag_moved) {
            float x0 = lx_minf(gx_of(GE.press.x), gx_of(m.x)), x1 = lx_maxf(gx_of(GE.press.x), gx_of(m.x));
            float y0 = lx_minf(gy_of(GE.press.y), gy_of(m.y)), y1 = lx_maxf(gy_of(GE.press.y), gy_of(m.y));
            for (int i = 0; i < g->state_count; i++) {
                if (!vis(g, i)) continue;
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
        if (lx_key_pressed(LX_KEY_F)) {   /* find: what's selected, to start with */
            LxGraphState *fs = ge_sel_state();
            ge_finder_open(fs ? fs->name : NULL);
            return true;
        }
        return false;   /* Ctrl+S, Ctrl+P... stay with the editor */
    }
    if (!g) return true;
    bool nothing = !GE.sel_count && GE.sel_comment < 0;
    if (lx_key_pressed(LX_KEY_BACKSPACE) && nothing && ge_scope() >= 0) scope_up();   /* Backspace with nothing selected: up a level */
    else if (lx_key_pressed(LX_KEY_DELETE) || lx_key_pressed(LX_KEY_BACKSPACE)) delete_selection();
    if (lx_key_pressed(LX_KEY_F)) ge_frame_all();
    if (lx_key_pressed(LX_KEY_HOME)) { GE.sel_count = 0; ge_frame_all(); }
    if (lx_key_pressed(LX_KEY_F2) && GE.sel_count) { g_menu_state = GE.sel[0]; m_rename(NULL); }
    if (lx_key_pressed(LX_KEY_ESCAPE)) {
        if (nothing && ge_scope() >= 0) scope_up();   /* nothing to deselect: back out of the Sub State */
        else { GE.sel_count = 0; GE.sel_trans = -1; GE.sel_comment = -1; GE.insp_dirty = true; }
    }
    if ((lx_key_pressed(LX_KEY_SPACE) || lx_key_pressed(LX_KEY_TAB)) && !lx_mouse_down(LX_MOUSE_LEFT)) {
        if (shift && lx_key_pressed(LX_KEY_SPACE)) ge_open_action_browser();
        else {
            LxVec2 m = lx_mouse_pos();
            bool in = GE.canvas && m.x >= GE.canvas->x && m.y >= GE.canvas->y && m.x < GE.canvas->x + GE.canvas->cw && m.y < GE.canvas->y + GE.canvas->ch;
            if (!in && GE.canvas) m = lx_v2(GE.canvas->x + GE.canvas->cw * 0.4f, GE.canvas->y + GE.canvas->ch * 0.4f);
            quick_add_open(m.x, m.y, false);
        }
    }
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
static void tb_find(UiNode *b) { LX_UNUSED(b); ge_finder_open(NULL); }
static void tb_frame(UiNode *b) { LX_UNUSED(b); GE.sel_count = 0; ge_frame_all(); }
static void tb_zoom(UiNode *b) { LX_UNUSED(b); GE.zoom = 1; }
static void tb_minimap(UiNode *b) { GE.minimap = !GE.minimap; b->selected = GE.minimap; ge_prefs_save(); }
static void tb_debug(UiNode *b) { GE.debug = !GE.debug; b->selected = GE.debug; GE.insp_dirty = true; ge_prefs_save(); }
static void tb_hints(UiNode *b) { GE.hints = !GE.hints; b->selected = GE.hints; ge_prefs_save(); }
static void tb_errors(UiNode *b) { LX_UNUSED(b); if (GE.insp_tabs) GE.insp_tabs->tab_index = 0; }
static void tb_ab(UiNode *b) { LX_UNUSED(b); ge_action_browser_toggle(); }
static void tb_pause(UiNode *b) { LX_UNUSED(b); ed_pause(); }
static void tb_step(UiNode *b) { LX_UNUSED(b); if (ED.play == ED_PLAYING) ed_pause(); ed_step(); }
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
        char path[256] = "";
        if (run && g) {   /* where it is, all the way in: Combat › Reload */
            size_t len = 0;
            for (LxFsmRun *r = run; r && r->state >= 0 && r->state < g->state_count && len + 2 < sizeof path;) {
                len += (size_t)snprintf(path + len, sizeof path - len, "%s%s", len ? "  >  " : "", g->states[r->state].name);
                LxFsmRun *next = NULL;
                for (int k = 0; k < r->kid_count && !next; k++) if (r->kids[k]->scope == r->state && r->kids[k]->region == 0) next = r->kids[k];
                r = next;
            }
        } else if (ge_sel_state()) lx_strcpy(path, sizeof path, ge_sel_state()->name);
        ui_set_text(GE.state_lbl, "%s", path);
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
    if (GE.pause_btn) {
        GE.pause_btn->hidden = ED.play == ED_EDIT;
        ui_set_text(GE.pause_btn, ED.play == ED_PAUSED ? "Resume" : "Pause");
        GE.pause_btn->icon = ED.play == ED_PAUSED ? UI_ICON_PLAY : UI_ICON_PAUSE;
        GE.pause_btn->selected = ED.play == ED_PAUSED;
    }
    if (GE.step_btn) GE.step_btn->hidden = ED.play == ED_EDIT;
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
    tbtn(tb, "Find", UI_ICON_SEARCH, "Find states, actions, variables and events in every graph (Ctrl+F)", tb_find)->w = 64;
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
    GE.pause_btn = tbtn(bb, "Pause", UI_ICON_PAUSE, "Pause the game to look at the graph (the timeline below the graph shows where it has been)", tb_pause);
    GE.pause_btn->w = 84;
    GE.step_btn = tbtn(bb, "Step", UI_ICON_STEP, "Play one frame", tb_step);
    GE.step_btn->w = 70;
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
    ui_track(&GE.pause_btn);
    ui_track(&GE.step_btn);
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
    if (lx_graph_parent_index(g, i) != ge_scope()) ge_enter_scope(lx_graph_parent_index(g, i));   /* inside a Sub State: look in there */
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
    switch (trnd(31)) {
    case 29: {   /* quick add: a new state doing a random action, sometimes from a link dropped in space */
        g_qa_gx = gx_of(cx);
        g_qa_gy = gy_of(cy);
        g_qa_new_state = trnd(2) == 0 || !GE.sel_count;
        g_qa_link_state = g_qa_link_trans = -1;
        if (si >= 0 && g->states[si].trans_count && trnd(2)) { g_qa_link_state = si; g_qa_link_trans = (int)trnd(g->states[si].trans_count); }
        UiNode tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.tag = trnd(5) == 0 ? -1 - (int)trnd(ge_scope() >= 0 ? 3 : 2) : (int)trnd(lx_fsm_action_count());
        qa_pick(&tmp);
        return "quick add";
    }
    case 30: m_select_all_level(); align_sel((int)trnd(8)); return "align the states";
    case 28: {   /* bend a link, or straighten it */
        if (si < 0 || !g->states[si].trans_count) return "bend (no transitions)";
        LxGraphTransition *tr = &g->states[si].trans[trnd(g->states[si].trans_count)];
        tr->via = trnd(3) != 0;
        tr->via_x = gx_of(cx);
        tr->via_y = gy_of(cy);
        ge_commit(true, NULL);
        return "bend a link";
    }
    case 20: if (GE.sel_count) { group_into_sub(); return "group into a Sub State"; } return "group (nothing selected)";
    case 21: {
        int subs[64], n = 0;
        for (int i = 0; i < g->state_count && n < 64; i++) if (g->states[i].kind == LX_GS_SUB) subs[n++] = i;
        if (n && trnd(2)) { ge_enter_scope(subs[trnd(n)]); return "open a Sub State"; }
        scope_up();
        return "back out of a Sub State";
    }
    case 22: add_kind_at(gx_of(cx), gy_of(cy), trnd(2) ? LX_GS_SUB : LX_GS_EXIT); return "add a Sub State or Exit";
    case 23: {
        if (si < 0) return "region (no state)";
        ge_select_state(si, false);
        UiNode tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.tag = (int)trnd(LX_FSM_REGIONS);
        if (tmp.tag > lx_graph_region_count(g, ge_scope())) tmp.tag = lx_graph_region_count(g, ge_scope());
        if (vis(g, si)) m_move_region_pick(&tmp);
        return "move a state to a region";
    }
    case 24: m_add_region(NULL); return "add a region";
    case 25: if (si >= 0) { g_menu_state = si; ge_select_state(si, false); m_toggle_collapse(NULL); } return "collapse a state";
    case 26: if (si >= 0) { g_menu_state = si; m_toggle_history(NULL); } return "toggle history";
    case 27: { g_menu_region = (int)trnd(LX_FSM_REGIONS); if (g_menu_region < lx_graph_region_count(g, ge_scope())) m_region_remove(NULL); return "remove a region"; }
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

void ed_graph_set_zoom(float z) { g_forced_zoom = z; }
void ed_graph_quick_add_on_open(void) { g_qa_on_open = true; }
void ed_graph_show_tab(int tab) {
    if (tab == 4) { ge_open_action_browser(); return; }
    if (GE.insp_tabs) GE.insp_tabs->tab_index = tab;
}
