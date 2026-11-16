/* Animator editor, right-hand side: the Parameters / State / Transition / Layers inspector, and the
 * .lxanim asset Inspector.
 *
 * Widgets edit copies in a binding pool rather than graph memory, which moves as the graph grows: each
 * binding remembers where its value belongs (layer, state, transition, condition, child) and writes it
 * back by looking that up again. The alternative - a pointer into a realloc'd array - is a crash
 * waiting for someone to add a state while a field has focus. */
#include "anim_editor.h"
#include <math.h>

#define COL_TEXT UI_HEX(0xdfe1e6)
#define COL_DIM UI_HEX(0x8d919a)
#define COL_FIELD UI_HEX(0x17181b)
#define COL_EDGE UI_HEX(0x34363c)
#define COL_ACCENT UI_HEX(0x3d7cf5)
#define COL_RUN UI_HEX(0xffb03a)
#define COL_PARAM UI_HEX(0xe2c8ff)

/* ======================================================================
 *  bindings
 * ====================================================================== */
enum { AB_PARAM = 1, AB_STATE, AB_TRANS, AB_COND, AB_CHILD, AB_LAYER, AB_TREE };
enum {
    F_NAME, F_CLIP, F_SPEED, F_SPEED_PARAM, F_LOOP, F_MIRROR, F_CYCLE, F_FOOTIK, F_TAG, F_MOTION, F_BREAK,
    F_TO, F_HAS_EXIT, F_EXIT, F_DUR, F_FIXED, F_OFFSET, F_INTERRUPT, F_TOSELF, F_MUTE, F_SOLO,
    F_CMODE, F_CVALUE,
    F_THRESH, F_PX, F_PY,
    F_WEIGHT, F_BLEND, F_MASK, F_START,
    F_TYPE, F_VALUE
};

typedef struct AB {
    int kind, field;
    int layer, state, trans, cond, child, param, tree;
    float f;
    int i;
    bool b;
    char s[LX_REF_MAX];
    const char *opts[48];
    int opt_count;
    char optbuf[1800];
} AB;

#define AB_MAX 420
static AB g_ab[AB_MAX];
static int g_ab_n;
static AB g_ab_spare;

static AB *ab_new(int kind, int field) {
    AB *b = g_ab_n < AB_MAX ? &g_ab[g_ab_n++] : &g_ab_spare;
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->field = field;
    b->layer = AE.layer;
    b->state = AE.sel;
    b->trans = AE.sel_trans;
    b->tree = -1;
    return b;
}

/* resolve a binding's target, freshly, every time it is used */
static LxAnimLayer *ab_layer(const AB *b) {
    LxAnimGraph *g = ae_graph();
    return g && b->layer >= 0 && b->layer < g->layer_count ? &g->layers[b->layer] : NULL;
}
static LxAnimNode *ab_state(const AB *b) {
    LxAnimLayer *l = ab_layer(b);
    return l && b->state >= 0 && b->state < l->state_count ? &l->states[b->state] : NULL;
}
static LxAnimTransition *ab_trans(const AB *b) {
    LxAnimLayer *l = ab_layer(b);
    if (!l || b->trans < 0) return NULL;
    if (b->state == AE_ANY) return b->trans < l->any_count ? &l->any[b->trans] : NULL;
    LxAnimNode *n = ab_state(b);
    return n && b->trans < n->trans_count ? &n->trans[b->trans] : NULL;
}
static LxAnimTree *ab_tree(const AB *b) {
    LxAnimGraph *g = ae_graph();
    int idx = b->tree;
    if (idx < 0) {
        LxAnimNode *n = ab_state(b);
        idx = n ? n->tree : -1;
    }
    return g && idx >= 0 && idx < g->tree_count ? &g->trees[idx] : NULL;
}

