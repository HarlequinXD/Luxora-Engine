/* Animator editor: the Animator tab, beside the Scene and State Graph tabs.
 *
 * It is a state machine over animation: states play clips or blend trees, and the arrows between them
 * are conditions on parameters rather than events. This file is the graph view - drawing the states,
 * the Entry and Any State nodes and the transitions between them, and the selecting, moving, linking
 * and context menus that go with it. The panel on its right (anim_inspector.c) is where a state's
 * motion, a transition's conditions, the parameters and the layers are edited.
 *
 * Graphs are assets (.lxanim): every edit is saved straight away, and animators playing the graph pick
 * the change up on the next frame - so a transition's duration can be tuned while the game runs. */
#include "anim_editor.h"
#include <math.h>

AnimEd AE = {.zoom = 1, .sel = -1, .sel_trans = -1, .link_from = -1};

static const uint32_t STATE_COLORS[AE_COLORS] = {0x2f3a4a, 0x7a2412, 0x1d3a5f, 0x4a2350, 0x24502c, 0x1d4a4a, 0x5a4a14, 0x2a2b30};
uint32_t ae_state_color(int c) { return UI_HEX(STATE_COLORS[c >= 0 && c < AE_COLORS ? c : 0]); }

#define COL_LINK UI_HEX(0xb1b5bd)
#define COL_HOT UI_HEX(0x5a92ff)
#define COL_ACCENT UI_HEX(0x3d7cf5)
#define COL_ENTRY UI_HEX(0x7cc24e)
#define COL_ANY UI_HEX(0x3fa7a0)
#define COL_TEXT UI_HEX(0xdfe1e6)
#define COL_DIM UI_HEX(0x8d919a)
#define COL_EDGE UI_HEX(0x34363c)
#define COL_RUN UI_HEX(0xffb03a)

/* ======================================================================
 *  what is being edited
 * ====================================================================== */
LxAnimGraph *ae_graph(void) { return AE.ref[0] ? lx_asset_anim_graph(AE.ref) : NULL; }

LxAnimLayer *ae_layer(void) {
    LxAnimGraph *g = ae_graph();
    if (!g || !g->layer_count) return NULL;
    if (AE.layer < 0 || AE.layer >= g->layer_count) AE.layer = 0;
    return &g->layers[AE.layer];
}

int ae_sel_index(void) {
    LxAnimLayer *l = ae_layer();
    if (AE.sel == AE_ANY) return AE_ANY;
    return l && AE.sel >= 0 && AE.sel < l->state_count ? AE.sel : -1;
}

LxAnimNode *ae_sel_state(void) {
    LxAnimLayer *l = ae_layer();
    int i = ae_sel_index();
    return l && i >= 0 ? &l->states[i] : NULL;
}

LxAnimTransition *ae_sel_transition(void) {
    LxAnimLayer *l = ae_layer();
    if (!l || AE.sel_trans < 0) return NULL;
    if (AE.sel == AE_ANY) return AE.sel_trans < l->any_count ? &l->any[AE.sel_trans] : NULL;
    LxAnimNode *n = ae_sel_state();
    return n && AE.sel_trans < n->trans_count ? &n->trans[AE.sel_trans] : NULL;
}

void ae_select(int state, int trans) {
    AE.sel = state;
    AE.sel_trans = trans;
    AE.insp_dirty = true;
    if (AE.insp_tabs) AE.insp_tabs->tab_index = trans >= 0 ? 2 : (state == -1 ? 0 : 1);
}

/* the state a layer is playing in the running game, for the live highlight */
int ae_live_state(int layer) {
    LxAnimGraph *g = ae_graph();
    if (ED.play == ED_EDIT || !AE.obj || !ED.scene || !g) return -1;
    if (!lx_entity_valid(ED.scene, AE.obj)) return -1;
    LxEntity a = lx_find_animator(ED.scene, AE.obj);
    if (!a) return -1;
    const char *name = lx_animator_state(ED.scene, a, layer);
    if (!name || !name[0] || layer < 0 || layer >= g->layer_count) return -1;
    return lx_anim_state_index(&g->layers[layer], name);
}

/* ======================================================================
 *  saving and undo
 * ====================================================================== */
static void clear_history(void) {
    for (int i = 0; i < AE.undo_count; i++) lx_free(AE.undo[i]);
    for (int i = 0; i < AE.redo_count; i++) lx_free(AE.redo[i]);
    AE.undo_count = AE.redo_count = 0;
    lx_free(AE.last_text);
    AE.last_text = NULL;
    AE.last_key = NULL;
}

static void save_graph(LxAnimGraph *g) {
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, AE.ref);
    if (!lx_anim_graph_save(g, path)) ui_notify(ED.ui, "Couldn't save %s", AE.ref);
}

void ae_commit(bool structural, const void *key) {
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    char *t = lx_anim_graph_to_text(g);
    if (AE.last_text && !strcmp(t, AE.last_text)) {
        lx_free(t);
        if (structural) AE.insp_dirty = true;
        return;
    }
    double now = lx_time();
    bool merge = key && key == AE.last_key && now - AE.last_commit_time < 1.0;   /* one undo step per drag */
    if (!merge && AE.last_text) {
        if (AE.undo_count == AE_UNDO) { lx_free(AE.undo[0]); memmove(AE.undo, AE.undo + 1, sizeof(char *) * (AE_UNDO - 1)); AE.undo_count--; }
        AE.undo[AE.undo_count++] = AE.last_text;
    } else lx_free(AE.last_text);
    AE.last_text = t;
    AE.last_key = key;
    AE.last_commit_time = now;
    for (int i = 0; i < AE.redo_count; i++) lx_free(AE.redo[i]);
    AE.redo_count = 0;
    lx_anim_graph_touch(g);
    AE.seen_version = g->version;
    if (structural) AE.insp_dirty = true;
    save_graph(g);
}

static void restore(const char *text) {
    LxAnimGraph *ng = lx_anim_graph_from_text(text);
    if (!ng) return;
    lx_anim_graph_replace(AE.ref, ng);
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    AE.seen_version = g->version;
    if (AE.layer >= g->layer_count) AE.layer = 0;
    LxAnimLayer *l = ae_layer();
    if (!l || (AE.sel >= 0 && AE.sel >= l->state_count)) AE.sel = -1;
    AE.sel_trans = -1;
    AE.insp_dirty = true;
    save_graph(g);
}

