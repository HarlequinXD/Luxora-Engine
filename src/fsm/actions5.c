/* Built-in State Graph actions, part five: Array actions (PlayMaker's Array category) and Vector2,
 * Quaternion and Rect maths. Arrays are Array variables (lx_array_* in the scripting API). */
#include "../lx_internal.h"
#include <stdio.h>

#define P(n, t, d) {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip) {n, t, d, 0, NULL, tip}
#define POUT(n, t) {n, t, "", LX_FSM_P_OUT, NULL, NULL}
#define POPT(n, t) {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n) {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PENUM(n, opts, d) {n, LX_FSM_ENUM, d, 0, opts, NULL}
#define POBJ(n) {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJN(n) {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PARR(n) {n, LX_FSM_ARRAY, "", 0, NULL, "an Array variable"}
#define PANY(n) {n, LX_FSM_ANY, "", 0, NULL, "a value, or a variable of any type"}
#define PANYOUT(n) {n, LX_FSM_ANY, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, "a variable of the array's type"}

static void ev(LxFsmCtx *c, int p) { lx_fsm_send(c, lx_fsm_event(c, p)); }
static LxEntity obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }

static int find_value(LxFsmCtx *c, LxArray a, int p) {
    LxFsmValue *v = lx_fsm_param_value(c, p);
    if (v->type == LX_FSM_OBJECT && lx_array_type(a) == LX_FSM_OBJECT) return lx_array_find_object(a, obj(c, p));
    LxFsmType t = lx_array_type(a);
    if (t == LX_FSM_FLOAT || t == LX_FSM_INT) return lx_array_find_float(a, lx_fsm_float(c, p));
    /* everything else: the item written the same way as the value */
    LxArray tmp = lx_array_create(t);
    lx_array_resize(tmp, 1);
    lx_fsm_array_set_from(c, p, tmp, 0);
    char want[LX_REF_MAX];
    lx_strcpy(want, sizeof want, lx_array_get_string(tmp, 0));
    lx_array_destroy(tmp);
    return lx_array_find_string(a, want);
}

/* ======================================================================
 *  Array
 * ====================================================================== */
