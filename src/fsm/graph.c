/* State Graph assets (.lxgraph): the data model, its text format, and editing helpers. */
#include "../lx_internal.h"
#include <stdio.h>

const char *const LX_FSM_SYSTEM_EVENTS[] = {
    "FINISHED", "TRIGGER_ENTER", "TRIGGER_EXIT", "COLLISION_ENTER", "COLLISION_EXIT", "MESSAGE",
};
const int LX_FSM_SYSTEM_EVENT_COUNT = (int)(sizeof LX_FSM_SYSTEM_EVENTS / sizeof LX_FSM_SYSTEM_EVENTS[0]);
bool lx_fsm_is_system_event(const char *name) {
    for (int i = 0; i < LX_FSM_SYSTEM_EVENT_COUNT; i++) if (!strcmp(name, LX_FSM_SYSTEM_EVENTS[i])) return true;
    return false;
}

static const char *TYPE_NAMES[LX_FSM_TYPE_COUNT] = {"float", "int", "bool", "string", "vector3", "color", "object", "asset", "event", "enum"};
const char *lx_fsm_type_name(LxFsmType t) { return t >= 0 && t < LX_FSM_TYPE_COUNT ? TYPE_NAMES[t] : "float"; }
LxFsmType lx_fsm_type_parse(const char *s) {
    for (int i = 0; i < LX_FSM_TYPE_COUNT; i++) if (!strcmp(s, TYPE_NAMES[i])) return (LxFsmType)i;
    return LX_FSM_FLOAT;
}
const char *lx_fsm_default_value(LxFsmType t) {
    switch (t) {
    case LX_FSM_FLOAT: case LX_FSM_INT: case LX_FSM_ENUM: return "0";
    case LX_FSM_BOOL: return "false";
    case LX_FSM_VEC3: return "0 0 0";
    case LX_FSM_COLOR: return "1 1 1 1";
    case LX_FSM_OBJECT: return "None";
    default: return "";
    }
}

/* ---------------- lifetime ---------------- */
LxGraph *lx_graph_new(const char *name) {
    LxGraph *g = lx_calloc(1, sizeof *g);
    lx_strcpy(g->name, sizeof g->name, name && name[0] ? name : "Graph");
    LxGraphState *st = lx_graph_add_state(g, "State 1", 80, 80);
    lx_strcpy(g->start, sizeof g->start, st->name);
    g->dirty = false;
    return g;
}

static void state_free(LxGraphState *s) { lx_free(s->actions); lx_free(s->trans); }

void lx_graph_free(LxGraph *g) {
    if (!g) return;
    for (int i = 0; i < g->state_count; i++) state_free(&g->states[i]);
    lx_free(g->states);
    lx_free(g->events);
    lx_free(g->vars);
    lx_free(g->globals);
    lx_free(g->comments);
    lx_free(g);
}

LxGraph *lx_graph_clone(const LxGraph *g) {
    char *t = lx_graph_to_text(g);
    LxGraph *c = lx_graph_from_text(t);
    lx_free(t);
    if (c) { lx_strcpy(c->ref, sizeof c->ref, g->ref); c->version = g->version; }
    return c;
}

void lx_graph_touch(LxGraph *g) { g->version++; g->dirty = true; }

/* ---------------- text format ---------------- */
/* values are one line: newlines in text (comments, descriptions) are written as \n */
static void put_escaped(LxStrBuf *sb, const char *key, const char *v) {
    char buf[1100];
    size_t n = 0;
    for (const char *p = v; *p && n < sizeof buf - 3; p++) {
        if (*p == '\n') { buf[n++] = '\\'; buf[n++] = 'n'; }
        else if (*p == '\\') { buf[n++] = '\\'; buf[n++] = '\\'; }
        else if ((unsigned char)*p >= 0x20) buf[n++] = *p;
    }
    buf[n] = 0;
    lx_sb_printf(sb, "%s = %s\n", key, buf);
}
static void unescape(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
        else if (*p == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
        else *o++ = *p;
    }
    *o = 0;
}
static const char *UPDATE_NAMES[] = {"once", "every_frame", "fixed", "late"};