void ae_undo(void) {
    if (!AE.undo_count) { ui_notify(ED.ui, "Nothing to undo"); return; }
    char *text = AE.undo[--AE.undo_count];
    if (AE.last_text) {
        if (AE.redo_count == AE_UNDO) { lx_free(AE.redo[0]); memmove(AE.redo, AE.redo + 1, sizeof(char *) * (AE_UNDO - 1)); AE.redo_count--; }
        AE.redo[AE.redo_count++] = AE.last_text;
    }
    AE.last_text = text;
    AE.last_key = NULL;
    restore(text);
}

void ae_redo(void) {
    if (!AE.redo_count) { ui_notify(ED.ui, "Nothing to redo"); return; }
    char *text = AE.redo[--AE.redo_count];
    if (AE.last_text) AE.undo[AE.undo_count < AE_UNDO ? AE.undo_count++ : AE.undo_count - 1] = AE.last_text;
    AE.last_text = text;
    AE.last_key = NULL;
    restore(text);
}

static void set_target(const char *ref, LxEntity obj) {
    AE.obj = obj;
    if (!strcmp(ref, AE.ref)) return;
    lx_strcpy(AE.ref, sizeof AE.ref, ref);
    clear_history();
    LxAnimGraph *g = ae_graph();
    if (g) { AE.last_text = lx_anim_graph_to_text(g); AE.seen_version = g->version; }
    AE.layer = 0;
    AE.sel = -1;
    AE.sel_trans = -1;
    AE.drag = AE_DRAG_NONE;
    AE.framed = false;
    AE.insp_dirty = true;
}

/* ======================================================================
 *  view transform
 * ====================================================================== */
static float SC(void) { return ui_scale(ED.ui) * AE.zoom; }
static float sx(float gx) { return AE.canvas ? AE.canvas->x + (gx - AE.pan_x) * SC() : 0; }
static float sy(float gy) { return AE.canvas ? AE.canvas->y + (gy - AE.pan_y) * SC() : 0; }
static float gx_of(float screen_x) { return AE.canvas ? AE.pan_x + (screen_x - AE.canvas->x) / SC() : 0; }
static float gy_of(float screen_y) { return AE.canvas ? AE.pan_y + (screen_y - AE.canvas->y) / SC() : 0; }

static void zoom_at(float factor, float mx, float my) {
    float before_x = gx_of(mx), before_y = gy_of(my);
    AE.zoom = lx_clampf(AE.zoom * factor, 0.25f, 2.5f);
    AE.pan_x += before_x - gx_of(mx);
    AE.pan_y += before_y - gy_of(my);
}

/* the Entry and Any State nodes sit to the left of everything else, so they never cover a state */
static void special_nodes(const LxAnimLayer *l, float *ex, float *ey, float *ax, float *ay) {
    float x0 = 1e9f, y0 = 1e9f;
    for (int i = 0; i < l->state_count; i++) {
        if (l->states[i].x < x0) x0 = l->states[i].x;
        if (l->states[i].y < y0) y0 = l->states[i].y;
    }
    if (x0 > 1e8f) { x0 = 120; y0 = 120; }
    *ex = x0 - 230;
    *ey = y0;
    *ax = x0 - 230;
    *ay = y0 + 70;
}

static void layer_bounds(const LxAnimLayer *l, float *x0, float *y0, float *x1, float *y1) {
    float ex, ey, ax, ay;
    special_nodes(l, &ex, &ey, &ax, &ay);
    *x0 = ex;
    *y0 = ey;
    *x1 = ax + AE_STATE_W;
    *y1 = ay + AE_STATE_H;
    for (int i = 0; i < l->state_count; i++) {
        const LxAnimNode *n = &l->states[i];
        if (n->x < *x0) *x0 = n->x;
        if (n->y < *y0) *y0 = n->y;
        if (n->x + AE_STATE_W > *x1) *x1 = n->x + AE_STATE_W;
        if (n->y + AE_STATE_H > *y1) *y1 = n->y + AE_STATE_H;
    }
}

static void frame_rect(float x0, float y0, float x1, float y1) {
    if (!AE.canvas || AE.canvas->cw < 20) return;
    float pad = 50;
    float w = lx_maxf(1, x1 - x0 + pad * 2), h = lx_maxf(1, y1 - y0 + pad * 2);
    float s = ui_scale(ED.ui);
    AE.zoom = lx_clampf(lx_minf(AE.canvas->cw / (w * s), AE.canvas->ch / (h * s)), 0.25f, 1.6f);
    AE.pan_x = x0 - pad - (AE.canvas->cw / SC() - w) * 0.5f;
    AE.pan_y = y0 - pad - (AE.canvas->ch / SC() - h) * 0.5f;
}

void ae_frame_all(void) {
    LxAnimLayer *l = ae_layer();
    if (!l) return;
    float x0, y0, x1, y1;
    layer_bounds(l, &x0, &y0, &x1, &y1);
    frame_rect(x0, y0, x1, y1);
}

/* ======================================================================
 *  hit testing
 * ====================================================================== */
enum { H_NONE, H_STATE, H_ANY, H_ENTRY, H_TRANS };
typedef struct Hit { int kind, state, trans; } Hit;

static bool in_box(float mx, float my, float gx, float gy) {
    float x = sx(gx), y = sy(gy), w = AE_STATE_W * SC(), h = AE_STATE_H * SC();
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

/* where a transition's line runs: box centre to box centre, cut back to the edges */
static bool trans_line(const LxAnimLayer *l, int from, int ti, float *out) {
    float fx, fy;
    if (from == AE_ANY) {
        float ex, ey, ax, ay;
        special_nodes(l, &ex, &ey, &ax, &ay);
        fx = ax;
        fy = ay;
    } else {
        if (from < 0 || from >= l->state_count) return false;
        fx = l->states[from].x;
        fy = l->states[from].y;
    }
    const LxAnimTransition *t = from == AE_ANY ? (ti < l->any_count ? &l->any[ti] : NULL)
                                               : (ti < l->states[from].trans_count ? &l->states[from].trans[ti] : NULL);
    if (!t) return false;
    int to = lx_anim_state_index(l, t->to);
    if (to < 0) return false;
    float tx = l->states[to].x, ty = l->states[to].y;
    float cx0 = fx + AE_STATE_W * 0.5f, cy0 = fy + AE_STATE_H * 0.5f;
    float cx1 = tx + AE_STATE_W * 0.5f, cy1 = ty + AE_STATE_H * 0.5f;
    float dx = cx1 - cx0, dy = cy1 - cy0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) return false;
    dx /= len;
    dy /= len;
    /* several transitions between the same two states: fan them out so each can be clicked */
    float off = (float)ti * 9.0f - 4.0f;
    float px = -dy * off, py = dx * off;
    float hw = AE_STATE_W * 0.5f, hh = AE_STATE_H * 0.5f;
    float scale = lx_minf(fabsf(dx) > 1e-4f ? hw / fabsf(dx) : 1e9f, fabsf(dy) > 1e-4f ? hh / fabsf(dy) : 1e9f);
    out[0] = cx0 + dx * scale + px;
    out[1] = cy0 + dy * scale + py;
    out[2] = cx1 - dx * scale + px;
    out[3] = cy1 - dy * scale + py;
    return true;
}

