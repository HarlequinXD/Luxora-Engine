/* State Graph runtime: actions registry, global variables, the State Graph component and running graphs.
 *
 * A run enters its start state: each action's on_enter runs top to bottom. Actions that update keep
 * running every frame (or fixed/late update) until they finish; when every action has finished the
 * graph gets FINISHED. An event moves it along a transition of the current state, or a global
 * transition. Events sent while an action is running are taken as soon as that action returns. */
#include "../lx_internal.h"
#include <stdio.h>

/* ======================================================================
 *  action registry
 * ====================================================================== */
static const LxFsmActionDesc **g_actions;
static int g_action_count, g_action_cap;

void lx_fsm_register_action(const LxFsmActionDesc *d) {
    if (!d || !d->name) return;
    for (int i = 0; i < g_action_count; i++)
        if (!strcmp(g_actions[i]->name, d->name)) { g_actions[i] = d; return; }   /* re-registering replaces (script reloads) */
    LX_VEC_PUSH(g_actions, g_action_count, g_action_cap, d);
}
int lx_fsm_action_count(void) { return g_action_count; }
const LxFsmActionDesc *lx_fsm_action_at(int i) { return i >= 0 && i < g_action_count ? g_actions[i] : NULL; }
const LxFsmActionDesc *lx_fsm_action_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_action_count; i++) if (!strcmp(g_actions[i]->name, name)) return g_actions[i];
    return NULL;
}

/* ======================================================================
 *  values
 * ====================================================================== */
static int enum_index(const char *options, const char *text) {
    if (!text || !text[0]) return 0;
    if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-') return atoi(text);
    int i = 0;
    for (const char *s = options; s && *s; i++) {
        size_t l = strcspn(s, "|");
        if (strlen(text) == l && !strncmp(s, text, l)) return i;
        s += l;
        if (*s == '|') s++;
    }
    return 0;
}

bool lx_fsm_value_parse(LxFsmValue *v, LxFsmType t, const char *text, const char *options) {
    memset(v, 0, sizeof *v);
    v->type = t;
    if (!text) text = "";
    switch (t) {
    case LX_FSM_FLOAT: v->f = (float)atof(text); if (!isfinite(v->f)) v->f = 0; v->i = (int)v->f; v->b = v->f != 0; break;
    case LX_FSM_INT: v->i = atoi(text); v->f = (float)v->i; v->b = v->i != 0; break;
    case LX_FSM_ENUM: v->i = enum_index(options, text); v->f = (float)v->i; break;
    case LX_FSM_BOOL: v->b = lx_parse_bool(text); v->f = v->b ? 1.0f : 0.0f; v->i = v->b; break;
    case LX_FSM_VEC3: v->v = lx_parse_vec3(text); break;
    case LX_FSM_COLOR: {
        LxVec4 c = lx_parse_vec4(text);
        int n = 0;
        for (const char *p = text; *p; p++) if (*p == ' ') n++;
        if (n < 3) c.w = 1;   /* "r g b" */
        if (!text[0]) c = lx_v4(1, 1, 1, 1);
        v->c = c;
        break;
    }
    default: lx_strcpy(v->s, sizeof v->s, text); break;
    }
    return true;
}

void lx_fsm_value_format(const LxFsmValue *v, char *out, size_t cap, const char *options) {
    switch (v->type) {
    case LX_FSM_FLOAT: snprintf(out, cap, "%g", (double)v->f); break;
    case LX_FSM_INT: snprintf(out, cap, "%d", v->i); break;
    case LX_FSM_ENUM: {
        int i = 0;
        const char *s = options;
        while (s && i < v->i && (s = strchr(s, '|'))) { s++; i++; }
        if (s && *s) { size_t l = strcspn(s, "|"); if (l >= cap) l = cap - 1; memcpy(out, s, l); out[l] = 0; }
        else snprintf(out, cap, "%d", v->i);
        break;
    }
    case LX_FSM_BOOL: lx_strcpy(out, cap, v->b ? "true" : "false"); break;
    case LX_FSM_VEC3: snprintf(out, cap, "%g %g %g", (double)v->v.x, (double)v->v.y, (double)v->v.z); break;
    case LX_FSM_COLOR: snprintf(out, cap, "%g %g %g %g", (double)v->c.x, (double)v->c.y, (double)v->c.z, (double)v->c.w); break;
    default: lx_strcpy(out, cap, v->s); break;
    }
}

/* ======================================================================
 *  global variables & events (Assets/Globals.lxglobals)
 * ====================================================================== */
static LxGlobals g_globals;
static bool g_globals_loaded;
static LxFsmValue *g_global_values;
static int g_global_value_count;

static void globals_path(char *out, size_t cap) { lx_asset_path(out, cap, "Globals.lxglobals"); }