char *lx_graph_to_text(const LxGraph *g) {
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora state graph v1\n[graph]\n");
    put_escaped(&sb, "name", g->name);
    put_escaped(&sb, "description", g->description);
    lx_sb_printf(&sb, "start = %s\n", g->start);
    for (int i = 0; i < g->event_count; i++) lx_sb_printf(&sb, "\n[event]\nname = %s\nglobal = %d\n", g->events[i].name, g->events[i].global);
    for (int i = 0; i < g->var_count; i++) {
        const LxGraphVar *v = &g->vars[i];
        lx_sb_printf(&sb, "\n[variable]\nname = %s\ntype = %s\n", v->name, lx_fsm_type_name(v->type));
        put_escaped(&sb, "value", v->value);
        lx_sb_printf(&sb, "inspector = %d\n", v->inspector);
        if (v->tooltip[0]) put_escaped(&sb, "tooltip", v->tooltip);
    }
    for (int i = 0; i < g->global_count; i++) lx_sb_printf(&sb, "\n[global]\nevent = %s\nto = %s\n", g->globals[i].event, g->globals[i].to);
    for (int i = 0; i < g->state_count; i++) {
        const LxGraphState *st = &g->states[i];
        lx_sb_printf(&sb, "\n[state]\nname = %s\npos = %g %g\ncolor = %d\nsequence = %d\n", st->name, (double)st->x, (double)st->y, st->color, st->sequence);
        if (st->description[0]) put_escaped(&sb, "description", st->description);
        for (int t = 0; t < st->trans_count; t++) lx_sb_printf(&sb, "transition = %s > %s\n", st->trans[t].event, st->trans[t].to);
        for (int a = 0; a < st->action_count; a++) {
            const LxGraphAction *ac = &st->actions[a];
            lx_sb_printf(&sb, "[action]\ntype = %s\nenabled = %d\nfolded = %d\nupdate = %s\n", ac->type, ac->enabled, ac->folded, UPDATE_NAMES[ac->update & 3]);
            const LxFsmActionDesc *d = lx_fsm_action_find(ac->type);
            for (int k = 0; k < LX_FSM_MAX_PARAMS; k++) {
                const char *pname = d && d->params[k].name ? d->params[k].name : NULL;
                if (!pname && !ac->p[k].value[0] && !ac->p[k].is_var) continue;
                char key[96];
                snprintf(key, sizeof key, "p%d", k);
                char val[LX_REF_MAX + 8];
                snprintf(val, sizeof val, "%s%s", ac->p[k].is_var ? "$" : "", ac->p[k].value);
                put_escaped(&sb, key, val);
            }
        }
    }
    for (int i = 0; i < g->comment_count; i++) {
        const LxGraphComment *c = &g->comments[i];
        lx_sb_printf(&sb, "\n[comment]\nrect = %g %g %g %g\ncolor = %d\n", (double)c->x, (double)c->y, (double)c->w, (double)c->h, c->color);
        put_escaped(&sb, "text", c->text);
    }
    if (!sb.data) lx_sb_printf(&sb, "");
    return sb.data;
}

/* appends a zeroed element and returns it */
static void *grow(void **arr, int *count, int *cap, size_t size) {
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = lx_realloc(*arr, (size_t)*cap * size);
    }
    void *e = (char *)*arr + (size_t)*count * size;
    memset(e, 0, size);
    (*count)++;
    return e;
}
#define PUSH(arr, count, cap, T) ((T *)grow((void **)&(arr), &(count), &(cap), sizeof(T)))