static void arr_add(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    if (!a) return;
    int n = lx_array_length(a);
    lx_array_resize(a, n + 1);
    if (lx_array_length(a) != n + 1) return;
    lx_fsm_array_set_from(c, 1, a, n);
    lx_fsm_set_int(c, 2, n);
}
static const LxFsmActionDesc A_ARR_ADD = {"Array Add", "Array", "Adds a value to the end of an array.", "{0} += {1}",
    {PARR("Array"), PANY("Value"), POPT("Store Index", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, arr_add, arr_add, NULL};
static void arr_insert(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    if (!a) return;
    int i = lx_fsm_int(c, 1);
    if (i < 0) i = 0;
    if (i > lx_array_length(a)) i = lx_array_length(a);
    lx_array_insert_at(a, i);
    lx_fsm_array_set_from(c, 2, a, i);
}
static const LxFsmActionDesc A_ARR_INSERT = {"Array Insert", "Array", "Puts a value into an array at an index; the rest move up.", "{0}[{1}] insert {2}",
    {PARR("Array"), P("Index", LX_FSM_INT, "0"), PANY("Value")}, 0, 0, LX_FSM_ONCE, arr_insert, arr_insert, NULL};
static void arr_remove_at(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int i = lx_fsm_int(c, 1);
    if (i < 0 || i >= lx_array_length(a)) { ev(c, 2); return; }
    lx_array_remove_at(a, i);
}
static const LxFsmActionDesc A_ARR_REMOVE_AT = {"Array Remove At", "Array", "Removes the item at an index; the rest move down.", "remove {0}[{1}]",
    {PARR("Array"), P("Index", LX_FSM_INT, "0"), PEV("Out Of Range Event")}, 0, 0, LX_FSM_ONCE, arr_remove_at, arr_remove_at, NULL};
static void arr_remove(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    if (!a) return;
    int removed = 0, i;
    while ((i = find_value(c, a, 1)) >= 0 && removed < 100000) {
        lx_array_remove_at(a, i);
        removed++;
        if (!lx_fsm_bool(c, 2)) break;
    }
    lx_fsm_set_int(c, 3, removed);
    ev(c, removed ? 4 : 5);
}
static const LxFsmActionDesc A_ARR_REMOVE = {"Array Remove", "Array", "Removes a value from an array (the first one, or every one).", "remove {1} from {0}",
    {PARR("Array"), PANY("Value"), P("All Of Them", LX_FSM_BOOL, "false"), POPT("Store Removed Count", LX_FSM_INT), PEV("Removed Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, arr_remove, arr_remove, NULL};
static void arr_get(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int i = lx_fsm_int(c, 1);
    if (!a || i < 0 || i >= lx_array_length(a)) { ev(c, 3); return; }
    lx_fsm_set_from_array(c, 2, a, i);
}
static const LxFsmActionDesc A_ARR_GET = {"Array Get", "Array", "Reads the item at an index (0 = the first).", "{2} = {0}[{1}]",
    {PARR("Array"), P("Index", LX_FSM_INT, "0"), PANYOUT("Store Value"), PEV("Out Of Range Event")}, 0, 0, LX_FSM_ONCE, arr_get, arr_get, NULL};
static void arr_set(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int i = lx_fsm_int(c, 1);
    if (!a || i < 0 || i >= lx_array_length(a)) { ev(c, 3); return; }
    lx_fsm_array_set_from(c, 2, a, i);
}
static const LxFsmActionDesc A_ARR_SET = {"Array Set", "Array", "Changes the item at an index.", "{0}[{1}] = {2}",
    {PARR("Array"), P("Index", LX_FSM_INT, "0"), PANY("Value"), PEV("Out Of Range Event")}, 0, 0, LX_FSM_ONCE, arr_set, arr_set, NULL};
static void arr_length(LxFsmCtx *c) {
    int n = lx_array_length(lx_fsm_array(c, 0));
    lx_fsm_set_int(c, 1, n);
    if (n == 0) ev(c, 2);
}
static const LxFsmActionDesc A_ARR_LENGTH = {"Array Length", "Array", "How many items an array has.", "{1} = length of {0}",
    {PARR("Array"), POUT("Store Length", LX_FSM_INT), PEV("Empty Event")}, 0, 0, LX_FSM_ONCE, arr_length, arr_length, NULL};
static void arr_resize(LxFsmCtx *c) { lx_array_resize(lx_fsm_array(c, 0), lx_fsm_int(c, 1)); }
static const LxFsmActionDesc A_ARR_RESIZE = {"Array Resize", "Array", "Makes an array a given length (new items are empty).", "{0} length = {1}",
    {PARR("Array"), P("Length", LX_FSM_INT, "0")}, 0, 0, LX_FSM_ONCE, arr_resize, arr_resize, NULL};
static void arr_clear(LxFsmCtx *c) { lx_array_clear(lx_fsm_array(c, 0)); }
static const LxFsmActionDesc A_ARR_CLEAR = {"Array Clear", "Array", "Removes every item.", "clear {0}", {PARR("Array")}, 0, 0, LX_FSM_ONCE, arr_clear, arr_clear, NULL};
static void arr_contains(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int i = a ? find_value(c, a, 1) : -1;
    lx_fsm_set_int(c, 2, i);
    lx_fsm_set_bool(c, 3, i >= 0);
    ev(c, i >= 0 ? 4 : 5);
}
static const LxFsmActionDesc A_ARR_CONTAINS = {"Array Contains", "Array", "Whether an array has a value, and where.", "{1} in {0}?",
    {PARR("Array"), PANY("Value"), POPT("Store Index", LX_FSM_INT), POPT("Store Contains", LX_FSM_BOOL), PEV("Yes Event"), PEV("No Event")},
    0, 0, LX_FSM_ONCE, arr_contains, arr_contains, NULL};
static void arr_random(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_length(a);
    if (!n) { ev(c, 3); return; }
    int i = lx_random_int(0, n - 1);
    lx_fsm_set_from_array(c, 1, a, i);
    lx_fsm_set_int(c, 2, i);
}
static const LxFsmActionDesc A_ARR_RANDOM = {"Array Get Random", "Array", "Picks a random item.", "{1} = random from {0}",
    {PARR("Array"), PANYOUT("Store Value"), POPT("Store Index", LX_FSM_INT), PEV("Empty Event")}, 0, 0, LX_FSM_ONCE, arr_random, arr_random, NULL};
static void arr_shuffle(LxFsmCtx *c) { lx_array_shuffle(lx_fsm_array(c, 0)); }
static const LxFsmActionDesc A_ARR_SHUFFLE = {"Array Shuffle", "Array", "Puts the items in a random order (card decks, spawn orders).", "shuffle {0}",
    {PARR("Array")}, 0, 0, LX_FSM_ONCE, arr_shuffle, arr_shuffle, NULL};
static void arr_reverse(LxFsmCtx *c) { lx_array_reverse(lx_fsm_array(c, 0)); }
static const LxFsmActionDesc A_ARR_REVERSE = {"Array Reverse", "Array", "Flips the order of the items.", "reverse {0}",
    {PARR("Array")}, 0, 0, LX_FSM_ONCE, arr_reverse, arr_reverse, NULL};
static void arr_sort(LxFsmCtx *c) { lx_array_sort(lx_fsm_array(c, 0), lx_fsm_int(c, 1) == 1); }
static const LxFsmActionDesc A_ARR_SORT = {"Array Sort", "Array", "Sorts numbers by size, text alphabetically, objects by name (high scores).", "sort {0}",
    {PARR("Array"), PENUM("Order", "Ascending|Descending", "Ascending")}, 0, 0, LX_FSM_ONCE, arr_sort, arr_sort, NULL};
static void arr_swap(LxFsmCtx *c) { lx_array_swap(lx_fsm_array(c, 0), lx_fsm_int(c, 1), lx_fsm_int(c, 2)); }
static const LxFsmActionDesc A_ARR_SWAP = {"Array Swap", "Array", "Swaps two items.", "swap {0}[{1}] and [{2}]",
    {PARR("Array"), P("Index 1", LX_FSM_INT, "0"), P("Index 2", LX_FSM_INT, "1")}, 0, 0, LX_FSM_ONCE, arr_swap, arr_swap, NULL};
static void arr_copy(LxFsmCtx *c) { lx_array_copy(lx_fsm_array(c, 1), lx_fsm_array(c, 0)); }
static const LxFsmActionDesc A_ARR_COPY = {"Array Copy", "Array", "Makes one array a copy of another.", "{1} = copy of {0}",
    {PARR("From"), PARR("To")}, 0, 0, LX_FSM_ONCE, arr_copy, arr_copy, NULL};

/* For Each: one item per visit; the Index variable remembers where it got to */
static void arr_next(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int i = lx_fsm_int(c, 1), n = lx_array_length(a);
    if (i < 0) i = 0;
    if (i >= n) { lx_fsm_set_int(c, 1, 0); ev(c, 5); return; }
    lx_fsm_set_from_array(c, 2, a, i);
    lx_fsm_set_int(c, 3, i);
    lx_fsm_set_int(c, 1, i + 1);
    ev(c, 4);
}
static const LxFsmActionDesc A_ARR_NEXT = {"Array For Each", "Array",
    "Goes through an array one item per visit: stores the next item and sends Loop, then Finished at the end (and starts over). "
    "Send the Loop event back to this state to visit every item.",
    "{2} = next of {0}",
    {PARR("Array"), {"Counter", LX_FSM_INT, "", LX_FSM_P_OUT, NULL, "an Int variable that keeps the place between visits: set it to 0 to start again"},
     PANYOUT("Store Value"), POPT("Store Index", LX_FSM_INT), PEV("Loop Event"), PEV("Finished Event")}, 0, 0, LX_FSM_ONCE, arr_next, NULL, NULL};

static void arr_fill_tag(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    if (!a) return;
    if (lx_fsm_bool(c, 2)) lx_array_clear(a);
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 1));
    int n = lx_scene_entity_count(c->scene);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(c->scene, i);
        if (e && lx_entity_has_tag(c->scene, e, tag)) lx_array_add_object(a, e);
    }
    lx_fsm_set_int(c, 3, lx_array_length(a));
}
static const LxFsmActionDesc A_ARR_FILL_TAG = {"Array Fill With Tag", "Array", "Fills an Object array with every object that has a tag.", "{0} = objects tagged {1}",
    {PARR("Array"), P("Tag", LX_FSM_STRING, "Enemy"), P("Clear First", LX_FSM_BOOL, "true"), POPT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, arr_fill_tag, arr_fill_tag, NULL};
static void arr_fill_children(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    LxEntity p = obj(c, 1);
    if (!a) return;
    if (lx_fsm_bool(c, 2)) lx_array_clear(a);
    for (LxEntity k = p ? lx_entity_first_child(c->scene, p) : 0; k; k = lx_entity_next_sibling(c->scene, k)) lx_array_add_object(a, k);
    lx_fsm_set_int(c, 3, lx_array_length(a));
}
static const LxFsmActionDesc A_ARR_FILL_CHILDREN = {"Array Fill With Children", "Array", "Fills an Object array with an object's children (waypoints, spawn points).",
    "{0} = children of {1}",
    {PARR("Array"), POBJ("Object"), P("Clear First", LX_FSM_BOOL, "true"), POPT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, arr_fill_children, arr_fill_children, NULL};
static void arr_remove_gone(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    if (lx_array_type(a) != LX_FSM_OBJECT) return;
    for (int i = lx_array_length(a) - 1; i >= 0; i--) {
        LxEntity e = lx_array_get_object(a, i);
        if (!e || !lx_entity_valid(c->scene, e)) lx_array_remove_at(a, i);
    }
}
static const LxFsmActionDesc A_ARR_REMOVE_GONE = {"Array Remove Destroyed", "Array", "Takes objects that no longer exist out of an Object array.", "tidy {0}",
    {PARR("Array")}, 0, 0, LX_FSM_ONCE, arr_remove_gone, arr_remove_gone, NULL};
static void arr_closest(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    LxEntity from = obj(c, 1);
    LxVec3 at = from ? lx_entity_world_position(c->scene, from) : lx_fsm_vec3(c, 2);
    int best = -1, n = lx_array_length(a);
    float bd = 1e30f;
    bool far = lx_fsm_int(c, 3) == 1;
    for (int i = 0; i < n; i++) {
        LxVec3 p;
        if (lx_array_type(a) == LX_FSM_OBJECT) {
            LxEntity e = lx_array_get_object(a, i);
            if (!e || e == from) continue;
            p = lx_entity_world_position(c->scene, e);
        } else p = lx_array_get_vec3(a, i);
        float d = lx_len3(lx_sub3(p, at));
        if (best < 0 || (far ? d > bd : d < bd)) { bd = d; best = i; }
    }
    if (best < 0) { ev(c, 7); return; }
    lx_fsm_set_from_array(c, 4, a, best);
    lx_fsm_set_int(c, 5, best);
    lx_fsm_set_float(c, 6, bd);
}
static const LxFsmActionDesc A_ARR_CLOSEST = {"Array Get Closest", "Array", "The nearest (or farthest) object or point in an array.", "{4} = closest in {0} to {1}",
    {PARR("Array"), POBJN("From Object"), PT("From Point", LX_FSM_VEC3, "0 0 0", "when From Object is None"), PENUM("Find", "Closest|Farthest", "Closest"),
     PANYOUT("Store Item"), POPT("Store Index", LX_FSM_INT), POPT("Store Distance", LX_FSM_FLOAT), PEV("Empty Event")}, 0, 0, LX_FSM_ONCE, arr_closest, arr_closest, NULL};
static void arr_stats(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_length(a), op = lx_fsm_int(c, 1), at = -1;
    float r = 0;
    for (int i = 0; i < n; i++) {
        float x = lx_array_get_float(a, i);
        if (op == 0 || op == 3) r += x;
        else if (at < 0 || (op == 1 ? x < r : x > r)) { r = x; at = i; }
    }
    if (op == 3 && n) r /= (float)n;
    lx_fsm_set_float(c, 2, r);
    lx_fsm_set_int(c, 3, at);
}
static const LxFsmActionDesc A_ARR_STATS = {"Array Sum Min Max", "Array", "The total, smallest, biggest or average of a number array.", "{2} = {1} of {0}",
    {PARR("Array"), PENUM("Operation", "Sum|Min|Max|Average", "Sum"), POUT("Store Result", LX_FSM_FLOAT), POPT("Store Index", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, arr_stats, arr_stats, NULL};
static void arr_join(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    char sep[32], out[LX_REF_MAX];
    lx_strcpy(sep, sizeof sep, lx_fsm_string(c, 1));
    out[0] = 0;
    int n = lx_array_length(a);
    for (int i = 0; i < n; i++) {
        size_t l = strlen(out);
        snprintf(out + l, sizeof out - l, "%s%s", i ? sep : "", lx_array_get_string(a, i));
    }
    lx_fsm_set_string(c, 2, out);
}
static const LxFsmActionDesc A_ARR_JOIN = {"Array Join", "Array", "Puts every item into one text with a separator between.", "{2} = {0} joined",
    {PARR("Array"), P("Separator", LX_FSM_STRING, ", "), POUT("Store String", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, arr_join, arr_join, NULL};
static void str_split_arr(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 2);
    if (!a) return;
    char text[LX_REF_MAX], sep[32];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 0));
    lx_strcpy(sep, sizeof sep, lx_fsm_string(c, 1));
    if (lx_fsm_bool(c, 3)) lx_array_clear(a);
    size_t sl = strlen(sep);
    char *p = text;
    for (;;) {
        char *e = sl ? strstr(p, sep) : NULL;
        if (e) *e = 0;
        lx_array_add_string(a, p);
        if (!e) break;
        p = e + sl;
    }
}
static const LxFsmActionDesc A_STR_SPLIT_ARR = {"String Split To Array", "Array", "Splits a text at a separator into an array (\"a,b,c\" -> a, b, c).", "{2} = {0} split",
    {P("String", LX_FSM_STRING, ""), P("Separator", LX_FSM_STRING, ","), PARR("Array"), P("Clear First", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_ONCE, str_split_arr, str_split_arr, NULL};

/* ======================================================================
 *  Vector2
 * ====================================================================== */
static void v2_set(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    if (!lx_fsm_is_none(c, 1)) v.x = lx_fsm_float(c, 1);
    if (!lx_fsm_is_none(c, 2)) v.y = lx_fsm_float(c, 2);
    lx_fsm_set_vec3(c, 0, lx_v3(v.x, v.y, 0));
}
#define OPTF(n) {n, LX_FSM_FLOAT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
static const LxFsmActionDesc A_V2_SET = {"Set Vector2 XY", "Math/Vector2", "Changes X and/or Y of a Vector2 variable (None = keep).", "{0} = ({1}, {2})",
    {POUT("Vector2 Variable", LX_FSM_VEC2), OPTF("X"), OPTF("Y")}, 0, 0, LX_FSM_ONCE, v2_set, v2_set, NULL};
static void v2_get(LxFsmCtx *c) { LxVec3 v = lx_fsm_vec3(c, 0); lx_fsm_set_float(c, 1, v.x); lx_fsm_set_float(c, 2, v.y); lx_fsm_set_float(c, 3, sqrtf(v.x * v.x + v.y * v.y)); }
static const LxFsmActionDesc A_V2_GET = {"Get Vector2 XY", "Math/Vector2", "Splits a Vector2 into X and Y, and its length.", "split {0}",
    {P("Vector2", LX_FSM_VEC2, "0 0"), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT), POPT("Store Length", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, v2_get, v2_get, NULL};
static void v2_op(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1), r = a;
    float f = lx_fsm_float(c, 3);
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_v3(a.x + b.x, a.y + b.y, 0); break;
    case 1: r = lx_v3(a.x - b.x, a.y - b.y, 0); break;
    case 2: r = lx_v3(a.x * f, a.y * f, 0); break;
    case 3: { float l = sqrtf(a.x * a.x + a.y * a.y); r = l > 1e-8f ? lx_v3(a.x / l, a.y / l, 0) : lx_v3(0, 0, 0); break; }
    case 4: r = lx_v3(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, 0); break;
    case 5: { float s = sinf(f * LX_DEG2RAD), co = cosf(f * LX_DEG2RAD); r = lx_v3(a.x * co - a.y * s, a.x * s + a.y * co, 0); break; }
    case 6: r = lx_v3(-a.y, a.x, 0); break;
    default: r = lx_v3(a.x * b.x, a.y * b.y, 0); break;
    }
    lx_fsm_set_vec3(c, 4, r);
}
static const LxFsmActionDesc A_V2_OP = {"Vector2 Operator", "Math/Vector2", "Add, subtract, scale, normalize, lerp, rotate (degrees), perpendicular or multiply 2D vectors.",
    "{4} = {0} {2} {1}",
    {P("Vector 1", LX_FSM_VEC2, "0 0"), P("Vector 2", LX_FSM_VEC2, "0 0"), PENUM("Operation", "Add|Subtract|Scale by Float|Normalize|Lerp by Float|Rotate by Float|Perpendicular|Multiply", "Add"),
     P("Float", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_VEC2)}, 0, 0, LX_FSM_ONCE, v2_op, v2_op, NULL};

/* ======================================================================
 *  Quaternion
 * ====================================================================== */
static LxQuat q_neg_fix(LxQuat a, LxQuat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0 ? (LxQuat){-b.x, -b.y, -b.z, -b.w} : b; }
static LxQuat q_slerp(LxQuat a, LxQuat b, float t) {
    b = q_neg_fix(a, b);
    float d = lx_clampf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, -1, 1);
    if (d > 0.9995f) return lx_quat_norm((LxQuat){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    float th = acosf(d), s = sinf(th), wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
    return lx_quat_norm((LxQuat){a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}
static void quat_op(LxFsmCtx *c) {
    LxQuat a = lx_fsm_quat(c, 0), b = lx_fsm_quat(c, 1), r = a;
    float f = lx_fsm_float(c, 3);
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_quat_mul(a, b); break;
    case 1: r = q_slerp(a, b, lx_clampf(f, 0, 1)); break;
    case 2: r = lx_quat_conj(a); break;
    default: {   /* rotate a towards b by at most f degrees */
        LxQuat bb = q_neg_fix(a, b);
        float d = lx_clampf(fabsf(a.x * bb.x + a.y * bb.y + a.z * bb.z + a.w * bb.w), 0, 1), ang = 2 * acosf(d) * LX_RAD2DEG;
        r = ang <= f || ang < 1e-4f ? b : q_slerp(a, b, f / ang);
        break;
    }
    }
    lx_fsm_set_quat(c, 4, r);
}
static const LxFsmActionDesc A_QUAT_OP = {"Quaternion Operator", "Math/Quaternion", "Combine rotations, blend them (slerp), invert, or turn one towards another by some degrees.",
    "{4} = {0} {2} {1}",
    {P("Rotation 1", LX_FSM_QUAT, "0 0 0"), P("Rotation 2", LX_FSM_QUAT, "0 0 0"), PENUM("Operation", "Multiply|Slerp by Float|Inverse|Rotate Towards by Degrees", "Multiply"),
     P("Float", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_QUAT)}, 0, 0, LX_FSM_ONCE, quat_op, quat_op, NULL};
static void quat_make(LxFsmCtx *c) {
    LxQuat q;
    switch (lx_fsm_int(c, 0)) {
    case 0: q = lx_quat_euler_deg(lx_fsm_vec3(c, 1)); break;
    case 1: {
        LxVec3 ax = lx_fsm_vec3(c, 1);
        q = lx_len3sq(ax) > 1e-10f ? lx_quat_axis_angle(lx_norm3(ax), lx_fsm_float(c, 2) * LX_DEG2RAD) : lx_quat_identity();
        break;
    }
    default: {
        LxVec3 f = lx_fsm_vec3(c, 1), up = lx_fsm_vec3(c, 3);
        if (lx_len3sq(f) < 1e-10f) return;
        if (lx_len3sq(up) < 1e-10f) up = lx_v3(0, 1, 0);
        q = lx_quat_look(lx_norm3(f), lx_norm3(up));
        break;
    }
    }
    lx_fsm_set_quat(c, 4, q);
}
static const LxFsmActionDesc A_QUAT_MAKE = {"Make Quaternion", "Math/Quaternion", "A rotation from angles (degrees), an axis and angle, or a direction to look along.",
    "{4} = rotation from {1}",
    {PENUM("From", "Euler Angles|Axis And Angle|Look Direction", "Euler Angles"), P("Vector", LX_FSM_VEC3, "0 0 0"), PT("Angle", LX_FSM_FLOAT, "0", "degrees (Axis And Angle)"),
     PT("Up", LX_FSM_VEC3, "0 1 0", "Look Direction"), POUT("Store Rotation", LX_FSM_QUAT)}, 0, 0, LX_FSM_ONCE, quat_make, quat_make, NULL};
static void quat_rotate_vec(LxFsmCtx *c) {
    LxQuat q = lx_fsm_quat(c, 0);
    lx_fsm_set_vec3(c, 2, lx_quat_rotate(q, lx_fsm_vec3(c, 1)));
    lx_fsm_set_vec3(c, 3, lx_quat_to_euler_deg(q));
}
static const LxFsmActionDesc A_QUAT_ROTATE = {"Quaternion Rotate Vector", "Math/Quaternion", "Turns a vector by a rotation; also gives the rotation's angles in degrees.",
    "{2} = {1} rotated by {0}",
    {P("Rotation", LX_FSM_QUAT, "0 0 0"), P("Vector", LX_FSM_VEC3, "0 0 -1"), POPT("Store Vector", LX_FSM_VEC3), POPT("Store Euler Angles", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, quat_rotate_vec, quat_rotate_vec, NULL};
static void obj_quat(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    bool world = lx_fsm_int(c, 2) == 1;
    if (lx_fsm_int(c, 1) == 0) lx_fsm_set_quat(c, 3, world ? lx_entity_world_rotation(c->scene, e) : lx_entity_rotation(c->scene, e));
    else {
        LxQuat q = lx_fsm_quat(c, 3);
        if (world) lx_entity_set_world_pose(c->scene, e, lx_entity_world_position(c->scene, e), q);
        else lx_entity_set_rotation(c->scene, e, q);
    }
}
static const LxFsmActionDesc A_OBJ_QUAT = {"Object Rotation Quaternion", "Math/Quaternion", "Reads or sets an object's rotation as a Quaternion.", "{1} {0} rotation {3}",
    {POBJ("Object"), PENUM("Mode", "Get|Set", "Get"), PENUM("Space", "Self|World", "World"), {"Rotation", LX_FSM_QUAT, "", 0, NULL, "Get: a Quaternion variable to store it in"}},
    0, 0, LX_FSM_ONCE, obj_quat, obj_quat, NULL};

/* ======================================================================
 *  Rect
 * ====================================================================== */
static void rect_contains(LxFsmCtx *c) {
    LxVec4 r = lx_fsm_color(c, 0);
    LxVec3 p = lx_fsm_vec3(c, 1);
    float x0 = fminf(r.x, r.x + r.z), x1 = fmaxf(r.x, r.x + r.z), y0 = fminf(r.y, r.y + r.w), y1 = fmaxf(r.y, r.y + r.w);
    bool in = p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
    lx_fsm_set_bool(c, 2, in);
    ev(c, in ? 3 : 4);
}
static const LxFsmActionDesc A_RECT_CONTAINS = {"Rect Contains", "Math/Rect", "Whether a point is inside a rectangle (a pointer over a screen area).", "{1} in {0}?",
    {P("Rect", LX_FSM_RECT, "0 0 100 100"), PT("Point", LX_FSM_VEC2, "0 0", "x y (a Vector3's x y works too)"), POPT("Store Inside", LX_FSM_BOOL), PEV("Inside Event"), PEV("Outside Event")},
    0, 0, LX_FSM_EVERY_FRAME, rect_contains, rect_contains, NULL};
static void rect_set(LxFsmCtx *c) {
    LxVec4 r = lx_fsm_color(c, 0);
    float *f[4] = {&r.x, &r.y, &r.z, &r.w};
    for (int i = 0; i < 4; i++) if (!lx_fsm_is_none(c, 1 + i)) *f[i] = lx_fsm_float(c, 1 + i);
    lx_fsm_set_color(c, 0, r);
    lx_fsm_set_float(c, 5, r.x + r.z * 0.5f);
    lx_fsm_set_float(c, 6, r.y + r.w * 0.5f);
}
static const LxFsmActionDesc A_RECT_SET = {"Set Rect", "Math/Rect", "Changes parts of a Rect variable (None = keep) and gives its centre.", "{0} = ({1}, {2}, {3}, {4})",
    {POUT("Rect Variable", LX_FSM_RECT), OPTF("X"), OPTF("Y"), OPTF("Width"), OPTF("Height"), POPT("Store Center X", LX_FSM_FLOAT), POPT("Store Center Y", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rect_set, rect_set, NULL};

void lx_fsm_register_array_actions(void) {
    static const LxFsmActionDesc *all[] = {
        &A_ARR_ADD, &A_ARR_INSERT, &A_ARR_REMOVE_AT, &A_ARR_REMOVE, &A_ARR_GET, &A_ARR_SET, &A_ARR_LENGTH, &A_ARR_RESIZE, &A_ARR_CLEAR,
        &A_ARR_CONTAINS, &A_ARR_RANDOM, &A_ARR_SHUFFLE, &A_ARR_REVERSE, &A_ARR_SORT, &A_ARR_SWAP, &A_ARR_COPY, &A_ARR_NEXT, &A_ARR_FILL_TAG,
        &A_ARR_FILL_CHILDREN, &A_ARR_REMOVE_GONE, &A_ARR_CLOSEST, &A_ARR_STATS, &A_ARR_JOIN, &A_STR_SPLIT_ARR,
        &A_V2_SET, &A_V2_GET, &A_V2_OP, &A_QUAT_OP, &A_QUAT_MAKE, &A_QUAT_ROTATE, &A_OBJ_QUAT, &A_RECT_CONTAINS, &A_RECT_SET,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
