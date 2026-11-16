/* Animator graph assets (.lxanim): the data model, its text format, and editing helpers.
 * The runtime that plays one of these lives in anim.c; blend tree weights in anim_blend.c. */
#include "../lx_internal.h"
#include "anim_graph.h"
#include <stdio.h>

static const char *PARAM_TYPES[LX_AP_TYPE_COUNT] = {"float", "int", "bool", "trigger"};
static const char *COND_MODES[LX_AC_MODE_COUNT] = {"greater", "less", "equals", "not_equal", "if", "if_not"};
static const char *TREE_TYPES[LX_AT_TYPE_COUNT] = {"1d", "2d_directional", "2d_cartesian", "direct"};
static const char *BLEND_MODES[] = {"override", "additive"};
static const char *INTERRUPTS[] = {"none", "current", "next", "both"};

const char *lx_anim_param_type_name(LxAnimParamType t) { return t >= 0 && t < LX_AP_TYPE_COUNT ? PARAM_TYPES[t] : "float"; }
const char *lx_anim_cond_mode_name(LxAnimCondMode m) { return m >= 0 && m < LX_AC_MODE_COUNT ? COND_MODES[m] : "greater"; }
const char *lx_anim_tree_type_name(LxAnimTreeType t) { return t >= 0 && t < LX_AT_TYPE_COUNT ? TREE_TYPES[t] : "1d"; }

static int name_index(const char *const *names, int count, const char *s) {
    for (int i = 0; i < count; i++) if (!strcmp(s, names[i])) return i;
    return -1;
}

/* ---------------- lifetime ---------------- */
LxAnimGraph *lx_anim_graph_new(const char *name) {
    LxAnimGraph *g = lx_calloc(1, sizeof *g);
    lx_strcpy(g->name, sizeof g->name, name && name[0] ? name : "Animator");
    LxAnimLayer *l = lx_anim_add_layer(g, "Base Layer");
    LxAnimNode *n = lx_anim_add_state(g, l, "New State", 120, 120);
    lx_strcpy(l->start, sizeof l->start, n->name);
    g->dirty = false;
    return g;
}

static void state_free(LxAnimNode *n) { lx_free(n->trans); }
static void layer_free(LxAnimLayer *l) {
    for (int i = 0; i < l->state_count; i++) state_free(&l->states[i]);
    lx_free(l->states);
    lx_free(l->any);
}

void lx_anim_graph_free(LxAnimGraph *g) {
    if (!g) return;
    for (int i = 0; i < g->layer_count; i++) layer_free(&g->layers[i]);
    lx_free(g->layers);
    lx_free(g->params);
    lx_free(g->trees);
    lx_free(g);
}

LxAnimGraph *lx_anim_graph_clone(const LxAnimGraph *g) {
    char *t = lx_anim_graph_to_text(g);
    LxAnimGraph *c = lx_anim_graph_from_text(t);
    lx_free(t);
    if (c) { lx_strcpy(c->ref, sizeof c->ref, g->ref); c->version = g->version; }
    return c;
}

void lx_anim_graph_touch(LxAnimGraph *g) { if (g) { g->version++; g->dirty = true; } }

/* ---------------- text format ---------------- */
/* Same conventions as the State Graph's: one line per value, newlines written as \n, leading and
 * trailing spaces as \s, so every text comes back exactly as it was. */
static const char *esc(const char *v, char *buf, size_t cap) {
    size_t n = 0, len = strlen(v);
    for (size_t i = 0; i < len && n < cap - 3; i++) {
        char ch = v[i];
        bool edge = i == 0 || i == len - 1;
        if (ch == '\n') { buf[n++] = '\\'; buf[n++] = 'n'; }
        else if (ch == '\\') { buf[n++] = '\\'; buf[n++] = '\\'; }
        else if ((ch == ' ' || ch == '\t') && edge) { buf[n++] = '\\'; buf[n++] = 's'; }
        else if ((unsigned char)ch >= 0x20) buf[n++] = ch;
    }
    buf[n] = 0;
    return buf;
}
static void put_escaped(LxStrBuf *sb, const char *key, const char *v) {
    char buf[1100];
    lx_sb_printf(sb, "%s = %s\n", key, esc(v, buf, sizeof buf));
}
static void unescape(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
        else if (*p == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
        else if (*p == '\\' && p[1] == 's') { *o++ = ' '; p++; }
        else *o++ = *p;
    }
    *o = 0;
}

