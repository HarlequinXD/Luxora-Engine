/* State Graph editor, right-hand side: the FSM / State / Events / Variables inspector (actions as
 * foldouts with a row per parameter and a purple "=" button to use a variable), the Action Browser
 * window, the State Graph section of the object Inspector and the .lxgraph asset Inspector. */
#include "graph_editor.h"
#include <math.h>

#define COL_TEXT UI_HEX(0xdfe1e6)
#define COL_DIM UI_HEX(0x8d919a)
#define COL_FIELD UI_HEX(0x17181b)
#define COL_EDGE UI_HEX(0x34363c)
#define COL_VAR_BG UI_HEX(0x2a1c33)
#define COL_VAR_ON UI_HEX(0x4a2c5a)
#define COL_VAR_TEXT UI_HEX(0xe2c8ff)
#define COL_ACCENT UI_HEX(0x3d7cf5)
#define COL_OK UI_HEX(0x7cc24e)
#define COL_RUN UI_HEX(0xffb03a)

/* text fields also report a change when they lose focus (clicking elsewhere): "add" fields act on Enter only */
static bool enter_pressed(void) { return lx_key_pressed(LX_KEY_ENTER); }
static void quick_add_enter(UiNode *n);
static void add_on_enter(UiNode *n);
static void add_var_enter(UiNode *n);

static int lx_strcasecmp_ascii(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
static bool lx_strcasestr_ascii(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

/* ======================================================================
 *  bindings: widgets edit copies here, never graph memory (which moves when the graph grows)
 * ====================================================================== */
enum { PB_PARAM, PB_UPDATE, PB_ENABLED, PB_VAR, PB_GVAR, PB_OVERRIDE, PB_MISC };
typedef struct PB {
    int kind;
    int st, ac, p;          /* PB_PARAM: state, action, parameter; PB_VAR/GVAR: variable index */
    LxFsmType type;
    float f[4];
    int i;
    bool b;
    char s[160];
    const char *opts[24];
    int opt_count;
    char optbuf[400];
    LxEntity e;             /* PB_OVERRIDE */
    char name[LX_FSM_NAME];
} PB;
#define PB_MAX 700
static PB g_pb[PB_MAX];
static int g_pb_n;
static PB g_spare;
static PB *pb_new(int kind) {
    PB *b = g_pb_n < PB_MAX ? &g_pb[g_pb_n++] : &g_spare;
    memset(b, 0, sizeof *b);
    b->kind = kind;
    return b;
}
/* the object Inspector's State Graph section has its own pool: it's rebuilt at other times */
#define OB_MAX 96
static PB g_ob[OB_MAX];
static int g_ob_n;
static PB *ob_new(void) {
    PB *b = g_ob_n < OB_MAX ? &g_ob[g_ob_n++] : &g_spare;
    memset(b, 0, sizeof *b);
    b->kind = PB_OVERRIDE;
    return b;
}

static void pb_options(PB *b, const char *options) {
    lx_strcpy(b->optbuf, sizeof b->optbuf, options ? options : "");
    b->opt_count = 0;
    for (char *s = b->optbuf; *s && b->opt_count < 24;) {
        b->opts[b->opt_count++] = s;
        char *bar = strchr(s, '|');
        if (!bar) break;
        *bar = 0;
        s = bar + 1;
    }
}
static void pb_load(PB *b, LxFsmType t, const char *text, const char *options) {
    LxFsmValue v;
    lx_fsm_value_parse(&v, t, text, options);
    b->type = t;
    switch (t) {
    case LX_FSM_FLOAT: b->f[0] = v.f; break;
    case LX_FSM_INT: case LX_FSM_ENUM: b->i = v.i; break;
    case LX_FSM_BOOL: b->b = v.b; break;
    case LX_FSM_VEC3: b->f[0] = v.v.x; b->f[1] = v.v.y; b->f[2] = v.v.z; break;
    case LX_FSM_COLOR: b->f[0] = v.c.x; b->f[1] = v.c.y; b->f[2] = v.c.z; b->f[3] = v.c.w; break;
    default: lx_strcpy(b->s, sizeof b->s, text ? text : ""); break;
    }
    if (t == LX_FSM_ENUM) pb_options(b, options);
}
static void pb_format(const PB *b, char *out, size_t cap) {
    switch (b->type) {
    case LX_FSM_FLOAT: snprintf(out, cap, "%g", (double)b->f[0]); break;
    case LX_FSM_INT: snprintf(out, cap, "%d", b->i); break;
    case LX_FSM_ENUM: lx_strcpy(out, cap, b->i >= 0 && b->i < b->opt_count ? b->opts[b->i] : "0"); break;
    case LX_FSM_BOOL: lx_strcpy(out, cap, b->b ? "true" : "false"); break;
    case LX_FSM_VEC3: snprintf(out, cap, "%g %g %g", (double)b->f[0], (double)b->f[1], (double)b->f[2]); break;
    case LX_FSM_COLOR: snprintf(out, cap, "%g %g %g %g", (double)b->f[0], (double)b->f[1], (double)b->f[2], (double)b->f[3]); break;
    default: lx_strcpy(out, cap, b->s); break;
    }
}

static LxGraphAction *pb_action(const PB *b, const LxFsmActionDesc **d) {
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->state_count) return NULL;
    LxGraphState *st = &g->states[b->st];
    if (b->ac < 0 || b->ac >= st->action_count) return NULL;
    if (d) *d = lx_fsm_action_find(st->actions[b->ac].type);
    return &st->actions[b->ac];
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
    l->w = 120;
    lx_strcpy(l->tooltip, sizeof l->tooltip, tip && tip[0] ? tip : name);
    return row;
}
static UiNode *field_button(UiNode *row, const char *text, void (*cb)(UiNode *)) {
    UiNode *b = ui_button(ED.ui, row, text, cb);
    b->grow = 1;
    b->h = 22;
    b->bg = COL_FIELD;
    b->border = COL_EDGE;
    b->radius = 3;
    b->center_text = false;
    b->icon = UI_ICON_CHEVRON_DOWN;
    return b;
}
static UiNode *small_btn(UiNode *row, UiIcon icon, const char *tip, void (*cb)(UiNode *)) {
    UiNode *b = ui_icon_button(ED.ui, row, icon, tip, cb);
    b->w = 22;
    b->h = 22;
    return b;
}

/* ======================================================================
 *  value fields (parameters, variables, overrides)
 * ====================================================================== */
static void asset_pick(UiNode *it);
static void collect_files(UiNode *popup, const char *dir, const char *rel, const char *ext, PB *b, int *count) {
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n && *count < 150; i++) {
        if (e[i].name[0] == '.') continue;
        char full[LX_PATH_MAX], r[LX_REF_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, e[i].name);
        else lx_strcpy(r, sizeof r, e[i].name);
        if (e[i].is_dir) { collect_files(popup, full, r, ext, b, count); continue; }
        size_t ln = strlen(e[i].name), le = strlen(ext);
        if (ln < le || lx_strcasecmp_ascii(e[i].name + ln - le, ext)) continue;
        UiNode *it = ui_menu_item(ED.ui, popup, r, NULL, asset_pick);
        it->user = b;
        it->icon = ed_icon_for_file(e[i].name, false);
        it->icon_color = ed_color_for_icon(it->icon);
        (*count)++;
    }
    lx_free(e);
}
static const char *kind_ext(const char *kind) {
    if (!kind) return "";
    if (!strcmp(kind, "prefab")) return ".lxprefab";
    if (!strcmp(kind, "sound")) return ".wav";
    if (!strcmp(kind, "scene")) return ".lxscene";
    if (!strcmp(kind, "material")) return ".lxmat";
    if (!strcmp(kind, "graph")) return ".lxgraph";
    if (!strcmp(kind, "mesh")) return ".obj";
    return "";
}

static void value_changed(UiNode *n);   /* routes by PB kind */
static void asset_pick(UiNode *it) {
    PB *b = it->user;
    lx_strcpy(b->s, sizeof b->s, it->text[0] == '(' ? "" : it->text);
    UiNode fake;
    memset(&fake, 0, sizeof fake);
    fake.user = b;
    value_changed(&fake);
}
static void asset_browse(UiNode *btn) {
    PB *b = btn->user;
    UiNode *field = btn->prev;
    UiNode *p = ui_popup(ED.ui, field->x, field->y + field->ch, field->cw / ui_scale(ED.ui) + 30);
    if (!p) return;
    p->owner = btn;
    UiNode *none = ui_menu_item(ED.ui, p, "(none)", NULL, asset_pick);
    none->user = b;
    int count = 0;
    const char *kind = btn->path;
    if (!strcmp(kind, "material")) {
        static const char *bi[] = {"builtin:default", "builtin:metal", "builtin:red", "builtin:green", "builtin:blue", "builtin:emissive", "builtin:concrete", "builtin:fabric", "builtin:grid"};
        for (int i = 0; i < 9; i++) { UiNode *it = ui_menu_item(ED.ui, p, bi[i], NULL, asset_pick); it->user = b; it->icon = UI_ICON_MATERIAL; }
    }
    if (kind_ext(kind)[0]) collect_files(p, ED.project.assets, "", kind_ext(kind), b, &count);
    if (!count && strcmp(kind, "material")) { UiNode *it = ui_menu_item(ED.ui, p, "No matching assets in the project", NULL, NULL); it->disabled = true; }
}
static void asset_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(type);
    PB *b = n->user;
    lx_strcpy(b->s, sizeof b->s, payload);
    value_changed(n);
}

static void object_pick(UiNode *it) {
    PB *b = it->user;
    lx_strcpy(b->s, sizeof b->s, it->path);
    UiNode fake;
    memset(&fake, 0, sizeof fake);
    fake.user = b;
    value_changed(&fake);
}
static void object_browse(UiNode *btn) {
    PB *b = btn->user;
    UiNode *field = btn->prev;
    UiNode *p = ui_popup(ED.ui, field->x, field->y + field->ch, 220);
    if (!p) return;
    p->owner = btn;
    UiNode *it = ui_menu_item(ED.ui, p, "Owner (this object)", NULL, object_pick);
    it->user = b; lx_strcpy(it->path, sizeof it->path, "Owner"); it->icon = UI_ICON_GRAPH;
    it = ui_menu_item(ED.ui, p, "None", NULL, object_pick);
    it->user = b; lx_strcpy(it->path, sizeof it->path, "None");
    ui_menu_separator(ED.ui, p);
    int n = 0;
    for (int i = 0; ED.scene && i < lx_scene_entity_count(ED.scene) && n < 40; i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        const char *nm = lx_entity_name(ED.scene, e);
        if (!nm || !nm[0]) continue;
        it = ui_menu_item(ED.ui, p, nm, NULL, object_pick);
        it->user = b;
        snprintf(it->path, sizeof it->path, "name:%s", nm);
        it->icon = ed_icon_for_entity(ED.scene, e);
        it->icon_color = COL_DIM;
        n++;
    }
    if (n) { UiNode *h = ui_menu_item(ED.ui, p, "Or type tag:Enemy for any object with a tag", NULL, NULL); h->disabled = true; }
}
static void object_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(type);
    PB *b = n->user;
    LxEntity e = (LxEntity)strtoul(payload, NULL, 10);
    if (!ED.scene || !lx_entity_valid(ED.scene, e)) return;
    if (e == GE.obj) lx_strcpy(b->s, sizeof b->s, "Owner");
    else snprintf(b->s, sizeof b->s, "name:%s", lx_entity_name(ED.scene, e));
    value_changed(n);
}

