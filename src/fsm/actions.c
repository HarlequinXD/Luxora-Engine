/* Built-in State Graph actions. Each one is a descriptor (parameters, summary line) plus a few small
 * C functions; parameters are read with lx_fsm_float/... and results stored with lx_fsm_set_... */
#include "../lx_internal.h"
#include <stdio.h>
#include <ctype.h>

static int lx_strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}

#define P(n, t, d) {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip) {n, t, d, 0, NULL, tip}
#define POUT(n, t) {n, t, "", LX_FSM_P_OUT, NULL, NULL}
#define POPT(n, t) {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n) {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PENUM(n, opts, d) {n, LX_FSM_ENUM, d, 0, opts, NULL}
#define PASSET(n, kind) {n, LX_FSM_ASSET, "", 0, kind, NULL}
#define POBJ(n) {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJN(n) {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}
#define SPACE "Self|World"
#define EASES "Linear|Ease In|Ease Out|Ease In Out|Back Out|Elastic Out|Bounce Out"

static void ev(LxFsmCtx *c, int p) { lx_fsm_send(c, lx_fsm_event(c, p)); }
static LxEntity obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }

/* ======================================================================
 *  Events & flow
 * ====================================================================== */
typedef struct { float t; } TimerState;

static void send_event_enter(LxFsmCtx *c) {
    const char *e = lx_fsm_event(c, 0);
    int to = lx_fsm_int(c, 1);
    float delay = lx_fsm_float(c, 4), value = lx_fsm_float(c, 5);
    if (!e[0]) return;
    if (delay > 0) {   /* delayed: the gameplay timer delivers it as a message (graphs receive those too) */
        LxEntity t = to == 1 ? obj(c, 2) : c->self;
        if (to <= 1 && t) lx_send_message_after(c->scene, t, e, value, delay);
        return;
    }
    switch (to) {
    case 0: lx_fsm_send(c, e); break;
    case 1: { LxEntity t = obj(c, 2); if (t) lx_send_message(c->scene, t, e, value, c->self); break; }
    case 2: { LxEntity t = obj(c, 2); for (LxEntity k = t ? lx_entity_first_child(c->scene, t) : 0; k; k = lx_entity_next_sibling(c->scene, k)) lx_send_message(c->scene, k, e, value, c->self); break; }
    case 3: lx_send_message_to_tag(c->scene, lx_fsm_string(c, 3), e, value, c->self); break;
    default: lx_broadcast(c->scene, e, value, c->self); break;
    }
}
static const LxFsmActionDesc A_SEND_EVENT = {
    "Send Event", "Events", "Sends an event to this graph, another object, its children, everything with a tag, or everyone.",
    "Send {0} to {1}",
    {PEV("Event"), PENUM("Send To", "This Graph|Object|Children|Tag|Everyone", "0"), POBJ("Object"), P("Tag", LX_FSM_STRING, "Player"),
     PT("Delay", LX_FSM_FLOAT, "0", "seconds; 0 = now"), PT("Value", LX_FSM_FLOAT, "0", "a number sent with the event (Get Event Info)")},
    0, 0, LX_FSM_ONCE, send_event_enter, NULL, NULL};

static void next_frame_update(LxFsmCtx *c) { ev(c, 0); lx_fsm_finish(c); }
static const LxFsmActionDesc A_NEXT_FRAME = {"Next Frame Event", "Events", "Sends an event on the next frame.", "Next frame: {0}",
    {PEV("Event")}, 0, LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, next_frame_update, NULL};