void lx_fsm_globals_reload(void) {
    lx_free(g_globals.vars);
    lx_free(g_globals.events);
    memset(&g_globals, 0, sizeof g_globals);
    g_globals_loaded = true;
    char path[LX_PATH_MAX];
    globals_path(path, sizeof path);
    char *text = lx_file_read(path, NULL);
    if (!text) return;
    const char *cur = text;
    char k[256], v[1024];
    int r, sec = 0;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r == 2) {
            if (!strcmp(k, "variable")) { sec = 1; LxGraphVar z; memset(&z, 0, sizeof z); LX_VEC_PUSH(g_globals.vars, g_globals.var_count, g_globals.var_cap, z); }
            else if (!strcmp(k, "event")) { sec = 2; LxGraphEvent z; memset(&z, 0, sizeof z); z.global = true; LX_VEC_PUSH(g_globals.events, g_globals.event_count, g_globals.event_cap, z); }
            else sec = 0;
            continue;
        }
        if (sec == 1) {
            LxGraphVar *x = &g_globals.vars[g_globals.var_count - 1];
            if (!strcmp(k, "name")) lx_strcpy(x->name, sizeof x->name, v);
            else if (!strcmp(k, "type")) x->type = lx_fsm_type_parse(v);
            else if (!strcmp(k, "value")) lx_strcpy(x->value, sizeof x->value, v);
            else if (!strcmp(k, "tooltip")) lx_strcpy(x->tooltip, sizeof x->tooltip, v);
        } else if (sec == 2 && !strcmp(k, "name")) lx_strcpy(g_globals.events[g_globals.event_count - 1].name, LX_FSM_NAME, v);
    }
    lx_free(text);
    lx_fsm_globals_reset_values();
}
LxGlobals *lx_fsm_globals(void) {
    if (!g_globals_loaded) lx_fsm_globals_reload();
    return &g_globals;
}
bool lx_fsm_globals_save(void) {
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora global variables and events\n");
    for (int i = 0; i < g_globals.var_count; i++)
        lx_sb_printf(&sb, "\n[variable]\nname = %s\ntype = %s\nvalue = %s\n", g_globals.vars[i].name, lx_fsm_type_name(g_globals.vars[i].type), g_globals.vars[i].value);
    for (int i = 0; i < g_globals.event_count; i++) lx_sb_printf(&sb, "\n[event]\nname = %s\n", g_globals.events[i].name);
    char path[LX_PATH_MAX];
    globals_path(path, sizeof path);
    bool ok = lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
    if (ok) g_globals.dirty = false;
    return ok;
}
void lx_fsm_globals_reset_values(void) {
    lx_fsm_globals();
    lx_free(g_global_values);
    g_global_value_count = g_globals.var_count;
    g_global_values = lx_calloc((size_t)(g_global_value_count ? g_global_value_count : 1), sizeof(LxFsmValue));
    for (int i = 0; i < g_global_value_count; i++) lx_fsm_value_parse(&g_global_values[i], g_globals.vars[i].type, g_globals.vars[i].value, NULL);
}
int lx_fsm_globals_index(const char *name) {
    lx_fsm_globals();
    for (int i = 0; i < g_globals.var_count; i++) if (!strcmp(g_globals.vars[i].name, name)) return i;
    return -1;
}
LxGraphVar *lx_fsm_globals_find_var(const char *name) {
    int i = lx_fsm_globals_index(name);
    return i >= 0 ? &g_globals.vars[i] : NULL;
}
static LxFsmValue *global_value(int i) {
    if (i < 0) return NULL;
    if (g_global_value_count != g_globals.var_count) lx_fsm_globals_reset_values();
    return i < g_global_value_count ? &g_global_values[i] : NULL;
}
LxFsmValue *lx_fsm_global_value(int i) { return global_value(i); }
float lx_fsm_global_float(const char *name) {
    LxFsmValue *v = global_value(lx_fsm_globals_index(name));
    return v ? (v->type == LX_FSM_INT ? (float)v->i : v->type == LX_FSM_BOOL ? (float)v->b : v->f) : 0;
}
void lx_fsm_set_global_float(const char *name, float f) {
    LxFsmValue *v = global_value(lx_fsm_globals_index(name));
    if (!v) return;
    v->f = f; v->i = (int)f; v->b = f != 0;
}

/* ======================================================================
 *  the component
 * ====================================================================== */
LxStateGraph *lx_state_graph_alloc(void) { return lx_calloc(1, sizeof(LxStateGraph)); }

LxStateGraph *lx_add_state_graph(LxScene *s, LxEntity e, const char *graph_ref) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return NULL;
    if (!d->sgraph) d->sgraph = lx_state_graph_alloc();
    d->comps |= LX_COMP_STATE_GRAPH;
    LxStateGraph *g = d->sgraph;
    if (graph_ref && graph_ref[0] && g->count < LX_FSM_GRAPHS_PER_OBJECT) {
        lx_strcpy(g->slots[g->count].graph, LX_REF_MAX, graph_ref);
        g->count++;
    } else if (!g->count) g->count = 1;   /* an empty slot to fill in */
    return g;
}
LxStateGraph *lx_get_state_graph(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && (d->comps & LX_COMP_STATE_GRAPH) ? d->sgraph : NULL;
}