/* a field for a value of type t, bound to b, calling value_changed */
static void value_field(UiNode *row, PB *b, LxFsmType t, const char *options) {
    UiCtx *ui = ED.ui;
    UiNode *n = NULL;
    switch (t) {
    case LX_FSM_FLOAT: n = ui_number(ui, row, &b->f[0], 0.05f); n->precision = 3; break;
    case LX_FSM_INT: n = ui_int(ui, row, &b->i, -1000000000, 1000000000); break;
    case LX_FSM_BOOL: {
        UiNode *box = ui_box(ui, row, UI_ROW);
        box->grow = 1;
        box->h = 22;
        n = ui_checkbox(ui, box, "", &b->b);
        n->user = b;
        n->on_change = value_changed;
        return;
    }
    case LX_FSM_ENUM: n = ui_dropdown(ui, row, &b->i, b->opts, b->opt_count); break;
    case LX_FSM_VEC3:
        for (int k = 0; k < 3; k++) {
            UiNode *x = ui_number(ui, row, &b->f[k], 0.05f);
            x->grow = 1;
            x->h = 22;
            x->precision = 2;
            x->user = b;
            x->on_change = value_changed;
        }
        return;
    case LX_FSM_COLOR: n = ui_color(ui, row, b->f, true); break;
    case LX_FSM_OBJECT: {
        n = ui_textfield(ui, row, b->s, sizeof b->s);
        lx_strcpy(n->hint, sizeof n->hint, "Owner, name:X or tag:X");
        n->accept = "entity";
        n->on_drop = object_drop;
        n->grow = 1;
        n->h = 22;
        n->user = b;
        n->on_change = value_changed;
        UiNode *bb = small_btn(row, UI_ICON_CHEVRON_DOWN, "Pick an object", object_browse);
        bb->user = b;
        return;
    }
    case LX_FSM_ASSET: {
        n = ui_textfield(ui, row, b->s, sizeof b->s);
        n->accept = options && options[0] ? options : "file";
        n->on_drop = asset_drop;
        lx_strcpy(n->hint, sizeof n->hint, "Drop an asset or browse");
        n->grow = 1;
        n->h = 22;
        n->user = b;
        n->on_change = value_changed;
        UiNode *bb = small_btn(row, UI_ICON_DOTS, "Browse", asset_browse);
        bb->user = b;
        lx_strcpy(bb->path, sizeof bb->path, options ? options : "");
        return;
    }
    default: n = ui_textfield(ui, row, b->s, sizeof b->s); break;
    }
    n->grow = 1;
    n->h = 22;
    n->user = b;
    n->on_change = value_changed;
}

/* ======================================================================
 *  writing values back
 * ====================================================================== */
static LxGraphVar *pb_var(const PB *b) {
    if (b->kind == PB_VAR) { LxGraph *g = ge_graph(); return g && b->st >= 0 && b->st < g->var_count ? &g->vars[b->st] : NULL; }
    if (b->kind == PB_GVAR) { LxGlobals *gl = lx_fsm_globals(); return gl && b->st >= 0 && b->st < gl->var_count ? &gl->vars[b->st] : NULL; }
    return NULL;
}
static void override_changed(PB *b);
static void value_changed(UiNode *n) {
    PB *b = n->user;
    if (!b) return;
    char v[160];
    pb_format(b, v, sizeof v);
    if (b->kind == PB_PARAM) {
        LxGraphAction *a = pb_action(b, NULL);
        if (!a || b->p < 0 || b->p >= LX_FSM_MAX_PARAMS) return;
        a->p[b->p].is_var = false;
        lx_strcpy(a->p[b->p].value, sizeof a->p[b->p].value, v);
        ge_commit(false, b);
    } else if (b->kind == PB_VAR) {
        LxGraphVar *x = pb_var(b);
        if (!x) return;
        lx_strcpy(x->value, sizeof x->value, v);
        ge_commit(false, b);
    } else if (b->kind == PB_GVAR) {
        LxGraphVar *x = pb_var(b);
        if (!x) return;
        lx_strcpy(x->value, sizeof x->value, v);
        if (!lx_fsm_globals_save()) ui_notify(ED.ui, "Couldn't save the global variables");
    } else if (b->kind == PB_OVERRIDE) override_changed(b);
}

/* ======================================================================
 *  parameters
 * ====================================================================== */
static struct { int st, ac, p; LxFsmType type; } g_newvar;
static void newvar_done(const char *text) {
    LxGraph *g = ge_graph();
    if (!g || !text[0]) return;
    char name[LX_FSM_NAME];
    lx_strcpy(name, sizeof name, text);
    LxGraphVar *v = lx_graph_find_var(g, name);
    if (v && v->type != g_newvar.type) { ui_notify(ED.ui, "%s already exists as a %s", name, lx_fsm_type_name(v->type)); return; }
    if (!v) lx_graph_add_var(g, name, g_newvar.type == LX_FSM_ENUM ? LX_FSM_INT : g_newvar.type);
    PB tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.st = g_newvar.st; tmp.ac = g_newvar.ac;
    LxGraphAction *a = pb_action(&tmp, NULL);
    if (a && g_newvar.p >= 0 && g_newvar.p < LX_FSM_MAX_PARAMS) {
        a->p[g_newvar.p].is_var = true;
        lx_strcpy(a->p[g_newvar.p].value, sizeof a->p[g_newvar.p].value, name);
    }
    ge_commit(true, NULL);
}
static void var_pick(UiNode *it) {
    PB *b = it->user;
    LxGraphAction *a = pb_action(b, NULL);
    if (!a) return;
    if (!strcmp(it->path, "+new")) {
        g_newvar.st = b->st; g_newvar.ac = b->ac; g_newvar.p = b->p; g_newvar.type = b->type;
        ed_prompt("New Variable", "Variable name", "", newvar_done);
        return;
    }
    a->p[b->p].is_var = true;
    lx_strcpy(a->p[b->p].value, sizeof a->p[b->p].value, it->path);
    ge_commit(true, NULL);
}
static bool type_fits(LxFsmType want, LxFsmType have) {
    if (want == have) return true;
    bool num_w = want == LX_FSM_FLOAT || want == LX_FSM_INT || want == LX_FSM_ENUM, num_h = have == LX_FSM_FLOAT || have == LX_FSM_INT;
    if (num_w && num_h) return true;
    if (want == LX_FSM_STRING && (have == LX_FSM_FLOAT || have == LX_FSM_INT)) return false;
    return (want == LX_FSM_ASSET && have == LX_FSM_STRING) || (want == LX_FSM_STRING && have == LX_FSM_ASSET);
}
static void var_browse(UiNode *btn) {
    PB *b = btn->user;
    LxGraph *g = ge_graph();
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y + btn->ch, btn->cw / ui_scale(ED.ui));
    if (!p || !g) return;
    p->owner = btn;
    UiNode *it = ui_menu_item(ED.ui, p, "None", NULL, var_pick);
    it->user = b;
    int n = 0;
    for (int i = 0; i < g->var_count; i++) {
        if (!type_fits(b->type, g->vars[i].type)) continue;
        it = ui_menu_item(ED.ui, p, g->vars[i].name, lx_fsm_type_name(g->vars[i].type), var_pick);
        it->user = b;
        lx_strcpy(it->path, sizeof it->path, g->vars[i].name);
        it->icon = UI_ICON_DOTS;
        it->icon_color = COL_VAR_TEXT;
        n++;
    }
    LxGlobals *gl = lx_fsm_globals();
    for (int i = 0; gl && i < gl->var_count; i++) {
        if (!type_fits(b->type, gl->vars[i].type) || lx_graph_find_var(g, gl->vars[i].name)) continue;
        char t[96];
        snprintf(t, sizeof t, "global %s", lx_fsm_type_name(gl->vars[i].type));
        it = ui_menu_item(ED.ui, p, gl->vars[i].name, t, var_pick);
        it->user = b;
        lx_strcpy(it->path, sizeof it->path, gl->vars[i].name);
        it->icon = UI_ICON_GLOBE;
        it->icon_color = COL_VAR_TEXT;
        n++;
    }
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "New Variable...", NULL, var_pick);
    it->user = b;
    it->icon = UI_ICON_PLUS;
    lx_strcpy(it->path, sizeof it->path, "+new");
}
static void toggle_var(UiNode *btn) {
    PB *b = btn->user;
    const LxFsmActionDesc *d = NULL;
    LxGraphAction *a = pb_action(b, &d);
    if (!a || !d) return;
    LxGraphParam *p = &a->p[b->p];
    if (p->is_var) {
        p->is_var = false;
        const char *def = d->params[b->p].def ? d->params[b->p].def : lx_fsm_default_value(d->params[b->p].type);
        lx_strcpy(p->value, sizeof p->value, def);
    } else {
        p->is_var = true;
        p->value[0] = 0;
        /* the only variable that fits: use it straight away */
        LxGraph *g = ge_graph();
        int fit = -1, count = 0;
        for (int i = 0; g && i < g->var_count; i++) if (type_fits(d->params[b->p].type, g->vars[i].type)) { fit = i; count++; }
        if (count == 1) lx_strcpy(p->value, sizeof p->value, g->vars[fit].name);
    }
    ge_commit(true, NULL);
}

static void event_param_pick(UiNode *it) {
    PB *b = it->user;
    LxGraph *g = ge_graph();
    LxGraphAction *a = pb_action(b, NULL);
    if (!a || !g) return;
    if (!strcmp(it->path, "+new")) return;
    a->p[b->p].is_var = false;
    lx_strcpy(a->p[b->p].value, sizeof a->p[b->p].value, it->path);
    /* the state gets a transition for it, ready to be linked */
    if (it->path[0] && strcmp(it->path, "None")) {
        if (!lx_fsm_is_system_event(it->path) && !lx_graph_find_event(g, it->path)) lx_graph_add_event(g, it->path);
        LxGraphState *st = &g->states[b->st];
        bool has = false;
        for (int t = 0; t < st->trans_count; t++) has |= !strcmp(st->trans[t].event, it->path);
        bool global = false;
        for (int t = 0; t < g->global_count; t++) global |= !strcmp(g->globals[t].event, it->path);
        if (!has && !global) lx_graph_add_transition(g, st, it->path);
    }
    ge_commit(true, NULL);
}
static PB g_newev_pb;
static void new_event_param_done(const char *text) {
    char ev[LX_FSM_NAME];
    size_t k = 0;
    for (const char *p = text; *p && k + 1 < sizeof ev; p++) ev[k++] = *p == ' ' ? '_' : (char)toupper((unsigned char)*p);
    ev[k] = 0;
    if (!ev[0]) return;
    UiNode fake;
    memset(&fake, 0, sizeof fake);
    fake.user = &g_newev_pb;
    lx_strcpy(fake.path, sizeof fake.path, ev);
    event_param_pick(&fake);
}
static void event_param_new(UiNode *it) {
    g_newev_pb = *(PB *)it->user;
    ed_prompt("New Event", "Event name (e.g. DOOR_OPENED)", "", new_event_param_done);
}
static void event_param_browse(UiNode *btn) {
    PB *b = btn->user;
    LxGraph *g = ge_graph();
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y + btn->ch, btn->cw / ui_scale(ED.ui));
    if (!p || !g) return;
    p->owner = btn;
    UiNode *it = ui_menu_item(ED.ui, p, "None", NULL, event_param_pick);
    it->user = b;
    for (int i = 0; i < g->event_count; i++) {
        it = ui_menu_item(ED.ui, p, g->events[i].name, NULL, event_param_pick);
        it->user = b;
        lx_strcpy(it->path, sizeof it->path, g->events[i].name);
    }
    it = ui_menu_item(ED.ui, p, "FINISHED", "system", event_param_pick);
    it->user = b;
    lx_strcpy(it->path, sizeof it->path, "FINISHED");
    LxGlobals *gl = lx_fsm_globals();
    for (int i = 0; gl && i < gl->event_count; i++) {
        if (lx_graph_find_event(g, gl->events[i].name)) continue;
        it = ui_menu_item(ED.ui, p, gl->events[i].name, "global", event_param_pick);
        it->user = b;
        lx_strcpy(it->path, sizeof it->path, gl->events[i].name);
    }
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "New Event...", NULL, event_param_new);
    it->user = b;
    it->icon = UI_ICON_PLUS;
}