static void ab_apply(UiNode *node) {
    AB *b = node->user;
    LxAnimGraph *g = ae_graph();
    if (!b || !g) return;
    bool structural = false;
    switch (b->kind) {
    case AB_PARAM: {
        if (b->param < 0 || b->param >= g->param_count) return;
        LxAnimParam *p = &g->params[b->param];
        if (b->field == F_NAME) { if (!lx_anim_rename_param(g, b->param, b->s)) { lx_strcpy(b->s, sizeof b->s, p->name); return; } structural = true; }
        else if (b->field == F_TYPE) { p->type = (LxAnimParamType)b->i; structural = true; }
        else if (b->field == F_VALUE) {
            if (p->type == LX_AP_INT) p->i = b->i;
            else if (p->type == LX_AP_FLOAT) p->f = b->f;
            else p->b = b->b;
        }
        break;
    }
    case AB_STATE: {
        LxAnimNode *n = ab_state(b);
        LxAnimLayer *l = ab_layer(b);
        if (!n || !l) return;
        switch (b->field) {
        case F_NAME: if (!lx_anim_rename_state(l, b->state, b->s)) { lx_strcpy(b->s, sizeof b->s, n->name); return; } structural = true; break;
        case F_CLIP: lx_strcpy(n->clip, sizeof n->clip, b->s); break;
        case F_SPEED: n->speed = b->f; break;
        case F_SPEED_PARAM: lx_strcpy(n->speed_param, sizeof n->speed_param, b->i > 0 ? g->params[b->i - 1].name : ""); break;
        case F_LOOP: n->loop = b->b; break;
        case F_MIRROR: n->mirror = b->b; break;
        case F_CYCLE: n->cycle_offset = b->f; break;
        case F_FOOTIK: n->foot_ik = b->b; break;
        case F_TAG: lx_strcpy(n->tag, sizeof n->tag, b->s); break;
        case F_BREAK: n->breakpoint = b->b; break;
        case F_MOTION:
            n->motion = b->i == 1 ? LX_AM_TREE : LX_AM_CLIP;
            if (n->motion == LX_AM_TREE && n->tree < 0) n->tree = lx_anim_add_tree(g, LX_AT_1D);
            structural = true;
            break;
        default: break;
        }
        break;
    }
    case AB_TRANS: {
        LxAnimTransition *t = ab_trans(b);
        LxAnimLayer *l = ab_layer(b);
        if (!t || !l) return;
        switch (b->field) {
        case F_TO: lx_strcpy(t->to, sizeof t->to, b->i > 0 && b->i - 1 < l->state_count ? l->states[b->i - 1].name : ""); structural = true; break;
        case F_HAS_EXIT: t->has_exit_time = b->b; structural = true; break;
        case F_EXIT: t->exit_time = b->f; break;
        case F_DUR: t->duration = lx_maxf(0, b->f); break;
        case F_FIXED: t->fixed_duration = b->b; break;
        case F_OFFSET: t->offset = b->f; break;
        case F_INTERRUPT: t->interrupt = (LxAnimInterrupt)b->i; break;
        case F_TOSELF: t->to_self = b->b; break;
        case F_MUTE: t->mute = b->b; break;
        case F_SOLO: t->solo = b->b; break;
        default: break;
        }
        break;
    }
    case AB_COND: {
        LxAnimTransition *t = ab_trans(b);
        if (!t || b->cond < 0 || b->cond >= t->cond_count) return;
        LxAnimCond *c = &t->cond[b->cond];
        if (b->field == F_NAME) { lx_strcpy(c->param, sizeof c->param, b->i > 0 ? g->params[b->i - 1].name : ""); structural = true; }
        else if (b->field == F_CMODE) { c->mode = (LxAnimCondMode)b->i; structural = true; }
        else if (b->field == F_CVALUE) c->value = b->f;
        break;
    }
    case AB_CHILD: {
        LxAnimTree *t = ab_tree(b);
        if (!t || b->child < 0 || b->child >= t->child_count) return;
        LxAnimChild *c = &t->child[b->child];
        switch (b->field) {
        case F_CLIP: lx_strcpy(c->clip, sizeof c->clip, b->s); break;
        case F_THRESH: c->threshold = b->f; break;
        case F_PX: c->px = b->f; break;
        case F_PY: c->py = b->f; break;
        case F_SPEED: c->speed = b->f; break;
        case F_MIRROR: c->mirror = b->b; break;
        case F_NAME: lx_strcpy(c->weight_param, sizeof c->weight_param, b->i > 0 ? g->params[b->i - 1].name : ""); break;
        default: break;
        }
        break;
    }
    case AB_TREE: {
        LxAnimTree *t = ab_tree(b);
        if (!t) return;
        if (b->field == F_TYPE) { t->type = (LxAnimTreeType)b->i; structural = true; }
        else if (b->field == F_PX) lx_strcpy(t->param_x, sizeof t->param_x, b->i > 0 ? g->params[b->i - 1].name : "");
        else if (b->field == F_PY) lx_strcpy(t->param_y, sizeof t->param_y, b->i > 0 ? g->params[b->i - 1].name : "");
        break;
    }
    case AB_LAYER: {
        LxAnimLayer *l = ab_layer(b);
        if (!l) return;
        switch (b->field) {
        case F_NAME: lx_strcpy(l->name, sizeof l->name, b->s); structural = true; break;
        case F_WEIGHT: l->weight = lx_clampf(b->f, 0, 1); break;
        case F_BLEND: l->blend = (LxAnimBlendMode)b->i; break;
        case F_START: lx_strcpy(l->start, sizeof l->start, b->i > 0 && b->i - 1 < l->state_count ? l->states[b->i - 1].name : ""); structural = true; break;
        case F_MASK:
            if (b->child >= 0 && b->child < 64) {
                if (b->b) l->mask |= 1ull << b->child;
                else l->mask &= ~(1ull << b->child);
            }
            break;
        default: break;
        }
        break;
    }
    default: break;
    }
    ae_commit(structural, node);
}

/* ======================================================================
 *  small widgets
 * ====================================================================== */
static UiNode *label(UiNode *parent, const char *text, UiFont f, uint32_t col) {
    UiNode *l = ui_label(ED.ui, parent, text);
    l->font = f;
    l->fg = col;
    return l;
}
static UiNode *heading(UiNode *parent, const char *text) {
    UiNode *l = label(parent, text, UI_FONT_BOLD, COL_TEXT);
    l->h = 26;
    ui_pad(l, 0, 6, 0, 0);
    return l;
}
static UiNode *hint(UiNode *parent, const char *text) {
    UiNode *l = label(parent, text, UI_FONT_SMALL, COL_DIM);
    l->wrap = true;
    return l;
}
static UiNode *prow(UiNode *parent, const char *name, const char *tip) {
    UiNode *row = ui_box(ED.ui, parent, UI_ROW);
    row->h = 24;
    row->gap = 4;
    row->cross_align = UI_ALIGN_CENTER;
    UiNode *l = label(row, name, UI_FONT_BODY, COL_DIM);
    l->w = 116;
    lx_strcpy(l->tooltip, sizeof l->tooltip, tip && tip[0] ? tip : name);
    return row;
}
static UiNode *small_btn(UiNode *row, UiIcon icon, const char *tip, void (*cb)(UiNode *)) {
    UiNode *b = ui_icon_button(ED.ui, row, icon, tip, cb);
    b->w = 22;
    b->h = 22;
    return b;
}