static float dist_to_segment(float px, float py, float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 1e-6f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0;
    t = lx_clampf(t, 0, 1);
    float cx = x0 + dx * t - px, cy = y0 + dy * t - py;
    return sqrtf(cx * cx + cy * cy);
}

static Hit hit_test(float mx, float my) {
    Hit h = {H_NONE, -1, -1};
    LxAnimLayer *l = ae_layer();
    if (!l) return h;
    for (int i = l->state_count - 1; i >= 0; i--)
        if (in_box(mx, my, l->states[i].x, l->states[i].y)) { h.kind = H_STATE; h.state = i; return h; }
    float ex, ey, ax, ay;
    special_nodes(l, &ex, &ey, &ax, &ay);
    if (in_box(mx, my, ax, ay)) { h.kind = H_ANY; h.state = AE_ANY; return h; }
    if (in_box(mx, my, ex, ey)) { h.kind = H_ENTRY; return h; }
    /* a transition line, within a few pixels of it */
    float best = 7 * ui_scale(ED.ui);
    for (int from = -1; from < l->state_count; from++) {
        int src = from < 0 ? AE_ANY : from;
        int count = src == AE_ANY ? l->any_count : l->states[src].trans_count;
        for (int t = 0; t < count; t++) {
            float p[4];
            if (!trans_line(l, src, t, p)) continue;
            float d = dist_to_segment(mx, my, sx(p[0]), sy(p[1]), sx(p[2]), sy(p[3]));
            if (d < best) { best = d; h.kind = H_TRANS; h.state = src; h.trans = t; }
        }
    }
    return h;
}

/* ======================================================================
 *  drawing
 * ====================================================================== */
static void draw_grid(UiCtx *ui, UiNode *n) {
    float s = SC(), step = 32 * s;
    if (step < 7) return;
    uint32_t c = UI_RGBA(255, 255, 255, 10);
    float ox = fmodf(-AE.pan_x * s, step), oy = fmodf(-AE.pan_y * s, step);
    if (ox < 0) ox += step;
    if (oy < 0) oy += step;
    for (float x = n->x + ox; x < n->x + n->cw; x += step) ui_draw_rect(ui, x, n->y, 1, n->ch, c, 0);
    for (float y = n->y + oy; y < n->y + n->ch; y += step) ui_draw_rect(ui, n->x, y, n->cw, 1, c, 0);
}

static void draw_arrow(UiCtx *ui, float x0, float y0, float x1, float y1, uint32_t col, float thick) {
    ui_draw_line(ui, x0, y0, x1, y1, thick, col);
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    dx /= len;
    dy /= len;
    /* the head sits in the middle of the line, as in Unity: it reads as direction, not as a target */
    float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
    float s = 8 * ui_scale(ui) * lx_clampf(AE.zoom, 0.6f, 1.4f);
    ui_draw_triangle(ui, mx + dx * s, my + dy * s,
                     mx - dx * s * 0.4f - dy * s * 0.5f, my - dy * s * 0.4f + dx * s * 0.5f,
                     mx - dx * s * 0.4f + dy * s * 0.5f, my - dy * s * 0.4f - dx * s * 0.5f, col);
}

static void draw_box(UiCtx *ui, float gx, float gy, uint32_t bg, const char *title, const char *sub,
                     bool selected, bool live, float live_progress) {
    float s = SC();
    float x = sx(gx), y = sy(gy), w = AE_STATE_W * s, h = AE_STATE_H * s;
    float r = 5 * s;
    ui_draw_rect(ui, x + 2 * s, y + 3 * s, w, h, UI_RGBA(0, 0, 0, 110), r);
    if (live) ui_draw_rect(ui, x - 5 * s, y - 5 * s, w + 10 * s, h + 10 * s, ui_alpha(COL_RUN, 0.20f), r + 4 * s);
    ui_draw_rect(ui, x, y, w, h, bg, r);
    if (live && live_progress >= 0) {
        /* how far through the state is, along its bottom edge */
        float frac = live_progress - floorf(live_progress);
        ui_draw_rect(ui, x, y + h - 3 * s, w * lx_clampf(frac, 0, 1), 3 * s, COL_RUN, 0);
    }
    ui_draw_border(ui, x, y, w, h, selected ? COL_ACCENT : (live ? COL_RUN : COL_EDGE), r, selected ? 2 * ui_scale(ui) : 1);
    UiFont tf = s > 0.55f ? UI_FONT_BOLD : UI_FONT_SMALL;
    float tw = ui_text_width(ui, tf, title);
    float th = ui_line_height(ui, tf);
    bool has_sub = sub && sub[0] && s > 0.5f;
    float ty = has_sub ? y + h * 0.5f - th : y + (h - th) * 0.5f;
    if (s > 0.3f) ui_draw_text(ui, tf, x + (w - tw) * 0.5f, ty, title, COL_TEXT);
    if (has_sub) {
        float sw = ui_text_width(ui, UI_FONT_SMALL, sub);
        ui_draw_text(ui, UI_FONT_SMALL, x + (w - sw) * 0.5f, y + h * 0.5f + 2 * s, sub, COL_DIM);
    }
}