static void write_transition(LxStrBuf *sb, const LxAnimTransition *t, const char *section) {
    lx_sb_printf(sb, "[%s]\n", section);
    put_escaped(sb, "to", t->to);
    lx_sb_printf(sb, "exit = %d %g\nduration = %g %d\noffset = %g\ninterrupt = %s\n",
                 t->has_exit_time, (double)t->exit_time, (double)t->duration, t->fixed_duration,
                 (double)t->offset, INTERRUPTS[t->interrupt & 3]);
    if (t->to_self) lx_sb_printf(sb, "to_self = 1\n");
    if (t->mute) lx_sb_printf(sb, "mute = 1\n");
    if (t->solo) lx_sb_printf(sb, "solo = 1\n");
    for (int c = 0; c < t->cond_count; c++) {
        char buf[200];
        lx_sb_printf(sb, "condition = %s %s %g\n", lx_anim_cond_mode_name(t->cond[c].mode),
                     esc(t->cond[c].param, buf, sizeof buf), (double)t->cond[c].value);
    }
}

char *lx_anim_graph_to_text(const LxAnimGraph *g) {
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora animator graph v1\n[animator]\n");
    put_escaped(&sb, "name", g->name);
    put_escaped(&sb, "description", g->description);
    for (int i = 0; i < g->param_count; i++) {
        const LxAnimParam *p = &g->params[i];
        lx_sb_printf(&sb, "\n[parameter]\n");
        put_escaped(&sb, "name", p->name);
        lx_sb_printf(&sb, "type = %s\nvalue = %g %d %d\n", lx_anim_param_type_name(p->type), (double)p->f, p->i, p->b);
    }
    for (int i = 0; i < g->tree_count; i++) {
        const LxAnimTree *t = &g->trees[i];
        lx_sb_printf(&sb, "\n[tree]\n");
        lx_sb_printf(&sb, "index = %d\ntype = %s\n", i, lx_anim_tree_type_name(t->type));
        put_escaped(&sb, "param_x", t->param_x);
        put_escaped(&sb, "param_y", t->param_y);
        for (int c = 0; c < t->child_count; c++) {
            const LxAnimChild *ch = &t->child[c];
            lx_sb_printf(&sb, "[child]\n");
            put_escaped(&sb, "clip", ch->clip);
            lx_sb_printf(&sb, "tree = %d\nthreshold = %g\npos = %g %g\nspeed = %g\nmirror = %d\n",
                         ch->tree, (double)ch->threshold, (double)ch->px, (double)ch->py, (double)ch->speed, ch->mirror);
            if (ch->weight_param[0]) put_escaped(&sb, "weight_param", ch->weight_param);
        }
    }
    for (int i = 0; i < g->layer_count; i++) {
        const LxAnimLayer *l = &g->layers[i];
        lx_sb_printf(&sb, "\n[layer]\n");
        put_escaped(&sb, "name", l->name);
        put_escaped(&sb, "start", l->start);
        lx_sb_printf(&sb, "weight = %g\nblend = %s\nmask = %llu\n", (double)l->weight,
                     BLEND_MODES[l->blend & 1], (unsigned long long)l->mask);
        for (int a = 0; a < l->any_count; a++) write_transition(&sb, &l->any[a], "any");
        for (int s = 0; s < l->state_count; s++) {
            const LxAnimNode *n = &l->states[s];
            lx_sb_printf(&sb, "[state]\n");
            put_escaped(&sb, "name", n->name);
            lx_sb_printf(&sb, "pos = %g %g\ncolor = %d\nmotion = %s\ntree = %d\nspeed = %g\n",
                         (double)n->x, (double)n->y, n->color, n->motion == LX_AM_TREE ? "tree" : "clip",
                         n->tree, (double)n->speed);
            put_escaped(&sb, "clip", n->clip);
            lx_sb_printf(&sb, "loop = %d\nmirror = %d\ncycle_offset = %g\nfoot_ik = %d\n",
                         n->loop, n->mirror, (double)n->cycle_offset, n->foot_ik);
            if (n->speed_param[0]) put_escaped(&sb, "speed_param", n->speed_param);
            if (n->tag[0]) put_escaped(&sb, "tag", n->tag);
            if (n->description[0]) put_escaped(&sb, "description", n->description);
            if (n->breakpoint) lx_sb_printf(&sb, "breakpoint = 1\n");
            for (int t = 0; t < n->trans_count; t++) write_transition(&sb, &n->trans[t], "transition");
        }
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

LxAnimGraph *lx_anim_graph_from_text(const char *text) {
    if (!text) return NULL;
    LxAnimGraph *g = lx_calloc(1, sizeof *g);
    const char *cur = text;
    char k[256], v[1024];
    int r;
    enum { NONE, ROOT, PARAM, TREE, CHILD, LAYER, STATE, TRANS, ANY } sec = NONE;
    LxAnimLayer *l = NULL;
    LxAnimNode *n = NULL;
    LxAnimTree *tr = NULL;
    LxAnimChild *ch = NULL;
    LxAnimTransition *t = NULL;
    while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
        if (r == 2) {
            if (!strcmp(k, "animator")) sec = ROOT;
            else if (!strcmp(k, "parameter")) { sec = PARAM; PUSH(g->params, g->param_count, g->param_cap, LxAnimParam); }
            else if (!strcmp(k, "tree")) {
                sec = TREE;
                tr = PUSH(g->trees, g->tree_count, g->tree_cap, LxAnimTree);
                ch = NULL;
            } else if (!strcmp(k, "child") && tr) {
                sec = CHILD;
                ch = tr->child_count < LX_ANIM_MAX_CHILDREN ? &tr->child[tr->child_count++] : NULL;
                if (ch) { memset(ch, 0, sizeof *ch); ch->tree = -1; ch->speed = 1; }
            } else if (!strcmp(k, "layer")) {
                sec = LAYER;
                l = PUSH(g->layers, g->layer_count, g->layer_cap, LxAnimLayer);
                l->weight = 1;
                n = NULL;
                tr = NULL;
            } else if (!strcmp(k, "state") && l) {
                sec = STATE;
                n = PUSH(l->states, l->state_count, l->state_cap, LxAnimNode);
                n->speed = 1;
                n->loop = true;
                n->tree = -1;
                t = NULL;
            } else if (!strcmp(k, "transition") && n) {
                sec = TRANS;
                t = PUSH(n->trans, n->trans_count, n->trans_cap, LxAnimTransition);
                t->duration = 0.25f;
                t->has_exit_time = true;
                t->exit_time = 1;
            } else if (!strcmp(k, "any") && l) {
                sec = ANY;
                t = PUSH(l->any, l->any_count, l->any_cap, LxAnimTransition);
                t->duration = 0.25f;
            } else sec = NONE;
            continue;
        }
        if (strcmp(k, "condition")) unescape(v);
        switch (sec) {
        case ROOT:
            if (!strcmp(k, "name")) lx_strcpy(g->name, sizeof g->name, v);
            else if (!strcmp(k, "description")) lx_strcpy(g->description, sizeof g->description, v);
            break;
        case PARAM: {
            LxAnimParam *p = &g->params[g->param_count - 1];
            if (!strcmp(k, "name")) lx_strcpy(p->name, sizeof p->name, v);
            else if (!strcmp(k, "type")) { int i = name_index(PARAM_TYPES, LX_AP_TYPE_COUNT, v); p->type = i < 0 ? LX_AP_FLOAT : (LxAnimParamType)i; }
            else if (!strcmp(k, "value")) {
                float f = 0;
                int iv = 0, bv = 0;
                sscanf(v, "%f %d %d", &f, &iv, &bv);
                p->f = f;
                p->i = iv;
                p->b = bv != 0;
            }
            break;
        }
        case TREE:
            if (!strcmp(k, "type")) { int i = name_index(TREE_TYPES, LX_AT_TYPE_COUNT, v); tr->type = i < 0 ? LX_AT_1D : (LxAnimTreeType)i; }
            else if (!strcmp(k, "param_x")) lx_strcpy(tr->param_x, sizeof tr->param_x, v);
            else if (!strcmp(k, "param_y")) lx_strcpy(tr->param_y, sizeof tr->param_y, v);
            break;
        case CHILD:
            if (!ch) break;
            if (!strcmp(k, "clip")) lx_strcpy(ch->clip, sizeof ch->clip, v);
            else if (!strcmp(k, "tree")) ch->tree = atoi(v);
            else if (!strcmp(k, "threshold")) ch->threshold = (float)atof(v);
            else if (!strcmp(k, "pos")) { LxVec3 p = lx_parse_vec3(v); ch->px = p.x; ch->py = p.y; }
            else if (!strcmp(k, "speed")) ch->speed = (float)atof(v);
            else if (!strcmp(k, "mirror")) ch->mirror = lx_parse_bool(v);
            else if (!strcmp(k, "weight_param")) lx_strcpy(ch->weight_param, sizeof ch->weight_param, v);
            break;
        case LAYER:
            if (!strcmp(k, "name")) lx_strcpy(l->name, sizeof l->name, v);
            else if (!strcmp(k, "start")) lx_strcpy(l->start, sizeof l->start, v);
            else if (!strcmp(k, "weight")) l->weight = (float)atof(v);
            else if (!strcmp(k, "blend")) l->blend = !strcmp(v, "additive") ? LX_ABM_ADDITIVE : LX_ABM_OVERRIDE;
            else if (!strcmp(k, "mask")) l->mask = strtoull(v, NULL, 10);
            break;
        case STATE:
            if (!strcmp(k, "name")) lx_strcpy(n->name, sizeof n->name, v);
            else if (!strcmp(k, "pos")) { LxVec3 p = lx_parse_vec3(v); n->x = p.x; n->y = p.y; }
            else if (!strcmp(k, "color")) n->color = atoi(v);
            else if (!strcmp(k, "motion")) n->motion = !strcmp(v, "tree") ? LX_AM_TREE : LX_AM_CLIP;
            else if (!strcmp(k, "tree")) n->tree = atoi(v);
            else if (!strcmp(k, "speed")) n->speed = (float)atof(v);
            else if (!strcmp(k, "clip")) lx_strcpy(n->clip, sizeof n->clip, v);
            else if (!strcmp(k, "loop")) n->loop = lx_parse_bool(v);
            else if (!strcmp(k, "mirror")) n->mirror = lx_parse_bool(v);
            else if (!strcmp(k, "cycle_offset")) n->cycle_offset = (float)atof(v);
            else if (!strcmp(k, "foot_ik")) n->foot_ik = lx_parse_bool(v);
            else if (!strcmp(k, "speed_param")) lx_strcpy(n->speed_param, sizeof n->speed_param, v);
            else if (!strcmp(k, "tag")) lx_strcpy(n->tag, sizeof n->tag, v);
            else if (!strcmp(k, "description")) lx_strcpy(n->description, sizeof n->description, v);
            else if (!strcmp(k, "breakpoint")) n->breakpoint = lx_parse_bool(v);
            break;
        case TRANS:
        case ANY:
            if (!t) break;
            if (!strcmp(k, "to")) lx_strcpy(t->to, sizeof t->to, v);
            else if (!strcmp(k, "exit")) {
                int has = 0;
                float e = 1;
                sscanf(v, "%d %f", &has, &e);
                t->has_exit_time = has != 0;
                t->exit_time = e;
            } else if (!strcmp(k, "duration")) {
                float dur = 0.25f;
                int fixed = 0;
                sscanf(v, "%f %d", &dur, &fixed);
                t->duration = dur;
                t->fixed_duration = fixed != 0;
            } else if (!strcmp(k, "offset")) t->offset = (float)atof(v);
            else if (!strcmp(k, "interrupt")) { int i = name_index(INTERRUPTS, 4, v); t->interrupt = i < 0 ? LX_AI_NONE : (LxAnimInterrupt)i; }
            else if (!strcmp(k, "to_self")) t->to_self = lx_parse_bool(v);
            else if (!strcmp(k, "mute")) t->mute = lx_parse_bool(v);
            else if (!strcmp(k, "solo")) t->solo = lx_parse_bool(v);
            else if (!strcmp(k, "condition") && t->cond_count < LX_ANIM_MAX_COND) {
                /* "mode Parameter Name 0.5": the mode and the threshold bracket the name, which may hold spaces */
                char mode[32] = "";
                const char *p = v;
                size_t mi = 0;
                while (*p && *p != ' ' && mi + 1 < sizeof mode) mode[mi++] = *p++;
                mode[mi] = 0;
                while (*p == ' ') p++;
                const char *last = strrchr(p, ' ');
                LxAnimCond *c = &t->cond[t->cond_count++];
                int mi2 = name_index(COND_MODES, LX_AC_MODE_COUNT, mode);
                c->mode = mi2 < 0 ? LX_AC_GREATER : (LxAnimCondMode)mi2;
                if (last) {
                    size_t len = (size_t)(last - p);
                    if (len >= sizeof c->param) len = sizeof c->param - 1;
                    memcpy(c->param, p, len);
                    c->param[len] = 0;
                    c->value = (float)atof(last + 1);
                } else lx_strcpy(c->param, sizeof c->param, p);
                unescape(c->param);
            }
            break;
        default: break;
        }
    }
    if (!g->layer_count) { lx_anim_graph_free(g); return NULL; }
    /* a layer with no valid start state starts in its first */
    for (int i = 0; i < g->layer_count; i++) {
        LxAnimLayer *lay = &g->layers[i];
        if (lay->state_count && !lx_anim_find_state(lay, lay->start)) lx_strcpy(lay->start, sizeof lay->start, lay->states[0].name);
        for (int s = 0; s < lay->state_count; s++)
            if (lay->states[s].tree >= g->tree_count) lay->states[s].tree = -1;
    }
    for (int i = 0; i < g->tree_count; i++)
        for (int c = 0; c < g->trees[i].child_count; c++) {
            LxAnimChild *cc = &g->trees[i].child[c];
            if (cc->tree >= g->tree_count || cc->tree == i) cc->tree = -1;   /* no dangling or self-referencing trees */
        }
    return g;
}

bool lx_anim_graph_save(LxAnimGraph *g, const char *path) {
    char *t = lx_anim_graph_to_text(g);
    bool ok = lx_file_write(path, t, strlen(t));
    lx_free(t);
    if (ok) g->dirty = false;
    return ok;
}

/* ---------------- registry ---------------- */
typedef struct AnimEntry { char ref[LX_REF_MAX]; LxAnimGraph *g; } AnimEntry;
static AnimEntry *g_anims;
static int g_anim_count, g_anim_cap;

LxAnimGraph *lx_asset_anim_graph(const char *ref) {
    if (!ref || !ref[0]) return NULL;
    for (int i = 0; i < g_anim_count; i++) if (!strcmp(g_anims[i].ref, ref)) return g_anims[i].g;
    AnimEntry e;
    memset(&e, 0, sizeof e);
    lx_strcpy(e.ref, sizeof e.ref, ref);
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, ref);
    char *text = lx_file_read(path, NULL);
    e.g = text ? lx_anim_graph_from_text(text) : NULL;
    lx_free(text);
    if (e.g) lx_strcpy(e.g->ref, sizeof e.g->ref, ref);
    else LX_WARN("Animator graph not found or unreadable: %s", ref);
    LX_VEC_PUSH(g_anims, g_anim_count, g_anim_cap, e);
    return e.g;
}

