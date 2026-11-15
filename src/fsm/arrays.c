/* Arrays: lists of values for State Graphs and scripts. A handle table (index + generation), so a stale
 * handle reads as empty instead of touching freed memory. Items are LxFsmValues, so any type converts. */
#include "../lx_internal.h"
#include <stdio.h>

typedef struct ArrayBox {
    bool used;
    uint16_t gen;
    LxFsmType type;
    LxFsmValue *items;
    int count, cap;
} ArrayBox;

static ArrayBox *g_boxes;
static int g_box_count, g_box_cap;

static ArrayBox *box(LxArray a) {
    int i = (a & 0xffff) - 1;
    if (a <= 0 || i < 0 || i >= g_box_count) return NULL;
    ArrayBox *b = &g_boxes[i];
    return b->used && b->gen == (uint16_t)((unsigned)a >> 16) ? b : NULL;
}
static bool element_type_ok(LxFsmType t) {
    return t == LX_FSM_FLOAT || t == LX_FSM_INT || t == LX_FSM_BOOL || t == LX_FSM_STRING || t == LX_FSM_VEC3 || t == LX_FSM_COLOR ||
           t == LX_FSM_OBJECT || t == LX_FSM_ASSET || t == LX_FSM_VEC2 || t == LX_FSM_QUAT || t == LX_FSM_RECT || t == LX_FSM_MATERIAL ||
           t == LX_FSM_TEXTURE;
}

LxArray lx_array_create(LxFsmType t) {
    if (!element_type_ok(t)) t = LX_FSM_FLOAT;
    int i = 0;
    while (i < g_box_count && g_boxes[i].used) i++;
    if (i == g_box_count) {
        if (g_box_count >= 0xfffe) return 0;
        ArrayBox z;
        memset(&z, 0, sizeof z);
        LX_VEC_PUSH(g_boxes, g_box_count, g_box_cap, z);
    }
    ArrayBox *b = &g_boxes[i];
    uint16_t gen = (uint16_t)(b->gen + 1);
    if (!gen) gen = 1;
    memset(b, 0, sizeof *b);
    b->used = true;
    b->gen = gen;
    b->type = t;
    return (LxArray)(((unsigned)gen << 16) | (unsigned)(i + 1));
}
void lx_array_destroy(LxArray a) {
    ArrayBox *b = box(a);
    if (!b) return;
    lx_free(b->items);
    uint16_t gen = b->gen;
    memset(b, 0, sizeof *b);
    b->gen = gen;
}
bool lx_array_valid(LxArray a) { return box(a) != NULL; }
LxFsmType lx_array_type(LxArray a) { ArrayBox *b = box(a); return b ? b->type : LX_FSM_FLOAT; }
int lx_array_length(LxArray a) { ArrayBox *b = box(a); return b ? b->count : 0; }
void lx_array_clear(LxArray a) { ArrayBox *b = box(a); if (b) b->count = 0; }