static void wait_update(LxFsmCtx *c) {
    TimerState *t = c->state;
    t->t += lx_fsm_bool(c, 2) ? lx_delta_unscaled(c->scene) : c->dt;
    if (t->t >= lx_fsm_float(c, 0)) { lx_fsm_finish(c); ev(c, 1); }
}
static void wait_enter(LxFsmCtx *c) { if (lx_fsm_float(c, 0) <= 0) { lx_fsm_finish(c); ev(c, 1); } }
static const LxFsmActionDesc A_WAIT = {"Wait", "Time", "Waits, then finishes (and sends an event if set).", "Wait {0} s, then {1}",
    {P("Time", LX_FSM_FLOAT, "1"), PEV("Finish Event"), PT("Real Time", LX_FSM_BOOL, "false", "ignore the time scale (pause menus)")},
    sizeof(TimerState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, wait_enter, wait_update, NULL};

typedef struct { float t, len; } RandWait;
static void rwait_enter(LxFsmCtx *c) { RandWait *w = c->state; w->len = lx_random_range(lx_fsm_float(c, 0), lx_fsm_float(c, 1)); }
static void rwait_update(LxFsmCtx *c) { RandWait *w = c->state; w->t += c->dt; if (w->t >= w->len) { lx_fsm_finish(c); ev(c, 2); } }
static const LxFsmActionDesc A_RANDOM_WAIT = {"Random Wait", "Time", "Waits a random time between Min and Max.", "Wait {0}-{1} s, then {2}",
    {P("Min", LX_FSM_FLOAT, "0.5"), P("Max", LX_FSM_FLOAT, "2"), PEV("Finish Event")}, sizeof(RandWait), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, rwait_enter, rwait_update, NULL};

static void random_event_enter(LxFsmCtx *c) {
    int idx[4], n = 0;
    for (int i = 0; i < 4; i++) if (lx_fsm_event(c, i)[0]) idx[n++] = i;
    if (n) ev(c, idx[lx_random_int(0, n - 1)]);
}
static const LxFsmActionDesc A_RANDOM_EVENT = {"Random Event", "Events", "Sends one of up to four events, picked at random.", "Randomly: {0}, {1}, {2} or {3}",
    {PEV("Event 1"), PEV("Event 2"), PEV("Event 3"), PEV("Event 4")}, 0, 0, LX_FSM_ONCE, random_event_enter, NULL, NULL};

static void event_info_enter(LxFsmCtx *c) {
    lx_fsm_set_float(c, 0, lx_fsm_event_value(c));
    lx_fsm_set_object(c, 1, lx_fsm_event_sender(c));
    lx_fsm_set_object(c, 2, lx_fsm_event_other(c));
    lx_fsm_set_string(c, 3, c->run ? c->run->last_event : "");
}
static const LxFsmActionDesc A_EVENT_INFO = {"Get Event Info", "Events", "Reads what came with the event that entered this state.", "Event value → {0}, sender → {1}",
    {POPT("Value", LX_FSM_FLOAT), POPT("Sender", LX_FSM_OBJECT), POPT("Other Object", LX_FSM_OBJECT), POPT("Event Name", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, event_info_enter, NULL, NULL};

static void finish_enter(LxFsmCtx *c) { ev(c, 0); }
static const LxFsmActionDesc A_GOTO = {"Go To State (Event)", "Events", "Sends an event straight away: useful as the last action of a state.", "Then {0}",
    {PEV("Event")}, 0, 0, LX_FSM_ONCE, finish_enter, NULL, NULL};

/* ======================================================================
 *  Logic
 * ====================================================================== */
static void float_compare(LxFsmCtx *c) {
    float a = lx_fsm_float(c, 0), b = lx_fsm_float(c, 1), tol = lx_fsm_float(c, 2);
    if (fabsf(a - b) <= tol) ev(c, 3);
    else if (a < b) ev(c, 4);
    else ev(c, 5);
}
static const LxFsmActionDesc A_FLOAT_COMPARE = {"Float Compare", "Logic", "Compares two numbers and sends an event: equal, less than or greater than.",
    "If {0} vs {1}: = {3}, < {4}, > {5}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), P("Tolerance", LX_FSM_FLOAT, "0"), PEV("Equal"), PEV("Less Than"), PEV("Greater Than")},
    0, 0, LX_FSM_EVERY_FRAME, float_compare, float_compare, NULL};

static void int_compare(LxFsmCtx *c) {
    int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1);
    ev(c, a == b ? 2 : a < b ? 3 : 4);
}
static const LxFsmActionDesc A_INT_COMPARE = {"Int Compare", "Logic", "Compares two whole numbers.", "If {0} vs {1}: = {2}, < {3}, > {4}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), PEV("Equal"), PEV("Less Than"), PEV("Greater Than")}, 0, 0, LX_FSM_EVERY_FRAME, int_compare, int_compare, NULL};

static void bool_test(LxFsmCtx *c) { ev(c, lx_fsm_bool(c, 0) ? 1 : 2); }
static const LxFsmActionDesc A_BOOL_TEST = {"Bool Test", "Logic", "Sends one event if a bool is true, another if false.", "If {0}: {1}, else {2}",
    {P("Bool", LX_FSM_BOOL, "false"), PEV("Is True"), PEV("Is False")}, 0, 0, LX_FSM_EVERY_FRAME, bool_test, bool_test, NULL};

static void string_compare(LxFsmCtx *c) {
    const char *a = lx_fsm_string(c, 0);
    char ac[LX_REF_MAX];
    lx_strcpy(ac, sizeof ac, a);
    const char *b = lx_fsm_string(c, 1);
    bool eq = lx_fsm_bool(c, 2) ? !lx_strcasecmp(ac, b) : !strcmp(ac, b);
    ev(c, eq ? 3 : 4);
}
static const LxFsmActionDesc A_STRING_COMPARE = {"String Compare", "Logic", "Compares two pieces of text.", "If {0} = {1}: {3}, else {4}",
    {P("String 1", LX_FSM_STRING, ""), P("String 2", LX_FSM_STRING, ""), P("Ignore Case", LX_FSM_BOOL, "true"), PEV("Equal"), PEV("Not Equal")},
    0, 0, LX_FSM_ONCE, string_compare, string_compare, NULL};

static void object_compare(LxFsmCtx *c) { ev(c, obj(c, 0) == obj(c, 1) ? 2 : 3); }
static const LxFsmActionDesc A_OBJECT_COMPARE = {"Object Compare", "Logic", "Checks whether two objects are the same one.", "If {0} is {1}: {2}, else {3}",
    {POBJ("Object 1"), POBJN("Object 2"), PEV("Same"), PEV("Different")}, 0, 0, LX_FSM_ONCE, object_compare, object_compare, NULL};

static void obj_valid(LxFsmCtx *c) { ev(c, obj(c, 0) ? 1 : 2); }
static const LxFsmActionDesc A_OBJECT_VALID = {"Is Object Valid", "Logic", "Checks that an object exists (not None, not destroyed).", "If {0} exists: {1}, else {2}",
    {POBJN("Object"), PEV("Exists"), PEV("Missing")}, 0, 0, LX_FSM_ONCE, obj_valid, obj_valid, NULL};

static void bool_flip(LxFsmCtx *c) { lx_fsm_set_bool(c, 0, !lx_fsm_bool(c, 0)); }
static const LxFsmActionDesc A_BOOL_FLIP = {"Bool Flip", "Logic", "true becomes false and false becomes true.", "Flip {0}",
    {POUT("Bool Variable", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, bool_flip, bool_flip, NULL};

static void bool_all(LxFsmCtx *c) {
    bool all = true, any = false;
    for (int i = 0; i < 4; i++) {
        if (lx_fsm_is_none(c, i)) continue;
        bool v = lx_fsm_bool(c, i);
        all &= v;
        any |= v;
    }
    ev(c, (lx_fsm_int(c, 4) == 0 ? all : any) ? 5 : 6);
}
static const LxFsmActionDesc A_BOOL_ALL = {"Bool Check (All / Any)", "Logic", "Tests up to four bools: all true, or any true.", "If {4} of {0}, {1}, {2}, {3}: {5}, else {6}",
    {P("Bool 1", LX_FSM_BOOL, "true"), P("Bool 2", LX_FSM_BOOL, ""), P("Bool 3", LX_FSM_BOOL, ""), P("Bool 4", LX_FSM_BOOL, ""), PENUM("Need", "All|Any", "0"), PEV("True"), PEV("False")},
    0, 0, LX_FSM_EVERY_FRAME, bool_all, bool_all, NULL};

/* ======================================================================
 *  Variables & math
 * ====================================================================== */
#define SETTER(FN, NAME, T, GET, SET, DEF)                                                                          \
    static void FN(LxFsmCtx *c) { SET(c, 0, GET(c, 1)); }                                                           \
    static const LxFsmActionDesc A_##FN = {NAME, "Variables", "Sets a variable.", "{0} = {1}",                      \
        {POUT("Variable", T), P("Value", T, DEF)}, 0, 0, LX_FSM_ONCE, FN, FN, NULL};
SETTER(set_float, "Set Float", LX_FSM_FLOAT, lx_fsm_float, lx_fsm_set_float, "0")
SETTER(set_int, "Set Int", LX_FSM_INT, lx_fsm_int, lx_fsm_set_int, "0")
SETTER(set_bool, "Set Bool", LX_FSM_BOOL, lx_fsm_bool, lx_fsm_set_bool, "true")
SETTER(set_string, "Set String", LX_FSM_STRING, lx_fsm_string, lx_fsm_set_string, "")
SETTER(set_vec3, "Set Vector3", LX_FSM_VEC3, lx_fsm_vec3, lx_fsm_set_vec3, "0 0 0")
SETTER(set_color, "Set Color", LX_FSM_COLOR, lx_fsm_color, lx_fsm_set_color, "1 1 1 1")
SETTER(set_object, "Set Object", LX_FSM_OBJECT, lx_fsm_object, lx_fsm_set_object, "Owner")

static void float_add(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_fsm_float(c, 0) + lx_fsm_float(c, 1) * (lx_fsm_bool(c, 2) ? c->dt : 1.0f)); }
static void float_add_enter(LxFsmCtx *c) { if (!lx_fsm_bool(c, 2)) float_add(c); }
static const LxFsmActionDesc A_FLOAT_ADD = {"Float Add", "Math", "Adds to a number variable (optionally per second).", "{0} += {1}",
    {POUT("Variable", LX_FSM_FLOAT), P("Add", LX_FSM_FLOAT, "1"), PT("Per Second", LX_FSM_BOOL, "false", "multiply by frame time: smooth changes every frame")},
    0, 0, LX_FSM_ONCE, float_add_enter, float_add, NULL};

static void int_add(LxFsmCtx *c) { lx_fsm_set_int(c, 0, lx_fsm_int(c, 0) + lx_fsm_int(c, 1)); }
static const LxFsmActionDesc A_INT_ADD = {"Int Add", "Math", "Adds to a whole-number variable (negative to subtract).", "{0} += {1}",
    {POUT("Variable", LX_FSM_INT), P("Add", LX_FSM_INT, "1")}, 0, 0, LX_FSM_ONCE, int_add, int_add, NULL};

static float op(int o, float a, float b) {
    switch (o) {
    case 0: return a + b; case 1: return a - b; case 2: return a * b; case 3: return b != 0 ? a / b : 0;
    case 4: return fminf(a, b); case 5: return fmaxf(a, b); case 6: return powf(a, b); case 7: return b != 0 ? fmodf(a, b) : 0;
    default: return a;
    }
}
static void float_op(LxFsmCtx *c) { lx_fsm_set_float(c, 3, op(lx_fsm_int(c, 2), lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_FLOAT_OP = {"Float Operator", "Math", "Does maths on two numbers and stores the result.", "{3} = {0} {2} {1}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), PENUM("Operation", "Add|Subtract|Multiply|Divide|Min|Max|Power|Remainder", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, float_op, float_op, NULL};

static void int_op(LxFsmCtx *c) {
    int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1), o = lx_fsm_int(c, 2), r = 0;
    switch (o) { case 0: r = a + b; break; case 1: r = a - b; break; case 2: r = a * b; break; case 3: r = b ? a / b : 0; break;
                 case 4: r = a < b ? a : b; break; case 5: r = a > b ? a : b; break; default: r = b ? a % b : 0; }
    lx_fsm_set_int(c, 3, r);
}
static const LxFsmActionDesc A_INT_OP = {"Int Operator", "Math", "Does maths on two whole numbers.", "{3} = {0} {2} {1}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), PENUM("Operation", "Add|Subtract|Multiply|Divide|Min|Max|Remainder", "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, int_op, int_op, NULL};

static void float_clamp(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_clampf(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2))); }
static const LxFsmActionDesc A_FLOAT_CLAMP = {"Float Clamp", "Math", "Keeps a number between a minimum and maximum.", "Clamp {0} to {1}..{2}",
    {POUT("Variable", LX_FSM_FLOAT), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "100")}, 0, 0, LX_FSM_ONCE, float_clamp, float_clamp, NULL};

static void float_fn(LxFsmCtx *c) {
    float x = lx_fsm_float(c, 0), r = x;
    switch (lx_fsm_int(c, 1)) { case 0: r = fabsf(x); break; case 1: r = floorf(x); break; case 2: r = ceilf(x); break; case 3: r = roundf(x); break;
                                case 4: r = sqrtf(fmaxf(0, x)); break; case 5: r = sinf(x * LX_DEG2RAD); break; case 6: r = cosf(x * LX_DEG2RAD); break; default: r = -x; }
    lx_fsm_set_float(c, 2, r);
}
static const LxFsmActionDesc A_FLOAT_FN = {"Float Function", "Math", "Abs, floor, ceil, round, square root, sin, cos, negate.", "{2} = {1}({0})",
    {P("Float", LX_FSM_FLOAT, "0"), PENUM("Function", "Abs|Floor|Ceil|Round|Square Root|Sin (degrees)|Cos (degrees)|Negate", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, float_fn, float_fn, NULL};

static void random_float(LxFsmCtx *c) { lx_fsm_set_float(c, 2, lx_random_range(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_RANDOM_FLOAT = {"Random Float", "Math", "A random number between Min and Max.", "{2} = random {0}..{1}",
    {P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, random_float, random_float, NULL};
static void random_int(LxFsmCtx *c) { lx_fsm_set_int(c, 2, lx_random_int(lx_fsm_int(c, 0), lx_fsm_int(c, 1))); }
static const LxFsmActionDesc A_RANDOM_INT = {"Random Int", "Math", "A random whole number from Min to Max (both included).", "{2} = random {0}..{1}",
    {P("Min", LX_FSM_INT, "1"), P("Max", LX_FSM_INT, "6"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, random_int, random_int, NULL};
static void random_bool(LxFsmCtx *c) { lx_fsm_set_bool(c, 1, lx_random_chance(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_RANDOM_BOOL = {"Random Chance", "Math", "true with the given probability (0..1).", "{1} = chance {0}",
    {P("Probability", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, random_bool, random_bool, NULL};

static void convert(LxFsmCtx *c) {
    switch (lx_fsm_int(c, 0)) {
    case 0: lx_fsm_set_int(c, 3, (int)lroundf(lx_fsm_float(c, 1))); break;
    case 1: lx_fsm_set_float(c, 3, (float)lx_fsm_int(c, 2)); break;
    case 2: { char b[64]; snprintf(b, sizeof b, "%g", (double)lx_fsm_float(c, 1)); lx_fsm_set_string(c, 3, b); break; }
    default: { char b[64]; snprintf(b, sizeof b, "%d", lx_fsm_int(c, 2)); lx_fsm_set_string(c, 3, b); break; }
    }
}
static const LxFsmActionDesc A_CONVERT = {"Convert", "Variables", "Float to Int, Int to Float, or either to text.", "{3} = {0}",
    {PENUM("Convert", "Float to Int|Int to Float|Float to String|Int to String", "0"), P("Float", LX_FSM_FLOAT, "0"), P("Int", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, convert, convert, NULL};

static void build_string(LxFsmCtx *c) {
    /* "Score: {0}" -> the number; {1} the second */
    const char *f = lx_fsm_string(c, 0);
    char fmt[LX_REF_MAX], out[LX_REF_MAX];
    lx_strcpy(fmt, sizeof fmt, f);
    size_t n = 0;
    for (const char *p = fmt; *p && n + 1 < sizeof out; p++) {
        if (*p == '{' && (p[1] == '0' || p[1] == '1') && p[2] == '}') {
            char v[48];
            float x = lx_fsm_float(c, p[1] == '0' ? 1 : 2);
            if (x == floorf(x) && fabsf(x) < 1e9f) snprintf(v, sizeof v, "%d", (int)x);
            else snprintf(v, sizeof v, "%.2f", (double)x);
            for (const char *q = v; *q && n + 1 < sizeof out; q++) out[n++] = *q;
            p += 2;
        } else out[n++] = *p;
    }
    out[n] = 0;
    lx_fsm_set_string(c, 3, out);
}
static const LxFsmActionDesc A_BUILD_STRING = {"Build String", "Variables", "Puts numbers into text: \"Score: {0}\".", "{3} = \"{0}\"",
    {P("Format", LX_FSM_STRING, "Score: {0}"), P("Number {0}", LX_FSM_FLOAT, "0"), P("Number {1}", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, build_string, build_string, NULL};

typedef struct { float t; float from; } InterpState;
static void finterp_enter(LxFsmCtx *c) { InterpState *s = c->state; s->from = lx_fsm_float(c, 0); }
static void finterp_update(LxFsmCtx *c) {
    InterpState *s = c->state;
    float len = lx_fsm_float(c, 2);
    s->t += c->dt;
    float k = len > 0 ? lx_clampf(s->t / len, 0, 1) : 1;
    lx_fsm_set_float(c, 4, s->from + (lx_fsm_float(c, 1) - s->from) * lx_ease((LxEase)lx_fsm_int(c, 3), k));
    if (k >= 1) { lx_fsm_finish(c); ev(c, 5); }
}
static const LxFsmActionDesc A_FLOAT_INTERP = {"Float Interpolate", "Math", "Changes a number from one value to another over time.", "{4}: {0} → {1} over {2} s",
    {P("From", LX_FSM_FLOAT, "0"), P("To", LX_FSM_FLOAT, "1"), P("Time", LX_FSM_FLOAT, "1"), PENUM("Ease", EASES, "3"), POUT("Store Result", LX_FSM_FLOAT), PEV("Finish Event")},
    sizeof(InterpState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, finterp_enter, finterp_update, NULL};

static void vec_op(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1), r = a;
    float k = lx_fsm_float(c, 3);
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_add3(a, b); break; case 1: r = lx_sub3(a, b); break; case 2: r = lx_scale3(a, k); break;
    case 3: r = lx_norm3(a); break; case 4: r = lx_cross3(a, b); break; case 5: r = lx_lerp3(a, b, k); break;
    default: r = lx_mul3(a, b); break;
    }
    lx_fsm_set_vec3(c, 4, r);
}
static const LxFsmActionDesc A_VEC_OP = {"Vector3 Operator", "Math", "Add, subtract, scale, normalize, cross, lerp or multiply vectors.", "{4} = {0} {2} {1}",
    {P("Vector 1", LX_FSM_VEC3, "0 0 0"), P("Vector 2", LX_FSM_VEC3, "0 0 0"), PENUM("Operation", "Add|Subtract|Scale by Float|Normalize|Cross|Lerp by Float|Multiply", "0"),
     P("Float", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, vec_op, vec_op, NULL};

static void vec_get(LxFsmCtx *c) { LxVec3 v = lx_fsm_vec3(c, 0); lx_fsm_set_float(c, 1, v.x); lx_fsm_set_float(c, 2, v.y); lx_fsm_set_float(c, 3, v.z); lx_fsm_set_float(c, 4, lx_len3(v)); }
static const LxFsmActionDesc A_VEC_GET = {"Get Vector3 XYZ", "Math", "Splits a vector into X, Y, Z (and its length).", "{0} → {1}, {2}, {3}",
    {P("Vector", LX_FSM_VEC3, "0 0 0"), POPT("X", LX_FSM_FLOAT), POPT("Y", LX_FSM_FLOAT), POPT("Z", LX_FSM_FLOAT), POPT("Length", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, vec_get, vec_get, NULL};
static void vec_set(LxFsmCtx *c) { lx_fsm_set_vec3(c, 3, lx_v3(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2))); }
static const LxFsmActionDesc A_VEC_SET = {"Set Vector3 XYZ", "Math", "Builds a vector from X, Y and Z.", "{3} = ({0}, {1}, {2})",
    {P("X", LX_FSM_FLOAT, "0"), P("Y", LX_FSM_FLOAT, "0"), P("Z", LX_FSM_FLOAT, "0"), POUT("Store Vector", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, vec_set, vec_set, NULL};

/* ======================================================================
 *  Transform
 * ====================================================================== */
static LxVec3 target_point(LxFsmCtx *c, int op_, int pp) {
    LxEntity t = obj(c, op_);
    LxVec3 p = lx_fsm_vec3(c, pp);
    return t ? lx_add3(lx_entity_world_position(c->scene, t), p) : p;
}
static void set_world_pos(LxScene *s, LxEntity e, LxVec3 p) {
    LxEntity par = lx_entity_parent(s, e);
    if (par) p = lx_mat4_point(lx_mat4_inverse(lx_entity_world_matrix(s, par)), p);
    lx_entity_set_position(s, e, p);
}

static void set_position(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    if (lx_fsm_int(c, 2) == 1) set_world_pos(c->scene, e, lx_fsm_vec3(c, 1));
    else lx_entity_set_position(c->scene, e, lx_fsm_vec3(c, 1));
}
static const LxFsmActionDesc A_SET_POSITION = {"Set Position", "Transform", "Puts an object at a position.", "Put {0} at {1}",
    {POBJ("Object"), P("Position", LX_FSM_VEC3, "0 0 0"), PENUM("Space", "Local|World", "1")}, 0, 0, LX_FSM_ONCE, set_position, set_position, NULL};

static void get_position(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    lx_fsm_set_vec3(c, 1, lx_fsm_int(c, 2) == 1 ? lx_entity_world_position(c->scene, e) : lx_entity_position(c->scene, e));
}
static const LxFsmActionDesc A_GET_POSITION = {"Get Position", "Transform", "Stores where an object is.", "{1} = position of {0}",
    {POBJ("Object"), POUT("Store Vector", LX_FSM_VEC3), PENUM("Space", "Local|World", "1")}, 0, 0, LX_FSM_ONCE, get_position, get_position, NULL};

static void translate(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 v = lx_fsm_vec3(c, 1);
    if (lx_fsm_bool(c, 3)) v = lx_scale3(v, c->dt);
    if (lx_fsm_int(c, 2) == 0) v = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), v);   /* along its own axes */
    set_world_pos(c->scene, e, lx_add3(lx_entity_world_position(c->scene, e), v));
}
static const LxFsmActionDesc A_TRANSLATE = {"Translate", "Transform", "Moves an object by an amount (per second when Every Frame).", "Move {0} by {1}",
    {POBJ("Object"), P("Vector", LX_FSM_VEC3, "0 0 -1"), PENUM("Space", SPACE, "0"), P("Per Second", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_EVERY_FRAME, translate, translate, NULL};

static void set_rotation(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_set_euler(c->scene, e, lx_fsm_vec3(c, 1)); }
static const LxFsmActionDesc A_SET_ROTATION = {"Set Rotation", "Transform", "Sets an object's rotation in degrees (X, Y, Z).", "Rotate {0} to {1}°",
    {POBJ("Object"), P("Euler Angles", LX_FSM_VEC3, "0 0 0")}, 0, 0, LX_FSM_ONCE, set_rotation, set_rotation, NULL};
static void get_rotation(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_fsm_set_vec3(c, 1, lx_entity_euler(c->scene, e)); }
static const LxFsmActionDesc A_GET_ROTATION = {"Get Rotation", "Transform", "Stores an object's rotation in degrees.", "{1} = rotation of {0}",
    {POBJ("Object"), POUT("Store Euler", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, get_rotation, get_rotation, NULL};
static void rotate(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 d = lx_fsm_vec3(c, 1);
    if (lx_fsm_bool(c, 2)) d = lx_scale3(d, c->dt);
    lx_entity_set_euler(c->scene, e, lx_add3(lx_entity_euler(c->scene, e), d));
}
static const LxFsmActionDesc A_ROTATE = {"Rotate", "Transform", "Turns an object (degrees, per second when Every Frame).", "Spin {0} by {1}°",
    {POBJ("Object"), P("Degrees", LX_FSM_VEC3, "0 90 0"), P("Per Second", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_EVERY_FRAME, rotate, rotate, NULL};

static void set_scale(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_set_scale(c->scene, e, lx_fsm_vec3(c, 1)); }
static const LxFsmActionDesc A_SET_SCALE = {"Set Scale", "Transform", "Sets an object's size.", "Scale {0} to {1}",
    {POBJ("Object"), P("Scale", LX_FSM_VEC3, "1 1 1")}, 0, 0, LX_FSM_ONCE, set_scale, set_scale, NULL};
static void get_scale(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_fsm_set_vec3(c, 1, lx_entity_scale(c->scene, e)); }
static const LxFsmActionDesc A_GET_SCALE = {"Get Scale", "Transform", "Stores an object's size.", "{1} = scale of {0}",
    {POBJ("Object"), POUT("Store Vector", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, get_scale, get_scale, NULL};

static void move_towards(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 pos = lx_entity_world_position(c->scene, e), goal = target_point(c, 1, 2);
    if (lx_fsm_bool(c, 6)) goal.y = pos.y;
    LxVec3 d = lx_sub3(goal, pos);
    float dist = lx_len3(d), stop = lx_fsm_float(c, 4);
    if (dist <= stop + 1e-4f) { ev(c, 5); return; }
    float step = lx_fsm_float(c, 3) * c->dt;
    if (step > dist - stop) step = dist - stop;
    set_world_pos(c->scene, e, lx_add3(pos, lx_scale3(d, step / dist)));
}
static const LxFsmActionDesc A_MOVE_TOWARDS = {"Move Towards", "Transform", "Moves an object towards a target at a set speed; sends an event when it gets there.",
    "Move {0} towards {1} at {3}, stop within {4} → {5}",
    {POBJ("Object"), POBJN("Target Object"), PT("Target Position", LX_FSM_VEC3, "0 0 0", "a point, or an offset from the target object"), P("Speed", LX_FSM_FLOAT, "3"),
     P("Finish Distance", LX_FSM_FLOAT, "0.2"), PEV("Finish Event"), P("Ignore Vertical", LX_FSM_BOOL, "true")},
    0, 0, LX_FSM_EVERY_FRAME, NULL, move_towards, NULL};

static void look_at(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 pos = lx_entity_world_position(c->scene, e), goal = target_point(c, 1, 2);
    if (lx_fsm_bool(c, 3)) goal.y = pos.y;
    if (lx_len3sq(lx_sub3(goal, pos)) < 1e-8f) return;
    lx_entity_look_at(c->scene, e, goal, lx_v3(0, 1, 0));
}
static const LxFsmActionDesc A_LOOK_AT = {"Look At", "Transform", "Turns an object to face a target.", "Turn {0} to face {1}",
    {POBJ("Object"), POBJN("Target Object"), P("Target Position", LX_FSM_VEC3, "0 0 0"), P("Keep Upright", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_EVERY_FRAME, look_at, look_at, NULL};

typedef struct { int tween; float t, len; } TweenState;
static void tween_common(LxFsmCtx *c, int kind) {
    LxEntity e = obj(c, 0);
    TweenState *s = c->state;
    if (!e) { lx_fsm_finish(c); return; }
    LxVec3 to = lx_fsm_vec3(c, 1);
    float len = lx_fsm_float(c, 2);
    LxEase ease = (LxEase)lx_fsm_int(c, 3);
    if (kind == 0 && lx_fsm_bool(c, 5)) to = lx_add3(lx_entity_position(c->scene, e), to);   /* relative */
    s->tween = kind == 0 ? lx_tween_position(c->scene, e, to, len, ease) : kind == 1 ? lx_tween_scale(c->scene, e, to, len, ease) : lx_tween_euler(c->scene, e, to, len, ease);
    s->len = len;
}
static void tween_update(LxFsmCtx *c) { TweenState *s = c->state; s->t += c->dt; if (s->t >= s->len) { lx_fsm_finish(c); ev(c, 4); } }
static void tween_exit(LxFsmCtx *c) { TweenState *s = c->state; if (s->t < s->len && s->tween) lx_tween_cancel(c->scene, s->tween); }
static void move_object_enter(LxFsmCtx *c) { tween_common(c, 0); }
static void tween_scale_enter(LxFsmCtx *c) { tween_common(c, 1); }
static void tween_rot_enter(LxFsmCtx *c) { tween_common(c, 2); }
static const LxFsmActionDesc A_MOVE_OBJECT = {"Move Object", "Transform", "Slides an object to a position over time with easing.", "Slide {0} to {1} in {2} s → {4}",
    {POBJ("Object"), P("To Position", LX_FSM_VEC3, "0 0 0"), P("Time", LX_FSM_FLOAT, "1"), PENUM("Ease", EASES, "3"), PEV("Finish Event"), PT("Relative", LX_FSM_BOOL, "false", "To Position is added to where it is now")},
    sizeof(TweenState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, move_object_enter, tween_update, tween_exit};
static const LxFsmActionDesc A_TWEEN_SCALE = {"Tween Scale", "Transform", "Grows or shrinks an object over time.", "Scale {0} to {1} in {2} s → {4}",
    {POBJ("Object"), P("To Scale", LX_FSM_VEC3, "1 1 1"), P("Time", LX_FSM_FLOAT, "0.3"), PENUM("Ease", EASES, "4"), PEV("Finish Event")},
    sizeof(TweenState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, tween_scale_enter, tween_update, tween_exit};
static const LxFsmActionDesc A_TWEEN_ROT = {"Tween Rotation", "Transform", "Turns an object to an angle over time.", "Turn {0} to {1}° in {2} s → {4}",
    {POBJ("Object"), P("To Euler", LX_FSM_VEC3, "0 180 0"), P("Time", LX_FSM_FLOAT, "1"), PENUM("Ease", EASES, "3"), PEV("Finish Event")},
    sizeof(TweenState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, tween_rot_enter, tween_update, tween_exit};

static void get_distance(LxFsmCtx *c) {
    LxEntity a = obj(c, 0), b = obj(c, 1);
    if (!a || !b) return;
    float d = lx_len3(lx_sub3(lx_entity_world_position(c->scene, a), lx_entity_world_position(c->scene, b)));
    lx_fsm_set_float(c, 2, d);
    float r = lx_fsm_float(c, 3);
    if (r > 0) ev(c, d <= r ? 4 : 5);
}
static const LxFsmActionDesc A_GET_DISTANCE = {"Get Distance", "Transform", "Distance between two objects; optionally sends Within / Outside a range.", "{2} = distance {0} ↔ {1}",
    {POBJ("Object"), POBJN("Other"), POPT("Store Distance", LX_FSM_FLOAT), PT("Range", LX_FSM_FLOAT, "0", "0 = no events"), PEV("Within Range"), PEV("Outside Range")},
    0, 0, LX_FSM_EVERY_FRAME, get_distance, get_distance, NULL};

static void set_parent(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxEntity p = obj(c, 1);
    LxVec3 wp = lx_entity_world_position(c->scene, e);
    lx_entity_set_parent(c->scene, e, p);
    if (lx_fsm_bool(c, 2)) set_world_pos(c->scene, e, wp);
}
static const LxFsmActionDesc A_SET_PARENT = {"Set Parent", "Transform", "Attaches an object to another (None = to the scene).", "Attach {0} to {1}",
    {POBJ("Object"), POBJN("Parent"), P("Keep World Position", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_ONCE, set_parent, set_parent, NULL};

/* ======================================================================
 *  Objects
 * ====================================================================== */
static void spawn(LxFsmCtx *c) {
    const char *ref = lx_fsm_asset(c, 0);
    if (!ref[0]) return;
    LxVec3 pos = target_point(c, 1, 2);
    LxEntity e = lx_prefab_instantiate(c->scene, ref, pos, obj(c, 3));
    if (e) lx_entity_set_euler(c->scene, e, lx_fsm_vec3(c, 4));
    lx_fsm_set_object(c, 5, e);
}
static const LxFsmActionDesc A_SPAWN = {"Spawn Prefab", "Objects", "Creates a copy of a prefab.", "Spawn {0} at {1} → {5}",
    {PASSET("Prefab", "prefab"), POBJN("At Object"), PT("Position", LX_FSM_VEC3, "0 0 0", "a point, or an offset from At Object"), POBJN("Parent"), P("Rotation", LX_FSM_VEC3, "0 0 0"), POPT("Store Object", LX_FSM_OBJECT)},
    0, 0, LX_FSM_ONCE, spawn, spawn, NULL};

typedef struct { float t; bool done; } DestroyState;
static void destroy_now(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_destroy(c->scene, e); }
static void destroy_enter(LxFsmCtx *c) { if (lx_fsm_float(c, 1) <= 0) { destroy_now(c); lx_fsm_finish(c); } }
static void destroy_update(LxFsmCtx *c) { DestroyState *s = c->state; s->t += c->dt; if (s->t >= lx_fsm_float(c, 1)) { lx_fsm_finish(c); destroy_now(c); } }
static const LxFsmActionDesc A_DESTROY = {"Destroy Object", "Objects", "Removes an object (and its children), optionally after a delay.", "Destroy {0} after {1} s",
    {POBJ("Object"), P("Delay", LX_FSM_FLOAT, "0")}, sizeof(DestroyState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, destroy_enter, destroy_update, NULL};

static void set_active(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_set_active(c->scene, e, lx_fsm_bool(c, 1)); }
static const LxFsmActionDesc A_SET_ACTIVE = {"Set Active", "Objects", "Shows/enables or hides/disables an object.", "Set {0} active = {1}",
    {POBJ("Object"), P("Active", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_ONCE, set_active, set_active, NULL};

static void find_object(LxFsmCtx *c) {
    LxEntity e = lx_fsm_int(c, 0) == 0 ? lx_entity_find(c->scene, lx_fsm_string(c, 1)) : lx_entity_find_with_tag(c->scene, lx_fsm_string(c, 1));
    lx_fsm_set_object(c, 2, e);
    ev(c, e ? 3 : 4);
}
static const LxFsmActionDesc A_FIND = {"Find Object", "Objects", "Finds an object by name or tag.", "{2} = object with {0} {1}",
    {PENUM("By", "Name|Tag", "1"), P("Name or Tag", LX_FSM_STRING, "Player"), POUT("Store Object", LX_FSM_OBJECT), PEV("Found"), PEV("Not Found")},
    0, 0, LX_FSM_ONCE, find_object, find_object, NULL};

static void get_child(LxFsmCtx *c) { LxEntity e = obj(c, 0); lx_fsm_set_object(c, 2, e ? lx_entity_find_child(c->scene, e, lx_fsm_string(c, 1)) : 0); }
static const LxFsmActionDesc A_GET_CHILD = {"Get Child", "Objects", "Finds a child (or deeper) object by name.", "{2} = child {1} of {0}",
    {POBJ("Object"), P("Name", LX_FSM_STRING, ""), POUT("Store Object", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, get_child, get_child, NULL};
static void get_parent(LxFsmCtx *c) { LxEntity e = obj(c, 0); lx_fsm_set_object(c, 1, e ? lx_entity_parent(c->scene, e) : 0); }
static const LxFsmActionDesc A_GET_PARENT = {"Get Parent", "Objects", "Stores the object this one is attached to.", "{1} = parent of {0}",
    {POBJ("Object"), POUT("Store Object", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, get_parent, get_parent, NULL};
static void get_owner(LxFsmCtx *c) { lx_fsm_set_object(c, 0, c->self); }
static const LxFsmActionDesc A_GET_OWNER = {"Get Owner", "Objects", "Stores the object this graph runs on.", "{0} = this object",
    {POUT("Store Object", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, get_owner, get_owner, NULL};

static void has_tag(LxFsmCtx *c) { LxEntity e = obj(c, 0); ev(c, e && lx_entity_has_tag(c->scene, e, lx_fsm_string(c, 1)) ? 2 : 3); }
static const LxFsmActionDesc A_HAS_TAG = {"Has Tag", "Objects", "Checks an object's tag.", "If {0} has tag {1}: {2}, else {3}",
    {POBJ("Object"), P("Tag", LX_FSM_STRING, "Player"), PEV("Yes"), PEV("No")}, 0, 0, LX_FSM_ONCE, has_tag, has_tag, NULL};
static void set_tag(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_set_tag(c->scene, e, lx_fsm_string(c, 1)); }
static const LxFsmActionDesc A_SET_TAG = {"Set Tag", "Objects", "Gives an object a tag.", "Tag {0} as {1}",
    {POBJ("Object"), P("Tag", LX_FSM_STRING, "Enemy")}, 0, 0, LX_FSM_ONCE, set_tag, set_tag, NULL};
static void set_name(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_entity_set_name(c->scene, e, lx_fsm_string(c, 1)); }
static const LxFsmActionDesc A_SET_NAME = {"Set Name", "Objects", "Renames an object.", "Rename {0} to {1}",
    {POBJ("Object"), P("Name", LX_FSM_STRING, "")}, 0, 0, LX_FSM_ONCE, set_name, set_name, NULL};
static void count_tag(LxFsmCtx *c) { LxEntity tmp[1]; lx_fsm_set_int(c, 1, lx_entities_with_tag(c->scene, lx_fsm_string(c, 0), tmp, 0)); }
static const LxFsmActionDesc A_COUNT_TAG = {"Count With Tag", "Objects", "How many objects have a tag (enemies left, coins...).", "{1} = count of {0}",
    {P("Tag", LX_FSM_STRING, "Enemy"), POUT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, count_tag, count_tag, NULL};

static void set_material(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxMeshRenderer *m = e ? lx_get_mesh(c->scene, e) : NULL;
    if (m && lx_fsm_asset(c, 1)[0]) lx_strcpy(m->material, sizeof m->material, lx_fsm_asset(c, 1));
}
static const LxFsmActionDesc A_SET_MATERIAL = {"Set Material", "Rendering", "Changes an object's material.", "Paint {0} with {1}",
    {POBJ("Object"), PASSET("Material", "material")}, 0, 0, LX_FSM_ONCE, set_material, set_material, NULL};
static void set_light(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) return;
    LxVec4 col = lx_fsm_color(c, 1);
    l->color = lx_v3(col.x, col.y, col.z);
    l->intensity = lx_fsm_float(c, 2);
}
static const LxFsmActionDesc A_SET_LIGHT = {"Set Light", "Rendering", "Changes a light's colour and brightness.", "Light {0}: {1} × {2}",
    {POBJ("Object"), P("Color", LX_FSM_COLOR, "1 1 1 1"), P("Intensity", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, set_light, set_light, NULL};

/* ======================================================================
 *  Physics
 * ====================================================================== */
static void add_force(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 f = lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 3) == 0) f = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), f);
    switch (lx_fsm_int(c, 2)) {
    case 0: lx_rigidbody_add_force(c->scene, e, f); break;
    case 1: lx_rigidbody_add_impulse(c->scene, e, f); break;
    default: lx_rigidbody_set_velocity(c->scene, e, lx_add3(lx_rigidbody_velocity(c->scene, e), f)); break;
    }
}
static const LxFsmActionDesc A_ADD_FORCE = {"Add Force", "Physics", "Pushes a Rigidbody: a force (Every Frame), an instant impulse, or a velocity change.", "Push {0} with {1} ({2})",
    {POBJ("Object"), P("Force", LX_FSM_VEC3, "0 5 0"), PENUM("Mode", "Force|Impulse|Velocity Change", "1"), PENUM("Space", SPACE, "1")}, 0, 0, LX_FSM_ONCE, add_force, add_force, NULL};
static void set_velocity(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_rigidbody_set_velocity(c->scene, e, lx_fsm_vec3(c, 1)); }
static const LxFsmActionDesc A_SET_VELOCITY = {"Set Velocity", "Physics", "Sets a Rigidbody's velocity.", "{0} velocity = {1}",
    {POBJ("Object"), P("Velocity", LX_FSM_VEC3, "0 0 0")}, 0, 0, LX_FSM_ONCE, set_velocity, set_velocity, NULL};
static void get_velocity(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) { LxVec3 v = lx_rigidbody_velocity(c->scene, e); lx_fsm_set_vec3(c, 1, v); lx_fsm_set_float(c, 2, lx_len3(v)); } }
static const LxFsmActionDesc A_GET_VELOCITY = {"Get Velocity", "Physics", "Stores a Rigidbody's velocity and speed.", "{1} = velocity of {0}",
    {POBJ("Object"), POPT("Store Velocity", LX_FSM_VEC3), POPT("Store Speed", LX_FSM_FLOAT)}, 0, 0, LX_FSM_EVERY_FRAME, get_velocity, get_velocity, NULL};

static void raycast(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxVec3 from = e ? lx_add3(lx_entity_world_position(c->scene, e), lx_fsm_vec3(c, 1)) : lx_fsm_vec3(c, 1);
    LxVec3 dir = lx_fsm_vec3(c, 2);
    if (e && lx_fsm_int(c, 3) == 0) dir = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), dir);
    if (lx_len3sq(dir) < 1e-10f) return;
    LxRaycastHit hit;
    bool got = lx_physics_raycast(c->scene, from, lx_norm3(dir), lx_fsm_float(c, 4), LX_ALL_LAYERS, &hit);
    if (got && hit.entity == e) got = false;
    lx_fsm_set_bool(c, 7, got);
    if (got) { lx_fsm_set_object(c, 8, hit.entity); lx_fsm_set_vec3(c, 9, hit.point); lx_fsm_set_float(c, 10, hit.distance); ev(c, 5); }
    else ev(c, 6);
}
static const LxFsmActionDesc A_RAYCAST = {"Raycast", "Physics", "Casts a ray and reports what it hits.", "Ray from {0} along {2}, {4} m: hit → {5}",
    {POBJ("From Object"), P("Offset", LX_FSM_VEC3, "0 0 0"), P("Direction", LX_FSM_VEC3, "0 0 -1"), PENUM("Space", SPACE, "0"), P("Distance", LX_FSM_FLOAT, "50"),
     PEV("Hit Event"), PEV("Miss Event"), POPT("Store Did Hit", LX_FSM_BOOL), POPT("Store Hit Object", LX_FSM_OBJECT), POPT("Store Hit Point", LX_FSM_VEC3), POPT("Store Distance", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, raycast, raycast, NULL};

/* collision/trigger filters: the system events come with "other"; these filter by tag and send your own event */
typedef struct { char last[LX_FSM_NAME]; } SysState;
static void sys_filter_update(LxFsmCtx *c, const char *enter_ev, const char *exit_ev) {
    LxFsmRun *r = c->run;
    if (!r || !r->last_system_event[0]) return;
    bool enter = !strcmp(r->last_system_event, enter_ev), exit_ = !strcmp(r->last_system_event, exit_ev);
    int want = lx_fsm_int(c, 0);
    if (!(want == 0 ? enter : want == 1 ? exit_ : (enter || exit_))) return;
    LxEntity o = r->last_other;
    const char *tag = lx_fsm_string(c, 1);
    if (tag[0] && (!o || !lx_entity_has_tag(c->scene, o, tag))) { r->last_system_event[0] = 0; return; }
    r->last_system_event[0] = 0;
    lx_fsm_set_object(c, 3, o);
    ev(c, 2);
}
static void coll_ev(LxFsmCtx *c) { sys_filter_update(c, "COLLISION_ENTER", "COLLISION_EXIT"); }
static void trig_ev(LxFsmCtx *c) { sys_filter_update(c, "TRIGGER_ENTER", "TRIGGER_EXIT"); }
static void sys_enter(LxFsmCtx *c) { if (c->run) c->run->last_system_event[0] = 0; }
static const LxFsmActionDesc A_COLLISION_EVENT = {"Collision Event", "Physics", "When this object bumps into something (optionally with a tag), sends an event.", "On {0} with {1} → {2}",
    {PENUM("When", "Enter|Exit|Either", "0"), PT("Tag", LX_FSM_STRING, "", "empty = anything"), PEV("Send Event"), POPT("Store Other", LX_FSM_OBJECT)},
    sizeof(SysState), 0, LX_FSM_EVERY_FRAME, sys_enter, coll_ev, NULL};
static const LxFsmActionDesc A_TRIGGER_EVENT = {"Trigger Event", "Physics", "When something enters/leaves this trigger (optionally with a tag), sends an event.", "On trigger {0} with {1} → {2}",
    {PENUM("When", "Enter|Exit|Either", "0"), PT("Tag", LX_FSM_STRING, "Player", "empty = anything"), PEV("Send Event"), POPT("Store Other", LX_FSM_OBJECT)},
    sizeof(SysState), 0, LX_FSM_EVERY_FRAME, sys_enter, trig_ev, NULL};
static void collision_info(LxFsmCtx *c) { lx_fsm_set_object(c, 0, c->run ? c->run->last_other : 0); }
static const LxFsmActionDesc A_COLLISION_INFO = {"Get Collision Info", "Physics", "The other object in the latest collision or trigger.", "{0} = what we touched",
    {POUT("Store Other", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, collision_info, collision_info, NULL};

static void char_move(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 v = lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 2) == 0) v = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), v);
    v = lx_scale3(v, c->dt);
    lx_character_move(c->scene, e, v);
    lx_fsm_set_bool(c, 3, lx_character_grounded(c->scene, e));
}
static const LxFsmActionDesc A_CHAR_MOVE = {"Character Move", "Physics", "Walks a Character Controller (velocity in m/s; it slides along walls and climbs steps).", "Walk {0} at {1}",
    {POBJ("Object"), P("Velocity", LX_FSM_VEC3, "0 0 0"), PENUM("Space", SPACE, "1"), POPT("Store Grounded", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, char_move, char_move, NULL};

/* ======================================================================
 *  Animation, audio
 * ====================================================================== */
typedef struct { bool wait; } ClipState;
static void play_clip_enter(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) { lx_fsm_finish(c); return; }
    LxAnimator *a = lx_get_animator(c->scene, e);
    if (!a) { lx_fsm_finish(c); return; }
    const char *clip = lx_fsm_string(c, 1);
    if (clip[0]) lx_animator_play(c->scene, e, clip, lx_fsm_float(c, 2));
    if (!lx_fsm_event(c, 3)[0]) lx_fsm_finish(c);
}
static void play_clip_update(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e || !lx_animator_playing(c->scene, e)) { lx_fsm_finish(c); ev(c, 3); }
}
static const LxFsmActionDesc A_PLAY_CLIP = {"Play Animation", "Animation", "Plays a clip on an Animator, cross-fading from the current one.", "Play {1} on {0} (fade {2} s)",
    {POBJ("Object"), PT("Clip", LX_FSM_STRING, "", "\"Models/Hero.fbx#Run\"; clips from any humanoid retarget"), P("Fade", LX_FSM_FLOAT, "0.2"), PEV("Finished Event")},
    0, LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, play_clip_enter, play_clip_update, NULL};
static void anim_speed(LxFsmCtx *c) { LxEntity e = obj(c, 0); LxAnimator *a = e ? lx_get_animator(c->scene, e) : NULL; if (a) a->speed = lx_fsm_float(c, 1); }
static const LxFsmActionDesc A_ANIM_SPEED = {"Set Animation Speed", "Animation", "Speeds up, slows or pauses (0) an Animator.", "{0} animation speed = {1}",
    {POBJ("Object"), P("Speed", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, anim_speed, anim_speed, NULL};
static void anim_stop(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_animator_stop(c->scene, e); }
static const LxFsmActionDesc A_ANIM_STOP = {"Stop Animation", "Animation", "Stops an Animator where it is.", "Stop {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, anim_stop, anim_stop, NULL};

static void play_sound(LxFsmCtx *c) {
    const char *clip = lx_fsm_asset(c, 0);
    if (!clip[0]) return;
    LxEntity at = obj(c, 1);
    lx_audio_play_oneshot(clip, at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 2), lx_fsm_float(c, 3));
}
static const LxFsmActionDesc A_PLAY_SOUND = {"Play Sound", "Audio", "Plays a sound once at a place (it fades with distance).", "Play {0} at {1}",
    {PASSET("Sound", "sound"), POBJ("At Object"), P("Position", LX_FSM_VEC3, "0 0 0"), P("Volume", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, play_sound, play_sound, NULL};
static void audio_play(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_audio_play(c->scene, e); }
static const LxFsmActionDesc A_AUDIO_PLAY = {"Play Audio Source", "Audio", "Starts an object's Audio Source.", "Play the Audio Source on {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, audio_play, audio_play, NULL};
static void audio_stop(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_audio_stop(c->scene, e); }
static const LxFsmActionDesc A_AUDIO_STOP = {"Stop Audio Source", "Audio", "Stops an object's Audio Source.", "Stop the Audio Source on {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, audio_stop, audio_stop, NULL};

/* ======================================================================
 *  Input
 * ====================================================================== */
static LxKey key_of(const char *name) {
    if (!name[0]) return LX_KEY_UNKNOWN;
    for (int k = 1; k < LX_KEY_COUNT; k++) {
        const char *n = lx_key_name((LxKey)k);
        if (n && n[0] && !lx_strcasecmp(n, name)) return (LxKey)k;
    }
    return LX_KEY_UNKNOWN;
}
static void key_event(LxFsmCtx *c) {
    LxKey k = key_of(lx_fsm_string(c, 0));
    if (!k) return;
    int when = lx_fsm_int(c, 1);
    bool hit = when == 0 ? lx_key_pressed(k) : when == 1 ? lx_key_released(k) : lx_key_down(k);
    lx_fsm_set_bool(c, 3, lx_key_down(k));
    if (hit) ev(c, 2);
}
static const LxFsmActionDesc A_KEY_EVENT = {"Key Event", "Input", "Sends an event when a key is pressed, released or held.", "When {0} is {1}: {2}",
    {PT("Key", LX_FSM_STRING, "Space", "a key name: A..Z, 0..9, Space, Enter, Escape, Left, Right, Up, Down, Shift, F1..."), PENUM("When", "Pressed|Released|Held", "0"), PEV("Send Event"), POPT("Store Is Down", LX_FSM_BOOL)},
    0, 0, LX_FSM_EVERY_FRAME, key_event, key_event, NULL};
static void action_event(LxFsmCtx *c) {
    const char *a = lx_fsm_string(c, 0);
    int when = lx_fsm_int(c, 1);
    bool hit = when == 0 ? lx_action_pressed(a) : when == 1 ? lx_action_released(a) : lx_action_down(a);
    lx_fsm_set_bool(c, 3, lx_action_down(a));
    if (hit) ev(c, 2);
}
static const LxFsmActionDesc A_INPUT_ACTION = {"Input Action Event", "Input", "Uses an action from the Input Manager (keyboard, gamepad and touch at once).", "When {0} is {1}: {2}",
    {P("Action", LX_FSM_STRING, "Jump"), PENUM("When", "Pressed|Released|Held", "0"), PEV("Send Event"), POPT("Store Is Down", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, action_event, action_event, NULL};
static void get_axis(LxFsmCtx *c) {
    LxVec2 v = lx_axis2(lx_fsm_string(c, 0), lx_fsm_string(c, 1));
    float k = lx_fsm_float(c, 2);
    lx_fsm_set_vec3(c, 3, lx_v3(v.x * k, 0, -v.y * k));
    lx_fsm_set_float(c, 4, v.x);
    lx_fsm_set_float(c, 5, v.y);
}
static const LxFsmActionDesc A_GET_AXIS = {"Get Axis", "Input", "Reads two Input Manager axes as a movement vector on the ground (X, -Z).", "{3} = ({0}, {1}) × {2}",
    {P("Horizontal Axis", LX_FSM_STRING, "Move X"), P("Vertical Axis", LX_FSM_STRING, "Move Y"), P("Multiplier", LX_FSM_FLOAT, "5"), POPT("Store Vector", LX_FSM_VEC3), POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, get_axis, get_axis, NULL};
static void mouse_event(LxFsmCtx *c) {
    LxMouseButton b = (LxMouseButton)lx_fsm_int(c, 0);
    int when = lx_fsm_int(c, 1);
    if (when == 0 ? lx_mouse_pressed(b) : when == 1 ? lx_mouse_released(b) : lx_mouse_down(b)) ev(c, 2);
}
static const LxFsmActionDesc A_MOUSE_EVENT = {"Mouse Button Event", "Input", "Sends an event on a mouse click (or tap).", "When {0} button is {1}: {2}",
    {PENUM("Button", "Left|Right|Middle", "0"), PENUM("When", "Pressed|Released|Held", "0"), PEV("Send Event")}, 0, 0, LX_FSM_EVERY_FRAME, mouse_event, mouse_event, NULL};

/* ======================================================================
 *  Time, scenes, save data, UI, debug
 * ====================================================================== */
static void get_time(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_game_time(c->scene)); lx_fsm_set_float(c, 1, c->dt); lx_fsm_set_float(c, 2, lx_fsm_state_time(c)); }
static const LxFsmActionDesc A_GET_TIME = {"Get Time", "Time", "Game time, this frame's time step, and time in this state.", "{0} = game time",
    {POPT("Store Game Time", LX_FSM_FLOAT), POPT("Store Delta Time", LX_FSM_FLOAT), POPT("Store State Time", LX_FSM_FLOAT)}, 0, 0, LX_FSM_EVERY_FRAME, get_time, get_time, NULL};
static void time_scale(LxFsmCtx *c) { lx_time_set_scale(lx_fsm_float(c, 0)); }
static const LxFsmActionDesc A_TIME_SCALE = {"Set Time Scale", "Time", "Slow motion (0.5), pause (0) or normal speed (1).", "Time scale = {0}",
    {P("Scale", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, time_scale, time_scale, NULL};

static void load_scene(LxFsmCtx *c) { const char *s = lx_fsm_asset(c, 0); if (s[0]) lx_load_scene(s); }
static const LxFsmActionDesc A_LOAD_SCENE = {"Load Scene", "Scenes", "Switches to another scene at the end of the frame.", "Load {0}",
    {PASSET("Scene", "scene")}, 0, 0, LX_FSM_ONCE, load_scene, NULL, NULL};
static void quit_game(LxFsmCtx *c) { LX_UNUSED(c); lx_quit(); }
static const LxFsmActionDesc A_QUIT = {"Quit Game", "Scenes", "Closes the game (ignored in the editor's Play mode).", "Quit the game",
    {{0}}, 0, 0, LX_FSM_ONCE, quit_game, NULL, NULL};

static void save_value(LxFsmCtx *c) {
    const char *k = lx_fsm_string(c, 0);
    char key[LX_REF_MAX];
    lx_strcpy(key, sizeof key, k);
    if (!key[0]) return;
    switch (lx_fsm_int(c, 1)) {
    case 0: lx_save_set_float(key, lx_fsm_float(c, 2)); break;
    case 1: lx_save_set_int(key, lx_fsm_int(c, 2)); break;
    default: lx_save_set_string(key, lx_fsm_string(c, 2)); break;
    }
}
static const LxFsmActionDesc A_SAVE = {"Save Value", "Save Data", "Keeps a value between runs of the game.", "Save {2} as {0}",
    {P("Key", LX_FSM_STRING, "best_score"), PENUM("Type", "Float|Int|String", "0"), P("Value", LX_FSM_FLOAT, "0")}, 0, 0, LX_FSM_ONCE, save_value, save_value, NULL};
static void load_value(LxFsmCtx *c) {
    const char *k = lx_fsm_string(c, 0);
    char key[LX_REF_MAX];
    lx_strcpy(key, sizeof key, k);
    bool has = lx_save_has(key);
    if (lx_fsm_int(c, 1) == 2) lx_fsm_set_string(c, 3, lx_save_get_string(key, lx_fsm_string(c, 2)));
    else lx_fsm_set_float(c, 3, lx_save_get_float(key, lx_fsm_float(c, 2)));
    ev(c, has ? 4 : 5);
}
static const LxFsmActionDesc A_LOAD = {"Load Value", "Save Data", "Reads a saved value (or the default when nothing was saved).", "{3} = saved {0}",
    {P("Key", LX_FSM_STRING, "best_score"), PENUM("Type", "Float|Int|String", "0"), P("Default", LX_FSM_FLOAT, "0"), POUT("Store Value", LX_FSM_FLOAT), PEV("Found"), PEV("Not Saved")},
    0, 0, LX_FSM_ONCE, load_value, load_value, NULL};
static void delete_save(LxFsmCtx *c) { char key[LX_REF_MAX]; lx_strcpy(key, sizeof key, lx_fsm_string(c, 0)); if (lx_fsm_bool(c, 1)) lx_save_clear(); else lx_save_delete(key); }
static const LxFsmActionDesc A_DELETE_SAVE = {"Delete Saved Value", "Save Data", "Forgets one saved value, or everything.", "Forget {0}",
    {P("Key", LX_FSM_STRING, ""), P("Delete Everything", LX_FSM_BOOL, "false")}, 0, 0, LX_FSM_ONCE, delete_save, NULL, NULL};

static uint32_t rgba(LxVec4 c) {
    return ((uint32_t)(lx_clampf(c.x, 0, 1) * 255) << 0) | ((uint32_t)(lx_clampf(c.y, 0, 1) * 255) << 8) | ((uint32_t)(lx_clampf(c.z, 0, 1) * 255) << 16) |
           ((uint32_t)(lx_clampf(c.w, 0, 1) * 255) << 24);
}
static void ui_text(LxFsmCtx *c) {
    LxVec3 p = lx_fsm_vec3(c, 1);
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    lx_ui_text(p.x, p.y, lx_fsm_float(c, 2), rgba(lx_fsm_color(c, 3)), "%s", lx_fsm_string(c, 0));
}
static const LxFsmActionDesc A_UI_TEXT = {"UI Text", "Game UI", "Draws text on screen every frame (canvas is 1080 units tall).", "Show \"{0}\" at {1}",
    {P("Text", LX_FSM_STRING, "Hello"), P("Position", LX_FSM_VEC3, "40 40 0"), P("Size", LX_FSM_FLOAT, "48"), P("Color", LX_FSM_COLOR, "1 1 1 1"),
     PENUM("Anchor", "Top Left|Top|Top Right|Left|Center|Right|Bottom Left|Bottom|Bottom Right", "0")}, 0, 0, LX_FSM_EVERY_FRAME, ui_text, ui_text, NULL};
static void ui_bar(LxFsmCtx *c) {
    LxVec3 p = lx_fsm_vec3(c, 2);
    float mx = lx_fsm_float(c, 1);
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 5));
    lx_ui_bar(p.x, p.y, lx_fsm_float(c, 3), 28, mx > 0 ? lx_fsm_float(c, 0) / mx : 0, rgba(lx_fsm_color(c, 4)), 0x99000000u);
}
static const LxFsmActionDesc A_UI_BAR = {"UI Bar", "Game UI", "Draws a bar (health, stamina, loading) every frame.", "Bar: {0} of {1}",
    {P("Value", LX_FSM_FLOAT, "50"), P("Max", LX_FSM_FLOAT, "100"), P("Position", LX_FSM_VEC3, "40 100 0"), P("Width", LX_FSM_FLOAT, "400"), P("Color", LX_FSM_COLOR, "0.85 0.2 0.1 1"),
     PENUM("Anchor", "Top Left|Top|Top Right|Left|Center|Right|Bottom Left|Bottom|Bottom Right", "0")}, 0, 0, LX_FSM_EVERY_FRAME, ui_bar, ui_bar, NULL};
static void ui_button(LxFsmCtx *c) {
    LxVec3 p = lx_fsm_vec3(c, 1), sz = lx_fsm_vec3(c, 2);
    lx_ui_anchor((LxUiAnchor)lx_fsm_int(c, 4));
    if (lx_ui_button(lx_fsm_string(c, 0), p.x, p.y, sz.x, sz.y)) ev(c, 3);
}
static const LxFsmActionDesc A_UI_BUTTON = {"UI Button", "Game UI", "Draws a button every frame; sends an event when it's clicked or tapped.", "Button \"{0}\" → {3}",
    {P("Label", LX_FSM_STRING, "Play"), P("Position", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "320 90 0"), PEV("Clicked"),
     PENUM("Anchor", "Top Left|Top|Top Right|Left|Center|Right|Bottom Left|Bottom|Bottom Right", "4")}, 0, 0, LX_FSM_EVERY_FRAME, ui_button, ui_button, NULL};

static void debug_log(LxFsmCtx *c) {
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 0));
    if (lx_fsm_is_none(c, 1)) LX_INFO("[%s] %s", lx_entity_name(c->scene, c->self), text);
    else LX_INFO("[%s] %s %g", lx_entity_name(c->scene, c->self), text, (double)lx_fsm_float(c, 1));
}
static const LxFsmActionDesc A_LOG = {"Log", "Debug", "Writes a line to the Console.", "Log \"{0}\" {1}",
    {P("Text", LX_FSM_STRING, "Hello"), {"Value", LX_FSM_FLOAT, "", LX_FSM_P_OPTIONAL, NULL, NULL}}, 0, 0, LX_FSM_ONCE, debug_log, debug_log, NULL};

/* ======================================================================
 *  registration
 * ====================================================================== */
void lx_fsm_register_builtin_actions(void) {
    static const LxFsmActionDesc *all[] = {
        &A_SEND_EVENT, &A_NEXT_FRAME, &A_RANDOM_EVENT, &A_EVENT_INFO, &A_GOTO,
        &A_WAIT, &A_RANDOM_WAIT, &A_GET_TIME, &A_TIME_SCALE,
        &A_FLOAT_COMPARE, &A_INT_COMPARE, &A_BOOL_TEST, &A_STRING_COMPARE, &A_OBJECT_COMPARE, &A_OBJECT_VALID, &A_BOOL_FLIP, &A_BOOL_ALL,
        &A_set_float, &A_set_int, &A_set_bool, &A_set_string, &A_set_vec3, &A_set_color, &A_set_object, &A_CONVERT, &A_BUILD_STRING,
        &A_FLOAT_ADD, &A_INT_ADD, &A_FLOAT_OP, &A_INT_OP, &A_FLOAT_CLAMP, &A_FLOAT_FN, &A_RANDOM_FLOAT, &A_RANDOM_INT, &A_RANDOM_BOOL, &A_FLOAT_INTERP,
        &A_VEC_OP, &A_VEC_GET, &A_VEC_SET,
        &A_SET_POSITION, &A_GET_POSITION, &A_TRANSLATE, &A_SET_ROTATION, &A_GET_ROTATION, &A_ROTATE, &A_SET_SCALE, &A_GET_SCALE,
        &A_MOVE_TOWARDS, &A_LOOK_AT, &A_MOVE_OBJECT, &A_TWEEN_SCALE, &A_TWEEN_ROT, &A_GET_DISTANCE, &A_SET_PARENT,
        &A_SPAWN, &A_DESTROY, &A_SET_ACTIVE, &A_FIND, &A_GET_CHILD, &A_GET_PARENT, &A_GET_OWNER, &A_HAS_TAG, &A_SET_TAG, &A_SET_NAME, &A_COUNT_TAG,
        &A_SET_MATERIAL, &A_SET_LIGHT,
        &A_ADD_FORCE, &A_SET_VELOCITY, &A_GET_VELOCITY, &A_RAYCAST, &A_COLLISION_EVENT, &A_TRIGGER_EVENT, &A_COLLISION_INFO, &A_CHAR_MOVE,
        &A_PLAY_CLIP, &A_ANIM_SPEED, &A_ANIM_STOP, &A_PLAY_SOUND, &A_AUDIO_PLAY, &A_AUDIO_STOP,
        &A_KEY_EVENT, &A_INPUT_ACTION, &A_GET_AXIS, &A_MOUSE_EVENT,
        &A_LOAD_SCENE, &A_QUIT, &A_SAVE, &A_LOAD, &A_DELETE_SAVE,
        &A_UI_TEXT, &A_UI_BAR, &A_UI_BUTTON, &A_LOG,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
    extern void lx_fsm_register_more_actions(void);
    lx_fsm_register_more_actions();
}