/* The cached graph keeps its address (running animators point at it): the new contents move in, the old go. */
void lx_anim_graph_replace(const char *ref, LxAnimGraph *g) {
    if (!g) return;
    for (int i = 0; i < g_anim_count; i++)
        if (!strcmp(g_anims[i].ref, ref)) {
            LxAnimGraph *cur = g_anims[i].g;
            if (!cur) { g_anims[i].g = g; lx_strcpy(g->ref, sizeof g->ref, ref); g->version++; return; }
            if (cur == g) { lx_anim_graph_touch(g); return; }
            uint32_t ver = cur->version + 1;
            LxAnimGraph tmp = *cur;
            *cur = *g;
            *g = tmp;
            lx_anim_graph_free(g);
            lx_strcpy(cur->ref, sizeof cur->ref, ref);
            cur->version = ver;
            cur->dirty = true;
            return;
        }
    AnimEntry e;
    memset(&e, 0, sizeof e);
    lx_strcpy(e.ref, sizeof e.ref, ref);
    lx_strcpy(g->ref, sizeof g->ref, ref);
    e.g = g;
    LX_VEC_PUSH(g_anims, g_anim_count, g_anim_cap, e);
}

void lx_anim_graphs_clear(void) {
    for (int i = 0; i < g_anim_count; i++) lx_anim_graph_free(g_anims[i].g);
    lx_free(g_anims);
    g_anims = NULL;
    g_anim_count = g_anim_cap = 0;
}