LxGraph *lx_graph_from_text(const char *text) {
    if (!text) return NULL;
    LxGraph *g = lx_calloc(1, sizeof *g);
    const char *cur = text;
    char k[256], v[1024];
    int r;
    enum { NONE, GRAPH, EVENT, VAR, GLOBAL, STATE, ACTION, COMMENT } sec = NONE;
    LxGraphState *st = NULL;
    LxGraphAction *ac = NULL;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r == 2) {
            if (!strcmp(k, "graph")) sec = GRAPH;
            else if (!strcmp(k, "event")) { sec = EVENT; PUSH(g->events, g->event_count, g->event_cap, LxGraphEvent); }
            else if (!strcmp(k, "variable")) { sec = VAR; LxGraphVar *x = PUSH(g->vars, g->var_count, g->var_cap, LxGraphVar); x->type = LX_FSM_FLOAT; }
            else if (!strcmp(k, "global")) { sec = GLOBAL; PUSH(g->globals, g->global_count, g->global_cap, LxGraphTransition); }
            else if (!strcmp(k, "state")) { sec = STATE; st = PUSH(g->states, g->state_count, g->state_cap, LxGraphState); ac = NULL; }
            else if (!strcmp(k, "action") && st) {
                sec = ACTION;
                ac = PUSH(st->actions, st->action_count, st->action_cap, LxGraphAction);
                ac->enabled = true;
            } else if (!strcmp(k, "comment")) { sec = COMMENT; PUSH(g->comments, g->comment_count, g->comment_cap, LxGraphComment); }
            else sec = NONE;
            continue;
        }
        unescape(v);
        switch (sec) {
        case GRAPH:
            if (!strcmp(k, "name")) lx_strcpy(g->name, sizeof g->name, v);
            else if (!strcmp(k, "description")) lx_strcpy(g->description, sizeof g->description, v);
            else if (!strcmp(k, "start")) lx_strcpy(g->start, sizeof g->start, v);
            break;
        case EVENT: {
            LxGraphEvent *e = &g->events[g->event_count - 1];
            if (!strcmp(k, "name")) lx_strcpy(e->name, sizeof e->name, v);
            else if (!strcmp(k, "global")) e->global = lx_parse_bool(v);
            break;
        }
        case VAR: {
            LxGraphVar *x = &g->vars[g->var_count - 1];
            if (!strcmp(k, "name")) lx_strcpy(x->name, sizeof x->name, v);
            else if (!strcmp(k, "type")) x->type = lx_fsm_type_parse(v);
            else if (!strcmp(k, "value")) lx_strcpy(x->value, sizeof x->value, v);
            else if (!strcmp(k, "inspector")) x->inspector = lx_parse_bool(v);
            else if (!strcmp(k, "tooltip")) lx_strcpy(x->tooltip, sizeof x->tooltip, v);
            break;
        }
        case GLOBAL: {
            LxGraphTransition *t = &g->globals[g->global_count - 1];
            if (!strcmp(k, "event")) lx_strcpy(t->event, sizeof t->event, v);
            else if (!strcmp(k, "to")) lx_strcpy(t->to, sizeof t->to, v);
            break;
        }
        case STATE:
            if (!strcmp(k, "name")) lx_strcpy(st->name, sizeof st->name, v);
            else if (!strcmp(k, "pos")) { LxVec3 p = lx_parse_vec3(v); st->x = p.x; st->y = p.y; }
            else if (!strcmp(k, "color")) st->color = atoi(v);
            else if (!strcmp(k, "sequence")) st->sequence = lx_parse_bool(v);
            else if (!strcmp(k, "description")) lx_strcpy(st->description, sizeof st->description, v);
            else if (!strcmp(k, "transition")) {
                LxGraphTransition *t = PUSH(st->trans, st->trans_count, st->trans_cap, LxGraphTransition);
                char *gt = strstr(v, " > ");
                if (gt) { *gt = 0; lx_strcpy(t->to, sizeof t->to, gt + 3); }
                else { size_t n = strlen(v); if (n >= 2 && v[n - 1] == '>') v[n - 2] = 0; }
                lx_strcpy(t->event, sizeof t->event, v);
            }
            break;
        case ACTION:
            if (!strcmp(k, "type")) lx_strcpy(ac->type, sizeof ac->type, v);
            else if (!strcmp(k, "enabled")) ac->enabled = lx_parse_bool(v);
            else if (!strcmp(k, "folded")) ac->folded = lx_parse_bool(v);
            else if (!strcmp(k, "update")) {
                for (int u = 0; u < 4; u++) if (!strcmp(v, UPDATE_NAMES[u])) ac->update = (LxFsmUpdate)u;
            } else if (k[0] == 'p' && k[1] >= '0' && k[1] <= '9') {
                int idx = atoi(k + 1);
                if (idx >= 0 && idx < LX_FSM_MAX_PARAMS) {
                    ac->p[idx].is_var = v[0] == '$';
                    lx_strcpy(ac->p[idx].value, sizeof ac->p[idx].value, v[0] == '$' ? v + 1 : v);
                }
            }
            break;
        case COMMENT: {
            LxGraphComment *c = &g->comments[g->comment_count - 1];
            if (!strcmp(k, "rect")) { LxVec4 q = lx_parse_vec4(v); c->x = q.x; c->y = q.y; c->w = q.z; c->h = q.w; }
            else if (!strcmp(k, "color")) c->color = atoi(v);
            else if (!strcmp(k, "text")) lx_strcpy(c->text, sizeof c->text, v);
            break;
        }
        default: break;
        }
    }
    if (!g->state_count) { lx_graph_free(g); return NULL; }
    if (!lx_graph_find_state(g, g->start)) lx_strcpy(g->start, sizeof g->start, g->states[0].name);
    return g;
}