static UiNode *num_field(UiNode *row, AB *b, float value, float step) {
    b->f = value;
    UiNode *n = ui_number(ED.ui, row, &b->f, step);
    n->grow = 1;
    n->h = 22;
    n->user = b;
    n->on_change = ab_apply;
    return n;
}
static UiNode *check_field(UiNode *row, AB *b, bool value, const char *text) {
    b->b = value;
    UiNode *n = ui_checkbox(ED.ui, row, text ? text : "", &b->b);
    n->user = b;
    n->on_change = ab_apply;
    n->h = 22;
    return n;
}
static UiNode *text_field(UiNode *row, AB *b, const char *value) {
    lx_strcpy(b->s, sizeof b->s, value ? value : "");
    UiNode *n = ui_textfield(ED.ui, row, b->s, (int)sizeof b->s);
    n->grow = 1;
    n->h = 22;
    n->user = b;
    n->on_change = ab_apply;
    return n;
}
static UiNode *enum_field(UiNode *row, AB *b, int value, const char *const *opts, int count) {
    b->i = value;
    UiNode *n = ui_dropdown(ED.ui, row, &b->i, opts, count);
    n->grow = 1;
    n->h = 22;
    n->user = b;
    n->on_change = ab_apply;
    return n;
}

/* a dropdown of the graph's parameters, with "(none)" first; the value is index + 1 */
/* Every parameter is listed, in order, so option index - 1 is the parameter's index: that is the whole
 * contract ab_apply relies on, and filtering the list would break it. */
static UiNode *param_field(UiNode *row, AB *b, const char *current) {
    LxAnimGraph *g = ae_graph();
    size_t used = 0;
    b->opt_count = 0;
    b->opts[b->opt_count++] = "(none)";
    int sel = 0;
    for (int i = 0; g && i < g->param_count && b->opt_count < 47; i++) {
        size_t len = strlen(g->params[i].name) + 1;
        if (used + len >= sizeof b->optbuf) break;
        memcpy(b->optbuf + used, g->params[i].name, len);
        b->opts[b->opt_count] = b->optbuf + used;
        if (!strcmp(g->params[i].name, current ? current : "")) sel = b->opt_count;
        b->opt_count++;
        used += len;
    }
    return enum_field(row, b, sel, b->opts, b->opt_count);
}

/* a dropdown of the layer's states, "(none)" first */
static UiNode *state_field(UiNode *row, AB *b, const char *current) {
    LxAnimLayer *l = ae_layer();
    size_t used = 0;
    b->opt_count = 0;
    b->opts[b->opt_count++] = "(none)";
    int sel = 0;
    for (int i = 0; l && i < l->state_count && b->opt_count < 47; i++) {
        size_t len = strlen(l->states[i].name) + 1;
        if (used + len >= sizeof b->optbuf) break;
        memcpy(b->optbuf + used, l->states[i].name, len);
        b->opts[b->opt_count] = b->optbuf + used;
        if (!strcmp(l->states[i].name, current ? current : "")) sel = b->opt_count;
        b->opt_count++;
        used += len;
    }
    return enum_field(row, b, sel, b->opts, b->opt_count);
}

/* ======================================================================
 *  the clip picker
 * ====================================================================== */
static struct { int kind, layer, state, child; } g_pick;

static void clip_chosen(void *user, const char *value) {
    LX_UNUSED(user);
    LxAnimGraph *g = ae_graph();
    if (!g || !value) return;
    if (g_pick.kind == AB_STATE) {
        if (g_pick.layer < 0 || g_pick.layer >= g->layer_count) return;
        LxAnimLayer *l = &g->layers[g_pick.layer];
        if (g_pick.state < 0 || g_pick.state >= l->state_count) return;
        lx_strcpy(l->states[g_pick.state].clip, sizeof l->states[g_pick.state].clip, value);
    } else if (g_pick.kind == AB_CHILD) {
        if (g_pick.layer < 0 || g_pick.layer >= g->layer_count) return;
        LxAnimLayer *l = &g->layers[g_pick.layer];
        if (g_pick.state < 0 || g_pick.state >= l->state_count) return;
        int ti = l->states[g_pick.state].tree;
        if (ti < 0 || ti >= g->tree_count) return;
        LxAnimTree *t = &g->trees[ti];
        if (g_pick.child < 0 || g_pick.child >= t->child_count) return;
        lx_strcpy(t->child[g_pick.child].clip, sizeof t->child[g_pick.child].clip, value);
    }
    ae_commit(true, NULL);
}

static void pick_state_clip(UiNode *n) {
    LxAnimNode *st = ae_sel_state();
    g_pick.kind = AB_STATE;
    g_pick.layer = AE.layer;
    g_pick.state = AE.sel;
    g_pick.child = -1;
    ed_picker_open("animation", st ? st->clip : "", NULL, n, clip_chosen, NULL);
}

static void pick_child_clip(UiNode *n) {
    g_pick.kind = AB_CHILD;
    g_pick.layer = AE.layer;
    g_pick.state = AE.sel;
    g_pick.child = (int)n->tag;
    LxAnimTree *t = NULL;
    LxAnimGraph *g = ae_graph();
    LxAnimNode *st = ae_sel_state();
    if (g && st && st->tree >= 0 && st->tree < g->tree_count) t = &g->trees[st->tree];
    ed_picker_open("animation", t && g_pick.child < t->child_count ? t->child[g_pick.child].clip : "", NULL, n, clip_chosen, NULL);
}