/* ======================================================================
 *  binding a graph to an object
 * ====================================================================== */
static void bound_free(LxFsmRun *r) {
    if (!r->bound) return;
    LxGraph *g = r->graph;
    int n = r->bound_states;
    for (int i = 0; i < n; i++) {
        if (!r->bound[i]) continue;
        for (int a = 0; a < r->bound_counts[i]; a++) lx_free(r->bound[i][a].state);
        lx_free(r->bound[i]);
    }
    LX_UNUSED(g);
    lx_free(r->bound);
    lx_free(r->bound_counts);
    r->bound = NULL;
    r->bound_counts = NULL;
    r->bound_states = 0;
}

static int var_index(const LxFsmRun *r, const char *name) {
    for (int i = 0; i < r->var_count; i++) if (!strcmp(r->graph->vars[i].name, name)) return i;
    return -1;
}

static void bind_states(LxFsmRun *r) {
    LxGraph *g = r->graph;
    r->bound_states = g->state_count;
    r->bound = lx_calloc((size_t)(g->state_count ? g->state_count : 1), sizeof(LxFsmBound *));
    r->bound_counts = lx_calloc((size_t)(g->state_count ? g->state_count : 1), sizeof(int));
    for (int i = 0; i < g->state_count; i++) {
        const LxGraphState *st = &g->states[i];
        r->bound_counts[i] = st->action_count;
        r->bound[i] = lx_calloc((size_t)(st->action_count ? st->action_count : 1), sizeof(LxFsmBound));
        for (int a = 0; a < st->action_count; a++) {
            LxFsmBound *b = &r->bound[i][a];
            const LxGraphAction *src = &st->actions[a];
            b->src = src;
            b->desc = lx_fsm_action_find(src->type);
            for (int k = 0; k < LX_FSM_MAX_PARAMS; k++) {
                b->var[k] = -1;
                const LxFsmParamDesc *pd = b->desc ? &b->desc->params[k] : NULL;
                if (!pd || !pd->name) continue;
                if (src->p[k].is_var) {
                    int vi = var_index(r, src->p[k].value);
                    if (vi >= 0) b->var[k] = vi;
                    else {
                        int gi = lx_fsm_globals_index(src->p[k].value);
                        b->var[k] = gi >= 0 ? -2 - gi : -1;
                    }
                    if (b->var[k] == -1) lx_fsm_value_parse(&b->lit[k], pd->type, "", pd->options);
                } else lx_fsm_value_parse(&b->lit[k], pd->type, src->p[k].value, pd->options);
            }
            if (b->desc && b->desc->state_size) b->state = lx_calloc(1, b->desc->state_size);
        }
    }
}

static void bind(LxFsmRun *r, LxScene *s, const LxStateGraphSlot *slot) {
    LxGraph *g = r->graph;
    /* variables: keep running values of the same name and type */
    LxFsmValue *old = r->vars;
    int old_count = r->var_count;
    char (*old_names)[LX_FSM_NAME] = r->var_names;
    r->var_count = g->var_count;
    r->vars = lx_calloc((size_t)(g->var_count ? g->var_count : 1), sizeof(LxFsmValue));
    r->var_names = lx_calloc((size_t)(g->var_count ? g->var_count : 1), LX_FSM_NAME);
    for (int i = 0; i < g->var_count; i++) {
        const LxGraphVar *v = &g->vars[i];
        lx_strcpy(r->var_names[i], LX_FSM_NAME, v->name);
        const char *text = v->value;
        for (int o = 0; slot && o < slot->override_count; o++)
            if (v->inspector && !strcmp(slot->overrides[o].var, v->name)) text = slot->overrides[o].value;
        lx_fsm_value_parse(&r->vars[i], v->type, text, NULL);
        for (int j = 0; j < old_count; j++)
            if (!strcmp(old_names[j], v->name) && old[j].type == v->type) r->vars[i] = old[j];
    }
    lx_free(old);
    lx_free(old_names);
    LX_UNUSED(s);
    bind_states(r);
    r->bound_version = g->version;
}

/* ======================================================================
 *  parameter access for actions
 * ====================================================================== */