/* what a state plays, in a few words */
static void state_sub(const LxAnimGraph *g, const LxAnimNode *n, char *out, size_t cap) {
    if (n->motion == LX_AM_TREE) {
        const LxAnimTree *t = n->tree >= 0 && n->tree < g->tree_count ? &g->trees[n->tree] : NULL;
        if (!t) lx_strcpy(out, cap, "(no blend tree)");
        else snprintf(out, cap, "%s blend, %d motion%s", lx_anim_tree_type_name(t->type), t->child_count,
                      t->child_count == 1 ? "" : "s");
        return;
    }
    if (!n->clip[0]) { lx_strcpy(out, cap, "(no clip)"); return; }
    const char *hash = strrchr(n->clip, '#');
    lx_strcpy(out, cap, hash && hash[1] ? hash + 1 : n->clip);
}

static void draw_center_message(UiCtx *ui, UiNode *n, const char *title, const char *sub) {
    float s = ui_scale(ui);
    float tw = ui_text_width(ui, UI_FONT_TITLE, title);
    ui_draw_icon(ui, UI_ICON_ANIMATOR, n->x + (n->cw - 48 * s) * 0.5f, n->y + n->ch * 0.5f - 90 * s, 48 * s, UI_HEX(0x45474e));
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
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l) {
        if (AE.ref[0]) draw_center_message(ui, n, "This animator graph can't be opened", AE.ref);
        else if (AE.obj && ED.scene && lx_entity_valid(ED.scene, AE.obj)) {
            char b[160];
            snprintf(b, sizeof b, "%s has no animator graph", lx_entity_name(ED.scene, AE.obj));
            draw_center_message(ui, n, b, "Right-click here to make one, or drop a .lxanim asset here.");
        } else draw_center_message(ui, n, "Animator", "Select an object with an Animator, or a .lxanim asset in the Content Browser.");
        ui_pop_clip(ui);
        return;
    }
    ui_draw_text(ui, UI_FONT_TITLE, n->x + 16 * s, n->y + 10 * s, g->name, UI_RGBA(241, 230, 225, 34));
    int live = ae_live_state(AE.layer);
    float ex, ey, ax, ay;
    special_nodes(l, &ex, &ey, &ax, &ay);
    /* the arrows first, so the states sit on top of them */
    for (int from = -1; from < l->state_count; from++) {
        int src = from < 0 ? AE_ANY : from;
        int count = src == AE_ANY ? l->any_count : l->states[src].trans_count;
        for (int t = 0; t < count; t++) {
            float p[4];
            if (!trans_line(l, src, t, p)) continue;
            const LxAnimTransition *tr = src == AE_ANY ? &l->any[t] : &l->states[src].trans[t];
            bool sel = AE.sel == src && AE.sel_trans == t;
            uint32_t col = sel ? COL_HOT : (src == AE_ANY ? ui_alpha(COL_ANY, 0.8f) : COL_LINK);
            if (tr->mute) col = ui_alpha(col, 0.35f);
            draw_arrow(ui, sx(p[0]), sy(p[1]), sx(p[2]), sy(p[3]), col, (sel ? 2.6f : 1.6f) * s * lx_clampf(AE.zoom, 0.7f, 1.3f));
        }
    }
    /* Entry -> the layer's starting state */
    int start = lx_anim_state_index(l, l->start);
    if (start >= 0) {
        float x0 = ex + AE_STATE_W, y0 = ey + AE_STATE_H * 0.5f;
        float x1 = l->states[start].x, y1 = l->states[start].y + AE_STATE_H * 0.5f;
        draw_arrow(ui, sx(x0), sy(y0), sx(x1), sy(y1), COL_ENTRY, 2 * s);
    }
    draw_box(ui, ex, ey, UI_HEX(0x245024), "Entry", l->start[0] ? l->start : "(no starting state)", false, false, -1);
    draw_box(ui, ax, ay, UI_HEX(0x1d4a4a), "Any State", l->any_count ? NULL : "drag out a transition",
             AE.sel == AE_ANY, false, -1);
    for (int i = 0; i < l->state_count; i++) {
        const LxAnimNode *st = &l->states[i];
        char sub[96];
        state_sub(g, st, sub, sizeof sub);
        bool is_live = i == live;
        float prog = -1;
        if (is_live && ED.scene && AE.obj) {
            LxEntity a = lx_find_animator(ED.scene, AE.obj);
            if (a) prog = lx_animator_normalized_time(ED.scene, a, AE.layer);
        }
        uint32_t bg = i == start ? UI_HEX(0x2d4634) : ae_state_color(st->color);
        draw_box(ui, st->x, st->y, bg, st->name, sub, AE.sel == i && AE.sel_trans < 0, is_live, prog);
        if (st->breakpoint) ui_draw_circle(ui, sx(st->x) + 10 * SC(), sy(st->y) + 10 * SC(), 4.5f * SC(), UI_HEX(0xe5484d), 12);
    }
    /* dragging a new transition out */
    LxVec2 m = lx_mouse_pos();
    if (AE.drag == AE_DRAG_LINK && AE.drag_moved) {
        float fx, fy;
        if (AE.link_from == AE_ANY) { fx = ax + AE_STATE_W * 0.5f; fy = ay + AE_STATE_H * 0.5f; }
        else if (AE.link_from >= 0 && AE.link_from < l->state_count) {
            fx = l->states[AE.link_from].x + AE_STATE_W * 0.5f;
            fy = l->states[AE.link_from].y + AE_STATE_H * 0.5f;
        } else { fx = fy = 0; }
        ui_draw_line(ui, sx(fx), sy(fy), m.x, m.y, 2 * s, COL_HOT);
        ui_draw_circle(ui, m.x, m.y, 4 * s, COL_HOT, 12);
    }
    if (AE.drag == AE_DRAG_ENTRY && AE.drag_moved) {
        ui_draw_line(ui, sx(ex + AE_STATE_W), sy(ey + AE_STATE_H * 0.5f), m.x, m.y, 2 * s, COL_ENTRY);
        ui_draw_circle(ui, m.x, m.y, 4 * s, COL_ENTRY, 12);
    }
    ui_pop_clip(ui);
}

/* ======================================================================
 *  editing
 * ====================================================================== */
static int g_menu_state = -1, g_menu_trans = -1;

static void add_state_at(float gx, float gy, bool blend_tree) {
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l) return;
    LxAnimNode *n = lx_anim_add_state(g, l, blend_tree ? "Blend Tree" : "New State", gx, gy);
    if (!n) return;
    if (blend_tree) {
        n->motion = LX_AM_TREE;
        n->tree = lx_anim_add_tree(g, LX_AT_1D);
        l = ae_layer();   /* the trees array moved; the layer did not, but be safe */
    }
    ae_select(l->state_count - 1, -1);
    ae_commit(true, NULL);
}