#define LX_ARRAY_MAX 100000
static void empty_item(ArrayBox *b, LxFsmValue *v) { lx_fsm_value_parse(v, b->type, "", NULL); }
void lx_array_resize(LxArray a, int n) {
    ArrayBox *b = box(a);
    if (!b) return;
    if (n < 0) n = 0;
    if (n > LX_ARRAY_MAX) n = LX_ARRAY_MAX;
    if (n > b->cap) {
        int cap = b->cap ? b->cap : 8;
        while (cap < n) cap *= 2;
        b->items = lx_realloc(b->items, sizeof(LxFsmValue) * (size_t)cap);
        b->cap = cap;
    }
    for (int i = b->count; i < n; i++) empty_item(b, &b->items[i]);
    b->count = n;
}
static LxFsmValue *item(LxArray a, int i) {
    ArrayBox *b = box(a);
    return b && i >= 0 && i < b->count ? &b->items[i] : NULL;
}
void lx_array_remove_at(LxArray a, int i) {
    ArrayBox *b = box(a);
    if (!b || i < 0 || i >= b->count) return;
    memmove(&b->items[i], &b->items[i + 1], sizeof(LxFsmValue) * (size_t)(b->count - i - 1));
    b->count--;
}
void lx_array_insert_at(LxArray a, int i) {
    ArrayBox *b = box(a);
    if (!b || b->count >= LX_ARRAY_MAX) return;
    if (i < 0) i = 0;
    if (i > b->count) i = b->count;
    lx_array_resize(a, b->count + 1);
    memmove(&b->items[i + 1], &b->items[i], sizeof(LxFsmValue) * (size_t)(b->count - 1 - i));
    empty_item(b, &b->items[i]);
}
void lx_array_swap(LxArray a, int i, int j) {
    LxFsmValue *x = item(a, i), *y = item(a, j);
    if (!x || !y || x == y) return;
    LxFsmValue t = *x;
    *x = *y;
    *y = t;
}
void lx_array_reverse(LxArray a) {
    int n = lx_array_length(a);
    for (int i = 0; i < n / 2; i++) lx_array_swap(a, i, n - 1 - i);
}
void lx_array_shuffle(LxArray a) {
    int n = lx_array_length(a);
    for (int i = n - 1; i > 0; i--) lx_array_swap(a, i, lx_random_int(0, i));
}

/* ---------------- reading and writing items (converting between types) ---------------- */
static LxScene *scene_now(void) { return lx_active_scene(); }
static LxEntity object_of(LxFsmValue *v) {
    if (v->type != LX_FSM_OBJECT) return 0;
    LxScene *s = scene_now();
    if (!s) return v->e;
    if (v->e && lx_entity_valid(s, v->e)) return v->e;
    const char *t = v->s;
    if (!t[0] || !strcmp(t, "None")) return v->e = 0;
    v->e = !strncmp(t, "tag:", 4) ? lx_entity_find_with_tag(s, t + 4) : lx_entity_find(s, !strncmp(t, "name:", 5) ? t + 5 : t);
    return v->e;
}
float lx_array_get_float(LxArray a, int i) {
    LxFsmValue *v = item(a, i);
    if (!v) return 0;
    switch (v->type) {
    case LX_FSM_INT: return (float)v->i;
    case LX_FSM_BOOL: return v->b ? 1.0f : 0.0f;
    case LX_FSM_STRING: return (float)atof(v->s);
    case LX_FSM_VEC3: case LX_FSM_VEC2: return lx_len3(v->v);
    default: return v->f;
    }
}
int lx_array_get_int(LxArray a, int i) {
    LxFsmValue *v = item(a, i);
    if (!v) return 0;
    if (v->type == LX_FSM_INT) return v->i;
    float f = lx_array_get_float(a, i);
    return f >= 2147483647.0f ? 2147483647 : f <= -2147483647.0f ? -2147483647 : (int)lroundf(f);
}
bool lx_array_get_bool(LxArray a, int i) {
    LxFsmValue *v = item(a, i);
    if (!v) return false;
    if (v->type == LX_FSM_BOOL) return v->b;
    if (v->type == LX_FSM_STRING) return lx_parse_bool(v->s);
    if (v->type == LX_FSM_OBJECT) return object_of(v) != 0;
    return lx_array_get_float(a, i) != 0;
}
const char *lx_array_get_string(LxArray a, int i) {
    static char buf[4][LX_REF_MAX];
    static int slot;
    LxFsmValue *v = item(a, i);
    if (!v) return "";
    if (v->type == LX_FSM_STRING || v->type == LX_FSM_ASSET || v->type == LX_FSM_MATERIAL || v->type == LX_FSM_TEXTURE) return v->s;
    slot = (slot + 1) & 3;
    if (v->type == LX_FSM_OBJECT) {
        LxEntity e = object_of(v);
        LxScene *s = scene_now();
        lx_strcpy(buf[slot], sizeof buf[slot], e && s ? lx_entity_name(s, e) : "None");
        return buf[slot];
    }
    lx_fsm_value_format(v, buf[slot], sizeof buf[slot], NULL);
    return buf[slot];
}
LxVec3 lx_array_get_vec3(LxArray a, int i) {
    LxFsmValue *v = item(a, i);
    if (!v) return lx_v3(0, 0, 0);
    if (v->type == LX_FSM_COLOR || v->type == LX_FSM_RECT || v->type == LX_FSM_QUAT) return lx_v3(v->c.x, v->c.y, v->c.z);
    if (v->type == LX_FSM_OBJECT) { LxEntity e = object_of(v); LxScene *s = scene_now(); return e && s ? lx_entity_world_position(s, e) : lx_v3(0, 0, 0); }
    if (v->type == LX_FSM_FLOAT || v->type == LX_FSM_INT) { float f = lx_array_get_float(a, i); return lx_v3(f, f, f); }
    return v->v;
}
LxVec4 lx_array_get_color(LxArray a, int i) {
    LxFsmValue *v = item(a, i);
    if (!v) return lx_v4(0, 0, 0, 0);
    if (v->type == LX_FSM_VEC3 || v->type == LX_FSM_VEC2) return lx_v4(v->v.x, v->v.y, v->v.z, 1);
    return v->c;
}
LxEntity lx_array_get_object(LxArray a, int i) { LxFsmValue *v = item(a, i); return v ? object_of(v) : 0; }