static LxFsmBound *cur_bound(LxFsmCtx *c) {
    LxFsmRun *r = c->run;
    if (!r || r->state < 0 || r->state >= r->bound_states || c->action < 0 || c->action >= r->bound_counts[r->state]) return NULL;
    return &r->bound[r->state][c->action];
}
static LxFsmValue *param(LxFsmCtx *c, int p) {
    static LxFsmValue zero;
    LxFsmBound *b = cur_bound(c);
    if (!b || p < 0 || p >= LX_FSM_MAX_PARAMS) { memset(&zero, 0, sizeof zero); return &zero; }
    int vi = b->var[p];
    if (vi >= 0 && vi < c->run->var_count) return &c->run->vars[vi];
    if (vi <= -2) { LxFsmValue *gv = global_value(-2 - vi); if (gv) return gv; }
    return &b->lit[p];
}
float lx_fsm_float(LxFsmCtx *c, int p) {
    LxFsmValue *v = param(c, p);
    return v->type == LX_FSM_INT || v->type == LX_FSM_ENUM ? (float)v->i : v->type == LX_FSM_BOOL ? (v->b ? 1.0f : 0.0f) : v->f;
}
int lx_fsm_int(LxFsmCtx *c, int p) {
    LxFsmValue *v = param(c, p);
    return v->type == LX_FSM_FLOAT ? (int)lroundf(v->f) : v->type == LX_FSM_BOOL ? v->b : v->i;
}
bool lx_fsm_bool(LxFsmCtx *c, int p) {
    LxFsmValue *v = param(c, p);
    return v->type == LX_FSM_FLOAT ? v->f != 0 : v->type == LX_FSM_INT ? v->i != 0 : v->b;
}
const char *lx_fsm_string(LxFsmCtx *c, int p) {
    LxFsmValue *v = param(c, p);
    if (v->type == LX_FSM_STRING || v->type == LX_FSM_ASSET || v->type == LX_FSM_EVENT || v->type == LX_FSM_OBJECT) return v->s;
    static char buf[64];
    lx_fsm_value_format(v, buf, sizeof buf, NULL);
    return buf;
}
LxVec3 lx_fsm_vec3(LxFsmCtx *c, int p) { LxFsmValue *v = param(c, p); return v->type == LX_FSM_COLOR ? lx_v3(v->c.x, v->c.y, v->c.z) : v->v; }
LxVec4 lx_fsm_color(LxFsmCtx *c, int p) { LxFsmValue *v = param(c, p); return v->type == LX_FSM_VEC3 ? lx_v4(v->v.x, v->v.y, v->v.z, 1) : v->c; }
const char *lx_fsm_asset(LxFsmCtx *c, int p) { return param(c, p)->s; }
const char *lx_fsm_event(LxFsmCtx *c, int p) { return param(c, p)->s; }

LxEntity lx_fsm_object(LxFsmCtx *c, int p) {
    LxFsmValue *v = param(c, p);
    if (v->type != LX_FSM_OBJECT) return 0;
    if (v->e && lx_entity_valid(c->scene, v->e)) return v->e;
    const char *s = v->s;
    if (!strcmp(s, "Owner")) return c->self;
    if (!s[0] || !strcmp(s, "None")) return v->e = 0;
    LxEntity e = 0;
    if (!strncmp(s, "tag:", 4)) e = lx_entity_find_with_tag(c->scene, s + 4);
    else e = lx_entity_find(c->scene, !strncmp(s, "name:", 5) ? s + 5 : s);
    v->e = e;   /* found once, kept while it lives */
    return e;
}
bool lx_fsm_is_none(LxFsmCtx *c, int p) {
    LxFsmBound *b = cur_bound(c);
    if (!b || p < 0 || p >= LX_FSM_MAX_PARAMS) return true;
    if (b->var[p] != -1) return false;
    const LxFsmValue *v = &b->lit[p];
    if (v->type == LX_FSM_OBJECT) return !v->s[0] || !strcmp(v->s, "None");
    if (v->type == LX_FSM_STRING || v->type == LX_FSM_ASSET || v->type == LX_FSM_EVENT) return !v->s[0] || !strcmp(v->s, "None");
    return b->src && !b->src->p[p].value[0];
}