static UiNode *clip_button(UiNode *row, const char *clip, void (*cb)(UiNode *)) {
    const char *shown = clip && clip[0] ? clip : "(no clip)";
    UiNode *b = ui_button(ED.ui, row, shown, cb);
    b->grow = 1;
    b->h = 22;
    b->bg = COL_FIELD;
    b->border = COL_EDGE;
    b->radius = 3;
    b->center_text = false;
    b->icon = UI_ICON_ANIMATOR;
    if (clip && clip[0]) lx_strcpy(b->tooltip, sizeof b->tooltip, clip);
    return b;
}

/* ======================================================================
 *  Parameters tab
 * ====================================================================== */
static int g_add_param_type;

static void add_param(UiNode *x) {
    LX_UNUSED(x);
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    static const char *const base[LX_AP_TYPE_COUNT] = {"New Float", "New Int", "New Bool", "New Trigger"};
    lx_anim_add_param(g, base[g_add_param_type & 3], (LxAnimParamType)(g_add_param_type & 3));
    ae_commit(true, NULL);
}

static void delete_param(UiNode *n) {
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    int i = (int)n->tag;
    if (lx_anim_param_uses(g, i >= 0 && i < g->param_count ? g->params[i].name : "") > 0)
        ui_notify(ED.ui, "That parameter is still used by a transition or a blend tree");
    lx_anim_remove_param(g, i);
    ae_commit(true, NULL);
}

static void build_params(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxAnimGraph *g = ae_graph();
    if (!g) { hint(body, "No animator graph."); return; }
    hint(body, "Parameters are what a State Graph sets and what transitions test. A trigger is a bool that one transition consumes.");
    LxEntity live = ED.play != ED_EDIT && AE.obj && ED.scene && lx_entity_valid(ED.scene, AE.obj) ? lx_find_animator(ED.scene, AE.obj) : 0;
    static const char *const types[LX_AP_TYPE_COUNT] = {"Float", "Int", "Bool", "Trigger"};
    for (int i = 0; i < g->param_count; i++) {
        LxAnimParam *p = &g->params[i];
        UiNode *card = ui_box(ui, body, UI_COL);
        card->bg = UI_HEX(0x222327);
        card->radius = 4;
        ui_pad(card, 8, 6, 8, 6);
        card->gap = 3;
        UiNode *top = ui_box(ui, card, UI_ROW);
        top->h = 24;
        top->gap = 4;
        top->cross_align = UI_ALIGN_CENTER;
        AB *nb = ab_new(AB_PARAM, F_NAME);
        nb->param = i;
        UiNode *tf = text_field(top, nb, p->name);
        tf->font = UI_FONT_BOLD;
        tf->fg = COL_PARAM;
        AB *tb = ab_new(AB_PARAM, F_TYPE);
        tb->param = i;
        UiNode *dd = enum_field(top, tb, (int)p->type, types, LX_AP_TYPE_COUNT);
        dd->grow = 0;
        dd->w = 86;
        UiNode *del = small_btn(top, UI_ICON_TRASH, "Delete this parameter", delete_param);
        del->tag = i;
        UiNode *row = prow(card, p->type == LX_AP_TRIGGER ? "Starts Set" : "Starting Value", "the value an animator starts with");
        AB *vb = ab_new(AB_PARAM, F_VALUE);
        vb->param = i;
        if (p->type == LX_AP_FLOAT) num_field(row, vb, p->f, 0.05f);
        else if (p->type == LX_AP_INT) { vb->i = p->i; UiNode *nn = ui_int(ui, row, &vb->i, -1000000, 1000000); nn->grow = 1; nn->h = 22; nn->user = vb; nn->on_change = ab_apply; }
        else check_field(row, vb, p->b, NULL);
        if (live) {
            /* while the game runs, what the animator actually holds */
            char val[80];
            if (p->type == LX_AP_FLOAT) snprintf(val, sizeof val, "now %.3f", (double)lx_animator_get_float(ED.scene, live, p->name));
            else if (p->type == LX_AP_INT) snprintf(val, sizeof val, "now %d", lx_animator_get_int(ED.scene, live, p->name));
            else snprintf(val, sizeof val, "now %s", lx_animator_get_bool(ED.scene, live, p->name) ? "set" : "not set");
            label(card, val, UI_FONT_SMALL, COL_RUN);
        }
    }
    UiNode *add = ui_box(ui, body, UI_ROW);
    add->h = 28;
    add->gap = 4;
    add->cross_align = UI_ALIGN_CENTER;
    ui_pad(add, 0, 8, 0, 0);
    UiNode *dd = ui_dropdown(ui, add, &g_add_param_type, types, LX_AP_TYPE_COUNT);
    dd->w = 100;
    dd->h = 24;
    UiNode *b = ui_button(ui, add, "Add Parameter", add_param);
    b->icon = UI_ICON_PLUS;
    b->h = 24;
    b->grow = 1;
}

/* ======================================================================
 *  State tab
 * ====================================================================== */
static void add_child(UiNode *x) {
    LX_UNUSED(x);
    LxAnimGraph *g = ae_graph();
    LxAnimNode *st = ae_sel_state();
    if (!g || !st) return;
    if (st->tree < 0 || st->tree >= g->tree_count) st->tree = lx_anim_add_tree(g, LX_AT_1D);
    LxAnimTree *t = &g->trees[st->tree];
    if (t->child_count >= LX_ANIM_MAX_CHILDREN) { ui_notify(ED.ui, "A blend tree holds at most %d motions", LX_ANIM_MAX_CHILDREN); return; }
    LxAnimChild *c = &t->child[t->child_count++];
    memset(c, 0, sizeof *c);
    c->tree = -1;
    c->speed = 1;
    /* spread the thresholds out, so a new motion lands somewhere usable rather than on top of another */
    float hi = 0;
    for (int i = 0; i + 1 < t->child_count; i++) if (t->child[i].threshold > hi) hi = t->child[i].threshold;
    c->threshold = t->child_count > 1 ? hi + 1 : 0;
    ae_commit(true, NULL);
}