static void m_add_state(UiNode *x) { LX_UNUSED(x); add_state_at(AE.press_gx - AE_STATE_W * 0.5f, AE.press_gy - AE_STATE_H * 0.5f, false); }
static void m_add_tree(UiNode *x) { LX_UNUSED(x); add_state_at(AE.press_gx - AE_STATE_W * 0.5f, AE.press_gy - AE_STATE_H * 0.5f, true); }

static void m_set_default(UiNode *x) {
    LX_UNUSED(x);
    LxAnimLayer *l = ae_layer();
    if (!l || g_menu_state < 0 || g_menu_state >= l->state_count) return;
    lx_strcpy(l->start, sizeof l->start, l->states[g_menu_state].name);
    ae_commit(true, NULL);
}

static void m_delete_state(UiNode *x) {
    LX_UNUSED(x);
    LxAnimLayer *l = ae_layer();
    if (!l || g_menu_state < 0 || g_menu_state >= l->state_count) return;
    lx_anim_remove_state(l, g_menu_state);
    AE.sel = -1;
    AE.sel_trans = -1;
    ae_commit(true, NULL);
}

static void m_duplicate_state(UiNode *x) {
    LX_UNUSED(x);
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l || g_menu_state < 0 || g_menu_state >= l->state_count) return;
    LxAnimNode src = l->states[g_menu_state];
    LxAnimNode *n = lx_anim_add_state(g, l, src.name, src.x + 30, src.y + 40);
    if (!n) return;
    char name[LX_ANIM_NAME];
    lx_strcpy(name, sizeof name, n->name);
    LxAnimTransition *keep_trans = n->trans;
    int keep_cap = n->trans_cap;
    *n = src;
    n->trans = keep_trans;      /* the copy starts with no transitions of its own */
    n->trans_cap = keep_cap;
    n->trans_count = 0;
    n->x = src.x + 30;
    n->y = src.y + 40;
    lx_strcpy(n->name, sizeof n->name, name);
    ae_select(l->state_count - 1, -1);
    ae_commit(true, NULL);
}

static void m_breakpoint(UiNode *x) {
    LX_UNUSED(x);
    LxAnimLayer *l = ae_layer();
    if (!l || g_menu_state < 0 || g_menu_state >= l->state_count) return;
    l->states[g_menu_state].breakpoint = !l->states[g_menu_state].breakpoint;
    ae_commit(true, NULL);
}

static void m_delete_trans(UiNode *x) {
    LX_UNUSED(x);
    LxAnimLayer *l = ae_layer();
    if (!l || g_menu_trans < 0) return;
    if (g_menu_state == AE_ANY) {
        if (g_menu_trans >= l->any_count) return;
        memmove(&l->any[g_menu_trans], &l->any[g_menu_trans + 1], sizeof(LxAnimTransition) * (size_t)(l->any_count - g_menu_trans - 1));
        l->any_count--;
    } else {
        if (g_menu_state < 0 || g_menu_state >= l->state_count) return;
        lx_anim_remove_transition(&l->states[g_menu_state], g_menu_trans);
    }
    AE.sel_trans = -1;
    ae_commit(true, NULL);
}

static void m_frame(UiNode *x) { LX_UNUSED(x); ae_frame_all(); }

static void m_new_graph(UiNode *x) {
    LX_UNUSED(x);
    if (AE.obj) ae_new_graph_for(AE.obj);
}

static void canvas_right_click(UiNode *n) {
    LxVec2 m = lx_mouse_pos();
    AE.press_gx = gx_of(m.x);
    AE.press_gy = gy_of(m.y);
    LxAnimGraph *g = ae_graph();
    UiNode *p = ui_popup(ED.ui, m.x, m.y, 210);
    LX_UNUSED(n);
    if (!p) return;
    if (!g) {
        if (AE.obj) ui_menu_item(ED.ui, p, "New Animator Graph", NULL, m_new_graph);
        else ui_menu_item(ED.ui, p, "Select an object with an Animator", NULL, NULL);
        return;
    }
    Hit h = hit_test(m.x, m.y);
    g_menu_state = h.state;
    g_menu_trans = h.trans;
    if (h.kind == H_STATE) {
        LxAnimLayer *l = ae_layer();
        ae_select(h.state, -1);
        ui_menu_item(ED.ui, p, "Set As Layer Default State", NULL, m_set_default);
        ui_menu_item(ED.ui, p, "Duplicate", NULL, m_duplicate_state);
        ui_menu_item(ED.ui, p, l && l->states[h.state].breakpoint ? "Clear Breakpoint" : "Breakpoint (pause Play here)",
                     NULL, m_breakpoint);
        ui_menu_separator(ED.ui, p);
        ui_menu_item(ED.ui, p, "Delete State", "Del", m_delete_state);
        return;
    }
    if (h.kind == H_TRANS) {
        ae_select(h.state, h.trans);
        ui_menu_item(ED.ui, p, "Delete Transition", "Del", m_delete_trans);
        return;
    }
    ui_menu_item(ED.ui, p, "Create State", NULL, m_add_state);
    ui_menu_item(ED.ui, p, "Create Blend Tree State", NULL, m_add_tree);
    ui_menu_separator(ED.ui, p);
    ui_menu_item(ED.ui, p, "Frame All", "F", m_frame);
}

/* finishing a transition drag: on a state it connects, on empty space it does nothing */
static void finish_link(float mx, float my) {
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l) return;
    Hit h = hit_test(mx, my);
    if (h.kind != H_STATE) return;
    const char *to = l->states[h.state].name;
    if (AE.link_from == AE_ANY) {
        LxAnimTransition *t = lx_anim_add_any_transition(l, to);
        if (t) ae_select(AE_ANY, l->any_count - 1);
    } else {
        if (AE.link_from < 0 || AE.link_from >= l->state_count) return;
        LxAnimNode *from = &l->states[AE.link_from];
        LxAnimTransition *t = lx_anim_add_transition(from, to);
        if (t) ae_select(AE.link_from, from->trans_count - 1);
    }
    ae_commit(true, NULL);
}