static void param_row(UiNode *body, int si, int ai, int pi, const LxGraphAction *a, const LxFsmParamDesc *pd, const LxFsmRun *run) {
    const LxGraphParam *gp = &a->p[pi];
    bool out = (pd->flags & LX_FSM_P_OUT) != 0;
    UiNode *row = prow(body, pd->name, pd->tooltip);
    PB *b = pb_new(PB_PARAM);
    b->st = si; b->ac = ai; b->p = pi; b->type = pd->type;
    if (gp->is_var || out) {
        UiNode *vb = field_button(row, gp->value[0] ? gp->value : out ? "None (not stored)" : "None", var_browse);
        vb->user = b;
        bool known = gp->value[0] && (lx_graph_find_var(ge_graph(), gp->value) || lx_fsm_globals_find_var(gp->value));
        vb->bg = COL_VAR_BG;
        vb->border = gp->value[0] && !known ? UI_HEX(0xe5534b) : UI_HEX(0x4a2c5a);
        vb->fg = gp->value[0] ? COL_VAR_TEXT : COL_DIM;
        if (gp->value[0] && !known) lx_strcpy(vb->tooltip, sizeof vb->tooltip, "This variable doesn't exist: pick another or add it in the Variables tab");
        if (GE.debug && run && gp->value[0]) {   /* live value */
            int vi = -1;
            for (int i = 0; i < run->var_count; i++) if (!strcmp(run->var_names[i], gp->value)) vi = i;
            if (vi >= 0) {
                char v[64];
                lx_fsm_value_format(&run->vars[vi], v, sizeof v, NULL);
                lx_strcpy(vb->hint, sizeof vb->hint, v);
            }
        }
    } else if (pd->type == LX_FSM_EVENT) {
        UiNode *eb = field_button(row, gp->value[0] ? gp->value : "None", event_param_browse);
        eb->user = b;
        eb->fg = gp->value[0] && strcmp(gp->value, "None") ? COL_TEXT : COL_DIM;
    } else {
        pb_load(b, pd->type, gp->value, pd->options);
        value_field(row, b, pd->type, pd->options);
    }
    if (!out && pd->type != LX_FSM_EVENT) {
        UiNode *eq = ui_button(ED.ui, row, "=", toggle_var);
        eq->w = 22;
        eq->h = 22;
        eq->font = UI_FONT_MONO;
        eq->radius = 3;
        eq->bg = gp->is_var ? COL_VAR_ON : UI_HEX(0x28292e);
        eq->fg = gp->is_var ? COL_VAR_TEXT : COL_DIM;
        eq->border = gp->is_var ? UI_HEX(0x7a4a8a) : COL_EDGE;
        eq->user = b;
        lx_strcpy(eq->tooltip, sizeof eq->tooltip, gp->is_var ? "Type a value instead of using a variable" : "Use a variable");
    } else {
        UiNode *sp = ui_box(ED.ui, row, UI_ROW);
        sp->w = 22;
    }
}

/* ======================================================================
 *  State tab
 * ====================================================================== */
static const char *const UPDATE_OPTS[] = {"Once (when entered)", "Every Frame", "Fixed Update (physics)", "Late Update (after movement)"};
static const char *const UPDATE_SHORT[] = {"on enter", "every frame", "fixed update", "late update"};

static void update_changed(UiNode *n) {
    PB *b = n->user;
    LxGraphAction *a = pb_action(b, NULL);
    if (!a) return;
    a->update = (LxFsmUpdate)b->i;
    ge_commit(true, b);
}
static void enabled_changed(UiNode *n) {
    PB *b = n->user;
    LxGraphAction *a = pb_action(b, NULL);
    if (!a) return;
    a->enabled = b->b;
    ge_commit(false, NULL);
}
static void fold_click(UiNode *n) {
    PB *b = n->user;
    LxGraphAction *a = pb_action(b, NULL);
    if (!a) return;
    a->folded = !a->folded;
    ge_commit(true, NULL);
}
static PB g_amenu;
static void am_move(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!pb_action(&g_amenu, NULL)) return;
    LxGraphState *st = &g->states[g_amenu.st];
    int to = g_amenu.ac + (int)n->tag;
    if (to < 0 || to >= st->action_count) return;
    lx_graph_move_action(st, g_amenu.ac, to);
    ge_commit(true, NULL);
}
static void am_remove(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    if (!pb_action(&g_amenu, NULL)) return;
    lx_graph_remove_action(&g->states[g_amenu.st], g_amenu.ac);
    ge_commit(true, NULL);
}
static void am_duplicate(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    const LxFsmActionDesc *d = NULL;
    LxGraphAction *a = pb_action(&g_amenu, &d);
    if (!a || !d) return;
    LxGraphAction copy = *a;
    LxGraphAction *na = lx_graph_add_action(&g->states[g_amenu.st], d, g_amenu.ac + 1);
    if (na) *na = copy;
    ge_commit(true, NULL);
}
static void am_reset(UiNode *n) {
    LX_UNUSED(n);
    LxGraph *g = ge_graph();
    const LxFsmActionDesc *d = NULL;
    LxGraphAction *a = pb_action(&g_amenu, &d);
    if (!a || !d) return;
    lx_graph_remove_action(&g->states[g_amenu.st], g_amenu.ac);
    lx_graph_add_action(&g->states[g_amenu.st], d, g_amenu.ac);
    ge_commit(true, NULL);
}
static void am_browse(UiNode *n) {
    LX_UNUSED(n);
    LxGraphAction *a = pb_action(&g_amenu, NULL);
    if (!a) return;
    lx_strcpy(GE.ab_sel, sizeof GE.ab_sel, a->type);
    GE.ab_dirty = true;
    ge_open_action_browser();
}
static void action_menu(UiNode *btn) {
    g_amenu = *(PB *)btn->user;
    UiNode *p = ui_popup(ED.ui, btn->x - 160 * ui_scale(ED.ui), btn->y + btn->ch, 180);
    if (!p) return;
    UiNode *it = ui_menu_item(ED.ui, p, "Move Up", NULL, am_move); it->tag = -1; it->icon = UI_ICON_CHEVRON_UP;
    it = ui_menu_item(ED.ui, p, "Move Down", NULL, am_move); it->tag = 1; it->icon = UI_ICON_CHEVRON_DOWN;
    it = ui_menu_item(ED.ui, p, "Duplicate", NULL, am_duplicate); it->icon = UI_ICON_COPY;
    it = ui_menu_item(ED.ui, p, "Reset", NULL, am_reset); it->icon = UI_ICON_REFRESH;
    it = ui_menu_item(ED.ui, p, "Show in Action Browser", NULL, am_browse); it->icon = UI_ICON_LIST;
    ui_menu_separator(ED.ui, p);
    it = ui_menu_item(ED.ui, p, "Remove", NULL, am_remove); it->icon = UI_ICON_TRASH;
}

static void action_running_frame(UiNode *n) {
    /* a dot that's lit while this action is still running (play mode) */
    PB *b = n->user;
    LxFsmRun *r = ge_run();
    bool on = false;
    if (r && r->state == b->st && b->st < r->bound_states && b->ac < r->bound_counts[b->st]) {
        const LxFsmBound *bd = &r->bound[b->st][b->ac];
        on = bd->entered && !bd->finished;
    }
    n->icon = on ? UI_ICON_PLAY : UI_ICON_NONE;
    n->icon_color = COL_OK;
}

static void action_block(UiNode *body, int si, int ai, const LxGraphAction *a, const LxFsmRun *run) {
    UiCtx *ui = ED.ui;
    const LxFsmActionDesc *d = lx_fsm_action_find(a->type);
    UiNode *wrap = ui_box(ui, body, UI_COL);
    wrap->gap = 2;
    UiNode *h = ui_box(ui, wrap, UI_ROW);
    h->h = 26;
    h->gap = 4;
    h->bg = UI_HEX(0x28292e);
    h->border = COL_EDGE;
    h->radius = 4;
    h->cross_align = UI_ALIGN_CENTER;
    ui_pad(h, 4, 0, 2, 0);
    PB *b = pb_new(PB_ENABLED);
    b->st = si; b->ac = ai; b->b = a->enabled;
    UiNode *cb = ui_checkbox(ui, h, "", &b->b);
    cb->user = b;
    cb->on_change = enabled_changed;
    lx_strcpy(cb->tooltip, sizeof cb->tooltip, "Enabled");
    UiNode *chev = ui_button(ui, h, "", fold_click);
    chev->icon = a->folded ? UI_ICON_CHEVRON_RIGHT : UI_ICON_CHEVRON_DOWN;
    chev->w = 20;
    chev->h = 20;
    chev->bg = 0;
    chev->user = b;
    UiNode *name = ui_button(ui, h, d ? d->name : a->type, fold_click);
    name->bg = 0;
    name->font = UI_FONT_BOLD;
    name->fg = !d ? UI_HEX(0xff6b5a) : a->enabled ? COL_TEXT : COL_DIM;
    name->center_text = false;
    name->user = b;
    if (d && d->description) lx_strcpy(name->tooltip, sizeof name->tooltip, d->description);
    char note[192] = "";
    if (!d) snprintf(note, sizeof note, "missing action");
    else if (a->folded) lx_graph_action_summary(a, d, note, sizeof note);
    else if (d->on_update && !(d->flags & LX_FSM_A_CONTINUOUS)) snprintf(note, sizeof note, "%s", UPDATE_SHORT[a->update & 3]);
    UiNode *nl = label(h, note, UI_FONT_SMALL, COL_DIM);
    nl->grow = 1;
    nl->clip = true;
    lx_strcpy(nl->tooltip, sizeof nl->tooltip, note);
    UiNode *dot = ui_label(ui, h, "");
    dot->icon = UI_ICON_NONE;
    dot->icon_size = 10;
    dot->w = 12;
    dot->user = b;
    dot->on_frame = action_running_frame;
    UiNode *gear = small_btn(h, UI_ICON_GEAR, "Action options", action_menu);
    gear->user = b;
    if (!d) { hint(wrap, "This action isn't available (a script that defined it may not be compiled). It's skipped when the graph runs."); return; }
    if (a->folded) return;
    UiNode *pb = ui_box(ui, wrap, UI_COL);
    pb->gap = 1;
    ui_pad(pb, 6, 2, 0, 6);
    if (d->description) { UiNode *dl = hint(pb, d->description); ui_pad(dl, 0, 0, 0, 4); }
    for (int k = 0; k < LX_FSM_MAX_PARAMS && d->params[k].name; k++) param_row(pb, si, ai, k, a, &d->params[k], run);
    if (d->on_update && !(d->flags & LX_FSM_A_CONTINUOUS)) {   /* actions that run until they're done always update every frame */
        UiNode *row = prow(pb, "Update", "When the action runs: once when the state is entered, or every frame until the state is left");
        PB *ub = pb_new(PB_UPDATE);
        ub->st = si; ub->ac = ai; ub->i = a->update & 3;
        UiNode *dd = ui_dropdown(ui, row, &ub->i, UPDATE_OPTS, 4);
        dd->grow = 1;
        dd->h = 22;
        dd->user = ub;
        dd->on_change = update_changed;
        UiNode *sp = ui_box(ui, row, UI_ROW);
        sp->w = 22;
    }
}