/* writes convert to the array's type */
static void set_text(LxFsmValue *v, const char *text) { LxFsmType t = v->type; lx_fsm_value_parse(v, t, text, NULL); }
static void set_num(LxFsmValue *v, float f) {
    if (!isfinite(f)) return;
    switch (v->type) {
    case LX_FSM_FLOAT: v->f = f; v->i = (int)f; v->b = f != 0; break;
    case LX_FSM_INT: v->i = (int)lroundf(lx_clampf(f, -2147483520.0f, 2147483520.0f)); v->f = (float)v->i; v->b = v->i != 0; break;
    case LX_FSM_BOOL: v->b = f != 0; v->f = v->b; v->i = v->b; break;
    case LX_FSM_VEC3: case LX_FSM_VEC2: v->v = lx_v3(f, f, v->type == LX_FSM_VEC2 ? 0 : f); break;
    case LX_FSM_STRING: case LX_FSM_ASSET: case LX_FSM_MATERIAL: case LX_FSM_TEXTURE: snprintf(v->s, sizeof v->s, "%g", (double)f); break;
    default: break;
    }
}
void lx_array_set_float(LxArray a, int i, float x) { LxFsmValue *v = item(a, i); if (v) set_num(v, x); }
void lx_array_set_int(LxArray a, int i, int x) {
    LxFsmValue *v = item(a, i);
    if (!v) return;
    if (v->type == LX_FSM_INT) { v->i = x; v->f = (float)x; v->b = x != 0; } else set_num(v, (float)x);
}
void lx_array_set_bool(LxArray a, int i, bool x) {
    LxFsmValue *v = item(a, i);
    if (!v) return;
    if (v->type == LX_FSM_STRING) lx_strcpy(v->s, sizeof v->s, x ? "true" : "false");
    else set_num(v, x ? 1.0f : 0.0f);
}
void lx_array_set_string(LxArray a, int i, const char *x) {
    LxFsmValue *v = item(a, i);
    if (!v) return;
    if (v->type == LX_FSM_OBJECT) { lx_strcpy(v->s, sizeof v->s, x ? x : ""); v->e = 0; return; }
    if (v->type == LX_FSM_STRING || v->type == LX_FSM_ASSET || v->type == LX_FSM_MATERIAL || v->type == LX_FSM_TEXTURE) lx_strcpy(v->s, sizeof v->s, x ? x : "");
    else set_text(v, x ? x : "");
}
void lx_array_set_vec3(LxArray a, int i, LxVec3 x) {
    LxFsmValue *v = item(a, i);
    if (!v || !isfinite(x.x) || !isfinite(x.y) || !isfinite(x.z)) return;
    switch (v->type) {
    case LX_FSM_VEC3: v->v = x; break;
    case LX_FSM_VEC2: v->v = lx_v3(x.x, x.y, 0); break;
    case LX_FSM_COLOR: v->c = lx_v4(x.x, x.y, x.z, v->c.w); break;
    case LX_FSM_QUAT: { LxQuat q = lx_quat_euler_deg(x); v->c = lx_v4(q.x, q.y, q.z, q.w); break; }   /* degrees */
    case LX_FSM_STRING: snprintf(v->s, sizeof v->s, "%g %g %g", (double)x.x, (double)x.y, (double)x.z); break;
    default: set_num(v, lx_len3(x)); break;
    }
}
void lx_array_set_color(LxArray a, int i, LxVec4 x) {
    LxFsmValue *v = item(a, i);
    if (!v) return;
    if (v->type == LX_FSM_VEC3 || v->type == LX_FSM_VEC2) v->v = lx_v3(x.x, x.y, v->type == LX_FSM_VEC2 ? 0 : x.z);
    else if (v->type == LX_FSM_COLOR || v->type == LX_FSM_RECT || v->type == LX_FSM_QUAT) v->c = x;
    else if (v->type == LX_FSM_STRING) snprintf(v->s, sizeof v->s, "%g %g %g %g", (double)x.x, (double)x.y, (double)x.z, (double)x.w);
}
void lx_array_set_object(LxArray a, int i, LxEntity x) {
    LxFsmValue *v = item(a, i);
    if (!v) return;
    LxScene *s = scene_now();
    if (v->type == LX_FSM_OBJECT) { v->e = x; lx_strcpy(v->s, sizeof v->s, x && s ? lx_entity_name(s, x) : "None"); }
    else if (v->type == LX_FSM_STRING) lx_strcpy(v->s, sizeof v->s, x && s ? lx_entity_name(s, x) : "None");
    else if ((v->type == LX_FSM_VEC3) && x && s) v->v = lx_entity_world_position(s, x);
}
static int add_one(LxArray a) {
    int n = lx_array_length(a);
    if (!box(a) || n >= LX_ARRAY_MAX) return -1;
    lx_array_resize(a, n + 1);
    return n;
}
int lx_array_add_float(LxArray a, float v) { int i = add_one(a); lx_array_set_float(a, i, v); return i; }
int lx_array_add_int(LxArray a, int v) { int i = add_one(a); lx_array_set_int(a, i, v); return i; }
int lx_array_add_bool(LxArray a, bool v) { int i = add_one(a); lx_array_set_bool(a, i, v); return i; }
int lx_array_add_string(LxArray a, const char *v) { int i = add_one(a); lx_array_set_string(a, i, v); return i; }
int lx_array_add_vec3(LxArray a, LxVec3 v) { int i = add_one(a); lx_array_set_vec3(a, i, v); return i; }
int lx_array_add_color(LxArray a, LxVec4 v) { int i = add_one(a); lx_array_set_color(a, i, v); return i; }
int lx_array_add_object(LxArray a, LxEntity v) { int i = add_one(a); lx_array_set_object(a, i, v); return i; }