static LxFsmValue *out_var(LxFsmCtx *c, int p) {
    LxFsmBound *b = cur_bound(c);
    if (!b || p < 0 || p >= LX_FSM_MAX_PARAMS) return NULL;
    int vi = b->var[p];
    if (vi >= 0 && vi < c->run->var_count) return &c->run->vars[vi];
    if (vi <= -2) return global_value(-2 - vi);
    return NULL;
}
static void set_num(LxFsmValue *v, float f) {
    switch (v->type) {
    case LX_FSM_INT: case LX_FSM_ENUM: v->i = (int)lroundf(f); v->f = (float)v->i; break;
    case LX_FSM_BOOL: v->b = f != 0; v->f = v->b; v->i = v->b; break;
    case LX_FSM_STRING: snprintf(v->s, sizeof v->s, "%g", (double)f); break;
    default: v->f = f; v->i = (int)f; v->b = f != 0; break;
    }
}
void lx_fsm_set_float(LxFsmCtx *c, int p, float f) { LxFsmValue *v = out_var(c, p); if (v && isfinite(f)) set_num(v, f); }
void lx_fsm_set_int(LxFsmCtx *c, int p, int i) {
    LxFsmValue *v = out_var(c, p);
    if (!v) return;
    if (v->type == LX_FSM_INT || v->type == LX_FSM_ENUM) { v->i = i; v->f = (float)i; v->b = i != 0; } else set_num(v, (float)i);
}
void lx_fsm_set_bool(LxFsmCtx *c, int p, bool b) {
    LxFsmValue *v = out_var(c, p);
    if (!v) return;
    if (v->type == LX_FSM_STRING) lx_strcpy(v->s, sizeof v->s, b ? "true" : "false");
    else { v->b = b; v->f = b; v->i = b; }
}
void lx_fsm_set_string(LxFsmCtx *c, int p, const char *s) {
    LxFsmValue *v = out_var(c, p);
    if (!v) return;
    if (v->type == LX_FSM_FLOAT || v->type == LX_FSM_INT || v->type == LX_FSM_BOOL) { LxFsmType t = v->type; lx_fsm_value_parse(v, t, s, NULL); }
    else lx_strcpy(v->s, sizeof v->s, s ? s : "");
}
void lx_fsm_set_vec3(LxFsmCtx *c, int p, LxVec3 x) {
    LxFsmValue *v = out_var(c, p);
    if (!v || !isfinite(x.x) || !isfinite(x.y) || !isfinite(x.z)) return;
    if (v->type == LX_FSM_COLOR) v->c = lx_v4(x.x, x.y, x.z, v->c.w);
    else if (v->type == LX_FSM_FLOAT) v->f = lx_len3(x);
    else v->v = x;
}
void lx_fsm_set_color(LxFsmCtx *c, int p, LxVec4 x) {
    LxFsmValue *v = out_var(c, p);
    if (!v) return;
    if (v->type == LX_FSM_VEC3) v->v = lx_v3(x.x, x.y, x.z);
    else v->c = x;
}
void lx_fsm_set_object(LxFsmCtx *c, int p, LxEntity e) {
    LxFsmValue *v = out_var(c, p);
    if (!v) return;
    v->type = LX_FSM_OBJECT;
    v->e = e;
    lx_strcpy(v->s, sizeof v->s, e ? lx_entity_name(c->scene, e) : "None");
}
float lx_fsm_state_time(LxFsmCtx *c) { return c->run ? c->run->state_time : 0; }
float lx_fsm_event_value(LxFsmCtx *c) { return c->run ? c->run->event_value : 0; }
LxEntity lx_fsm_event_sender(LxFsmCtx *c) { return c->run ? c->run->event_sender : 0; }
LxEntity lx_fsm_event_other(LxFsmCtx *c) { return c->run ? c->run->event_other : 0; }

/* ======================================================================
 *  running
 * ====================================================================== */
static void enter_state(LxScene *s, LxFsmRun *r, int idx);

static int transition_target(const LxFsmRun *r, const char *event) {
    const LxGraph *g = r->graph;
    if (r->state >= 0 && r->state < g->state_count) {
        const LxGraphState *st = &g->states[r->state];
        for (int t = 0; t < st->trans_count; t++)
            if (!strcmp(st->trans[t].event, event)) return lx_graph_state_index(g, st->trans[t].to);
    }
    for (int t = 0; t < g->global_count; t++)
        if (!strcmp(g->globals[t].event, event)) return lx_graph_state_index(g, g->globals[t].to);
    return -1;
}

static LxFsmCtx ctx_for(LxScene *s, LxFsmRun *r, int a, float dt) {
    LxFsmCtx c;
    c.scene = s;
    c.self = r->self;
    c.dt = dt;
    c.run = r;
    c.action = a;
    c.state = r->bound[r->state][a].state;
    return c;
}

static void exit_state(LxScene *s, LxFsmRun *r) {
    if (r->state < 0 || r->state >= r->bound_states) return;
    int st = r->state;
    for (int a = 0; a < r->bound_counts[st]; a++) {
        LxFsmBound *b = &r->bound[st][a];
        if (b->entered && b->desc && b->desc->on_exit) {
            LxFsmCtx c = ctx_for(s, r, a, 0);
            r->in_action++;
            b->desc->on_exit(&c);
            r->in_action--;
        }
        b->entered = false;
    }
}

static void change_state(LxScene *s, LxFsmRun *r, int target, const char *event) {
    if (target < 0) return;
    if (++r->loops > LX_FSM_MAX_LOOP) {
        if (!r->error) LX_ERROR("State graph %s on %s: more than %d state changes in one frame (an endless loop?) - stopped",
                                r->graph->name, lx_entity_name(s, r->self), LX_FSM_MAX_LOOP);
        r->error = true;   /* stays on the state it reached, so the editor shows where it looped */
        return;
    }
    exit_state(s, r);
    r->prev_state = r->state;
    if (r->history_count == 64) { memmove(r->history, r->history + 1, sizeof r->history[0] * 63); r->history_count--; }
    r->history[r->history_count].state = target;
    lx_strcpy(r->history[r->history_count].event, LX_FSM_NAME, event ? event : "");
    r->history[r->history_count].time = lx_game_time(s);
    r->history_count++;
    enter_state(s, r, target);
}