static void delete_child(UiNode *n) {
    LxAnimGraph *g = ae_graph();
    LxAnimNode *st = ae_sel_state();
    if (!g || !st || st->tree < 0 || st->tree >= g->tree_count) return;
    LxAnimTree *t = &g->trees[st->tree];
    int i = (int)n->tag;
    if (i < 0 || i >= t->child_count) return;
    memmove(&t->child[i], &t->child[i + 1], sizeof(LxAnimChild) * (size_t)(t->child_count - i - 1));
    t->child_count--;
    ae_commit(true, NULL);
}

static void build_blend_tree(UiNode *body, LxAnimGraph *g, LxAnimNode *st) {
    UiCtx *ui = ED.ui;
    static const char *const kinds[LX_AT_TYPE_COUNT] = {"1D", "2D Directional", "2D Cartesian", "Direct"};
    if (st->tree < 0 || st->tree >= g->tree_count) { hint(body, "This state has no blend tree yet - add a motion to make one."); }
    LxAnimTree *t = st->tree >= 0 && st->tree < g->tree_count ? &g->trees[st->tree] : NULL;
    heading(body, "Blend Tree");
    if (t) {
        UiNode *row = prow(body, "Blend Type", "how the motions are laid out");
        AB *b = ab_new(AB_TREE, F_TYPE);
        b->tree = st->tree;
        enum_field(row, b, (int)t->type, kinds, LX_AT_TYPE_COUNT);
        if (t->type != LX_AT_DIRECT) {
            row = prow(body, "Parameter", "the parameter the blend follows");
            AB *px = ab_new(AB_TREE, F_PX);
            px->tree = st->tree;
            param_field(row, px, t->param_x);
        }
        if (t->type == LX_AT_2D_DIRECTIONAL || t->type == LX_AT_2D_CARTESIAN) {
            row = prow(body, "Parameter Y", "the second axis");
            AB *py = ab_new(AB_TREE, F_PY);
            py->tree = st->tree;
            param_field(row, py, t->param_y);
        }
        hint(body, t->type == LX_AT_1D ? "Each motion sits at a threshold on the parameter; between two of them they blend."
             : t->type == LX_AT_DIRECT ? "Each motion's weight is its own parameter - for facial poses and additive layers."
             : "Each motion sits at a position on the two parameters; the nearest few blend.");
        for (int i = 0; i < t->child_count; i++) {
            LxAnimChild *c = &t->child[i];
            UiNode *card = ui_box(ui, body, UI_COL);
            card->bg = UI_HEX(0x222327);
            card->radius = 4;
            ui_pad(card, 8, 6, 8, 6);
            card->gap = 3;
            UiNode *top = ui_box(ui, card, UI_ROW);
            top->h = 24;
            top->gap = 4;
            top->cross_align = UI_ALIGN_CENTER;
            UiNode *cb = clip_button(top, c->clip, pick_child_clip);
            cb->tag = i;
            UiNode *del = small_btn(top, UI_ICON_TRASH, "Remove this motion", delete_child);
            del->tag = i;
            if (t->type == LX_AT_1D) {
                UiNode *row = prow(card, "Threshold", "where on the parameter this motion sits");
                AB *b = ab_new(AB_CHILD, F_THRESH);
                b->child = i;
                b->tree = st->tree;
                num_field(row, b, c->threshold, 0.1f);
            } else if (t->type == LX_AT_DIRECT) {
                UiNode *row = prow(card, "Weight From", "the parameter holding this motion's weight");
                AB *b = ab_new(AB_CHILD, F_NAME);
                b->child = i;
                b->tree = st->tree;
                param_field(row, b, c->weight_param);
            } else {
                UiNode *row = prow(card, "Position", "its place on the two parameters");
                AB *bx = ab_new(AB_CHILD, F_PX);
                bx->child = i;
                bx->tree = st->tree;
                num_field(row, bx, c->px, 0.1f);
                AB *by = ab_new(AB_CHILD, F_PY);
                by->child = i;
                by->tree = st->tree;
                num_field(row, by, c->py, 0.1f);
            }
            UiNode *row = prow(card, "Speed", "playback speed of this motion");
            AB *sb = ab_new(AB_CHILD, F_SPEED);
            sb->child = i;
            sb->tree = st->tree;
            num_field(row, sb, c->speed, 0.05f);
            AB *mb = ab_new(AB_CHILD, F_MIRROR);
            mb->child = i;
            mb->tree = st->tree;
            check_field(card, mb, c->mirror, "Mirror");
        }
    }
    UiNode *b = ui_button(ui, body, "Add Motion", add_child);
    b->icon = UI_ICON_PLUS;
    b->h = 26;
}

/* clicking a transition in the state's list opens it in the Transition tab */
static void pick_transition(UiNode *n) { ae_select(AE.sel, (int)n->tag); }