static void state_name_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->state_count) return;
    if (!b->s[0] || !lx_graph_rename_state(g, b->st, b->s)) {
        if (b->s[0]) ui_notify(ED.ui, "There's already a state called %s", b->s);
        lx_strcpy(b->s, sizeof b->s, g->states[b->st].name);
        return;
    }
    ge_commit(true, NULL);
}
static void state_desc_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->state_count) return;
    lx_strcpy(g->states[b->st].description, sizeof g->states[b->st].description, b->s);
    ge_commit(false, NULL);
}
static void state_seq_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->state_count) return;
    g->states[b->st].sequence = b->b;
    ge_commit(false, NULL);
}
static void color_click(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g) return;
    int c = (int)n->tag;
    if (GE.sel_comment >= 0 && GE.sel_comment < g->comment_count) g->comments[GE.sel_comment].color = c;
    for (int k = 0; k < GE.sel_count; k++) if (GE.sel[k] < g->state_count) g->states[GE.sel[k]].color = c;
    ge_commit(true, NULL);
}
static void color_row(UiNode *body, int current) {
    UiNode *row = prow(body, "Color", NULL);
    row->gap = 3;
    for (int i = 0; i < GE_COLORS; i++) {
        UiNode *b = ui_button(ED.ui, row, "", color_click);
        b->w = 17;
        b->h = 17;
        b->radius = 3;
        b->bg = i == 0 ? UI_HEX(0x34363c) : ge_state_color(i);
        b->bg_hover = ui_lerp_color(b->bg, UI_HEX(0xffffff), 0.2f);
        b->border = i == current ? COL_ACCENT : COL_EDGE;
        b->tag = i;
    }
}
static void trans_remove(UiNode *n) {
    LxGraph *g = ge_graph();
    LxGraphState *st = ge_sel_state();
    if (!g || !st) return;
    lx_graph_remove_transition(st, (int)n->tag);
    GE.sel_trans = -1;
    ge_commit(true, NULL);
}
static void trans_select(UiNode *n) { GE.sel_trans = (int)n->tag; }
static void open_ab(UiNode *n) { LX_UNUSED(n); ge_open_action_browser(); }
static void quick_add(UiNode *n) {
    PB *b = n->user;
    if (!b->s[0]) return;
    const LxFsmActionDesc *best = lx_fsm_action_find(b->s);
    for (int i = 0; !best && i < lx_fsm_action_count(); i++) {
        const LxFsmActionDesc *d = lx_fsm_action_at(i);
        if (d && lx_strcasestr_ascii(d->name, b->s)) best = d;
    }
    if (!best) { ui_notify(ED.ui, "No action matches \"%s\"", b->s); return; }
    ge_add_action(best->name);
}

static void comment_text_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || GE.sel_comment < 0 || GE.sel_comment >= g->comment_count) return;
    lx_strcpy(g->comments[GE.sel_comment].text, sizeof g->comments[GE.sel_comment].text, b->s);
    ge_commit(false, NULL);
}

static void build_state_tab(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxGraph *g = ge_graph();
    if (!g) { hint(body, "No graph."); return; }
    if (GE.sel_comment >= 0 && GE.sel_comment < g->comment_count) {
        heading(body, "Comment");
        PB *b = pb_new(PB_MISC);
        lx_strcpy(b->s, sizeof b->s, g->comments[GE.sel_comment].text);
        UiNode *row = prow(body, "Text", NULL);
        UiNode *tf = ui_textfield(ui, row, b->s, sizeof b->s);
        tf->grow = 1; tf->h = 22; tf->user = b; tf->on_change = comment_text_changed;
        color_row(body, g->comments[GE.sel_comment].color);
        return;
    }
    if (GE.sel_count > 1) {
        char t[64];
        snprintf(t, sizeof t, "%d states selected", GE.sel_count);
        heading(body, t);
        color_row(body, -1);
        hint(body, "Drag them together, delete them (Del), or copy them (Ctrl+C). Select one state to edit its actions.");
        return;
    }
    int si = ge_sel_index();
    if (si < 0) {
        hint(body, "Select a state in the graph to edit its actions and transitions.");
        return;
    }
    const LxGraphState *st = &g->states[si];
    LxFsmRun *run = ge_run();
    /* name, colour, description */
    UiNode *top = ui_box(ui, body, UI_ROW);
    top->h = 26;
    top->gap = 6;
    top->cross_align = UI_ALIGN_CENTER;
    PB *nb = pb_new(PB_MISC);
    nb->st = si;
    lx_strcpy(nb->s, sizeof nb->s, st->name);
    UiNode *tf = ui_textfield(ui, top, nb->s, LX_FSM_NAME);
    tf->grow = 1; tf->h = 24; tf->font = UI_FONT_BOLD; tf->user = nb; tf->on_change = state_name_changed;
    if (run && run->state == si) { UiNode *r = label(top, "running", UI_FONT_SMALL, COL_RUN); r->icon = UI_ICON_PLAY; r->icon_color = COL_RUN; }
    PB *db = pb_new(PB_MISC);
    db->st = si;
    lx_strcpy(db->s, sizeof db->s, st->description);
    UiNode *dtf = ui_textfield(ui, body, db->s, sizeof st->description < sizeof db->s ? (int)sizeof st->description : (int)sizeof db->s);
    dtf->h = 22; dtf->user = db; dtf->on_change = state_desc_changed;
    lx_strcpy(dtf->hint, sizeof dtf->hint, "Description (what this state does)");
    color_row(body, st->color);
    PB *sb = pb_new(PB_MISC);
    sb->st = si;
    sb->b = st->sequence;
    UiNode *row = prow(body, "Sequence", "Run the actions one after another instead of all at once");
    UiNode *sq = ui_checkbox(ui, row, "one action at a time", &sb->b);
    sq->user = sb;
    sq->on_change = state_seq_changed;
    /* transitions */
    heading(body, "Transitions");
    if (!st->trans_count) hint(body, "None yet: right-click the state › Add Transition, or pick an event in an action below.");
    for (int t = 0; t < st->trans_count; t++) {
        UiNode *tr = ui_box(ui, body, UI_ROW);
        tr->h = 22;
        tr->gap = 6;
        tr->cross_align = UI_ALIGN_CENTER;
        tr->radius = 3;
        tr->bg = GE.sel_trans == t ? UI_HEX(0x1f3050) : 0;
        tr->on_click = trans_select;
        tr->tag = t;
        UiNode *e = label(tr, st->trans[t].event, UI_FONT_BODY, COL_TEXT);
        e->w = 130;
        UiNode *arrow = label(tr, "", UI_FONT_BODY, COL_DIM);
        arrow->icon = UI_ICON_CHEVRON_RIGHT;
        arrow->w = 16;
        UiNode *to = label(tr, st->trans[t].to[0] ? st->trans[t].to : "(drag it onto a state)", UI_FONT_BODY, st->trans[t].to[0] ? COL_TEXT : COL_DIM);
        to->grow = 1;
        UiNode *x = small_btn(tr, UI_ICON_CLOSE, "Remove this transition", trans_remove);
        x->tag = t;
    }
    /* actions */
    heading(body, "Actions");
    if (!st->action_count) hint(body, "No actions: the state finishes straight away (FINISHED). Add actions from the Action Browser.");
    for (int a = 0; a < st->action_count; a++) action_block(body, si, a, &st->actions[a], run);
    UiNode *add = ui_box(ui, body, UI_ROW);
    add->h = 30;
    add->gap = 6;
    add->cross_align = UI_ALIGN_CENTER;
    ui_pad(add, 0, 6, 0, 0);
    PB *qb = pb_new(PB_MISC);
    UiNode *q = ui_textfield(ui, add, qb->s, 64);
    q->grow = 1; q->h = 24; q->user = qb; q->on_change = quick_add_enter; q->icon = UI_ICON_SEARCH;
    lx_strcpy(q->hint, sizeof q->hint, "Add action: type a name, Enter");
    UiNode *ab = ui_button(ui, add, "Browse...", open_ab);
    ab->w = 90;
    ab->h = 24;
    ab->icon = UI_ICON_LIST;
}

/* ======================================================================
 *  FSM tab
 * ====================================================================== */
static void graph_name_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || !b->s[0]) return;
    lx_strcpy(g->name, sizeof g->name, b->s);
    ge_commit(false, NULL);
}
static void graph_desc_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g) return;
    lx_strcpy(g->description, sizeof g->description, b->s);
    ge_commit(false, NULL);
}
static void start_pick(UiNode *it) {
    LxGraph *g = ge_graph();
    if (!g || !lx_graph_find_state(g, it->text)) return;
    lx_strcpy(g->start, sizeof g->start, it->text);
    ge_commit(true, NULL);
}
static void start_browse(UiNode *btn) {
    LxGraph *g = ge_graph();
    UiNode *p = ui_popup(ED.ui, btn->x, btn->y + btn->ch, btn->cw / ui_scale(ED.ui));
    if (!p || !g) return;
    p->owner = btn;
    for (int i = 0; i < g->state_count && i < 80; i++) ui_menu_item(ED.ui, p, g->states[i].name, NULL, start_pick);
}
static void problem_click(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g) return;
    int i = lx_graph_state_index(g, n->path);
    if (i >= 0) { ge_select_state(i, false); ge_frame_all(); }
}
static void build_fsm_tab(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxGraph *g = ge_graph();
    if (!g) {
        hint(body, GE.obj ? "This object has no State Graph. Click in the graph view to add one." : "Select an object with a State Graph, or a .lxgraph asset.");
        return;
    }
    PB *nb = pb_new(PB_MISC);
    lx_strcpy(nb->s, sizeof nb->s, g->name);
    UiNode *row = prow(body, "Name", NULL);
    UiNode *tf = ui_textfield(ui, row, nb->s, (int)sizeof g->name);
    tf->grow = 1; tf->h = 22; tf->user = nb; tf->on_change = graph_name_changed;
    PB *db = pb_new(PB_MISC);
    lx_strcpy(db->s, sizeof db->s, g->description);
    row = prow(body, "Description", NULL);
    tf = ui_textfield(ui, row, db->s, (int)sizeof db->s);
    tf->grow = 1; tf->h = 22; tf->user = db; tf->on_change = graph_desc_changed;
    row = prow(body, "Start State", "The state the graph enters when the game starts");
    field_button(row, g->start, start_browse);
    row = prow(body, "Asset", NULL);
    UiNode *al = label(row, GE.ref, UI_FONT_SMALL, COL_DIM);
    al->grow = 1;
    if (GE.obj && ED.scene && lx_entity_valid(ED.scene, GE.obj)) {
        row = prow(body, "On Object", NULL);
        char t[160];
        snprintf(t, sizeof t, "%s (graph %d)", lx_entity_name(ED.scene, GE.obj), GE.slot + 1);
        label(row, t, UI_FONT_BODY, COL_TEXT)->grow = 1;
    }
    int actions = 0, trans = 0;
    for (int i = 0; i < g->state_count; i++) { actions += g->states[i].action_count; trans += g->states[i].trans_count; }
    char stats[160];
    snprintf(stats, sizeof stats, "%d states · %d actions · %d transitions · %d global transitions · %d events · %d variables", g->state_count, actions, trans,
             g->global_count, g->event_count, g->var_count);
    hint(body, stats);
    heading(body, "Problems");
    char probs[32][160];
    int np = lx_graph_problems(g, probs, 32);
    if (!np) { UiNode *ok = label(body, "No problems found", UI_FONT_BODY, COL_OK); ok->icon = UI_ICON_SUCCESS; ok->icon_color = COL_OK; }
    for (int i = 0; i < np && i < 32; i++) {
        UiNode *pl = ui_button(ui, body, probs[i], problem_click);
        pl->icon = UI_ICON_WARNING;
        pl->icon_color = UI_HEX(0xffb03a);
        pl->bg = 0;
        pl->center_text = false;
        pl->font = UI_FONT_SMALL;
        pl->h = 22;
        /* "State: message" -> the state to select */
        const char *colon = strchr(probs[i], ':');
        if (colon) { size_t l = (size_t)(colon - probs[i]); if (l >= sizeof pl->path) l = sizeof pl->path - 1; memcpy(pl->path, probs[i], l); pl->path[l] = 0; }
    }
    LxFsmRun *run = ge_run();
    if (run) {
        heading(body, "Running");
        char t[200];
        snprintf(t, sizeof t, "State: %s  (%.1f s)", run->state >= 0 && run->state < g->state_count ? g->states[run->state].name : "-", (double)run->state_time);
        label(body, t, UI_FONT_BODY, COL_RUN);
        if (run->last_event[0]) { snprintf(t, sizeof t, "Last event: %s", run->last_event); label(body, t, UI_FONT_SMALL, COL_DIM); }
        heading(body, "History");
        for (int i = run->history_count - 1; i >= 0 && i >= run->history_count - 12; i--) {
            const char *sn = run->history[i].state >= 0 && run->history[i].state < g->state_count ? g->states[run->history[i].state].name : "?";
            snprintf(t, sizeof t, "%s  ‹  %s", sn, run->history[i].event[0] ? run->history[i].event : "START");
            label(body, t, UI_FONT_SMALL, i == run->history_count - 1 ? COL_TEXT : COL_DIM);
        }
    } else if (ED.play != ED_EDIT) hint(body, "Playing: select an object that runs this graph to see it live.");
}