/* ---------------- editing helpers ---------------- */
void lx_anim_unique_name(const LxAnimGraph *g, const LxAnimLayer *l, const char *base, char *out, size_t cap, int kind) {
    char want[LX_ANIM_NAME];
    lx_strcpy(want, sizeof want, base && base[0] ? base : "New");
    for (int n = 1; n < 1000; n++) {
        bool taken = false;
        if (kind == 0 && l) taken = lx_anim_find_state(l, want) != NULL;
        else if (kind == 1) taken = lx_anim_find_param(g, want) != NULL;
        else if (kind == 2) {
            for (int i = 0; i < g->layer_count; i++) if (!strcmp(g->layers[i].name, want)) { taken = true; break; }
        }
        if (!taken) break;
        snprintf(want, sizeof want, "%s %d", base && base[0] ? base : "New", n + 1);
    }
    lx_strcpy(out, cap, want);
}

LxAnimLayer *lx_anim_add_layer(LxAnimGraph *g, const char *name) {
    if (g->layer_count >= LX_ANIM_MAX_LAYERS) return NULL;
    LxAnimLayer *l = PUSH(g->layers, g->layer_count, g->layer_cap, LxAnimLayer);
    lx_anim_unique_name(g, NULL, name && name[0] ? name : "Layer", l->name, sizeof l->name, 2);
    l->weight = 1;
    lx_anim_graph_touch(g);
    return l;
}