/* an event arrives: taken now, or right after the running action returns */
static bool deliver(LxScene *s, LxFsmRun *r, const char *event, float value, LxEntity sender, LxEntity other) {
    if (!r->graph || r->state < 0 || !event || !event[0]) return false;
    int target = transition_target(r, event);
    if (target < 0) return false;
    if (r->in_action) {
        if (r->pending < 0) {   /* the first event wins until it's taken */
            r->pending = target;
            lx_strcpy(r->pending_event, LX_FSM_NAME, event);
            r->pend_value = value;
            r->pend_sender = sender;
            r->pend_other = other;
        }
        return true;
    }
    lx_strcpy(r->last_event, LX_FSM_NAME, event);
    r->event_value = value;
    r->event_sender = sender;
    r->event_other = other;
    change_state(s, r, target, event);
    return true;
}

/* after an action returns: take the transition it asked for */
static bool take_pending(LxScene *s, LxFsmRun *r) {
    if (r->pending < 0 || r->in_action) return false;
    int t = r->pending;
    char ev[LX_FSM_NAME];
    lx_strcpy(ev, sizeof ev, r->pending_event);
    r->pending = -1;
    lx_strcpy(r->last_event, LX_FSM_NAME, ev);
    r->event_value = r->pend_value;
    r->event_sender = r->pend_sender;
    r->event_other = r->pend_other;
    change_state(s, r, t, ev);
    return true;
}

static bool all_finished(const LxFsmRun *r) {
    if (r->state < 0) return false;
    for (int a = 0; a < r->bound_counts[r->state]; a++) {
        const LxFsmBound *b = &r->bound[r->state][a];
        if (b->src->enabled && b->desc && !b->finished) return false;
    }
    return true;
}

static void check_finished(LxScene *s, LxFsmRun *r) {
    if (r->state < 0 || r->finished_sent || !all_finished(r)) return;
    r->finished_sent = true;
    deliver(s, r, "FINISHED", 0, r->self, 0);
}

/* runs one action's enter; returns false if the state changed meanwhile */
static bool start_action(LxScene *s, LxFsmRun *r, int st, int a) {
    LxFsmBound *b = &r->bound[st][a];
    if (!b->src->enabled || !b->desc) { b->finished = true; return true; }
    if (b->state && b->desc->state_size) memset(b->state, 0, b->desc->state_size);
    b->finished = false;
    b->entered = true;
    LxFsmCtx c = ctx_for(s, r, a, 0);
    r->in_action++;
    if (b->desc->on_enter) b->desc->on_enter(&c);
    r->in_action--;
    if (r->state != st) return false;
    if (!(b->desc->flags & LX_FSM_A_CONTINUOUS) && b->src->update == LX_FSM_ONCE) b->finished = true;
    if (take_pending(s, r)) return false;
    return r->state == st;
}

static void enter_state(LxScene *s, LxFsmRun *r, int idx) {
    if (idx < 0 || idx >= r->graph->state_count) { r->state = -1; return; }
    r->state = idx;
    lx_strcpy(r->state_name, sizeof r->state_name, r->graph->states[idx].name);
    r->state_time = 0;
    r->finished_sent = false;
    r->enter_time = lx_time();
    int n = r->bound_counts[idx];
    for (int a = 0; a < n; a++) { r->bound[idx][a].finished = false; r->bound[idx][a].entered = false; }
    bool seq = r->graph->states[idx].sequence;
    for (int a = 0; a < n; a++) {
        if (!start_action(s, r, idx, a)) return;
        if (seq && !r->bound[idx][a].finished) break;   /* the rest start one by one */
    }
    check_finished(s, r);
}

static void run_start(LxScene *s, LxFsmRun *r) {
    r->started = true;
    r->pending = -1;
    int st = lx_graph_state_index(r->graph, r->graph->start);
    if (st < 0 && r->graph->state_count) st = 0;
    r->history_count = 0;
    r->history[0].state = st;
    lx_strcpy(r->history[0].event, LX_FSM_NAME, "START");
    r->history[0].time = lx_game_time(s);
    r->history_count = 1;
    lx_strcpy(r->last_event, LX_FSM_NAME, "START");
    enter_state(s, r, st);
}