/* ======================================================================
 *  Events tab
 * ====================================================================== */
static void add_event_click(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || !b->s[0]) return;
    char ev[LX_FSM_NAME];
    size_t k = 0;
    for (const char *p = b->s; *p && k + 1 < sizeof ev; p++) ev[k++] = *p == ' ' ? '_' : (char)toupper((unsigned char)*p);
    ev[k] = 0;
    if (lx_graph_find_event(g, ev) || lx_fsm_is_system_event(ev)) { ui_notify(ED.ui, "%s already exists", ev); return; }
    lx_graph_add_event(g, ev);
    b->s[0] = 0;
    ge_commit(true, NULL);
}
static void remove_event(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g) return;
    lx_graph_remove_event(g, (int)n->tag);
    ge_commit(true, NULL);
}
static void event_global_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->event_count) return;
    g->events[b->st].global = b->b;
    ge_commit(false, NULL);
}
static void add_gevent_click(UiNode *n) {
    PB *b = n->user;
    LxGlobals *gl = lx_fsm_globals();
    if (!gl || !b->s[0]) return;
    char ev[LX_FSM_NAME];
    size_t k = 0;
    for (const char *p = b->s; *p && k + 1 < sizeof ev; p++) ev[k++] = *p == ' ' ? '_' : (char)toupper((unsigned char)*p);
    ev[k] = 0;
    for (int i = 0; i < gl->event_count; i++) if (!strcmp(gl->events[i].name, ev)) return;
    LxGraphEvent e;
    memset(&e, 0, sizeof e);
    lx_strcpy(e.name, sizeof e.name, ev);
    e.global = true;
    LX_VEC_PUSH(gl->events, gl->event_count, gl->event_cap, e);
    lx_fsm_globals_save();
    GE.insp_dirty = true;
}
static void remove_gevent(UiNode *n) {
    LxGlobals *gl = lx_fsm_globals();
    int i = (int)n->tag;
    if (!gl || i < 0 || i >= gl->event_count) return;
    memmove(&gl->events[i], &gl->events[i + 1], sizeof *gl->events * (size_t)(gl->event_count - i - 1));
    gl->event_count--;
    lx_fsm_globals_save();
    GE.insp_dirty = true;
}
static void add_row(UiNode *body, const char *hint_text, void (*cb)(UiNode *)) {
    UiNode *row = ui_box(ED.ui, body, UI_ROW);
    row->h = 28;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    PB *b = pb_new(PB_MISC);
    UiNode *tf = ui_textfield(ED.ui, row, b->s, LX_FSM_NAME);
    tf->grow = 1; tf->h = 24; tf->user = b; tf->on_change = add_on_enter;   /* Enter: the Add button next to it */
    lx_strcpy(tf->hint, sizeof tf->hint, hint_text);
    UiNode *bt = ui_button(ED.ui, row, "Add", cb);
    bt->w = 56; bt->h = 24; bt->user = b;
}
static void build_events_tab(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxGraph *g = ge_graph();
    if (!g) { hint(body, "No graph."); return; }
    hint(body, "Events move the graph along its transitions. Send them with Send Event, from scripts with lx_fsm_send_event / "
               "lx_send_message, or from other actions (Key Event, Trigger Event...).");
    add_row(body, "New event, e.g. OPEN_DOOR", add_event_click);
    for (int i = 0; i < g->event_count; i++) {
        UiNode *row = ui_box(ui, body, UI_ROW);
        row->h = 24;
        row->gap = 6;
        row->cross_align = UI_ALIGN_CENTER;
        UiNode *n = label(row, g->events[i].name, UI_FONT_BODY, COL_TEXT);
        n->grow = 1;
        char u[32];
        int uses = lx_graph_event_uses(g, g->events[i].name);
        snprintf(u, sizeof u, uses ? "used %d×" : "unused", uses);
        label(row, u, UI_FONT_SMALL, uses ? COL_DIM : UI_HEX(0xffb03a))->w = 60;
        PB *b = pb_new(PB_MISC);
        b->st = i;
        b->b = g->events[i].global;
        UiNode *cb = ui_checkbox(ui, row, "global", &b->b);
        cb->user = b;
        cb->on_change = event_global_changed;
        lx_strcpy(cb->tooltip, sizeof cb->tooltip, "Other graphs and scripts may send it (it's listed for them)");
        UiNode *x = small_btn(row, UI_ICON_TRASH, "Delete the event and its transitions", remove_event);
        x->tag = i;
    }
    heading(body, "Global Events (every graph)");
    add_row(body, "New global event", add_gevent_click);
    LxGlobals *gl = lx_fsm_globals();
    for (int i = 0; gl && i < gl->event_count; i++) {
        UiNode *row = ui_box(ui, body, UI_ROW);
        row->h = 22;
        row->cross_align = UI_ALIGN_CENTER;
        label(row, gl->events[i].name, UI_FONT_BODY, COL_TEXT)->grow = 1;
        UiNode *x = small_btn(row, UI_ICON_TRASH, "Delete", remove_gevent);
        x->tag = i;
    }
    heading(body, "System Events");
    hint(body, "Sent by the engine: FINISHED when every action in a state is done; COLLISION_ENTER / EXIT and TRIGGER_ENTER / EXIT "
               "from physics (Get Collision Info tells you what was hit).");
}

/* ======================================================================
 *  Variables tab
 * ====================================================================== */
static const char *const VAR_TYPES[] = {"Float", "Int", "Bool", "String", "Vector3", "Color", "Object", "Asset"};
static const LxFsmType VAR_TYPE_IDS[] = {LX_FSM_FLOAT, LX_FSM_INT, LX_FSM_BOOL, LX_FSM_STRING, LX_FSM_VEC3, LX_FSM_COLOR, LX_FSM_OBJECT, LX_FSM_ASSET};
static int g_new_var_type;
static PB *g_new_var_pb;
static void add_var(bool global) {
    if (!g_new_var_pb || !g_new_var_pb->s[0]) return;
    char name[LX_FSM_NAME];
    lx_strcpy(name, sizeof name, g_new_var_pb->s);
    LxFsmType t = VAR_TYPE_IDS[g_new_var_type & 7];
    if (global) {
        LxGlobals *gl = lx_fsm_globals();
        if (!gl || lx_fsm_globals_find_var(name)) { ui_notify(ED.ui, "%s already exists", name); return; }
        LxGraphVar v;
        memset(&v, 0, sizeof v);
        lx_strcpy(v.name, sizeof v.name, name);
        v.type = t;
        lx_strcpy(v.value, sizeof v.value, lx_fsm_default_value(t));
        LX_VEC_PUSH(gl->vars, gl->var_count, gl->var_cap, v);
        lx_fsm_globals_save();
        GE.insp_dirty = true;
        return;
    }
    LxGraph *g = ge_graph();
    if (!g) return;
    if (lx_graph_find_var(g, name)) { ui_notify(ED.ui, "%s already exists", name); return; }
    lx_graph_add_var(g, name, t);
    ge_commit(true, NULL);
}
static void add_var_click(UiNode *n) { LX_UNUSED(n); add_var(false); }
static void add_gvar_click(UiNode *n) { LX_UNUSED(n); add_var(true); }
static void remove_var(UiNode *n) {
    LxGraph *g = ge_graph();
    if (!g) return;
    lx_graph_remove_var(g, (int)n->tag);
    ge_commit(true, NULL);
}
static void remove_gvar(UiNode *n) {
    LxGlobals *gl = lx_fsm_globals();
    int i = (int)n->tag;
    if (!gl || i < 0 || i >= gl->var_count) return;
    memmove(&gl->vars[i], &gl->vars[i + 1], sizeof *gl->vars * (size_t)(gl->var_count - i - 1));
    gl->var_count--;
    lx_fsm_globals_save();
    GE.insp_dirty = true;
}
static void var_name_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->var_count) return;
    if (!b->name[0] || !lx_graph_rename_var(g, b->st, b->name)) { lx_strcpy(b->name, sizeof b->name, g->vars[b->st].name); return; }
    ge_commit(true, NULL);
}
static void var_type_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->var_count) return;
    LxFsmType t = VAR_TYPE_IDS[b->p & 7];
    g->vars[b->st].type = t;
    lx_strcpy(g->vars[b->st].value, sizeof g->vars[b->st].value, lx_fsm_default_value(t));
    ge_commit(true, NULL);
}
static void var_inspector_changed(UiNode *n) {
    PB *b = n->user;
    LxGraph *g = ge_graph();
    if (!g || b->st < 0 || b->st >= g->var_count) return;
    g->vars[b->st].inspector = b->b;
    ge_commit(false, NULL);
    ED.inspector_dirty = true;   /* the object's Inspector shows exposed variables */
}
static void live_value_frame(UiNode *n) {
    PB *b = n->user;
    char v[96] = "";
    if (b->kind == PB_VAR) {
        LxFsmRun *r = ge_run();
        if (r && b->st < r->var_count) lx_fsm_value_format(&r->vars[b->st], v, sizeof v, NULL);
    } else if (ED.play != ED_EDIT) {
        LxFsmValue *gv = lx_fsm_global_value(b->st);
        if (gv) lx_fsm_value_format(gv, v, sizeof v, NULL);
    }
    if (strcmp(n->text, v)) ui_set_text(n, "%s", v);
}
static void var_block(UiNode *body, int i, const LxGraphVar *v, bool global) {
    UiCtx *ui = ED.ui;
    UiNode *wrap = ui_box(ui, body, UI_COL);
    wrap->gap = 2;
    wrap->bg = UI_HEX(0x1d1e22);
    wrap->radius = 4;
    ui_pad(wrap, 6, 4, 4, 4);
    UiNode *row = ui_box(ui, wrap, UI_ROW);
    row->h = 24;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    if (global) {
        UiNode *n = label(row, v->name, UI_FONT_BOLD, COL_VAR_TEXT);
        n->grow = 1;
        label(row, lx_fsm_type_name(v->type), UI_FONT_SMALL, COL_DIM);
        UiNode *x = small_btn(row, UI_ICON_TRASH, "Delete", remove_gvar);
        x->tag = i;
    } else {
        PB *nb = pb_new(PB_MISC);
        nb->st = i;
        lx_strcpy(nb->name, sizeof nb->name, v->name);
        UiNode *tf = ui_textfield(ui, row, nb->name, LX_FSM_NAME);
        tf->grow = 1; tf->h = 22; tf->user = nb; tf->on_change = var_name_changed; tf->fg = COL_VAR_TEXT; tf->font = UI_FONT_BOLD;
        PB *tb = pb_new(PB_MISC);
        tb->st = i;
        for (int k = 0; k < 8; k++) if (VAR_TYPE_IDS[k] == v->type) tb->p = k;
        UiNode *dd = ui_dropdown(ui, row, &tb->p, VAR_TYPES, 8);
        dd->w = 90; dd->h = 22; dd->user = tb; dd->on_change = var_type_changed;
        char u[32];
        int uses = lx_graph_var_uses(ge_graph(), v->name);
        snprintf(u, sizeof u, uses ? "used %d×" : "unused", uses);
        label(row, u, UI_FONT_SMALL, COL_DIM)->w = 52;
        UiNode *x = small_btn(row, UI_ICON_TRASH, "Delete", remove_var);
        x->tag = i;
    }
    UiNode *vr = ui_box(ui, wrap, UI_ROW);
    vr->h = 24;
    vr->gap = 4;
    vr->cross_align = UI_ALIGN_CENTER;
    UiNode *vl = label(vr, "Starts at", UI_FONT_SMALL, COL_DIM);
    vl->w = 60;
    PB *b = pb_new(global ? PB_GVAR : PB_VAR);
    b->st = i;
    pb_load(b, v->type, v->value, NULL);
    value_field(vr, b, v->type, NULL);
    if (!global) {
        PB *ib = pb_new(PB_MISC);
        ib->st = i;
        ib->b = v->inspector;
        UiNode *cb = ui_checkbox(ui, vr, "Inspector", &ib->b);
        cb->user = ib;
        cb->on_change = var_inspector_changed;
        lx_strcpy(cb->tooltip, sizeof cb->tooltip, "Show it in the object's Inspector, so each object can use its own value");
    }
    if (ED.play != ED_EDIT && GE.debug) {
        UiNode *lr = ui_box(ui, wrap, UI_ROW);
        lr->h = 20;
        label(lr, "Now", UI_FONT_SMALL, COL_RUN)->w = 60;
        PB *lb = pb_new(global ? PB_GVAR : PB_VAR);
        lb->st = i;
        UiNode *live = label(lr, "", UI_FONT_MONO, COL_RUN);
        live->grow = 1;
        live->user = lb;
        live->on_frame = live_value_frame;
    }
}
static void build_vars_tab(UiNode *body) {
    UiCtx *ui = ED.ui;
    LxGraph *g = ge_graph();
    if (!g) { hint(body, "No graph."); return; }
    hint(body, "Variables hold values for this graph's actions (click \"=\" next to a parameter to use one). Tick Inspector to let each "
               "object set its own value.");
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->h = 28;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    PB *nb = pb_new(PB_MISC);
    g_new_var_pb = nb;
    UiNode *tf = ui_textfield(ui, row, nb->s, LX_FSM_NAME);
    tf->grow = 1; tf->h = 24; tf->on_change = add_var_enter;
    lx_strcpy(tf->hint, sizeof tf->hint, "New variable name");
    UiNode *dd = ui_dropdown(ui, row, &g_new_var_type, VAR_TYPES, 8);
    dd->w = 90; dd->h = 24;
    UiNode *bt = ui_button(ui, row, "Add", add_var_click);
    bt->w = 56; bt->h = 24;
    for (int i = 0; i < g->var_count; i++) var_block(body, i, &g->vars[i], false);
    heading(body, "Global Variables (every graph)");
    row = ui_box(ui, body, UI_ROW);
    row->h = 28;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    UiNode *gb = ui_button(ui, row, "Add the name above as a global", add_gvar_click);
    gb->h = 24;
    gb->grow = 1;
    gb->icon = UI_ICON_GLOBE;
    LxGlobals *gl = lx_fsm_globals();
    for (int i = 0; gl && i < gl->var_count; i++) var_block(body, i, &gl->vars[i], true);
}