int lx_array_find_float(LxArray a, float x) {
    int n = lx_array_length(a);
    for (int i = 0; i < n; i++) if (fabsf(lx_array_get_float(a, i) - x) <= 1e-6f * fmaxf(1, fabsf(x))) return i;
    return -1;
}
int lx_array_find_string(LxArray a, const char *x) {
    int n = lx_array_length(a);
    for (int i = 0; i < n; i++) if (!strcmp(lx_array_get_string(a, i), x ? x : "")) return i;
    return -1;
}
int lx_array_find_object(LxArray a, LxEntity x) {
    int n = lx_array_length(a);
    for (int i = 0; i < n; i++) if (lx_array_get_object(a, i) == x) return i;
    return -1;
}

static bool g_sort_desc;
static int sort_cmp(const void *pa, const void *pb) {
    const LxFsmValue *x = pa, *y = pb;
    int r;
    switch (x->type) {
    case LX_FSM_FLOAT: r = (x->f > y->f) - (x->f < y->f); break;
    case LX_FSM_INT: r = (x->i > y->i) - (x->i < y->i); break;
    case LX_FSM_BOOL: r = (int)x->b - (int)y->b; break;
    case LX_FSM_VEC3: case LX_FSM_VEC2: { float a = lx_len3sq(x->v), b = lx_len3sq(y->v); r = (a > b) - (a < b); break; }
    default: r = strcmp(x->s, y->s); break;
    }
    return g_sort_desc ? -r : r;
}
void lx_array_sort(LxArray a, bool descending) {
    ArrayBox *b = box(a);
    if (!b || b->count < 2) return;
    if (b->type == LX_FSM_OBJECT)   /* by name: make sure the names are current */
        for (int i = 0; i < b->count; i++) { LxEntity e = object_of(&b->items[i]); LxScene *s = scene_now(); if (e && s) lx_strcpy(b->items[i].s, sizeof b->items[i].s, lx_entity_name(s, e)); }
    g_sort_desc = descending;
    qsort(b->items, (size_t)b->count, sizeof(LxFsmValue), sort_cmp);
}
void lx_array_copy(LxArray to, LxArray from) {
    ArrayBox *t = box(to), *f = box(from);
    if (!t || !f || t == f) return;
    if (t->type == f->type) {
        lx_array_resize(to, f->count);
        t = box(to);
        f = box(from);
        memcpy(t->items, f->items, sizeof(LxFsmValue) * (size_t)f->count);
        return;
    }
    int n = f->count;
    lx_array_resize(to, n);
    for (int i = 0; i < n; i++) lx_array_set_string(to, i, lx_array_get_string(from, i));
}