/* the graph was edited while running: keep the current state, re-enter it only if its actions changed */
static void rebind(LxScene *s, LxFsmRun *r, const LxStateGraphSlot *slot) {
    char cur[LX_FSM_NAME] = "";
    char types[64][LX_FSM_NAME];
    int ntypes = 0;
    bool flags_f[64] = {0}, flags_e[64] = {0};
    void *states_[64] = {0};
    if (r->state >= 0 && r->state < r->bound_states) {
        lx_strcpy(cur, sizeof cur, r->state_name);
        ntypes = r->bound_counts[r->state] < 64 ? r->bound_counts[r->state] : 64;
        for (int a = 0; a < ntypes; a++) {
            LxFsmBound *b = &r->bound[r->state][a];
            lx_strcpy(types[a], LX_FSM_NAME, b->desc ? b->desc->name : "");
            flags_f[a] = b->finished;
            flags_e[a] = b->entered;
            states_[a] = b->state;
            b->state = NULL;   /* kept */
        }
    }
    bound_free(r);
    bind(r, s, slot);
    int idx = lx_graph_state_index(r->graph, cur);
    if (idx < 0) {   /* the state was deleted or renamed: start over */
        for (int a = 0; a < ntypes; a++) lx_free(states_[a]);
        if (r->started && r->graph->state_count) { r->state = -1; run_start(s, r); }
        return;
    }
    bool same = r->bound_counts[idx] == ntypes;
    for (int a = 0; same && a < ntypes; a++) same = r->bound[idx][a].desc && !strcmp(r->bound[idx][a].desc->name, types[a]);
    r->state = idx;
    if (same) {
        for (int a = 0; a < ntypes; a++) {
            LxFsmBound *b = &r->bound[idx][a];
            lx_free(b->state);
            b->state = states_[a];
            b->finished = flags_f[a];
            b->entered = flags_e[a];
        }
    } else {
        for (int a = 0; a < ntypes; a++) lx_free(states_[a]);
        enter_state(s, r, idx);
    }
}

static void run_free(LxScene *s, LxFsmRun *r) {
    if (s && r->graph && r->bound) exit_state(s, r);
    bound_free(r);
    lx_free(r->vars);
    lx_free(r->var_names);
    memset(r, 0, sizeof *r);
    r->state = -1;
}

void lx_fsm_entity_free(LxEntityData *d) {
    if (d->fsm) {
        for (int i = 0; i < LX_FSM_GRAPHS_PER_OBJECT; i++) run_free(NULL, &d->fsm->runs[i]);
        lx_free(d->fsm);
        d->fsm = NULL;
    }
    if (!(d->comps & LX_COMP_STATE_GRAPH) && d->sgraph) { lx_free(d->sgraph); d->sgraph = NULL; }
}

/* the runs an object should have, (re)built from its component */
static LxFsmInstance *instance_for(LxScene *s, LxEntity e, LxEntityData *d) {
    if (!d->sgraph || !(d->comps & LX_COMP_STATE_GRAPH)) return NULL;
    if (!d->fsm) {
        d->fsm = lx_calloc(1, sizeof *d->fsm);
        for (int i = 0; i < LX_FSM_GRAPHS_PER_OBJECT; i++) { d->fsm->runs[i].state = -1; d->fsm->runs[i].pending = -1; }
    }
    LxFsmInstance *in = d->fsm;
    in->count = d->sgraph->count;
    for (int i = 0; i < in->count && i < LX_FSM_GRAPHS_PER_OBJECT; i++) {
        LxFsmRun *r = &in->runs[i];
        LxGraph *g = lx_asset_graph(d->sgraph->slots[i].graph);
        if (r->graph != g) {
            run_free(s, r);
            r->graph = g;
            if (g) { r->self = e; r->slot = i; r->pending = -1; bind(r, s, &d->sgraph->slots[i]); }
        } else if (g && r->bound_version != g->version) rebind(s, r, &d->sgraph->slots[i]);
        r->self = e;
    }
    return in;
}

LxFsmRun *lx_fsm_run_of(LxScene *s, LxEntity e, int slot) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !d->fsm || slot < 0 || slot >= d->fsm->count) return NULL;
    return &d->fsm->runs[slot];
}

static void update_run(LxScene *s, LxFsmRun *r, float dt, LxFsmUpdate phase) {
    if (!r->graph || r->error) return;
    if (!r->started) { if (phase == LX_FSM_EVERY_FRAME) run_start(s, r); return; }
    if (r->state < 0) return;
    int st = r->state;
    if (phase == LX_FSM_EVERY_FRAME) r->state_time += dt;
    int n = r->bound_counts[st];
    bool seq = r->graph->states[st].sequence;
    for (int a = 0; a < n; a++) {
        LxFsmBound *b = &r->bound[st][a];
        if (!b->entered) {
            /* a sequence state starts its next action once the one before has finished */
            if (seq && a > 0 && r->bound[st][a - 1].finished && phase == LX_FSM_EVERY_FRAME) {
                if (!start_action(s, r, st, a)) return;
                if (!b->finished) break;
                continue;
            }
            if (seq) break;
            continue;
        }
        if (b->finished || !b->desc || !b->desc->on_update) { if (seq && !b->finished) break; continue; }
        bool cont = (b->desc->flags & LX_FSM_A_CONTINUOUS) != 0;
        LxFsmUpdate mode = cont ? LX_FSM_EVERY_FRAME : b->src->update;
        if (mode != phase) { if (seq) break; continue; }
        LxFsmCtx c = ctx_for(s, r, a, dt);
        r->in_action++;
        b->desc->on_update(&c);
        r->in_action--;
        if (r->state != st) return;
        if (take_pending(s, r)) return;
        if (seq && !b->finished) break;
    }
    check_finished(s, r);
}