static void build_state(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxAnimGraph *g = ae_graph();
    LxAnimLayer *l = ae_layer();
    if (!g || !l) { hint(body, "No animator graph."); return; }
    if (AE.sel == AE_ANY) {
        heading(body, "Any State");
        hint(body, "Transitions out of Any State are taken from whichever state is playing - a death, a hit reaction, a teleport. Ctrl-drag from it to a state to add one.");
        return;
    }
    LxAnimNode *st = ae_sel_state();
    if (!st) { hint(body, "Select a state in the graph."); return; }
    static const char *const motions[] = {"Clip", "Blend Tree"};
    UiNode *row = prow(body, "Name", "what this state is called");
    AB *nb = ab_new(AB_STATE, F_NAME);
    UiNode *tf = text_field(row, nb, st->name);
    tf->font = UI_FONT_BOLD;
    row = prow(body, "Motion", "a single clip, or a blend tree that mixes several");
    AB *mb = ab_new(AB_STATE, F_MOTION);
    enum_field(row, mb, st->motion == LX_AM_TREE ? 1 : 0, motions, 2);
    if (st->motion == LX_AM_CLIP) {
        row = prow(body, "Clip", "the animation it plays");
        clip_button(row, st->clip, pick_state_clip);
    }
    row = prow(body, "Speed", "1 is normal; negative plays backwards");
    AB *sb = ab_new(AB_STATE, F_SPEED);
    num_field(row, sb, st->speed, 0.05f);
    row = prow(body, "Speed From", "a parameter that multiplies the speed");
    AB *spb = ab_new(AB_STATE, F_SPEED_PARAM);
    param_field(row, spb, st->speed_param);
    row = prow(body, "Cycle Offset", "where in the clip the state starts, in cycles");
    AB *cb = ab_new(AB_STATE, F_CYCLE);
    num_field(row, cb, st->cycle_offset, 0.05f);
    AB *lb = ab_new(AB_STATE, F_LOOP);
    check_field(body, lb, st->loop, "Loop");
    AB *mib = ab_new(AB_STATE, F_MIRROR);
    check_field(body, mib, st->mirror, "Mirror (humanoid rigs)");
    AB *fb = ab_new(AB_STATE, F_FOOTIK);
    check_field(body, fb, st->foot_ik, "Foot IK while this plays");
    AB *bb = ab_new(AB_STATE, F_BREAK);
    check_field(body, bb, st->breakpoint, "Breakpoint (pause Play here)");
    row = prow(body, "Tag", "a label a graph can ask about");
    AB *tgb = ab_new(AB_STATE, F_TAG);
    text_field(row, tgb, st->tag);
    if (st->motion == LX_AM_TREE) build_blend_tree(body, g, st);
    /* its transitions, as a list that selects */
    heading(body, "Transitions");
    if (!st->trans_count) hint(body, "None yet. Ctrl-drag from this state to another to add one.");
    for (int t = 0; t < st->trans_count; t++) {
        char line[200];
        lx_anim_transition_summary(&st->trans[t], line, sizeof line);
        UiNode *b = ui_button(ui, body, line, pick_transition);
        b->h = 24;
        b->center_text = false;
        b->bg = AE.sel_trans == t ? UI_HEX(0x2a3650) : COL_FIELD;
        b->border = COL_EDGE;
        b->radius = 3;
        b->tag = t;
        lx_strcpy(b->tooltip, sizeof b->tooltip, "Edit this transition in the Transition tab");
    }
}

/* ======================================================================
 *  Transition tab
 * ====================================================================== */
static void add_cond(UiNode *x) {
    LX_UNUSED(x);
    LxAnimGraph *g = ae_graph();
    LxAnimTransition *t = ae_sel_transition();
    if (!g || !t) return;
    if (!lx_anim_add_cond(t, g->param_count ? g->params[0].name : "", LX_AC_GREATER, 0))
        ui_notify(ED.ui, "A transition holds at most %d conditions", LX_ANIM_MAX_COND);
    ae_commit(true, NULL);
}

static void delete_cond(UiNode *n) {
    LxAnimTransition *t = ae_sel_transition();
    if (!t) return;
    lx_anim_remove_cond(t, (int)n->tag);
    ae_commit(true, NULL);
}

static void build_transition(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxAnimGraph *g = ae_graph();
    LxAnimTransition *t = ae_sel_transition();
    if (!g) { hint(body, "No animator graph."); return; }
    if (!t) { hint(body, "Select a transition - click its arrow in the graph."); return; }
    static const char *const interrupts[] = {"None", "Current State", "Next State", "Both"};
    static const char *const modes[LX_AC_MODE_COUNT] = {"Greater", "Less", "Equals", "Not Equal", "If", "If Not"};
    UiNode *row = prow(body, "To", "the state it goes to");
    AB *tob = ab_new(AB_TRANS, F_TO);
    state_field(row, tob, t->to);
    AB *heb = ab_new(AB_TRANS, F_HAS_EXIT);
    check_field(body, heb, t->has_exit_time, "Has Exit Time");
    if (t->has_exit_time) {
        row = prow(body, "Exit Time", "how far through the state it may leave, in cycles");
        AB *eb = ab_new(AB_TRANS, F_EXIT);
        num_field(row, eb, t->exit_time, 0.05f);
    }
    row = prow(body, "Duration", t->fixed_duration ? "the cross-fade, in seconds" : "the cross-fade, in cycles of the state being left");
    AB *db = ab_new(AB_TRANS, F_DUR);
    num_field(row, db, t->duration, 0.05f);
    AB *fdb = ab_new(AB_TRANS, F_FIXED);
    check_field(body, fdb, t->fixed_duration, "Fixed Duration (seconds)");
    row = prow(body, "Offset", "where the state being entered starts, in cycles");
    AB *ob = ab_new(AB_TRANS, F_OFFSET);
    num_field(row, ob, t->offset, 0.05f);
    row = prow(body, "Interruption", "what may cut this transition short");
    AB *ib = ab_new(AB_TRANS, F_INTERRUPT);
    enum_field(row, ib, (int)t->interrupt, interrupts, 4);
    AB *tsb = ab_new(AB_TRANS, F_TOSELF);
    check_field(body, tsb, t->to_self, "Can Transition To Self");
    AB *mb = ab_new(AB_TRANS, F_MUTE);
    check_field(body, mb, t->mute, "Mute (ignore it)");
    AB *sb = ab_new(AB_TRANS, F_SOLO);
    check_field(body, sb, t->solo, "Solo (only solo transitions are taken)");
    heading(body, "Conditions");
    if (!t->cond_count)
        hint(body, t->has_exit_time ? "No conditions: it fires when the exit time is reached."
                                    : "No conditions and no exit time: it fires the moment the state is entered.");
    for (int c = 0; c < t->cond_count; c++) {
        const LxAnimCond *cd = &t->cond[c];
        UiNode *crow = ui_box(ui, body, UI_ROW);
        crow->h = 24;
        crow->gap = 4;
        crow->cross_align = UI_ALIGN_CENTER;
        AB *pb = ab_new(AB_COND, F_NAME);
        pb->cond = c;
        param_field(crow, pb, cd->param);
        AB *mdb = ab_new(AB_COND, F_CMODE);
        mdb->cond = c;
        UiNode *dd = enum_field(crow, mdb, (int)cd->mode, modes, LX_AC_MODE_COUNT);
        dd->grow = 0;
        dd->w = 86;
        bool flag = cd->mode == LX_AC_IF || cd->mode == LX_AC_IF_NOT;
        if (!flag) {
            AB *vb = ab_new(AB_COND, F_CVALUE);
            vb->cond = c;
            UiNode *nf = num_field(crow, vb, cd->value, 0.05f);
            nf->grow = 0;
            nf->w = 70;
        }
        UiNode *del = small_btn(crow, UI_ICON_TRASH, "Remove this condition", delete_cond);
        del->tag = c;
    }
    UiNode *b = ui_button(ui, body, "Add Condition", add_cond);
    b->icon = UI_ICON_PLUS;
    b->h = 26;
}