void lx_anim_remove_layer(LxAnimGraph *g, int index) {
    if (index < 0 || index >= g->layer_count || g->layer_count <= 1) return;   /* a graph keeps a base layer */
    layer_free(&g->layers[index]);
    memmove(&g->layers[index], &g->layers[index + 1], sizeof(LxAnimLayer) * (size_t)(g->layer_count - index - 1));
    g->layer_count--;
    lx_anim_graph_touch(g);
}

LxAnimNode *lx_anim_add_state(LxAnimGraph *g, LxAnimLayer *l, const char *name, float x, float y) {
    if (!l) return NULL;
    LxAnimNode *n = PUSH(l->states, l->state_count, l->state_cap, LxAnimNode);
    lx_anim_unique_name(g, l, name && name[0] ? name : "New State", n->name, sizeof n->name, 0);
    n->x = x;
    n->y = y;
    n->speed = 1;
    n->loop = true;
    n->tree = -1;
    n->motion = LX_AM_CLIP;
    if (!l->start[0]) lx_strcpy(l->start, sizeof l->start, n->name);
    lx_anim_graph_touch(g);
    return n;
}

LxAnimNode *lx_anim_find_state(const LxAnimLayer *l, const char *name) {
    if (!l || !name || !name[0]) return NULL;
    for (int i = 0; i < l->state_count; i++) if (!strcmp(l->states[i].name, name)) return (LxAnimNode *)&l->states[i];
    return NULL;
}

int lx_anim_state_index(const LxAnimLayer *l, const char *name) {
    if (!l || !name || !name[0]) return -1;
    for (int i = 0; i < l->state_count; i++) if (!strcmp(l->states[i].name, name)) return i;
    return -1;
}