bool lx_graph_save(LxGraph *g, const char *path) {
    char *t = lx_graph_to_text(g);
    bool ok = lx_file_write(path, t, strlen(t));
    lx_free(t);
    if (ok) g->dirty = false;
    return ok;
}

/* ---------------- registry ---------------- */
typedef struct GraphEntry { char ref[LX_REF_MAX]; LxGraph *g; bool missing; } GraphEntry;
static GraphEntry *g_graphs;
static int g_graph_count, g_graph_cap;

LxGraph *lx_asset_graph(const char *ref) {
    if (!ref || !ref[0]) return NULL;
    for (int i = 0; i < g_graph_count; i++) if (!strcmp(g_graphs[i].ref, ref)) return g_graphs[i].g;
    GraphEntry e;
    memset(&e, 0, sizeof e);
    lx_strcpy(e.ref, sizeof e.ref, ref);
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    char *text = lx_file_read(path, NULL);
    e.g = text ? lx_graph_from_text(text) : NULL;
    lx_free(text);
    if (e.g) lx_strcpy(e.g->ref, sizeof e.g->ref, ref);
    else LX_WARN("State graph not found or unreadable: %s", ref);
    LX_VEC_PUSH(g_graphs, g_graph_count, g_graph_cap, e);
    return e.g;
}

/* The cached graph keeps its address (running graphs point at it): the new contents move in, the old go. */
void lx_graph_replace(const char *ref, LxGraph *g) {
    if (!g) return;
    for (int i = 0; i < g_graph_count; i++)
        if (!strcmp(g_graphs[i].ref, ref)) {
            LxGraph *cur = g_graphs[i].g;
            if (!cur) { g_graphs[i].g = g; lx_strcpy(g->ref, sizeof g->ref, ref); g->version++; return; }
            if (cur == g) { lx_graph_touch(g); return; }
            uint32_t ver = cur->version + 1;
            LxGraph tmp = *cur;
            *cur = *g;
            *g = tmp;
            lx_graph_free(g);
            lx_strcpy(cur->ref, sizeof cur->ref, ref);
            cur->version = ver;
            cur->dirty = true;
            return;
        }
    GraphEntry e;
    memset(&e, 0, sizeof e);
    lx_strcpy(e.ref, sizeof e.ref, ref);
    e.g = g;
    lx_strcpy(g->ref, sizeof g->ref, ref);
    LX_VEC_PUSH(g_graphs, g_graph_count, g_graph_cap, e);
}

void lx_graphs_clear(void) {
    for (int i = 0; i < g_graph_count; i++) lx_graph_free(g_graphs[i].g);
    lx_free(g_graphs);
    g_graphs = NULL;
    g_graph_count = g_graph_cap = 0;
}

/* ---------------- editing helpers ---------------- */
LxGraphState *lx_graph_find_state(const LxGraph *g, const char *name) {
    int i = lx_graph_state_index(g, name);
    return i >= 0 ? &g->states[i] : NULL;
}
int lx_graph_state_index(const LxGraph *g, const char *name) {
    if (!g || !name || !name[0]) return -1;
    for (int i = 0; i < g->state_count; i++) if (!strcmp(g->states[i].name, name)) return i;
    return -1;
}