void lx_fsm_scene_update(LxScene *s, float dt, LxFsmUpdate phase) {
    int n = s->count;
    for (int i = 1; i < n; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_STATE_GRAPH) || d->pending_destroy) continue;
        LxEntity e = lx_entity_make((uint32_t)i, d->generation & 0xfffu);
        LxFsmInstance *in = instance_for(s, e, d);
        if (!in) continue;
        for (int k = 0; k < in->count; k++) {
            d = &s->entities[i];
            if (!d->fsm || !d->alive) break;
            LxFsmRun *r = &d->fsm->runs[k];
            if (phase == LX_FSM_EVERY_FRAME) r->loops = 0;
            update_run(s, r, dt, phase);
        }
    }
}

void lx_fsm_scene_stop(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->fsm) continue;
        for (int k = 0; k < LX_FSM_GRAPHS_PER_OBJECT; k++) run_free(d->alive ? s : NULL, &d->fsm->runs[k]);
        lx_free(d->fsm);
        d->fsm = NULL;
    }
}

/* ======================================================================
 *  events from code, physics and messages
 * ====================================================================== */
static int send_to(LxScene *s, LxEntity target, const char *event, float value, LxEntity sender, LxEntity other) {
    LxEntityData *d = lx_entity_data(s, target);
    if (!d || !d->alive || !d->active || !(d->comps & LX_COMP_STATE_GRAPH) || !s->simulating) return 0;
    LxFsmInstance *in = instance_for(s, target, d);
    if (!in) return 0;
    int n = 0;
    for (int k = 0; k < in->count; k++) {
        d = lx_entity_data(s, target);
        if (!d || !d->fsm) break;
        LxFsmRun *r = &d->fsm->runs[k];
        if (!r->started) continue;
        n += deliver(s, r, event, value, sender, other);
    }
    return n;
}

int lx_fsm_send_event(LxScene *s, LxEntity target, const char *event, float value, LxEntity sender) {
    return send_to(s, target, event, value, sender, 0);
}
int lx_fsm_broadcast_event(LxScene *s, const char *event, float value, LxEntity sender) {
    int n = 0, count = s->count;
    for (int i = 1; i < count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !(d->comps & LX_COMP_STATE_GRAPH)) continue;
        n += send_to(s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), event, value, sender, 0);
    }
    return n;
}
void lx_fsm_system_event(LxScene *s, LxEntity self, const char *event, LxEntity other, float value) {
    LxEntityData *d = lx_entity_data(s, self);
    if (!d || !(d->comps & LX_COMP_STATE_GRAPH)) return;
    /* remembered for "Get Collision Info" even when no transition uses it */
    if (d->fsm)
        for (int k = 0; k < d->fsm->count; k++) {
            LxFsmRun *r = &d->fsm->runs[k];
            r->last_other = other;
            lx_strcpy(r->last_system_event, LX_FSM_NAME, event);
        }
    send_to(s, self, event, value, other, other);
}

const char *lx_fsm_current_state(LxScene *s, LxEntity e, int slot) {
    LxFsmRun *r = lx_fsm_run_of(s, e, slot);
    return r && r->graph && r->state >= 0 && r->state < r->graph->state_count ? r->graph->states[r->state].name : "";
}
float lx_fsm_get_var_float(LxScene *s, LxEntity e, int slot, const char *var) {
    LxFsmRun *r = lx_fsm_run_of(s, e, slot);
    if (!r || !r->graph) return 0;
    int i = var_index(r, var);
    if (i < 0) return 0;
    LxFsmValue *v = &r->vars[i];
    return v->type == LX_FSM_INT ? (float)v->i : v->type == LX_FSM_BOOL ? (float)v->b : v->f;
}
bool lx_fsm_set_var_float(LxScene *s, LxEntity e, int slot, const char *var, float f) {
    LxFsmRun *r = lx_fsm_run_of(s, e, slot);
    if (!r || !r->graph) return false;
    int i = var_index(r, var);
    if (i < 0) return false;
    set_num(&r->vars[i], f);
    return true;
}

void lx_fsm_finish(LxFsmCtx *c) {
    LxFsmBound *b = cur_bound(c);
    if (b) b->finished = true;
}
void lx_fsm_send(LxFsmCtx *c, const char *event) {
    if (!c->run || !event || !event[0] || !strcmp(event, "None")) return;
    deliver(c->scene, c->run, event, 0, c->self, 0);
}