/* ======================================================================
 *  Layers tab
 * ====================================================================== */
/* The mask names humanoid bones, and each one brings everything under it: masking the chest gives the
 * whole upper body without listing every finger. These are the ones worth a checkbox. */
static const struct { int bone; const char *name; } MASK_BONES[] = {
    {LX_HB_HIPS, "Hips (and all below)"}, {LX_HB_SPINE, "Spine"}, {LX_HB_CHEST, "Chest"},
    {LX_HB_NECK, "Neck"}, {LX_HB_HEAD, "Head"},
    {LX_HB_LEFT_SHOULDER, "Left Shoulder"}, {LX_HB_RIGHT_SHOULDER, "Right Shoulder"},
    {LX_HB_LEFT_UPPER_ARM, "Left Arm"}, {LX_HB_RIGHT_UPPER_ARM, "Right Arm"},
    {LX_HB_LEFT_HAND, "Left Hand"}, {LX_HB_RIGHT_HAND, "Right Hand"},
    {LX_HB_LEFT_UPPER_LEG, "Left Leg"}, {LX_HB_RIGHT_UPPER_LEG, "Right Leg"},
    {LX_HB_LEFT_FOOT, "Left Foot"}, {LX_HB_RIGHT_FOOT, "Right Foot"},
};

static void add_layer(UiNode *x) {
    LX_UNUSED(x);
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    if (!lx_anim_add_layer(g, "Layer")) { ui_notify(ED.ui, "A graph holds at most %d layers", LX_ANIM_MAX_LAYERS); return; }
    AE.layer = g->layer_count - 1;
    AE.sel = -1;
    AE.sel_trans = -1;
    ae_commit(true, NULL);
}

static void delete_layer(UiNode *n) {
    LxAnimGraph *g = ae_graph();
    if (!g) return;
    lx_anim_remove_layer(g, (int)n->tag);
    if (AE.layer >= g->layer_count) AE.layer = g->layer_count - 1;
    AE.sel = -1;
    AE.sel_trans = -1;
    ae_commit(true, NULL);
}

static void show_layer(UiNode *n) {
    AE.layer = (int)n->tag;
    AE.sel = -1;
    AE.sel_trans = -1;
    AE.framed = false;
    AE.insp_dirty = true;
}