/* ======================================================================
 *  the inspector panel
 * ====================================================================== */
static UiNode *tab_body(UiNode *tabs, const char *name) {
    UiNode *p = ui_box(ED.ui, tabs, UI_COL);
    lx_strcpy(p->text, sizeof p->text, name);
    p->bg = UI_HEX(0x232428);
    UiNode *b = ui_box(ED.ui, p, UI_COL);
    b->grow = 1;
    b->scroll = true;
    b->gap = 3;
    ui_pad(b, 8, 8, 8, 12);
    return b;
}
UiNode *ge_inspector(UiNode *parent) {
    UiNode *tabs = ui_tabs(ED.ui, parent);
    tabs->tab_index = 1;
    GE.insp_tabs = tabs;
    GE.insp_fsm = tab_body(tabs, "FSM");
    GE.insp_state = tab_body(tabs, "State");
    GE.insp_events = tab_body(tabs, "Events");
    GE.insp_vars = tab_body(tabs, "Variables");
    ui_track(&GE.insp_tabs);
    ui_track(&GE.insp_fsm);
    ui_track(&GE.insp_state);
    ui_track(&GE.insp_events);
    ui_track(&GE.insp_vars);
    return tabs;
}
void ge_inspector_frame(void) {
    static double last_live;
    /* while playing with Debug on, the FSM tab's running info refreshes a few times a second */
    if (ge_run() && GE.debug && lx_time() - last_live > 0.25 && !ui_wants_keyboard(ED.ui) && !lx_mouse_down(LX_MOUSE_LEFT)) {
        last_live = lx_time();
        if (GE.insp_tabs && GE.insp_tabs->tab_index == 0) GE.insp_dirty = true;
    }
    if (!GE.insp_dirty || !GE.insp_fsm || !GE.insp_state || !GE.insp_events || !GE.insp_vars) return;
    GE.insp_dirty = false;
    float keep[4] = {GE.insp_fsm->scroll_pos, GE.insp_state->scroll_pos, GE.insp_events->scroll_pos, GE.insp_vars->scroll_pos};
    ui_clear(ED.ui, GE.insp_fsm);
    ui_clear(ED.ui, GE.insp_state);
    ui_clear(ED.ui, GE.insp_events);
    ui_clear(ED.ui, GE.insp_vars);
    g_pb_n = 0;
    build_fsm_tab(GE.insp_fsm);
    build_state_tab(GE.insp_state);
    build_events_tab(GE.insp_events);
    build_vars_tab(GE.insp_vars);
    GE.insp_fsm->scroll_pos = keep[0];
    GE.insp_state->scroll_pos = keep[1];
    GE.insp_events->scroll_pos = keep[2];
    GE.insp_vars->scroll_pos = keep[3];
}

/* ======================================================================
 *  Action Browser
 * ====================================================================== */
static bool is_fav(const char *name) {
    for (int i = 0; i < GE.fav_count; i++) if (!strcmp(GE.favs[i], name)) return true;
    return false;
}
static bool cat_closed(const char *c) {
    for (int i = 0; i < GE.ab_closed_count; i++) if (!strcmp(GE.ab_closed_cats[i], c)) return true;
    return false;
}
void ge_note_recent(const char *type) {
    for (int i = 0; i < GE.recent_count; i++)
        if (!strcmp(GE.recent[i], type)) { memmove(GE.recent + i, GE.recent + i + 1, sizeof GE.recent[0] * (size_t)(GE.recent_count - i - 1)); GE.recent_count--; break; }
    if (GE.recent_count == 12) GE.recent_count--;
    memmove(GE.recent + 1, GE.recent, sizeof GE.recent[0] * (size_t)GE.recent_count);
    lx_strcpy(GE.recent[0], sizeof GE.recent[0], type);
    GE.recent_count++;
    ge_prefs_save();
}

void ge_add_action(const char *type) {
    LxGraph *g = ge_graph();
    int si = ge_sel_index();
    const LxFsmActionDesc *d = lx_fsm_action_find(type);
    if (!g || !d) return;
    if (si < 0 || GE.sel_count != 1) { ui_notify(ED.ui, "Select one state first"); return; }
    lx_graph_add_action(&g->states[si], d, -1);
    ge_note_recent(type);
    if (GE.insp_tabs) GE.insp_tabs->tab_index = 1;
    ge_commit(true, NULL);
    /* scroll the State tab to the new action */
    if (GE.insp_state) GE.insp_state->scroll_pos = 1e9f;
}

static void ab_item_click(UiNode *n) {
    lx_strcpy(GE.ab_sel, sizeof GE.ab_sel, n->path);
    GE.ab_dirty = true;
}
static void ab_item_double(UiNode *n) { ge_add_action(n->path); }
static void ab_cat_click(UiNode *n) {
    const char *c = n->path;
    for (int i = 0; i < GE.ab_closed_count; i++)
        if (!strcmp(GE.ab_closed_cats[i], c)) {
            memmove(GE.ab_closed_cats + i, GE.ab_closed_cats + i + 1, sizeof GE.ab_closed_cats[0] * (size_t)(GE.ab_closed_count - i - 1));
            GE.ab_closed_count--;
            GE.ab_dirty = true;
            return;
        }
    if (GE.ab_closed_count < 24) lx_strcpy(GE.ab_closed_cats[GE.ab_closed_count++], sizeof GE.ab_closed_cats[0], c);
    GE.ab_dirty = true;
}
static void ab_add_click(UiNode *n) { LX_UNUSED(n); if (GE.ab_sel[0]) ge_add_action(GE.ab_sel); }
static void ab_fav_click(UiNode *n) {
    LX_UNUSED(n);
    if (!GE.ab_sel[0]) return;
    for (int i = 0; i < GE.fav_count; i++)
        if (!strcmp(GE.favs[i], GE.ab_sel)) {
            memmove(GE.favs + i, GE.favs + i + 1, sizeof GE.favs[0] * (size_t)(GE.fav_count - i - 1));
            GE.fav_count--;
            GE.ab_dirty = true;
            ge_prefs_save();
            return;
        }
    if (GE.fav_count < 32) lx_strcpy(GE.favs[GE.fav_count++], sizeof GE.favs[0], GE.ab_sel);
    GE.ab_dirty = true;
    ge_prefs_save();
}