void lx_graph_unique_name(const LxGraph *g, const char *base, char *out, size_t cap, int kind) {
    char b[LX_FSM_NAME];
    lx_strcpy(b, sizeof b, base && base[0] ? base : (kind == 0 ? "State" : kind == 1 ? "EVENT" : "Variable"));
    for (int n = 1; n < 10000; n++) {
        if (n == 1) lx_strcpy(out, cap, b);
        else snprintf(out, cap, "%s %d", b, n);
        bool used = kind == 0 ? lx_graph_find_state(g, out) != NULL : kind == 1 ? lx_graph_find_event(g, out) != NULL : lx_graph_find_var(g, out) != NULL;
        if (!used) return;
    }
}

LxGraphState *lx_graph_add_state(LxGraph *g, const char *name, float x, float y) {
    char nm[LX_FSM_NAME];
    lx_graph_unique_name(g, name, nm, sizeof nm, 0);
    LxGraphState *st = PUSH(g->states, g->state_count, g->state_cap, LxGraphState);
    lx_strcpy(st->name, sizeof st->name, nm);
    st->x = x;
    st->y = y;
    if (!g->start[0]) lx_strcpy(g->start, sizeof g->start, nm);
    lx_graph_touch(g);
    return st;
}

static void retarget(LxGraph *g, const char *from, const char *to) {
    for (int i = 0; i < g->state_count; i++)
        for (int t = 0; t < g->states[i].trans_count; t++)
            if (!strcmp(g->states[i].trans[t].to, from)) lx_strcpy(g->states[i].trans[t].to, LX_FSM_NAME, to);
    for (int t = 0; t < g->global_count; t++)
        if (!strcmp(g->globals[t].to, from)) lx_strcpy(g->globals[t].to, LX_FSM_NAME, to);
    if (!strcmp(g->start, from)) lx_strcpy(g->start, sizeof g->start, to);
}

void lx_graph_remove_state(LxGraph *g, int index) {
    if (index < 0 || index >= g->state_count || g->state_count <= 1) return;   /* a graph keeps at least one state */
    char name[LX_FSM_NAME];
    lx_strcpy(name, sizeof name, g->states[index].name);
    state_free(&g->states[index]);
    memmove(&g->states[index], &g->states[index + 1], sizeof *g->states * (size_t)(g->state_count - index - 1));
    g->state_count--;
    retarget(g, name, "");
    /* global transitions to it go away */
    for (int t = g->global_count - 1; t >= 0; t--)
        if (!g->globals[t].to[0]) { memmove(&g->globals[t], &g->globals[t + 1], sizeof *g->globals * (size_t)(g->global_count - t - 1)); g->global_count--; }
    if (!g->start[0]) lx_strcpy(g->start, sizeof g->start, g->states[0].name);
    lx_graph_touch(g);
}

bool lx_graph_rename_state(LxGraph *g, int index, const char *name) {
    if (index < 0 || index >= g->state_count || !name || !name[0]) return false;
    if (!strcmp(g->states[index].name, name)) return true;
    if (lx_graph_find_state(g, name)) return false;
    char old[LX_FSM_NAME];
    lx_strcpy(old, sizeof old, g->states[index].name);
    lx_strcpy(g->states[index].name, LX_FSM_NAME, name);
    retarget(g, old, name);
    lx_graph_touch(g);
    return true;
}