static void build_layers(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxAnimGraph *g = ae_graph();
    if (!g) { hint(body, "No animator graph."); return; }
    static const char *const blends[] = {"Override", "Additive"};
    hint(body, "Layer 0 is the whole body. The rest play over it by weight, through the bones their mask allows - an upper-body aim over a full-body walk.");
    for (int i = 0; i < g->layer_count; i++) {
        LxAnimLayer *l = &g->layers[i];
        UiNode *card = ui_box(ui, body, UI_COL);
        card->bg = i == AE.layer ? UI_HEX(0x262a33) : UI_HEX(0x222327);
        card->radius = 4;
        ui_pad(card, 8, 6, 8, 6);
        card->gap = 3;
        UiNode *top = ui_box(ui, card, UI_ROW);
        top->h = 24;
        top->gap = 4;
        top->cross_align = UI_ALIGN_CENTER;
        AB *nb = ab_new(AB_LAYER, F_NAME);
        nb->layer = i;
        UiNode *tf = text_field(top, nb, l->name);
        tf->font = UI_FONT_BOLD;
        UiNode *show = small_btn(top, UI_ICON_FOCUS, "Show this layer's state machine", show_layer);
        show->tag = i;
        show->selected = i == AE.layer;
        if (i > 0) {
            UiNode *del = small_btn(top, UI_ICON_TRASH, "Delete this layer", delete_layer);
            del->tag = i;
        }
        UiNode *row = prow(card, "Starting State", "the state this layer begins in");
        AB *stb = ab_new(AB_LAYER, F_START);
        stb->layer = i;
        /* the state list is the shown layer's, so only offer it for that one */
        if (i == AE.layer) state_field(row, stb, l->start);
        else label(row, l->start[0] ? l->start : "(none)", UI_FONT_BODY, COL_DIM);
        if (i > 0) {
            row = prow(card, "Weight", "how much of this layer shows");
            AB *wb = ab_new(AB_LAYER, F_WEIGHT);
            wb->layer = i;
            wb->f = l->weight;
            UiNode *sl = ui_slider(ui, row, &wb->f, 0, 1);
            sl->grow = 1;
            sl->h = 22;
            sl->user = wb;
            sl->on_change = ab_apply;
            row = prow(card, "Blending", "override what is underneath, or add to it");
            AB *bb = ab_new(AB_LAYER, F_BLEND);
            bb->layer = i;
            enum_field(row, bb, (int)l->blend, blends, 2);
            label(card, l->mask ? "Mask: some bones" : "Mask: every bone", UI_FONT_SMALL, COL_DIM);
            for (int k = 0; k < (int)(sizeof MASK_BONES / sizeof MASK_BONES[0]); k++) {
                AB *mb = ab_new(AB_LAYER, F_MASK);
                mb->layer = i;
                mb->child = MASK_BONES[k].bone;
                check_field(card, mb, (l->mask & (1ull << MASK_BONES[k].bone)) != 0, MASK_BONES[k].name);
            }
        }
    }
    UiNode *b = ui_button(ui, body, "Add Layer", add_layer);
    b->icon = UI_ICON_PLUS;
    b->h = 26;
}

/* ======================================================================
 *  the inspector itself
 * ====================================================================== */
static UiNode *tab_body(UiNode *tabs, const char *title, UiIcon icon) {
    UiNode *p = ui_box(ED.ui, tabs, UI_COL);
    lx_strcpy(p->text, sizeof p->text, title);
    p->icon = icon;
    p->scroll = true;
    p->gap = 2;
    ui_pad(p, 10, 8, 10, 10);
    return p;
}

UiNode *ae_inspector(UiNode *parent) {
    UiCtx *ui = ED.ui;
    UiNode *tabs = ui_tabs(ui, parent);
    AE.insp_tabs = tabs;
    ui_track(&AE.insp_tabs);
    AE.insp_params = tab_body(tabs, "Parameters", UI_ICON_LIST);
    AE.insp_state = tab_body(tabs, "State", UI_ICON_ANIMATOR);
    AE.insp_trans = tab_body(tabs, "Transition", UI_ICON_GRAPH);
    AE.insp_layers = tab_body(tabs, "Layers", UI_ICON_LAYERS);
    ui_track(&AE.insp_params);
    ui_track(&AE.insp_state);
    ui_track(&AE.insp_trans);
    ui_track(&AE.insp_layers);
    AE.insp_dirty = true;
    return tabs;
}

void ae_inspector_frame(void) {
    if (!AE.insp_dirty || !AE.insp_tabs) return;
    AE.insp_dirty = false;
    g_ab_n = 0;
    UiNode *bodies[4] = {AE.insp_params, AE.insp_state, AE.insp_trans, AE.insp_layers};
    for (int i = 0; i < 4; i++) if (bodies[i]) ui_clear(ED.ui, bodies[i]);
    if (AE.insp_params) build_params(AE.insp_params);
    if (AE.insp_state) build_state(AE.insp_state);
    if (AE.insp_trans) build_transition(AE.insp_trans);
    if (AE.insp_layers) build_layers(AE.insp_layers);
}

/* ======================================================================
 *  the .lxanim asset Inspector
 * ====================================================================== */
static void open_asset_click(UiNode *n) { ed_animator_open_asset(n->path); }

void ed_animator_asset_inspector(UiNode *body, const char *ref) {
    UiCtx *ui = ED.ui;
    LxAnimGraph *g = lx_asset_anim_graph(ref);
    UiNode *wrap = ui_box(ui, body, UI_COL);
    ui_pad(wrap, 12, 8, 12, 8);
    wrap->gap = 6;
    if (!g) { hint(wrap, "This animator graph can't be read."); return; }
    label(wrap, g->name, UI_FONT_TITLE, COL_TEXT);
    if (g->description[0]) hint(wrap, g->description);
    int states = 0, trans = 0;
    for (int i = 0; i < g->layer_count; i++) {
        states += g->layers[i].state_count;
        trans += g->layers[i].any_count;
        for (int s = 0; s < g->layers[i].state_count; s++) trans += g->layers[i].states[s].trans_count;
    }
    char t[200];
    snprintf(t, sizeof t, "%d layer%s · %d states · %d transitions · %d parameters · %d blend trees",
             g->layer_count, g->layer_count == 1 ? "" : "s", states, trans, g->param_count, g->tree_count);
    hint(wrap, t);
    UiNode *b = ui_button(ui, wrap, "Open in Animator", open_asset_click);
    b->icon = UI_ICON_ANIMATOR;
    b->h = 30;
    b->bg = COL_ACCENT;
    lx_strcpy(b->path, sizeof b->path, ref);
    int n_prob = lx_anim_graph_problems(g, NULL, 0);
    if (n_prob) {
        char lines[6][160];
        int shown = lx_anim_graph_problems(g, lines, 6);
        heading(wrap, n_prob == 1 ? "1 problem" : "Problems");
        for (int i = 0; i < shown; i++) label(wrap, lines[i], UI_FONT_SMALL, UI_HEX(0xffb03a));
    }
}