static bool matches(const LxFsmActionDesc *d, const char *q) {
    if (!q[0]) return true;
    return lx_strcasestr_ascii(d->name, q) || (d->category && lx_strcasestr_ascii(d->category, q)) || (d->description && lx_strcasestr_ascii(d->description, q));
}
static void ab_item(UiNode *list, const LxFsmActionDesc *d, int indent) {
    UiNode *it = ui_button(ED.ui, list, d->name, ab_item_click);
    it->on_double_click = ab_item_double;
    it->h = 22;
    it->center_text = false;
    it->radius = 3;
    ui_pad(it, (float)indent, 0, 4, 0);
    bool sel = !strcmp(GE.ab_sel, d->name);
    it->bg = sel ? UI_HEX(0x2b4a86) : 0;
    if (sel) { it->bg_hover = UI_HEX(0x34589c); it->bg_active = UI_HEX(0x34589c); }
    it->fg = sel ? UI_HEX(0xffffff) : COL_TEXT;
    it->drag_type = NULL;
    lx_strcpy(it->path, sizeof it->path, d->name);
    if (d->description) lx_strcpy(it->tooltip, sizeof it->tooltip, d->description);
    if (is_fav(d->name)) { it->icon = UI_ICON_SUCCESS; it->icon_color = UI_HEX(0xffb03a); it->icon_size = 11; }
}
static UiNode *ab_cat(UiNode *list, const char *name, int count, bool open, bool searching) {
    UiNode *h = ui_button(ED.ui, list, "", searching ? NULL : ab_cat_click);
    char t[96];
    snprintf(t, sizeof t, "%s   %d", name, count);
    ui_set_text(h, "%s", t);
    h->h = 24;
    h->center_text = false;
    h->icon = open ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_RIGHT;
    h->icon_size = 11;
    h->bg = UI_HEX(0x2a2c31);
    h->border = COL_EDGE;
    h->radius = 4;
    h->font = UI_FONT_BOLD;
    lx_strcpy(h->path, sizeof h->path, name);
    return h;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void ab_build_list(void) {
    UiNode *list = GE.ab_list;
    if (!list) return;
    float keep = list->scroll_pos;
    ui_clear(ED.ui, list);
    const char *q = GE.ab_search;
    bool searching = q[0] != 0;
    int total = lx_fsm_action_count();
    /* favourites and recent */
    if (!searching) {
        const char *special[2] = {"Favourites", "Recent"};
        for (int s = 0; s < 2; s++) {
            int n = s == 0 ? GE.fav_count : GE.recent_count;
            int have = 0;
            for (int i = 0; i < n; i++) have += lx_fsm_action_find(s == 0 ? GE.favs[i] : GE.recent[i]) != NULL;
            if (!have) continue;
            bool open = !cat_closed(special[s]);
            ab_cat(list, special[s], have, open, false);
            for (int i = 0; open && i < n; i++) {
                const LxFsmActionDesc *d = lx_fsm_action_find(s == 0 ? GE.favs[i] : GE.recent[i]);
                if (d) ab_item(list, d, 26);
            }
        }
    }
    /* categories, sorted */
    const char *cats[64];
    int nc = 0;
    for (int i = 0; i < total; i++) {
        const LxFsmActionDesc *d = lx_fsm_action_at(i);
        const char *c = d && d->category && d->category[0] ? d->category : "Other";
        bool dup = false;
        for (int k = 0; k < nc; k++) dup |= !strcmp(cats[k], c);
        if (!dup && nc < 64) cats[nc++] = c;
    }
    qsort(cats, (size_t)nc, sizeof cats[0], cmp_str);
    int shown = 0;
    for (int c = 0; c < nc; c++) {
        const LxFsmActionDesc *items[256];
        int ni = 0;
        for (int i = 0; i < total && ni < 256; i++) {
            const LxFsmActionDesc *d = lx_fsm_action_at(i);
            const char *dc = d && d->category && d->category[0] ? d->category : "Other";
            if (d && !strcmp(dc, cats[c]) && matches(d, q)) items[ni++] = d;
        }
        if (!ni) continue;
        for (int a = 0; a < ni; a++)
            for (int b = a + 1; b < ni; b++) if (strcmp(items[b]->name, items[a]->name) < 0) { const LxFsmActionDesc *t = items[a]; items[a] = items[b]; items[b] = t; }
        bool open = searching || !cat_closed(cats[c]);
        ab_cat(list, cats[c], ni, open, searching);
        for (int a = 0; open && a < ni; a++) ab_item(list, items[a], 26);
        shown += ni;
        if (searching && !GE.ab_sel[0] && ni) { lx_strcpy(GE.ab_sel, sizeof GE.ab_sel, items[0]->name); GE.ab_dirty = true; }   /* highlighted next frame */
    }
    if (!shown) hint(list, "No action matches. Try a shorter word.");
    list->scroll_pos = keep;
}

static void ab_build_details(void) {
    UiNode *det = GE.ab_details;
    if (!det) return;
    ui_clear(ED.ui, det);
    const LxFsmActionDesc *d = lx_fsm_action_find(GE.ab_sel);
    if (!d) { hint(det, "Pick an action to see what it does. Double-click adds it to the selected state."); return; }
    /* the text scrolls in its own area; the buttons stay pinned underneath, however long it is */
    UiNode *info = ui_box(ED.ui, det, UI_COL);
    info->grow = 1;
    info->scroll = true;
    info->gap = 4;
    UiNode *row = ui_box(ED.ui, info, UI_ROW);
    row->h = 24;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    label(row, d->name, UI_FONT_BOLD, COL_TEXT)->grow = 1;
    label(row, d->category ? d->category : "", UI_FONT_SMALL, COL_DIM);
    if (d->description) hint(info, d->description);
    /* parameters, briefly */
    char ps[400] = "";
    for (int k = 0; k < LX_FSM_MAX_PARAMS && d->params[k].name; k++) {
        size_t l = strlen(ps);
        snprintf(ps + l, sizeof ps - l, "%s%s", k ? ", " : "Parameters: ", d->params[k].name);
    }
    if (ps[0]) hint(info, ps);
    /* where it's used: every graph asset in the project */
    char used[480] = "";
    int uses = 0;
    LxGraph *gs[64];
    int ng = lx_graphs_loaded(gs, 64);
    for (int i = 0; i < ng; i++)
        for (int s = 0; s < gs[i]->state_count; s++)
            for (int a = 0; a < gs[i]->states[s].action_count; a++)
                if (!strcmp(gs[i]->states[s].actions[a].type, d->name)) {
                    if (uses < 6) {
                        size_t l = strlen(used);
                        snprintf(used + l, sizeof used - l, "%s%s › %s", uses ? " · " : "Used in: ", gs[i]->name, gs[i]->states[s].name);
                    }
                    uses++;
                    a = gs[i]->states[s].action_count;
                }
    if (uses > 6) { size_t l = strlen(used); snprintf(used + l, sizeof used - l, " and %d more", uses - 6); }
    hint(info, uses ? used : "Not used in any open graph yet.");
    row = ui_box(ED.ui, det, UI_ROW);
    row->h = 30;
    row->gap = 6;
    row->cross_align = UI_ALIGN_CENTER;
    UiNode *fav = ui_button(ED.ui, row, is_fav(d->name) ? "Unfavourite" : "Favourite", ab_fav_click);
    fav->w = 110;
    fav->h = 26;
    fav->icon = UI_ICON_SUCCESS;
    fav->icon_color = is_fav(d->name) ? UI_HEX(0xffb03a) : COL_DIM;
    ui_spacer(ED.ui, row);
    UiNode *add = ui_button(ED.ui, row, "Add Action To State", ab_add_click);
    add->w = 170;
    add->h = 26;
    add->bg = UI_HEX(0x3d7cf5);
    add->bg_hover = UI_HEX(0x5a92ff);
    add->fg = UI_HEX(0xffffff);
    add->font = UI_FONT_BOLD;
    add->disabled = ge_sel_index() < 0 || GE.sel_count != 1;
    if (add->disabled) lx_strcpy(add->tooltip, sizeof add->tooltip, "Select one state in the graph first");
}

static char g_ab_last_search[64];
static int g_ab_last_count = -1;
static int g_ab_last_sel = -2;
static void ab_frame(UiNode *n) {
    LX_UNUSED(n);
    int selk = GE.sel_count == 1 ? GE.sel[0] : -1;
    if (strcmp(g_ab_last_search, GE.ab_search) || g_ab_last_count != lx_fsm_action_count()) {
        lx_strcpy(g_ab_last_search, sizeof g_ab_last_search, GE.ab_search);
        g_ab_last_count = lx_fsm_action_count();
        if (GE.ab_search[0]) GE.ab_sel[0] = 0;   /* the first match gets picked */
        GE.ab_dirty = true;
    }
    if (selk != g_ab_last_sel) { g_ab_last_sel = selk; GE.ab_dirty = true; }
    if (!GE.ab_dirty) return;
    GE.ab_dirty = false;
    ab_build_list();
    ab_build_details();
}
static void ab_search_enter(UiNode *n) { LX_UNUSED(n); if (enter_pressed() && GE.ab_sel[0]) ge_add_action(GE.ab_sel); }
static void ab_closed(UiNode *win) { LX_UNUSED(win); GE.ab_win = NULL; GE.ab_list = NULL; GE.ab_details = NULL; }

static void load_all_graphs(void) {
    /* so "Used in" can list every graph in the project */
    static uint64_t last;
    uint64_t now = (uint64_t)lx_time();
    if (last && now - last < 10) return;
    last = now;
    char stack[16][LX_PATH_MAX];
    int sp = 0, loaded = 0;
    lx_strcpy(stack[sp++], LX_PATH_MAX, ED.project.assets);
    while (sp && loaded < 200) {
        char dir[LX_PATH_MAX];
        lx_strcpy(dir, sizeof dir, stack[--sp]);
        LxDirEntry *e;
        int n = lx_dir_list(dir, &e);
        for (int i = 0; i < n; i++) {
            if (e[i].name[0] == '.') continue;
            char full[LX_PATH_MAX];
            lx_path_join(full, sizeof full, dir, e[i].name);
            if (e[i].is_dir) { if (sp < 16) lx_strcpy(stack[sp++], LX_PATH_MAX, full); continue; }
            if (ed_icon_for_file(e[i].name, false) != UI_ICON_GRAPH) continue;
            char rel[LX_REF_MAX];
            ed_relative_asset(rel, sizeof rel, full);
            lx_asset_graph(rel);
            loaded++;
        }
        lx_free(e);
    }
}

void ge_open_action_browser(void) {
    if (GE.ab_win) { ui_window_front(ED.ui, GE.ab_win); return; }
    UiCtx *ui = ED.ui;
    load_all_graphs();
    float s = ui_scale(ui);
    float x = GE.canvas ? GE.canvas->x + GE.canvas->cw - 400 * s : 200, y = GE.canvas ? GE.canvas->y + 20 * s : 120;
    UiNode *body = ui_window(ui, "Action Browser", lx_maxf(10, x), y, 390, 640);
    if (!body) return;
    GE.ab_win = body->parent;
    GE.ab_win->on_change = ab_closed;
    ui_track(&GE.ab_win);
    body->on_frame = ab_frame;
    ui_pad(body, 8, 8, 8, 8);
    body->gap = 6;
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->h = 28;
    row->gap = 6;
    UiNode *sf = ui_textfield(ui, row, GE.ab_search, sizeof GE.ab_search);
    sf->grow = 1;
    sf->h = 26;
    sf->icon = UI_ICON_SEARCH;
    sf->on_change = ab_search_enter;
    lx_strcpy(sf->hint, sizeof sf->hint, "Search actions (Enter adds the first)");
    ui_focus(ui, sf);
    UiNode *list = ui_box(ui, body, UI_COL);
    list->grow = 1;
    list->scroll = true;
    list->gap = 2;
    GE.ab_list = list;
    ui_track(&GE.ab_list);
    UiNode *det = ui_box(ui, body, UI_COL);
    det->h = 200;
    det->gap = 4;
    det->bg = UI_HEX(0x1d1e22);
    det->radius = 4;
    ui_pad(det, 10, 8, 10, 8);
    GE.ab_details = det;
    ui_track(&GE.ab_details);
    GE.ab_dirty = true;
    g_ab_last_count = -1;
}
void ge_action_browser_toggle(void) {
    if (GE.ab_win) { UiNode *w = GE.ab_win; ab_closed(w); ui_remove(ED.ui, w); return; }
    ge_open_action_browser();
}

/* ======================================================================
 *  preferences (per project, Library/StateGraphEditor.txt)
 * ====================================================================== */
static void prefs_path(char *out, size_t cap) { lx_path_join(out, cap, ED.project.root, "Library/StateGraphEditor.txt"); }
void ge_prefs_load(void) {
    GE.fav_count = GE.recent_count = 0;
    if (!ED.has_project) return;
    char path[LX_PATH_MAX];
    prefs_path(path, sizeof path);
    char *text = lx_file_read(path, NULL);
    if (!text) return;
    const char *cur = text;
    char k[64], v[1024];
    int r;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v)) != 0) {
        if (r < 0) continue;
        if (!strcmp(k, "favourite") && GE.fav_count < 32) lx_strcpy(GE.favs[GE.fav_count++], sizeof GE.favs[0], v);
        else if (!strcmp(k, "recent") && GE.recent_count < 12) lx_strcpy(GE.recent[GE.recent_count++], sizeof GE.recent[0], v);
        else if (!strcmp(k, "debug")) GE.debug = lx_parse_bool(v);
        else if (!strcmp(k, "hints")) GE.hints = lx_parse_bool(v);
        else if (!strcmp(k, "minimap")) GE.minimap = lx_parse_bool(v);
    }
    lx_free(text);
}
void ge_prefs_save(void) {
    if (!ED.has_project) return;
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# State Graph editor preferences\ndebug = %d\nhints = %d\nminimap = %d\n", GE.debug, GE.hints, GE.minimap);
    for (int i = 0; i < GE.fav_count; i++) lx_sb_printf(&sb, "favourite = %s\n", GE.favs[i]);
    for (int i = 0; i < GE.recent_count; i++) lx_sb_printf(&sb, "recent = %s\n", GE.recent[i]);
    char path[LX_PATH_MAX], dir[LX_PATH_MAX];
    lx_path_join(dir, sizeof dir, ED.project.root, "Library");
    lx_mkdir_p(dir);
    prefs_path(path, sizeof path);
    lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
}