LxGraphAction *lx_graph_add_action(LxGraphState *st, const LxFsmActionDesc *d, int at) {
    LxGraphAction a;
    memset(&a, 0, sizeof a);
    lx_strcpy(a.type, sizeof a.type, d->name);
    a.enabled = true;
    a.update = d->default_update;
    for (int k = 0; k < LX_FSM_MAX_PARAMS && d->params[k].name; k++) {
        const char *def = d->params[k].def ? d->params[k].def : lx_fsm_default_value(d->params[k].type);
        if (d->params[k].flags & LX_FSM_P_OUT) def = "";
        lx_strcpy(a.p[k].value, sizeof a.p[k].value, def);
    }
    LX_VEC_PUSH(st->actions, st->action_count, st->action_cap, a);
    if (at >= 0 && at < st->action_count - 1) {
        LxGraphAction tmp = st->actions[st->action_count - 1];
        memmove(&st->actions[at + 1], &st->actions[at], sizeof tmp * (size_t)(st->action_count - 1 - at));
        st->actions[at] = tmp;
        return &st->actions[at];
    }
    return &st->actions[st->action_count - 1];
}
void lx_graph_remove_action(LxGraphState *st, int index) {
    if (index < 0 || index >= st->action_count) return;
    memmove(&st->actions[index], &st->actions[index + 1], sizeof *st->actions * (size_t)(st->action_count - index - 1));
    st->action_count--;
}
void lx_graph_move_action(LxGraphState *st, int from, int to) {
    if (from < 0 || from >= st->action_count || to < 0 || to >= st->action_count || from == to) return;
    LxGraphAction tmp = st->actions[from];
    if (from < to) memmove(&st->actions[from], &st->actions[from + 1], sizeof tmp * (size_t)(to - from));
    else memmove(&st->actions[to + 1], &st->actions[to], sizeof tmp * (size_t)(from - to));
    st->actions[to] = tmp;
}

LxGraphTransition *lx_graph_add_transition(LxGraph *g, LxGraphState *st, const char *event) {
    for (int t = 0; t < st->trans_count; t++) if (!strcmp(st->trans[t].event, event)) return &st->trans[t];   /* one per event */
    if (!lx_fsm_is_system_event(event) && !lx_graph_find_event(g, event)) lx_graph_add_event(g, event);
    LxGraphTransition *t = PUSH(st->trans, st->trans_count, st->trans_cap, LxGraphTransition);
    lx_strcpy(t->event, sizeof t->event, event);
    lx_graph_touch(g);
    return t;
}
void lx_graph_remove_transition(LxGraphState *st, int index) {
    if (index < 0 || index >= st->trans_count) return;
    memmove(&st->trans[index], &st->trans[index + 1], sizeof *st->trans * (size_t)(st->trans_count - index - 1));
    st->trans_count--;
}

LxGraphEvent *lx_graph_find_event(const LxGraph *g, const char *name) {
    for (int i = 0; i < g->event_count; i++) if (!strcmp(g->events[i].name, name)) return &g->events[i];
    return NULL;
}
LxGraphEvent *lx_graph_add_event(LxGraph *g, const char *name) {
    LxGraphEvent *e = lx_graph_find_event(g, name);
    if (e || !name || !name[0]) return e;
    e = PUSH(g->events, g->event_count, g->event_cap, LxGraphEvent);
    lx_strcpy(e->name, sizeof e->name, name);
    lx_graph_touch(g);
    return e;
}
void lx_graph_remove_event(LxGraph *g, int index) {
    if (index < 0 || index >= g->event_count) return;
    char name[LX_FSM_NAME];
    lx_strcpy(name, sizeof name, g->events[index].name);
    memmove(&g->events[index], &g->events[index + 1], sizeof *g->events * (size_t)(g->event_count - index - 1));
    g->event_count--;
    /* transitions on it go too */
    for (int i = 0; i < g->state_count; i++)
        for (int t = g->states[i].trans_count - 1; t >= 0; t--)
            if (!strcmp(g->states[i].trans[t].event, name)) lx_graph_remove_transition(&g->states[i], t);
    for (int t = g->global_count - 1; t >= 0; t--)
        if (!strcmp(g->globals[t].event, name)) { memmove(&g->globals[t], &g->globals[t + 1], sizeof *g->globals * (size_t)(g->global_count - t - 1)); g->global_count--; }
    lx_graph_touch(g);
}
int lx_graph_event_uses(const LxGraph *g, const char *name) {
    int n = 0;
    for (int i = 0; i < g->state_count; i++) {
        const LxGraphState *st = &g->states[i];
        for (int t = 0; t < st->trans_count; t++) n += !strcmp(st->trans[t].event, name);
        for (int a = 0; a < st->action_count; a++) {
            const LxFsmActionDesc *d = lx_fsm_action_find(st->actions[a].type);
            for (int k = 0; d && k < LX_FSM_MAX_PARAMS && d->params[k].name; k++)
                if (d->params[k].type == LX_FSM_EVENT && !st->actions[a].p[k].is_var && !strcmp(st->actions[a].p[k].value, name)) n++;
        }
    }
    for (int t = 0; t < g->global_count; t++) n += !strcmp(g->globals[t].event, name);
    return n;
}