static void finish_entry(float mx, float my) {
    LxAnimLayer *l = ae_layer();
    if (!l) return;
    Hit h = hit_test(mx, my);
    if (h.kind != H_STATE) return;
    lx_strcpy(l->start, sizeof l->start, l->states[h.state].name);
    ae_commit(true, NULL);
}

static void delete_selection(void) {
    if (AE.sel_trans >= 0) { g_menu_state = AE.sel; g_menu_trans = AE.sel_trans; m_delete_trans(NULL); return; }
    if (AE.sel >= 0) { g_menu_state = AE.sel; m_delete_state(NULL); }
}

/* ======================================================================
 *  canvas input
 * ====================================================================== */
static void canvas_frame(UiNode *n) {
    UiCtx *ui = ED.ui;
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (g && l && !AE.framed && n->cw > 50) {
        AE.framed = true;
        ae_frame_all();
    }
    bool blocked = ui_has_modal(ui) || ui_popup_open(ui);
    bool hot = !blocked && ui_is_inside(ui, n) && (ui_is_hot(ui, n) || ui_active(ui) == n);
    LxVec2 m = lx_mouse_pos();
    bool pressed_l = lx_mouse_pressed(LX_MOUSE_LEFT), pressed_m = lx_mouse_pressed(LX_MOUSE_MIDDLE);
    if ((pressed_l || pressed_m || lx_mouse_pressed(LX_MOUSE_RIGHT)) && !blocked) AE.focused = hot;
    if (!g || !l) return;
    float wheel = lx_mouse_wheel();
    if (hot && wheel != 0 && AE.drag == AE_DRAG_NONE) zoom_at(powf(1.12f, wheel), m.x, m.y);
    if (hot && AE.drag == AE_DRAG_NONE && (pressed_l || pressed_m)) {
        AE.press = AE.last = m;
        AE.press_gx = gx_of(m.x);
        AE.press_gy = gy_of(m.y);
        AE.drag_moved = false;
        bool alt = lx_key_down(LX_KEY_ALT), space = lx_key_down(LX_KEY_SPACE), ctrl = lx_key_down(LX_KEY_CTRL);
        double now = lx_time();
        bool dbl = now - AE.last_click < 0.35 && fabsf(m.x - AE.last_click_pos.x) < 6 && fabsf(m.y - AE.last_click_pos.y) < 6;
        AE.last_click = dbl ? 0 : now;
        AE.last_click_pos = m;
        if (pressed_m || alt || space) AE.drag = AE_DRAG_PAN;
        else {
            Hit h = hit_test(m.x, m.y);
            switch (h.kind) {
            case H_STATE:
                ae_select(h.state, -1);
                /* Ctrl-drag pulls a transition out of a state; a plain drag moves it */
                AE.drag = ctrl ? AE_DRAG_LINK : AE_DRAG_MOVE;
                AE.link_from = h.state;
                break;
            case H_ANY:
                ae_select(AE_ANY, -1);
                AE.drag = AE_DRAG_LINK;
                AE.link_from = AE_ANY;
                break;
            case H_ENTRY:
                AE.drag = AE_DRAG_ENTRY;
                break;
            case H_TRANS:
                ae_select(h.state, h.trans);
                break;
            default:
                if (dbl) { add_state_at(AE.press_gx - AE_STATE_W * 0.5f, AE.press_gy - AE_STATE_H * 0.5f, false); break; }
                ae_select(-1, -1);
                AE.drag = AE_DRAG_PAN;
                break;
            }
        }
    }
    if (AE.drag == AE_DRAG_NONE) return;
    float dx = m.x - AE.last.x, dy = m.y - AE.last.y;
    if (!AE.drag_moved && (fabsf(m.x - AE.press.x) > 4 || fabsf(m.y - AE.press.y) > 4)) AE.drag_moved = true;
    bool down = lx_mouse_down(LX_MOUSE_LEFT) || (AE.drag == AE_DRAG_PAN && lx_mouse_down(LX_MOUSE_MIDDLE));
    if (AE.drag == AE_DRAG_PAN) {
        AE.pan_x -= dx / SC();
        AE.pan_y -= dy / SC();
    } else if (AE.drag == AE_DRAG_MOVE && AE.drag_moved && AE.sel >= 0 && AE.sel < l->state_count) {
        l->states[AE.sel].x += dx / SC();
        l->states[AE.sel].y += dy / SC();
    }
    AE.last = m;
    if (!down) {
        if (AE.drag == AE_DRAG_LINK && AE.drag_moved) finish_link(m.x, m.y);
        if (AE.drag == AE_DRAG_ENTRY && AE.drag_moved) finish_entry(m.x, m.y);
        if (AE.drag == AE_DRAG_MOVE && AE.drag_moved) ae_commit(false, &l->states[AE.sel < l->state_count ? AE.sel : 0]);
        AE.drag = AE_DRAG_NONE;
        AE.drag_moved = false;
    }
}

static void canvas_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(n);
    LX_UNUSED(type);
    if (!payload || !payload[0]) return;
    /* an .lxanim dropped here is opened; a clip dropped on empty space becomes a state that plays it */
    size_t len = strlen(payload);
    if (len > 7 && !strcmp(payload + len - 7, ".lxanim")) {
        set_target(payload, AE.obj);
        return;
    }
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l) return;
    LxVec2 m = lx_mouse_pos();
    const char *hash = strrchr(payload, '#');
    LxAnimNode *st = lx_anim_add_state(g, l, hash && hash[1] ? hash + 1 : "New State",
                                       gx_of(m.x) - AE_STATE_W * 0.5f, gy_of(m.y) - AE_STATE_H * 0.5f);
    if (!st) return;
    lx_strcpy(st->clip, sizeof st->clip, payload);
    ae_select(l->state_count - 1, -1);
    ae_commit(true, NULL);
}

/* ======================================================================
 *  toolbar
 * ====================================================================== */
static UiNode *tbtn(UiNode *row, const char *text, UiIcon icon, const char *tip, void (*cb)(UiNode *)) {
    UiNode *b = ui_button(ED.ui, row, text, cb);
    b->h = 24;
    b->icon = icon;
    b->center_text = false;
    if (tip) lx_strcpy(b->tooltip, sizeof b->tooltip, tip);
    return b;
}