/* ======================================================================
 *  the object Inspector: State Graph component
 * ====================================================================== */
static void slot_edit(UiNode *n) { ed_graph_edit(ED.inspector_entity, (int)n->tag); }
static void slot_new(UiNode *n) { LX_UNUSED(n); ge_new_graph_for(ED.inspector_entity); }
static void slot_add(UiNode *n) {
    LX_UNUSED(n);
    LxStateGraph *sg = lx_get_state_graph(ED.scene, ED.inspector_entity);
    if (!sg || sg->count >= LX_FSM_GRAPHS_PER_OBJECT) return;
    memset(&sg->slots[sg->count], 0, sizeof sg->slots[0]);
    sg->count++;
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}
static void slot_remove(UiNode *n) {
    LxStateGraph *sg = lx_get_state_graph(ED.scene, ED.inspector_entity);
    int i = (int)n->tag;
    if (!sg || i < 0 || i >= sg->count) return;
    memmove(&sg->slots[i], &sg->slots[i + 1], sizeof sg->slots[0] * (size_t)(sg->count - i - 1));
    sg->count--;
    if (!sg->count) sg->count = 1, memset(&sg->slots[0], 0, sizeof sg->slots[0]);
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}
static void slot_graph_changed(UiNode *n) {
    PB *b = n->user;
    LxStateGraph *sg = ED.scene ? lx_get_state_graph(ED.scene, b->e) : NULL;
    if (!sg || b->p < 0 || b->p >= sg->count) return;
    lx_strcpy(sg->slots[b->p].graph, sizeof sg->slots[b->p].graph, b->s);
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}
static void slot_graph_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(type);
    PB *b = n->user;
    lx_strcpy(b->s, sizeof b->s, payload);
    slot_graph_changed(n);
}
static void slot_graph_pick(UiNode *it) {
    PB *b = it->user;
    lx_strcpy(b->s, sizeof b->s, it->text[0] == '(' ? "" : it->text);
    UiNode fake;
    memset(&fake, 0, sizeof fake);
    fake.user = b;
    slot_graph_changed(&fake);
}
static void slot_browse(UiNode *btn) {
    PB *b = btn->user;
    UiNode *p = ui_popup(ED.ui, btn->x - 200 * ui_scale(ED.ui), btn->y + btn->ch, 230);
    if (!p) return;
    p->owner = btn;
    int count = 0;
    UiNode *none = ui_menu_item(ED.ui, p, "(none)", NULL, slot_graph_pick);
    none->user = b;
    /* collect_files writes into b->s via asset_pick: use our own handler instead */
    LxDirEntry *e;
    char stack[16][LX_PATH_MAX];
    int sp = 0;
    lx_strcpy(stack[sp++], LX_PATH_MAX, ED.project.assets);
    while (sp && count < 150) {
        char dir[LX_PATH_MAX];
        lx_strcpy(dir, sizeof dir, stack[--sp]);
        int n = lx_dir_list(dir, &e);
        for (int i = 0; i < n; i++) {
            if (e[i].name[0] == '.') continue;
            char full[LX_PATH_MAX];
            lx_path_join(full, sizeof full, dir, e[i].name);
            if (e[i].is_dir) { if (sp < 16) lx_strcpy(stack[sp++], LX_PATH_MAX, full); continue; }
            if (ed_icon_for_file(e[i].name, false) != UI_ICON_GRAPH) continue;
            char rel[LX_REF_MAX];
            ed_relative_asset(rel, sizeof rel, full);
            UiNode *it = ui_menu_item(ED.ui, p, rel, NULL, slot_graph_pick);
            it->user = b;
            it->icon = UI_ICON_GRAPH;
            it->icon_color = ed_color_for_icon(UI_ICON_GRAPH);
            count++;
        }
        lx_free(e);
    }
    if (!count) { UiNode *it = ui_menu_item(ED.ui, p, "No graphs yet: use New Graph", NULL, NULL); it->disabled = true; }
}
static void override_changed(PB *b) {
    LxStateGraph *sg = ED.scene ? lx_get_state_graph(ED.scene, b->e) : NULL;
    if (!sg || b->p < 0 || b->p >= sg->count) return;
    LxStateGraphSlot *slot = &sg->slots[b->p];
    LxGraph *g = lx_asset_graph(slot->graph);
    const LxGraphVar *v = g ? lx_graph_find_var(g, b->name) : NULL;
    char val[160];
    pb_format(b, val, sizeof val);
    int at = -1;
    for (int i = 0; i < slot->override_count; i++) if (!strcmp(slot->overrides[i].var, b->name)) at = i;
    bool is_default = v && !strcmp(v->value, val);
    if (is_default && at >= 0) {   /* back to the graph's value: forget the override */
        memmove(&slot->overrides[at], &slot->overrides[at + 1], sizeof slot->overrides[0] * (size_t)(slot->override_count - at - 1));
        slot->override_count--;
    } else if (!is_default) {
        if (at < 0) {
            if (slot->override_count >= (int)(sizeof slot->overrides / sizeof slot->overrides[0])) { ui_notify(ED.ui, "Too many overridden variables"); return; }
            at = slot->override_count++;
            lx_strcpy(slot->overrides[at].var, sizeof slot->overrides[at].var, b->name);
        }
        lx_strcpy(slot->overrides[at].value, sizeof slot->overrides[at].value, val);
    }
    ed_mark_changed();
}

void ed_state_graph_section(UiNode *sec, LxEntity e) {
    UiCtx *ui = ED.ui;
    LxStateGraph *sg = lx_get_state_graph(ED.scene, e);
    if (!sg) return;
    g_ob_n = 0;
    for (int i = 0; i < sg->count; i++) {
        LxStateGraphSlot *slot = &sg->slots[i];
        char lbl[32];
        snprintf(lbl, sizeof lbl, sg->count > 1 ? "Graph %d" : "Graph", i + 1);
        UiNode *row = ed_prop_row(sec, lbl);
        PB *b = ob_new();
        b->e = e;
        b->p = i;
        lx_strcpy(b->s, sizeof b->s, slot->graph);
        UiNode *tf = ui_textfield(ui, row, b->s, (int)sizeof slot->graph);
        tf->grow = 1;
        tf->h = 22;
        tf->accept = "graph";
        tf->on_drop = slot_graph_drop;
        tf->user = b;
        tf->on_change = slot_graph_changed;
        tf->icon = UI_ICON_GRAPH;
        lx_strcpy(tf->hint, sizeof tf->hint, "Drop a .lxgraph or browse");
        UiNode *br = ui_icon_button(ui, row, UI_ICON_DOTS, "Browse graphs", slot_browse);
        br->w = 24; br->h = 22; br->user = b;
        if (sg->count > 1) { UiNode *x = ui_icon_button(ui, row, UI_ICON_CLOSE, "Remove this graph", slot_remove); x->w = 22; x->h = 22; x->tag = i; }
        LxGraph *g = slot->graph[0] ? lx_asset_graph(slot->graph) : NULL;
        UiNode *brow = ui_box(ui, sec, UI_ROW);
        brow->h = 26;
        brow->gap = 6;
        if (g) {
            UiNode *ed = ui_button(ui, brow, "Edit Graph", slot_edit);
            ed->icon = UI_ICON_GRAPH;
            ed->h = 24;
            ed->grow = 1;
            ed->tag = i;
            if (ED.play != ED_EDIT) {
                const char *cur = lx_fsm_current_state(ED.scene, e, i);
                char t[96];
                snprintf(t, sizeof t, "%s", cur[0] ? cur : "-");
                UiNode *st = label(brow, t, UI_FONT_BOLD, COL_RUN);
                st->icon = UI_ICON_PLAY;
                st->icon_color = COL_RUN;
            }
        } else {
            UiNode *nb = ui_button(ui, brow, "New Graph", slot_new);
            nb->icon = UI_ICON_PLUS;
            nb->h = 24;
            nb->grow = 1;
        }
        if (!g) {
            if (slot->graph[0]) hint(sec, "This graph asset is missing.");
            continue;
        }
        if (g->description[0]) hint(sec, g->description);
        /* variables the graph lets each object set */
        for (int v = 0; v < g->var_count; v++) {
            const LxGraphVar *var = &g->vars[v];
            if (!var->inspector) continue;
            UiNode *vr = ed_prop_row(sec, var->name);
            if (var->tooltip[0]) lx_strcpy(vr->first->tooltip, sizeof vr->first->tooltip, var->tooltip);
            PB *ob = ob_new();
            ob->e = e;
            ob->p = i;
            lx_strcpy(ob->name, sizeof ob->name, var->name);
            const char *val = var->value;
            for (int o = 0; o < slot->override_count; o++) if (!strcmp(slot->overrides[o].var, var->name)) val = slot->overrides[o].value;
            pb_load(ob, var->type, val, NULL);
            value_field(vr, ob, var->type, NULL);
        }
    }
    UiNode *row = ui_box(ui, sec, UI_ROW);
    row->h = 26;
    row->gap = 6;
    if (sg->count < LX_FSM_GRAPHS_PER_OBJECT) {
        UiNode *a = ui_button(ui, row, "Add Graph Slot", slot_add);
        a->icon = UI_ICON_PLUS;
        a->h = 24;
        a->grow = 1;
    }
}

/* the Inspector for a .lxgraph asset */
static void open_asset_click(UiNode *n) { ed_graph_open_asset(n->path); }
void ed_graph_asset_inspector(UiNode *body, const char *ref) {
    UiCtx *ui = ED.ui;
    LxGraph *g = lx_asset_graph(ref);
    UiNode *wrap = ui_box(ui, body, UI_COL);
    ui_pad(wrap, 12, 8, 12, 8);
    wrap->gap = 6;
    if (!g) { hint(wrap, "This graph can't be read."); return; }
    label(wrap, g->name, UI_FONT_TITLE, COL_TEXT);
    if (g->description[0]) hint(wrap, g->description);
    int actions = 0;
    for (int i = 0; i < g->state_count; i++) actions += g->states[i].action_count;
    char t[160];
    snprintf(t, sizeof t, "%d states · %d actions · %d events · %d variables", g->state_count, actions, g->event_count, g->var_count);
    hint(wrap, t);
    UiNode *b = ui_button(ui, wrap, "Open in State Graph Editor", open_asset_click);
    b->icon = UI_ICON_GRAPH;
    b->h = 30;
    b->bg = UI_HEX(0x3d7cf5);
    b->bg_hover = UI_HEX(0x5a92ff);
    b->fg = UI_HEX(0xffffff);
    lx_strcpy(b->path, sizeof b->path, ref);
    hint(wrap, "Give an object this behaviour: drag the asset onto a State Graph component in the Inspector. Each object can override the "
               "variables marked Inspector.");
    char probs[16][160];
    int np = lx_graph_problems(g, probs, 16);
    for (int i = 0; i < np && i < 16; i++) { UiNode *p = label(wrap, probs[i], UI_FONT_SMALL, UI_HEX(0xffb03a)); p->icon = UI_ICON_WARNING; p->icon_color = UI_HEX(0xffb03a); }
}

static void quick_add_enter(UiNode *n) { if (enter_pressed()) quick_add(n); }
static void add_on_enter(UiNode *n) { if (enter_pressed() && n->next && n->next->on_click) n->next->on_click(n->next); }
static void add_var_enter(UiNode *n) { if (enter_pressed()) add_var_click(n); }