LxGraphVar *lx_graph_find_var(const LxGraph *g, const char *name) {
    for (int i = 0; i < g->var_count; i++) if (!strcmp(g->vars[i].name, name)) return &g->vars[i];
    return NULL;
}
LxGraphVar *lx_graph_add_var(LxGraph *g, const char *name, LxFsmType type) {
    char nm[LX_FSM_NAME];
    lx_graph_unique_name(g, name, nm, sizeof nm, 2);
    LxGraphVar *v = PUSH(g->vars, g->var_count, g->var_cap, LxGraphVar);
    lx_strcpy(v->name, sizeof v->name, nm);
    v->type = type;
    lx_strcpy(v->value, sizeof v->value, lx_fsm_default_value(type));
    lx_graph_touch(g);
    return v;
}
static void rename_var_refs(LxGraph *g, const char *from, const char *to) {
    for (int i = 0; i < g->state_count; i++)
        for (int a = 0; a < g->states[i].action_count; a++)
            for (int k = 0; k < LX_FSM_MAX_PARAMS; k++) {
                LxGraphParam *p = &g->states[i].actions[a].p[k];
                if (p->is_var && !strcmp(p->value, from)) {
                    if (to) lx_strcpy(p->value, sizeof p->value, to);
                    else { p->is_var = false; p->value[0] = 0; }
                }
            }
}
void lx_graph_remove_var(LxGraph *g, int index) {
    if (index < 0 || index >= g->var_count) return;
    char name[LX_FSM_NAME];
    lx_strcpy(name, sizeof name, g->vars[index].name);
    memmove(&g->vars[index], &g->vars[index + 1], sizeof *g->vars * (size_t)(g->var_count - index - 1));
    g->var_count--;
    rename_var_refs(g, name, NULL);
    lx_graph_touch(g);
}
bool lx_graph_rename_var(LxGraph *g, int index, const char *name) {
    if (index < 0 || index >= g->var_count || !name || !name[0]) return false;
    if (!strcmp(g->vars[index].name, name)) return true;
    if (lx_graph_find_var(g, name)) return false;
    char old[LX_FSM_NAME];
    lx_strcpy(old, sizeof old, g->vars[index].name);
    lx_strcpy(g->vars[index].name, LX_FSM_NAME, name);
    rename_var_refs(g, old, name);
    lx_graph_touch(g);
    return true;
}
int lx_graph_var_uses(const LxGraph *g, const char *name) {
    int n = 0;
    for (int i = 0; i < g->state_count; i++)
        for (int a = 0; a < g->states[i].action_count; a++)
            for (int k = 0; k < LX_FSM_MAX_PARAMS; k++)
                n += g->states[i].actions[a].p[k].is_var && !strcmp(g->states[i].actions[a].p[k].value, name);
    return n;
}

/* ---------------- summaries & problems ---------------- */
void lx_graph_action_summary(const LxGraphAction *a, const LxFsmActionDesc *d, char *out, size_t cap) {
    out[0] = 0;
    if (!d) { snprintf(out, cap, "Unknown action \"%s\"", a->type); return; }
    const char *t = d->summary ? d->summary : d->description ? d->description : d->name;
    size_t n = 0;
    for (const char *p = t; *p && n + 1 < cap; p++) {
        if (*p == '{' && p[1] >= '0' && p[1] <= '9') {
            int k = atoi(p + 1);
            const char *close = strchr(p, '}');
            if (close && k >= 0 && k < LX_FSM_MAX_PARAMS) {
                char val[LX_REF_MAX];
                const LxGraphParam *gp = &a->p[k];
                if (gp->is_var) lx_strcpy(val, sizeof val, gp->value);
                else if (d->params[k].type == LX_FSM_ENUM && d->params[k].options) {
                    /* the option's name */
                    int idx = atoi(gp->value), o = 0;
                    const char *s = d->params[k].options;
                    while (o < idx && (s = strchr(s, '|'))) { s++; o++; }
                    size_t l = s ? strcspn(s, "|") : 0;
                    if (l >= sizeof val) l = sizeof val - 1;
                    if (s) memcpy(val, s, l);
                    val[s ? l : 0] = 0;
                } else if (d->params[k].type == LX_FSM_OBJECT && (!strcmp(gp->value, "Owner") || !gp->value[0])) lx_strcpy(val, sizeof val, gp->value[0] ? "self" : "None");
                else if (d->params[k].type == LX_FSM_ASSET) lx_strcpy(val, sizeof val, gp->value[0] ? lx_path_filename(gp->value) : "None");
                else if (!gp->value[0]) lx_strcpy(val, sizeof val, "None");
                else lx_strcpy(val, sizeof val, gp->value);
                if (!strncmp(val, "tag:", 4)) memmove(val, val + 4, strlen(val + 4) + 1);
                if (!strncmp(val, "name:", 5)) memmove(val, val + 5, strlen(val + 5) + 1);
                size_t l = strlen(val);
                if (n + l >= cap) l = cap - n - 1;
                memcpy(out + n, val, l);
                n += l;
                p = close;
                continue;
            }
        }
        out[n++] = *p;
    }
    out[n] = 0;
}