/* transitions pointing at a state that goes away become unconnected, as in the State Graph */
static void forget_state(LxAnimLayer *l, const char *name) {
    for (int a = 0; a < l->any_count; a++) if (!strcmp(l->any[a].to, name)) l->any[a].to[0] = 0;
    for (int s = 0; s < l->state_count; s++)
        for (int t = 0; t < l->states[s].trans_count; t++)
            if (!strcmp(l->states[s].trans[t].to, name)) l->states[s].trans[t].to[0] = 0;
}

void lx_anim_remove_state(LxAnimLayer *l, int index) {
    if (!l || index < 0 || index >= l->state_count) return;
    char gone[LX_ANIM_NAME];
    lx_strcpy(gone, sizeof gone, l->states[index].name);
    state_free(&l->states[index]);
    memmove(&l->states[index], &l->states[index + 1], sizeof(LxAnimNode) * (size_t)(l->state_count - index - 1));
    l->state_count--;
    forget_state(l, gone);
    if (!strcmp(l->start, gone)) lx_strcpy(l->start, sizeof l->start, l->state_count ? l->states[0].name : "");
}

bool lx_anim_rename_state(LxAnimLayer *l, int index, const char *name) {
    if (!l || index < 0 || index >= l->state_count || !name || !name[0]) return false;
    for (int i = 0; i < l->state_count; i++) if (i != index && !strcmp(l->states[i].name, name)) return false;
    char old[LX_ANIM_NAME];
    lx_strcpy(old, sizeof old, l->states[index].name);
    lx_strcpy(l->states[index].name, sizeof l->states[index].name, name);
    for (int a = 0; a < l->any_count; a++) if (!strcmp(l->any[a].to, old)) lx_strcpy(l->any[a].to, sizeof l->any[a].to, name);
    for (int s = 0; s < l->state_count; s++)
        for (int t = 0; t < l->states[s].trans_count; t++)
            if (!strcmp(l->states[s].trans[t].to, old))
                lx_strcpy(l->states[s].trans[t].to, sizeof l->states[s].trans[t].to, name);
    if (!strcmp(l->start, old)) lx_strcpy(l->start, sizeof l->start, name);
    return true;
}

static void transition_init(LxAnimTransition *t, const char *to, bool exit_time) {
    t->duration = 0.25f;
    t->has_exit_time = exit_time;
    t->exit_time = 1;
    if (to) lx_strcpy(t->to, sizeof t->to, to);
}

LxAnimTransition *lx_anim_add_transition(LxAnimNode *from, const char *to) {
    if (!from) return NULL;
    LxAnimTransition *t = PUSH(from->trans, from->trans_count, from->trans_cap, LxAnimTransition);
    transition_init(t, to, true);
    return t;
}

LxAnimTransition *lx_anim_add_any_transition(LxAnimLayer *l, const char *to) {
    if (!l) return NULL;
    LxAnimTransition *t = PUSH(l->any, l->any_count, l->any_cap, LxAnimTransition);
    transition_init(t, to, false);   /* Any State transitions do not wait for the clip to finish */
    return t;
}

void lx_anim_remove_transition(LxAnimNode *from, int index) {
    if (!from || index < 0 || index >= from->trans_count) return;
    memmove(&from->trans[index], &from->trans[index + 1], sizeof(LxAnimTransition) * (size_t)(from->trans_count - index - 1));
    from->trans_count--;
}

LxAnimCond *lx_anim_add_cond(LxAnimTransition *t, const char *param, LxAnimCondMode mode, float value) {
    if (!t || t->cond_count >= LX_ANIM_MAX_COND) return NULL;
    LxAnimCond *c = &t->cond[t->cond_count++];
    memset(c, 0, sizeof *c);
    lx_strcpy(c->param, sizeof c->param, param ? param : "");
    c->mode = mode;
    c->value = value;
    return c;
}

void lx_anim_remove_cond(LxAnimTransition *t, int index) {
    if (!t || index < 0 || index >= t->cond_count) return;
    memmove(&t->cond[index], &t->cond[index + 1], sizeof(LxAnimCond) * (size_t)(t->cond_count - index - 1));
    t->cond_count--;
}

LxAnimParam *lx_anim_add_param(LxAnimGraph *g, const char *name, LxAnimParamType type) {
    LxAnimParam *p = PUSH(g->params, g->param_count, g->param_cap, LxAnimParam);
    lx_anim_unique_name(g, NULL, name && name[0] ? name : "New Parameter", p->name, sizeof p->name, 1);
    p->type = type;
    lx_anim_graph_touch(g);
    return p;
}

LxAnimParam *lx_anim_find_param(const LxAnimGraph *g, const char *name) {
    if (!g || !name || !name[0]) return NULL;
    for (int i = 0; i < g->param_count; i++) if (!strcmp(g->params[i].name, name)) return (LxAnimParam *)&g->params[i];
    return NULL;
}