static void pick_object(UiNode *n) {
    LxEntity e = (LxEntity)n->tag;
    if (!ED.scene || !lx_entity_valid(ED.scene, e)) return;
    AE.locked = false;   /* following an object again */
    LxEntity a = lx_find_animator(ED.scene, e);
    LxAnimator *an = a ? lx_get_animator(ED.scene, a) : NULL;
    set_target(an && an->graph[0] ? an->graph : "", e);
    ed_select(e);
}

static void tb_object(UiNode *btn) {
    if (!ED.scene) return;
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y + btn->ch, 230);
    if (!p) return;
    int shown = 0;
    for (int i = 0; i < lx_scene_entity_count(ED.scene) && shown < 60; i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        if (!(lx_entity_components(ED.scene, e) & LX_COMP_ANIMATOR)) continue;
        LxAnimator *an = lx_get_animator(ED.scene, e);
        UiNode *it = ui_menu_item(ED.ui, p, lx_entity_name(ED.scene, e), an && an->graph[0] ? "graph" : NULL, pick_object);
        it->tag = (intptr_t)e;
        it->icon = UI_ICON_ANIMATOR;
        shown++;
    }
    if (!shown) { UiNode *it = ui_menu_item(ED.ui, p, "No objects have an Animator yet", NULL, NULL); it->disabled = true; }
}

static void graph_chosen(void *user, const char *value) {
    LX_UNUSED(user);
    if (!value) return;
    if (AE.obj && ED.scene && lx_entity_valid(ED.scene, AE.obj)) {
        LxEntity a = lx_find_animator(ED.scene, AE.obj);
        LxAnimator *an = a ? lx_get_animator(ED.scene, a) : NULL;
        if (an) lx_strcpy(an->graph, sizeof an->graph, value);
        ED.dirty = true;
    }
    set_target(value, AE.obj);
}

static void tb_graph(UiNode *btn) {
    LX_UNUSED(btn);
    ed_picker_open("animator", AE.ref, NULL, NULL, graph_chosen, NULL);
}

static void pick_layer(UiNode *n) {
    AE.layer = (int)n->tag;
    AE.sel = -1;
    AE.sel_trans = -1;
    AE.framed = false;
    AE.insp_dirty = true;
}

static void tb_layer(UiNode *btn) {
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y + btn->ch, 200);
    if (!p) return;
    for (int i = 0; i < g->layer_count; i++) {
        char sub[48];
        snprintf(sub, sizeof sub, "weight %g", (double)g->layers[i].weight);
        UiNode *it = ui_menu_item(ED.ui, p, g->layers[i].name, i ? sub : "base", pick_layer);
        it->tag = i;
        it->selected = i == AE.layer;
    }
}

static void tb_zoom(UiNode *btn) {
    LX_UNUSED(btn);
    AE.zoom = 1;
}

static void tb_problems(UiNode *btn) {
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    char lines[12][160];
    int n = lx_anim_graph_problems(g, lines, 12);
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y - 10, 420);
    if (!p) return;
    if (!n) { ui_menu_item(ED.ui, p, "No problems found", NULL, NULL); return; }
    for (int i = 0; i < n && i < 12; i++) ui_menu_item(ED.ui, p, lines[i], NULL, NULL);
    if (n > 12) {
        char more[64];
        snprintf(more, sizeof more, "and %d more", n - 12);
        ui_menu_item(ED.ui, p, more, NULL, NULL);
    }
}

/* ======================================================================
 *  the panel
 * ====================================================================== */
/* follow the selected object, unless the view is locked on something */
static void sync_target(void) {
    if (AE.locked || !ED.scene) return;
    LxEntity sel = ED.selected;
    if (!sel || !lx_entity_valid(ED.scene, sel)) return;
    LxEntity a = lx_find_animator(ED.scene, sel);
    if (!a) return;
    if (sel == AE.obj) return;
    LxAnimator *an = lx_get_animator(ED.scene, a);
    set_target(an && an->graph[0] ? an->graph : "", sel);
}

static void panel_frame(UiNode *n) {
    LX_UNUSED(n);
    sync_target();
    LxAnimGraph *g = ae_graph();
    if (g && g->version != AE.seen_version) {   /* changed elsewhere: an action, another editor, a reload */
        AE.seen_version = g->version;
        AE.insp_dirty = true;
        lx_free(AE.last_text);
        AE.last_text = lx_anim_graph_to_text(g);
        LxAnimLayer *l = ae_layer();
        if (AE.sel >= 0 && (!l || AE.sel >= l->state_count)) { AE.sel = -1; AE.sel_trans = -1; }
    }
    char b[160];
    if (AE.obj_btn) {
        if (AE.obj && ED.scene && lx_entity_valid(ED.scene, AE.obj)) snprintf(b, sizeof b, "%s", lx_entity_name(ED.scene, AE.obj));
        else snprintf(b, sizeof b, AE.ref[0] ? "(graph asset)" : "No object");
        ui_set_text(AE.obj_btn, "%s", b);
    }
    if (AE.graph_btn) ui_set_text(AE.graph_btn, "%s", g ? g->name : AE.ref[0] ? "(missing)" : "No graph");
    if (AE.layer_btn) {
        LxAnimLayer *l = ae_layer();
        ui_set_text(AE.layer_btn, "%s", l ? l->name : "No layers");
    }
    if (AE.zoom_btn) ui_set_text(AE.zoom_btn, "%d%%", (int)(AE.zoom * 100 + 0.5f));
    if (AE.state_lbl) {
        int live = ae_live_state(AE.layer);
        LxAnimLayer *l = ae_layer();
        const LxAnimNode *st = l && live >= 0 ? &l->states[live] : ae_sel_state();
        ui_set_text(AE.state_lbl, "%s", st ? st->name : "");
        AE.state_lbl->fg = live >= 0 ? COL_RUN : COL_TEXT;
    }
    if (AE.problems_btn) {
        int n_prob = g ? lx_anim_graph_problems(g, NULL, 0) : 0;
        ui_set_text(AE.problems_btn, n_prob ? "%d problems" : "No problems", n_prob);
        AE.problems_btn->fg = n_prob ? UI_HEX(0xffb03a) : COL_DIM;
    }
    ae_inspector_frame();
}