/* ---------------- text: "float: 1, 2, 3" (the variable's starting value) ---------------- */
LxArray lx_array_from_text(const char *text) {
    char type[32] = "float";
    const char *colon = text ? strchr(text, ':') : NULL;
    const char *items = text ? text : "";
    if (colon && (size_t)(colon - text) < sizeof type) {
        memcpy(type, text, (size_t)(colon - text));
        type[colon - text] = 0;
        items = colon + 1;
    }
    LxArray a = lx_array_create(lx_fsm_type_parse(type));
    ArrayBox *b = box(a);
    if (!b) return 0;
    while (*items) {
        while (*items == ' ') items++;
        if (!*items) break;
        size_t l = strcspn(items, ",");
        char one[LX_REF_MAX];
        size_t k = l < sizeof one - 1 ? l : sizeof one - 1;
        memcpy(one, items, k);
        one[k] = 0;
        while (k && one[k - 1] == ' ') one[--k] = 0;
        int i = add_one(a);
        if (i < 0) break;
        b = box(a);
        lx_fsm_value_parse(&b->items[i], b->type, one, NULL);
        items += l;
        if (*items == ',') items++;
    }
    return a;
}
void lx_array_to_text(LxArray a, char *out, size_t cap) {
    ArrayBox *b = box(a);
    snprintf(out, cap, "%s:", lx_fsm_type_name(b ? b->type : LX_FSM_FLOAT));
    for (int i = 0; b && i < b->count; i++) {
        size_t len = strlen(out);
        if (len + 4 >= cap) break;
        snprintf(out + len, cap - len, "%s %s", i ? "," : "", lx_array_get_string(a, i));
    }
}
LxFsmType lx_array_text_type(const char *text) {
    char type[32] = "float";
    const char *colon = text ? strchr(text, ':') : NULL;
    if (colon && (size_t)(colon - text) < sizeof type) { memcpy(type, text, (size_t)(colon - text)); type[colon - text] = 0; }
    return lx_fsm_type_parse(type);
}