int lx_anim_param_index(const LxAnimGraph *g, const char *name) {
    if (!g || !name || !name[0]) return -1;
    for (int i = 0; i < g->param_count; i++) if (!strcmp(g->params[i].name, name)) return i;
    return -1;
}

/* how many conditions, speed multipliers and blend tree axes name this parameter */
int lx_anim_param_uses(const LxAnimGraph *g, const char *name) {
    int uses = 0;
    if (!g || !name || !name[0]) return 0;
    for (int i = 0; i < g->tree_count; i++) {
        if (!strcmp(g->trees[i].param_x, name)) uses++;
        if (!strcmp(g->trees[i].param_y, name)) uses++;
        for (int c = 0; c < g->trees[i].child_count; c++) if (!strcmp(g->trees[i].child[c].weight_param, name)) uses++;
    }
    for (int i = 0; i < g->layer_count; i++) {
        const LxAnimLayer *l = &g->layers[i];
        for (int a = 0; a < l->any_count; a++)
            for (int c = 0; c < l->any[a].cond_count; c++) if (!strcmp(l->any[a].cond[c].param, name)) uses++;
        for (int s = 0; s < l->state_count; s++) {
            if (!strcmp(l->states[s].speed_param, name)) uses++;
            for (int t = 0; t < l->states[s].trans_count; t++)
                for (int c = 0; c < l->states[s].trans[t].cond_count; c++)
                    if (!strcmp(l->states[s].trans[t].cond[c].param, name)) uses++;
        }
    }
    return uses;
}

void lx_anim_remove_param(LxAnimGraph *g, int index) {
    if (!g || index < 0 || index >= g->param_count) return;
    memmove(&g->params[index], &g->params[index + 1], sizeof(LxAnimParam) * (size_t)(g->param_count - index - 1));
    g->param_count--;
    lx_anim_graph_touch(g);
}

static void rename_in_conds(LxAnimTransition *t, const char *old, const char *name) {
    for (int c = 0; c < t->cond_count; c++)
        if (!strcmp(t->cond[c].param, old)) lx_strcpy(t->cond[c].param, sizeof t->cond[c].param, name);
}

bool lx_anim_rename_param(LxAnimGraph *g, int index, const char *name) {
    if (!g || index < 0 || index >= g->param_count || !name || !name[0]) return false;
    for (int i = 0; i < g->param_count; i++) if (i != index && !strcmp(g->params[i].name, name)) return false;
    char old[LX_ANIM_NAME];
    lx_strcpy(old, sizeof old, g->params[index].name);
    lx_strcpy(g->params[index].name, sizeof g->params[index].name, name);
    for (int i = 0; i < g->tree_count; i++) {
        LxAnimTree *tr = &g->trees[i];
        if (!strcmp(tr->param_x, old)) lx_strcpy(tr->param_x, sizeof tr->param_x, name);
        if (!strcmp(tr->param_y, old)) lx_strcpy(tr->param_y, sizeof tr->param_y, name);
        for (int c = 0; c < tr->child_count; c++)
            if (!strcmp(tr->child[c].weight_param, old)) lx_strcpy(tr->child[c].weight_param, sizeof tr->child[c].weight_param, name);
    }
    for (int i = 0; i < g->layer_count; i++) {
        LxAnimLayer *l = &g->layers[i];
        for (int a = 0; a < l->any_count; a++) rename_in_conds(&l->any[a], old, name);
        for (int s = 0; s < l->state_count; s++) {
            if (!strcmp(l->states[s].speed_param, old)) lx_strcpy(l->states[s].speed_param, sizeof l->states[s].speed_param, name);
            for (int t = 0; t < l->states[s].trans_count; t++) rename_in_conds(&l->states[s].trans[t], old, name);
        }
    }
    lx_anim_graph_touch(g);
    return true;
}

int lx_anim_add_tree(LxAnimGraph *g, LxAnimTreeType type) {
    LxAnimTree *t = PUSH(g->trees, g->tree_count, g->tree_cap, LxAnimTree);
    t->type = type;
    for (int i = 0; i < LX_ANIM_MAX_CHILDREN; i++) { t->child[i].tree = -1; t->child[i].speed = 1; }
    lx_anim_graph_touch(g);
    return g->tree_count - 1;
}

/* ---------------- descriptions and problems ---------------- */
static const char *mode_text(LxAnimCondMode m) {
    switch (m) {
    case LX_AC_GREATER: return ">";
    case LX_AC_LESS: return "<";
    case LX_AC_EQUALS: return "==";
    case LX_AC_NOT_EQUAL: return "!=";
    case LX_AC_IF: return "is set";
    default: return "is not set";
    }
}