UiNode *ed_animator_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    UiNode *p = ui_box(ui, parent, UI_COL);
    lx_strcpy(p->text, sizeof p->text, "Animator");
    p->icon = UI_ICON_ANIMATOR;
    p->bg = UI_HEX(0x1f2023);
    p->on_frame = panel_frame;
    AE.panel = p;
    ui_track(&AE.panel);
    UiNode *tb = ui_box(ui, p, UI_ROW);
    tb->h = 32;
    tb->gap = 6;
    tb->cross_align = UI_ALIGN_CENTER;
    ui_pad(tb, 6, 0, 6, 0);
    tb->bg = UI_HEX(0x232428);
    AE.obj_btn = tbtn(tb, "No object", UI_ICON_ENTITY, "Objects with an Animator", tb_object);
    AE.obj_btn->w = 170;
    AE.graph_btn = tbtn(tb, "No graph", UI_ICON_ANIMATOR, "Which animator graph this object uses", tb_graph);
    AE.graph_btn->w = 160;
    AE.layer_btn = tbtn(tb, "Base Layer", UI_ICON_LIST, "Which layer's state machine to show", tb_layer);
    AE.layer_btn->w = 130;
    UiNode *sl = ui_label(ui, tb, "State:");
    sl->font = UI_FONT_SMALL;
    sl->fg = COL_DIM;
    AE.state_lbl = ui_label(ui, tb, "");
    AE.state_lbl->font = UI_FONT_BOLD;
    AE.state_lbl->grow = 1;
    tbtn(tb, "Frame", UI_ICON_FOCUS, "Fit the layer in the view (F)", m_frame)->w = 70;
    AE.zoom_btn = tbtn(tb, "100%", UI_ICON_SEARCH, "Zoom (scroll in the view); click for 100%", tb_zoom);
    AE.zoom_btn->w = 72;
    UiNode *split = ui_split(ui, p, UI_ROW, 0.68f);
    split->split_min = 300;
    split->grow = 1;
    UiNode *c = ui_new(ui, split, UI_CUSTOM);
    c->grow = 1;
    c->on_frame = canvas_frame;
    c->on_draw = canvas_draw;
    c->on_right_click = canvas_right_click;
    c->accept = "animator,model";
    c->on_drop = canvas_drop;
    AE.canvas = c;
    ui_track(&AE.canvas);
    ae_inspector(split);
    UiNode *bb = ui_box(ui, p, UI_ROW);
    bb->h = 28;
    bb->gap = 6;
    bb->cross_align = UI_ALIGN_CENTER;
    ui_pad(bb, 6, 0, 6, 0);
    bb->bg = UI_HEX(0x232428);
    UiNode *tip = ui_label(ui, bb, "Ctrl-drag a state to make a transition. Double-click empty space for a new state.");
    tip->font = UI_FONT_SMALL;
    tip->fg = COL_DIM;
    tip->grow = 1;
    AE.problems_btn = tbtn(bb, "No problems", UI_ICON_WARNING, "What is wrong with this animator graph", tb_problems);
    AE.problems_btn->w = 130;
    return p;
}

/* ======================================================================
 *  opening, keys
 * ====================================================================== */
void ed_animator_open_asset(const char *ref) {
    set_target(ref, 0);
    /* asked for by name: hold on to it, or the next object selected in the scene would take the view away */
    AE.locked = true;
    if (ED.view_tabs) ED.view_tabs->tab_index = 2;
}

void ae_new_graph_for(LxEntity e) {
    if (!ED.scene || !lx_entity_valid(ED.scene, e)) return;
    LxEntity a = lx_find_animator(ED.scene, e);
    LxAnimator *an = a ? lx_get_animator(ED.scene, a) : NULL;
    if (!an) { ui_notify(ED.ui, "%s has no Animator component", lx_entity_name(ED.scene, e)); return; }
    char ref[LX_REF_MAX];
    snprintf(ref, sizeof ref, "Animators/%s.lxanim", lx_entity_name(ED.scene, e));
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    LxAnimGraph *g = lx_anim_graph_new(lx_entity_name(ED.scene, e));
    if (!lx_anim_graph_save(g, path)) {
        lx_anim_graph_free(g);
        ui_notify(ED.ui, "Couldn't create %s", ref);
        return;
    }
    lx_anim_graph_replace(ref, g);
    lx_strcpy(an->graph, sizeof an->graph, ref);
    ED.dirty = true;
    set_target(ref, e);
    if (ED.view_tabs) ED.view_tabs->tab_index = 2;
}

/* ---- helpers the rest of the editor (and the smoke test) can reach ---- */
bool ed_animator_add_state(const char *name) {
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l) return false;
    float x = 140 + (float)(l->state_count % 3) * 230, y = 120 + (float)(l->state_count / 3) * 110;
    LxAnimNode *n = lx_anim_add_state(g, l, name, x, y);
    if (!n) return false;
    ae_select(l->state_count - 1, -1);
    ae_commit(true, NULL);
    return true;
}

bool ed_animator_select_state(const char *name) {
    LxAnimLayer *l = ae_layer();
    int i = lx_anim_state_index(l, name);
    if (i < 0) return false;
    ae_select(i, -1);
    return true;
}

bool ed_animator_add_transition(const char *from, const char *to) {
    LxAnimLayer *l = ae_layer();
    int fi = lx_anim_state_index(l, from);
    if (!l || fi < 0 || !lx_anim_find_state(l, to)) return false;
    LxAnimTransition *t = lx_anim_add_transition(&l->states[fi], to);
    if (!t) return false;
    ae_select(fi, l->states[fi].trans_count - 1);
    ae_commit(true, NULL);
    return true;
}

bool ed_animator_has_focus(void) {
    return AE.focused && AE.panel && !AE.panel->hidden && ED.view_tabs && ED.view_tabs->tab_index == 2 && !ui_wants_keyboard(ED.ui);
}

bool ed_animator_keys(void) {
    if (!ed_animator_has_focus()) return false;
    bool ctrl = lx_key_down(LX_KEY_CTRL), shift = lx_key_down(LX_KEY_SHIFT);
    if (ctrl) {
        if (lx_key_pressed(LX_KEY_Z)) { if (shift) ae_redo(); else ae_undo(); return true; }
        if (lx_key_pressed(LX_KEY_Y)) { ae_redo(); return true; }
        return false;
    }
    if (!ae_graph()) return true;
    if (lx_key_pressed(LX_KEY_DELETE) || lx_key_pressed(LX_KEY_BACKSPACE)) delete_selection();
    if (lx_key_pressed(LX_KEY_F)) ae_frame_all();
    if (lx_key_pressed(LX_KEY_ESCAPE)) ae_select(-1, -1);
    return true;
}