int lx_graph_problems(const LxGraph *g, char (*out)[160], int max) {
    int n = 0;
#define PROBLEM(...) do { if (n < max) snprintf(out[n], 160, __VA_ARGS__); n++; } while (0)
    for (int i = 0; i < g->state_count; i++) {
        const LxGraphState *st = &g->states[i];
        for (int t = 0; t < st->trans_count; t++) {
            if (!st->trans[t].to[0]) PROBLEM("%s: transition %s isn't connected", st->name, st->trans[t].event);
            else if (!lx_graph_find_state(g, st->trans[t].to)) PROBLEM("%s: %s goes to a missing state", st->name, st->trans[t].event);
        }
        for (int a = 0; a < st->action_count; a++) {
            const LxGraphAction *ac = &st->actions[a];
            const LxFsmActionDesc *d = lx_fsm_action_find(ac->type);
            if (!d) { PROBLEM("%s: unknown action %s", st->name, ac->type); continue; }
            for (int k = 0; k < LX_FSM_MAX_PARAMS && d->params[k].name; k++) {
                const LxGraphParam *p = &ac->p[k];
                if (p->is_var && !lx_graph_find_var(g, p->value) && !lx_fsm_globals_find_var(p->value))
                    PROBLEM("%s / %s: variable %s doesn't exist", st->name, d->name, p->value);
                if ((d->params[k].flags & LX_FSM_P_OUT) && !p->is_var && !(d->params[k].flags & LX_FSM_P_OPTIONAL))
                    PROBLEM("%s / %s: choose a variable for %s", st->name, d->name, d->params[k].name);
            }
        }
    }
    for (int e = 0; e < g->event_count; e++) {
        const char *name = g->events[e].name;
        bool handled = false, sent = false;
        for (int i = 0; i < g->state_count && !handled; i++)
            for (int t = 0; t < g->states[i].trans_count; t++) handled |= !strcmp(g->states[i].trans[t].event, name);
        for (int t = 0; t < g->global_count; t++) handled |= !strcmp(g->globals[t].event, name);
        for (int i = 0; i < g->state_count && !sent; i++)
            for (int a = 0; a < g->states[i].action_count && !sent; a++)
                for (int k = 0; k < LX_FSM_MAX_PARAMS; k++)
                    if (!g->states[i].actions[a].p[k].is_var && !strcmp(g->states[i].actions[a].p[k].value, name)) sent = true;
        if (handled && !sent && !g->events[e].global) PROBLEM("Event %s is never sent in this graph (mark it Global if other objects send it)", name);
    }
    return n;
#undef PROBLEM
}

void lx_graphs_touch_all(void) {
    for (int i = 0; i < g_graph_count; i++) if (g_graphs[i].g) g_graphs[i].g->version++;
}

int lx_graphs_loaded(LxGraph **out, int max) {
    int n = 0;
    for (int i = 0; i < g_graph_count && n < max; i++) if (g_graphs[i].g) out[n++] = g_graphs[i].g;
    return n;
}