void lx_anim_transition_summary(const LxAnimTransition *t, char *out, size_t cap) {
    char buf[320];
    int n = 0;
    if (!t || !cap) { if (cap) out[0] = 0; return; }
    n += snprintf(buf + n, sizeof buf - (size_t)n, "to %s", t->to[0] ? t->to : "(nothing)");
    for (int c = 0; c < t->cond_count && n < (int)sizeof buf - 1; c++) {
        const LxAnimCond *cd = &t->cond[c];
        bool flag = cd->mode == LX_AC_IF || cd->mode == LX_AC_IF_NOT;
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s %s %s", c ? " and" : " when",
                      cd->param[0] ? cd->param : "(nothing)", mode_text(cd->mode));
        if (!flag) n += snprintf(buf + n, sizeof buf - (size_t)n, " %g", (double)cd->value);
    }
    if (t->has_exit_time && n < (int)sizeof buf - 1)
        n += snprintf(buf + n, sizeof buf - (size_t)n, "%s %g%% through", t->cond_count ? "," : " after",
                      (double)(t->exit_time * 100));
    if (!t->cond_count && !t->has_exit_time && n < (int)sizeof buf - 1)
        snprintf(buf + n, sizeof buf - (size_t)n, " straight away");
    lx_strcpy(out, cap, buf);
}

int lx_anim_graph_problems(const LxAnimGraph *g, char (*out)[160], int max) {
    int found = 0;
    if (!g) return 0;
#define PROBLEM(...)                                                            \
    do {                                                                        \
        if (found < max && out) snprintf(out[found], 160, __VA_ARGS__);         \
        found++;                                                                \
    } while (0)
    for (int i = 0; i < g->layer_count; i++) {
        const LxAnimLayer *l = &g->layers[i];
        if (!l->state_count) { PROBLEM("Layer \"%s\" has no states", l->name); continue; }
        if (!lx_anim_find_state(l, l->start)) PROBLEM("Layer \"%s\" has no starting state", l->name);
        for (int a = 0; a < l->any_count; a++) {
            if (!l->any[a].to[0]) PROBLEM("Layer \"%s\": an Any State transition goes nowhere", l->name);
            else if (!lx_anim_find_state(l, l->any[a].to))
                PROBLEM("Layer \"%s\": an Any State transition goes to \"%s\", which is gone", l->name, l->any[a].to);
            for (int c = 0; c < l->any[a].cond_count; c++)
                if (!lx_anim_find_param(g, l->any[a].cond[c].param))
                    PROBLEM("Layer \"%s\": an Any State condition uses \"%s\", which is not a parameter", l->name, l->any[a].cond[c].param);
        }
        for (int s = 0; s < l->state_count; s++) {
            const LxAnimNode *n = &l->states[s];
            if (n->motion == LX_AM_CLIP && !n->clip[0]) PROBLEM("State \"%s\" has no clip", n->name);
            if (n->motion == LX_AM_TREE && (n->tree < 0 || n->tree >= g->tree_count)) PROBLEM("State \"%s\" has no blend tree", n->name);
            if (n->motion == LX_AM_TREE && n->tree >= 0 && n->tree < g->tree_count && !g->trees[n->tree].child_count)
                PROBLEM("State \"%s\": its blend tree is empty", n->name);
            if (n->speed_param[0] && !lx_anim_find_param(g, n->speed_param))
                PROBLEM("State \"%s\": its speed uses \"%s\", which is not a parameter", n->name, n->speed_param);
            for (int t = 0; t < n->trans_count; t++) {
                const LxAnimTransition *tr = &n->trans[t];
                if (!tr->to[0]) PROBLEM("State \"%s\": a transition goes nowhere", n->name);
                else if (!lx_anim_find_state(l, tr->to))
                    PROBLEM("State \"%s\": a transition goes to \"%s\", which is gone", n->name, tr->to);
                if (!tr->cond_count && !tr->has_exit_time)
                    PROBLEM("State \"%s\": a transition has no conditions and no exit time, so it fires at once", n->name);
                for (int c = 0; c < tr->cond_count; c++)
                    if (!lx_anim_find_param(g, tr->cond[c].param))
                        PROBLEM("State \"%s\": a condition uses \"%s\", which is not a parameter", n->name, tr->cond[c].param);
            }
        }
    }
    for (int i = 0; i < g->tree_count; i++) {
        const LxAnimTree *t = &g->trees[i];
        if (t->type != LX_AT_DIRECT && !t->param_x[0]) PROBLEM("A blend tree has no parameter to blend along");
        else if (t->type != LX_AT_DIRECT && !lx_anim_find_param(g, t->param_x))
            PROBLEM("A blend tree blends along \"%s\", which is not a parameter", t->param_x);
        if ((t->type == LX_AT_2D_DIRECTIONAL || t->type == LX_AT_2D_CARTESIAN) && t->param_y[0] && !lx_anim_find_param(g, t->param_y))
            PROBLEM("A blend tree blends along \"%s\", which is not a parameter", t->param_y);
    }
#undef PROBLEM
    return found;
}
