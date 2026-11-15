/* The graph's own machinery: State Machine, Events, Logic, Flow, Loop, Sequence (waits), Time,
 * Timer, Per Second and Debug.
 *
 * Reaching into another object's graph needs only two actions, not one per variable type: the "Any"
 * parameter takes a variable of any type, so Get Graph Variable and Set Graph Variable cover the lot. */
#include "fsm_actions.h"

/* ==========================================================================================
 *  reaching another object's graph
 * ========================================================================================== */
static int fsm_var_index(LxFsmRun *r, const char *name) {
    if (!r || !name || !name[0]) return -1;
    for (int i = 0; i < r->var_count; i++) if (!strcmp(r->var_names[i], name)) return i;
    return -1;
}
static LxFsmValue *fsm_other_var(LxFsmCtx *c, LxEntity target, int slot, const char *name) {
    LxFsmRun *r = lx_fsm_run_of(c->scene, target, slot);
    int i = fsm_var_index(r, name);
    return i >= 0 ? &r->vars[i] : NULL;
}
/* copy one value into another, converting as best we can */
static void fsm_value_assign(LxFsmValue *dst, const LxFsmValue *src) {
    if (!dst || !src) return;
    switch (dst->type) {
    case LX_FSM_FLOAT:
        dst->f = src->type == LX_FSM_INT || src->type == LX_FSM_ENUM ? (float)src->i
               : src->type == LX_FSM_BOOL ? (src->b ? 1.0f : 0.0f)
               : src->type == LX_FSM_STRING || src->type == LX_FSM_ANY ? (float)atof(src->s) : src->f;
        break;
    case LX_FSM_INT: case LX_FSM_ENUM:
        dst->i = src->type == LX_FSM_FLOAT ? (int)lroundf(src->f)
               : src->type == LX_FSM_BOOL ? (src->b ? 1 : 0)
               : src->type == LX_FSM_STRING || src->type == LX_FSM_ANY ? atoi(src->s) : src->i;
        break;
    case LX_FSM_BOOL:
        dst->b = src->type == LX_FSM_FLOAT ? src->f != 0
               : src->type == LX_FSM_INT || src->type == LX_FSM_ENUM ? src->i != 0 : src->b;
        break;
    case LX_FSM_VEC3: case LX_FSM_VEC2: dst->v = src->v; break;
    case LX_FSM_COLOR: case LX_FSM_RECT: case LX_FSM_QUAT: dst->c = src->c; break;
    case LX_FSM_OBJECT: dst->e = src->e; break;
    default: {
        char buf[LX_REF_MAX];
        lx_fsm_value_format(src, buf, sizeof buf, NULL);
        lx_strcpy(dst->s, sizeof dst->s, buf);
        break;
    }
    }
}

/* ==========================================================================================
 *  State Machine
 * ========================================================================================== */
static void sm_finish_state(LxFsmCtx *c) { fsm_send(c, 0); lx_fsm_finish(c); }
static const LxFsmActionDesc A_SM_FINISH = {"Finish State", "State Machine", "Ends the state at once and sends an event: useful as the last action of a state.",
    "finish -> {0}",
    {PEV("Event")}, 0, 0, LX_FSM_ONCE, sm_finish_state, NULL, NULL};

static void sm_get_state(LxFsmCtx *c) {
    LxEntity t = fsm_obj(c, 0);
    const char *name = lx_fsm_current_state(c->scene, t, lx_fsm_int(c, 1));
    lx_fsm_set_string(c, 2, name);
    lx_fsm_set_bool(c, 3, name && name[0]);
}
static const LxFsmActionDesc A_SM_GET_STATE = {"Get Graph State", "State Machine", "The name of the state an object's graph is in right now.",
    "state of {0} -> {2}",
    {POBJ("Object"), PT("Graph Slot", LX_FSM_INT, "0", "which State Graph on the object (0 is the first)"),
     POUT("Store State Name", LX_FSM_STRING), POPT("Store Is Running", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, sm_get_state, sm_get_state, NULL};

static void sm_check_state(LxFsmCtx *c) {
    const char *name = lx_fsm_current_state(c->scene, fsm_obj(c, 0), lx_fsm_int(c, 1));
    bool match = !fsm_stricmp(name ? name : "", lx_fsm_string(c, 2));
    lx_fsm_set_bool(c, 3, match);
    fsm_send(c, match ? 4 : 5);
}
static const LxFsmActionDesc A_SM_CHECK_STATE = {"Check Graph State", "State Machine", "Tests whether an object's graph is in a particular state.",
    "{0} in \"{2}\"?",
    {POBJ("Object"), P("Graph Slot", LX_FSM_INT, "0"), P("State Name", LX_FSM_STRING, ""),
     POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, sm_check_state, sm_check_state, NULL};

static void sm_prev_state(LxFsmCtx *c) {
    LxFsmRun *r = c->run;
    const char *name = "";
    if (r && r->graph && r->prev_state >= 0 && r->prev_state < r->graph->state_count)
        name = r->graph->states[r->prev_state].name;
    lx_fsm_set_string(c, 0, name);
    lx_fsm_set_string(c, 1, r ? r->last_event : "");
}
static const LxFsmActionDesc A_SM_PREV_STATE = {"Get Previous State", "State Machine", "The state this graph was in before this one, and the event that left it.",
    "previous state -> {0}",
    {POUT("Store State Name", LX_FSM_STRING), POPT("Store Event Name", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, sm_prev_state, sm_prev_state, NULL};

static void sm_get_var(LxFsmCtx *c) {
    LxFsmValue *src = fsm_other_var(c, fsm_obj(c, 0), lx_fsm_int(c, 1), lx_fsm_string(c, 2));
    if (!src) { lx_fsm_set_bool(c, 4, false); fsm_send(c, 5); return; }
    LxFsmValue *dst = lx_fsm_param_value(c, 3);
    fsm_value_assign(dst, src);
    lx_fsm_set_bool(c, 4, true);
}
static const LxFsmActionDesc A_SM_GET_VAR = {"Get Graph Variable", "State Machine", "Reads a variable out of another object's graph. Works for every variable type.",
    "{0}.{2} -> {3}",
    {POBJ("Object"), P("Graph Slot", LX_FSM_INT, "0"), P("Variable Name", LX_FSM_STRING, ""),
     POUT("Store Value", LX_FSM_ANY), POPT("Store Found", LX_FSM_BOOL), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, sm_get_var, sm_get_var, NULL};

static void sm_set_var(LxFsmCtx *c) {
    LxFsmValue *dst = fsm_other_var(c, fsm_obj(c, 0), lx_fsm_int(c, 1), lx_fsm_string(c, 2));
    if (!dst) { lx_fsm_set_bool(c, 4, false); fsm_send(c, 5); return; }
    fsm_value_assign(dst, lx_fsm_param_value(c, 3));
    lx_fsm_set_bool(c, 4, true);
}
static const LxFsmActionDesc A_SM_SET_VAR = {"Set Graph Variable", "State Machine", "Writes a variable in another object's graph. Works for every variable type.",
    "{0}.{2} = {3}",
    {POBJ("Object"), P("Graph Slot", LX_FSM_INT, "0"), P("Variable Name", LX_FSM_STRING, ""),
     P("Value", LX_FSM_ANY, ""), POPT("Store Found", LX_FSM_BOOL), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, sm_set_var, sm_set_var, NULL};

static void sm_get_global(LxFsmCtx *c) {
    int i = lx_fsm_globals_index(lx_fsm_string(c, 0));
    LxFsmValue *src = i >= 0 ? lx_fsm_global_value(i) : NULL;
    if (!src) { lx_fsm_set_bool(c, 2, false); fsm_send(c, 3); return; }
    fsm_value_assign(lx_fsm_param_value(c, 1), src);
    lx_fsm_set_bool(c, 2, true);
}
static const LxFsmActionDesc A_SM_GET_GLOBAL = {"Get Global Variable", "State Machine", "Reads one of the project's global variables (Assets/Globals).",
    "global {0} -> {1}",
    {P("Variable Name", LX_FSM_STRING, ""), POUT("Store Value", LX_FSM_ANY), POPT("Store Found", LX_FSM_BOOL), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, sm_get_global, sm_get_global, NULL};

static void sm_set_global(LxFsmCtx *c) {
    int i = lx_fsm_globals_index(lx_fsm_string(c, 0));
    LxFsmValue *dst = i >= 0 ? lx_fsm_global_value(i) : NULL;
    if (!dst) { lx_fsm_set_bool(c, 2, false); fsm_send(c, 3); return; }
    fsm_value_assign(dst, lx_fsm_param_value(c, 1));
    lx_fsm_set_bool(c, 2, true);
}
static const LxFsmActionDesc A_SM_SET_GLOBAL = {"Set Global Variable", "State Machine", "Writes one of the project's global variables (Assets/Globals).",
    "global {0} = {1}",
    {P("Variable Name", LX_FSM_STRING, ""), P("Value", LX_FSM_ANY, ""), POPT("Store Found", LX_FSM_BOOL), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, sm_set_global, sm_set_global, NULL};

static void sm_state_time(LxFsmCtx *c) {
    lx_fsm_set_float(c, 0, lx_fsm_state_time(c));
    LxFsmRun *r = c->run;
    lx_fsm_set_string(c, 1, r ? r->last_event : "");
}
static const LxFsmActionDesc A_SM_STATE_TIME = {"Get State Time", "State Machine", "How long this graph has been in the current state.",
    "state time -> {0}",
    {POUT("Store Seconds", LX_FSM_FLOAT), POPT("Store Entry Event", LX_FSM_STRING)},
    0, 0, LX_FSM_EVERY_FRAME, sm_state_time, sm_state_time, NULL};

/* ==========================================================================================
 *  Events
 * ========================================================================================== */
typedef struct { float t; } FsmTimer;

static void ev_send_now(LxFsmCtx *c) {
    char e[LX_FSM_NAME];
    lx_strcpy(e, sizeof e, lx_fsm_event(c, 0));
    if (!e[0] || !strcmp(e, "None")) return;
    float value = lx_fsm_float(c, 5);
    switch (lx_fsm_int(c, 1)) {
    case 0: lx_fsm_send(c, e); break;
    case 1: { LxEntity t = fsm_obj(c, 2); if (t) lx_send_message(c->scene, t, e, value, c->self); break; }
    case 2: {
        /* collect the children first: a child's graph may change the hierarchy as it reacts */
        LxEntity t = fsm_obj(c, 2), kids[256];
        int n = 0;
        for (LxEntity k = t ? lx_entity_first_child(c->scene, t) : 0; k && n < 256; k = lx_entity_next_sibling(c->scene, k)) kids[n++] = k;
        for (int i = 0; i < n; i++) if (lx_entity_valid(c->scene, kids[i])) lx_send_message(c->scene, kids[i], e, value, c->self);
        break;
    }
    case 3: { char tag[LX_NAME_MAX]; lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 3)); lx_send_message_to_tag(c->scene, tag, e, value, c->self); break; }
    default: lx_broadcast(c->scene, e, value, c->self); break;
    }
}
static void ev_send_enter(LxFsmCtx *c) { if (lx_fsm_float(c, 4) <= 0) { ev_send_now(c); lx_fsm_finish(c); } }
static void ev_send_update(LxFsmCtx *c) {
    FsmTimer *t = c->state;
    t->t += c->dt;
    if (t->t >= lx_fsm_float(c, 4)) { lx_fsm_finish(c); ev_send_now(c); }
}
static const LxFsmActionDesc A_EV_SEND = {"Send Event", "Events", "Sends an event to this graph, another object, its children, everything with a tag, or everyone.",
    "send {0} to {1}",
    {PEV("Event"), PENUM("Send To", "This Graph|Object|Children|Tag|Everyone", "0"), POBJ("Object"), P("Tag", LX_FSM_STRING, "Player"),
     PT("Delay", LX_FSM_FLOAT, "0", "seconds; 0 = now. Leaving the state before then cancels it"),
     PT("Value", LX_FSM_FLOAT, "0", "a number carried with the event (read it with Get Event Info)")},
    sizeof(FsmTimer), LX_FSM_A_CONTINUOUS, LX_FSM_ONCE, ev_send_enter, ev_send_update, NULL};

static void ev_send_by_name(LxFsmCtx *c) {
    const char *name = lx_fsm_string(c, 0);
    if (!name || !name[0]) return;
    LxEntity t = fsm_obj(c, 1);
    if (lx_fsm_bool(c, 3)) lx_fsm_broadcast_event(c->scene, name, lx_fsm_float(c, 2), c->self);
    else if (t) lx_fsm_send_event(c->scene, t, name, lx_fsm_float(c, 2), c->self);
    else lx_fsm_send(c, name);
}
static const LxFsmActionDesc A_EV_SEND_BY_NAME = {"Send Event By Name", "Events", "Sends an event whose name is built at run time, rather than picked in the editor.",
    "send \"{0}\"",
    {P("Event Name", LX_FSM_STRING, ""), POBJN("Object"), P("Value", LX_FSM_FLOAT, "0"),
     PT("Everyone", LX_FSM_BOOL, "false", "send it to every object in the scene")},
    0, 0, LX_FSM_ONCE, ev_send_by_name, ev_send_by_name, NULL};

static void ev_next_frame(LxFsmCtx *c) { fsm_send(c, 0); lx_fsm_finish(c); }
static const LxFsmActionDesc A_EV_NEXT_FRAME = {"Next Frame Event", "Events", "Sends an event on the next frame rather than this one.", "next frame: {0}",
    {PEV("Event")}, 0, LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, ev_next_frame, NULL};

static void ev_random(LxFsmCtx *c) {
    int idx[4], n = 0;
    float w[4], total = 0;
    for (int i = 0; i < 4; i++) {
        if (!lx_fsm_event(c, i)[0]) continue;
        idx[n] = i;
        w[n] = lx_maxf(0, lx_fsm_float(c, 4 + i));
        total += w[n];
        n++;
    }
    if (n == 0) return;
    if (total <= 0) { fsm_send(c, idx[lx_random_int(0, n - 1)]); return; }
    float r = lx_random_range(0, total), acc = 0;
    for (int i = 0; i < n; i++) {
        acc += w[i];
        if (r <= acc) { fsm_send(c, idx[i]); return; }
    }
    fsm_send(c, idx[n - 1]);
}
static const LxFsmActionDesc A_EV_RANDOM = {"Random Event", "Events", "Sends one of up to four events, picked at random. Each has its own weight.",
    "randomly: {0}, {1}, {2} or {3}",
    {PEV("Event 1"), PEV("Event 2"), PEV("Event 3"), PEV("Event 4"),
     P("Weight 1", LX_FSM_FLOAT, "1"), P("Weight 2", LX_FSM_FLOAT, "1"), P("Weight 3", LX_FSM_FLOAT, "1"), P("Weight 4", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, ev_random, ev_random, NULL};

static void ev_info(LxFsmCtx *c) {
    lx_fsm_set_float(c, 0, lx_fsm_event_value(c));
    lx_fsm_set_object(c, 1, lx_fsm_event_sender(c));
    lx_fsm_set_object(c, 2, lx_fsm_event_other(c));
    lx_fsm_set_string(c, 3, c->run ? c->run->last_event : "");
}
static const LxFsmActionDesc A_EV_INFO = {"Get Event Info", "Events", "What came with the event that entered this state: its value, who sent it, and the other object of a collision.",
    "event info",
    {POPT("Store Value", LX_FSM_FLOAT), POPT("Store Sender", LX_FSM_OBJECT), POPT("Store Other Object", LX_FSM_OBJECT),
     POPT("Store Event Name", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, ev_info, ev_info, NULL};

/* ==========================================================================================
 *  Flow: switches and thresholds
 * ========================================================================================== */
static void fl_int_switch(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0);
    for (int i = 0; i < 5; i++) {
        if (lx_fsm_is_none(c, 1 + i * 2)) continue;
        if (lx_fsm_int(c, 1 + i * 2) == v) { fsm_send(c, 2 + i * 2); return; }
    }
    fsm_send(c, 11);
}
static const LxFsmActionDesc A_FL_INT_SWITCH = {"Int Switch", "Flow", "Sends a different event for each of up to five values of a whole number.",
    "switch on {0}",
    {P("Int Variable", LX_FSM_INT, "0"),
     PIN_OPT("Case 0", LX_FSM_INT), PEV("Event 0"), PIN_OPT("Case 1", LX_FSM_INT), PEV("Event 1"),
     PIN_OPT("Case 2", LX_FSM_INT), PEV("Event 2"), PIN_OPT("Case 3", LX_FSM_INT), PEV("Event 3"),
     PIN_OPT("Case 4", LX_FSM_INT), PEV("Event 4"), PEV("Default Event")},
    0, 0, LX_FSM_ONCE, fl_int_switch, fl_int_switch, NULL};

static void fl_string_switch(LxFsmCtx *c) {
    const char *v = lx_fsm_string(c, 0);
    bool ignore = lx_fsm_bool(c, 11);
    for (int i = 0; i < 5; i++) {
        if (lx_fsm_is_none(c, 1 + i * 2)) continue;
        const char *k = lx_fsm_string(c, 1 + i * 2);
        if ((ignore ? fsm_stricmp(v, k) : strcmp(v, k)) == 0) { fsm_send(c, 2 + i * 2); return; }
    }
    fsm_send(c, 12);
}
static const LxFsmActionDesc A_FL_STRING_SWITCH = {"String Switch", "Flow", "Sends a different event for each of up to five pieces of text.",
    "switch on \"{0}\"",
    {P("String Variable", LX_FSM_STRING, ""),
     PIN_OPT("Case 0", LX_FSM_STRING), PEV("Event 0"), PIN_OPT("Case 1", LX_FSM_STRING), PEV("Event 1"),
     PIN_OPT("Case 2", LX_FSM_STRING), PEV("Event 2"), PIN_OPT("Case 3", LX_FSM_STRING), PEV("Event 3"),
     PIN_OPT("Case 4", LX_FSM_STRING), PEV("Event 4"), P("Ignore Case", LX_FSM_BOOL, "false"), PEV("Default Event")},
    0, 0, LX_FSM_ONCE, fl_string_switch, fl_string_switch, NULL};

static void fl_float_switch(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    /* thresholds in order: the first the value is below wins */
    for (int i = 0; i < 4; i++) {
        if (lx_fsm_is_none(c, 1 + i * 2)) continue;
        if (v < lx_fsm_float(c, 1 + i * 2)) { fsm_send(c, 2 + i * 2); return; }
    }
    fsm_send(c, 9);
}
static const LxFsmActionDesc A_FL_FLOAT_SWITCH = {"Float Switch", "Flow", "Sends an event for the first threshold a number falls below: health bands, difficulty tiers.",
    "bands of {0}",
    {P("Float Variable", LX_FSM_FLOAT, "0"),
     PIN_OPT("Below 0", LX_FSM_FLOAT), PEV("Event 0"), PIN_OPT("Below 1", LX_FSM_FLOAT), PEV("Event 1"),
     PIN_OPT("Below 2", LX_FSM_FLOAT), PEV("Event 2"), PIN_OPT("Below 3", LX_FSM_FLOAT), PEV("Event 3"),
     PEV("Otherwise Event")},
    0, 0, LX_FSM_ONCE, fl_float_switch, fl_float_switch, NULL};

static void fl_tag_switch(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    const char *tag = e ? lx_entity_tag(c->scene, e) : "";
    for (int i = 0; i < 4; i++) {
        if (lx_fsm_is_none(c, 1 + i * 2)) continue;
        if (!fsm_stricmp(tag, lx_fsm_string(c, 1 + i * 2))) { fsm_send(c, 2 + i * 2); return; }
    }
    fsm_send(c, 9);
}
static const LxFsmActionDesc A_FL_TAG_SWITCH = {"Tag Switch", "Flow", "Sends a different event depending on an object's tag.", "switch on tag of {0}",
    {POBJ("Object"),
     PIN_OPT("Tag 0", LX_FSM_STRING), PEV("Event 0"), PIN_OPT("Tag 1", LX_FSM_STRING), PEV("Event 1"),
     PIN_OPT("Tag 2", LX_FSM_STRING), PEV("Event 2"), PIN_OPT("Tag 3", LX_FSM_STRING), PEV("Event 3"),
     PEV("Default Event")},
    0, 0, LX_FSM_ONCE, fl_tag_switch, fl_tag_switch, NULL};

typedef struct { int side; bool started; } FsmThreshold;
static void fl_threshold(LxFsmCtx *c) {
    FsmThreshold *s = c->state;
    float v = lx_fsm_float(c, 0), th = lx_fsm_float(c, 1);
    int side = v > th ? 1 : (v < th ? -1 : 0);
    if (!s->started) { s->started = true; s->side = side; return; }   /* the first frame only reads it */
    if (side != s->side) {
        if (side > 0) fsm_send(c, 2);
        else if (side < 0) fsm_send(c, 3);
        s->side = side;
    }
}
static const LxFsmActionDesc A_FL_THRESHOLD = {"Float Threshold Event", "Flow", "Watches a number and sends an event the moment it crosses a threshold, either way.",
    "{0} crosses {1}",
    {P("Float Variable", LX_FSM_FLOAT, "0"), P("Threshold", LX_FSM_FLOAT, "0"),
     PEV("Rose Above Event"), PEV("Fell Below Event")},
    sizeof(FsmThreshold), 0, LX_FSM_EVERY_FRAME, fl_threshold, fl_threshold, NULL};

typedef struct { bool last, started; } FsmBoolWatch;
static void fl_bool_changed(LxFsmCtx *c) {
    FsmBoolWatch *s = c->state;
    bool v = lx_fsm_bool(c, 0);
    if (!s->started) { s->started = true; s->last = v; return; }
    if (v != s->last) {
        s->last = v;
        fsm_send(c, v ? 1 : 2);
        fsm_send(c, 3);
    }
}
static const LxFsmActionDesc A_FL_BOOL_CHANGED = {"Bool Changed Event", "Flow", "Sends an event the moment a bool changes.", "{0} changes",
    {P("Bool Variable", LX_FSM_BOOL, "false"), PEV("Became True Event"), PEV("Became False Event"), PEV("Changed Event")},
    sizeof(FsmBoolWatch), 0, LX_FSM_EVERY_FRAME, fl_bool_changed, fl_bool_changed, NULL};

/* ==========================================================================================
 *  Logic: spatial checks
 * ========================================================================================== */
static void lg_check_distance(LxFsmCtx *c) {
    LxEntity a = fsm_obj(c, 0), b = fsm_obj(c, 1);
    if (!a) { fsm_send(c, 6); return; }
    LxVec3 pa = lx_entity_world_position(c->scene, a);
    LxVec3 pb = b ? lx_entity_world_position(c->scene, b) : lx_fsm_vec3(c, 2);
    LxVec3 d = lx_sub3(pb, pa);
    if (lx_fsm_bool(c, 3)) d.y = 0;
    float dist = lx_len3(d);
    lx_fsm_set_float(c, 5, dist);
    bool within = dist <= lx_fsm_float(c, 4);
    fsm_send(c, within ? 6 : 7);
    lx_fsm_set_bool(c, 8, within);
}
static const LxFsmActionDesc A_LG_DISTANCE = {"Check Distance", "Logic", "How far apart two objects (or an object and a point) are, and whether that is within a range.",
    "{0} to {1} within {4}?",
    {POBJ("Object"), POBJN("Target Object"), PT("Target Position", LX_FSM_VEC3, "0 0 0", "used when Target Object is None"),
     PT("Ignore Height", LX_FSM_BOOL, "false", "measure along the ground only"), P("Within Distance", LX_FSM_FLOAT, "5"),
     POPT("Store Distance", LX_FSM_FLOAT), PEV("In Range Event"), PEV("Out Of Range Event"), POPT("Store In Range", LX_FSM_BOOL)},
    0, 0, LX_FSM_EVERY_FRAME, lg_check_distance, lg_check_distance, NULL};

static void lg_check_angle(LxFsmCtx *c) {
    LxEntity a = fsm_obj(c, 0), b = fsm_obj(c, 1);
    if (!a || !b) { fsm_send(c, 5); return; }
    LxVec3 target = lx_entity_world_position(c->scene, b);
    float ang = lx_entity_angle_to(c->scene, a, target, lx_fsm_bool(c, 2));
    lx_fsm_set_float(c, 3, ang);
    bool within = fabsf(ang) <= lx_fsm_float(c, 4) * 0.5f;
    lx_fsm_set_bool(c, 6, within);
    fsm_send(c, within ? 5 : 7);
}
static const LxFsmActionDesc A_LG_ANGLE = {"Check Angle To Target", "Logic", "How far an object must turn to face another, and whether it is already within a view cone.",
    "{0} facing {1}?",
    {POBJ("Object"), POBJ("Target"), PT("Flat", LX_FSM_BOOL, "true", "ignore the height difference"),
     POPT("Store Angle", LX_FSM_FLOAT), PT("View Angle", LX_FSM_FLOAT, "60", "the full width of the cone, in degrees"),
     PEV("Facing Event"), POPT("Store Facing", LX_FSM_BOOL), PEV("Not Facing Event")},
    0, 0, LX_FSM_EVERY_FRAME, lg_check_angle, lg_check_angle, NULL};

/* ==========================================================================================
 *  Loop
 * ========================================================================================== */
typedef struct { int i; bool started; } FsmLoop;
static void lp_loop(LxFsmCtx *c) {
    FsmLoop *l = c->state;
    int times = lx_fsm_int(c, 0);
    if (!l->started) { l->started = true; l->i = 0; }
    lx_fsm_set_int(c, 1, l->i);
    l->i++;
    if (times > 0 && l->i >= times) { l->i = 0; fsm_send(c, 3); }
    else fsm_send(c, 2);
}
static const LxFsmActionDesc A_LP_LOOP = {"Loop", "Loop", "Counts how many times the state has run: sends Loop Event until the count is reached, then Finished.",
    "loop {0} times",
    {PT("Repeat", LX_FSM_INT, "3", "0 = forever"), POPT("Store Index", LX_FSM_INT), PEV("Loop Event"), PEV("Finished Event")},
    sizeof(FsmLoop), 0, LX_FSM_ONCE, lp_loop, lp_loop, NULL};

static void lp_for_each_child(LxFsmCtx *c) {
    FsmLoop *l = c->state;
    LxEntity parent = fsm_obj(c, 0);
    if (!parent) { fsm_send(c, 4); return; }
    LxEntity kids[256];
    int n = 0;
    for (LxEntity k = lx_entity_first_child(c->scene, parent); k && n < 256; k = lx_entity_next_sibling(c->scene, k)) kids[n++] = k;
    if (!l->started) { l->started = true; l->i = 0; }
    if (l->i >= n) { l->i = 0; fsm_send(c, 4); return; }
    lx_fsm_set_object(c, 1, kids[l->i]);
    lx_fsm_set_int(c, 2, l->i);
    l->i++;
    fsm_send(c, 3);
}
static const LxFsmActionDesc A_LP_FOR_EACH_CHILD = {"For Each Child", "Loop", "Hands you one child per run, walking through all of them, then sends Finished.",
    "each child of {0}",
    {POBJ("Parent"), POUT("Store Child", LX_FSM_OBJECT), POPT("Store Index", LX_FSM_INT), PEV("Next Event"), PEV("Finished Event")},
    sizeof(FsmLoop), 0, LX_FSM_ONCE, lp_for_each_child, lp_for_each_child, NULL};

static void lp_for_each_item(LxFsmCtx *c) {
    FsmLoop *l = c->state;
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    if (!l->started) { l->started = true; l->i = 0; }
    if (l->i >= n) { l->i = 0; fsm_send(c, 4); return; }
    lx_fsm_set_from_array(c, 1, a, l->i);
    lx_fsm_set_int(c, 2, l->i);
    l->i++;
    fsm_send(c, 3);
}
static const LxFsmActionDesc A_LP_FOR_EACH_ITEM = {"For Each List Item", "Loop", "Hands you one list item per run, walking through the whole list, then sends Finished.",
    "each item of {0}",
    {PARR("List"), POUT("Store Value", LX_FSM_ANY), POPT("Store Index", LX_FSM_INT), PEV("Next Event"), PEV("Finished Event")},
    sizeof(FsmLoop), 0, LX_FSM_ONCE, lp_for_each_item, lp_for_each_item, NULL};

static void lp_for_each_tag(LxFsmCtx *c) {
    FsmLoop *l = c->state;
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 0));
    LxEntity found[256];
    int n = lx_entities_with_tag(c->scene, tag, found, 256);
    if (!l->started) { l->started = true; l->i = 0; }
    if (l->i >= n) { l->i = 0; fsm_send(c, 4); return; }
    lx_fsm_set_object(c, 1, found[l->i]);
    lx_fsm_set_int(c, 2, l->i);
    l->i++;
    fsm_send(c, 3);
}
static const LxFsmActionDesc A_LP_FOR_EACH_TAG = {"For Each With Tag", "Loop", "Hands you one tagged object per run, walking through all of them, then sends Finished.",
    "each \"{0}\"",
    {P("Tag", LX_FSM_STRING, "Enemy"), POUT("Store Object", LX_FSM_OBJECT), POPT("Store Index", LX_FSM_INT),
     PEV("Next Event"), PEV("Finished Event")},
    sizeof(FsmLoop), 0, LX_FSM_ONCE, lp_for_each_tag, lp_for_each_tag, NULL};

/* ==========================================================================================
 *  Sequence: waiting
 * ========================================================================================== */
static void sq_wait_enter(LxFsmCtx *c) { if (lx_fsm_float(c, 0) <= 0) { lx_fsm_finish(c); fsm_send(c, 1); } }
static void sq_wait_update(LxFsmCtx *c) {
    FsmTimer *t = c->state;
    t->t += lx_fsm_bool(c, 2) ? lx_delta_unscaled(c->scene) : c->dt;
    lx_fsm_set_float(c, 3, t->t);
    float total = lx_fsm_float(c, 0);
    lx_fsm_set_float(c, 4, total > 0 ? lx_saturate(t->t / total) : 1.0f);
    if (t->t >= total) { lx_fsm_finish(c); fsm_send(c, 1); }
}
static const LxFsmActionDesc A_SQ_WAIT = {"Wait", "Sequence", "Waits a number of seconds, then finishes and sends an event.", "wait {0}s, then {1}",
    {P("Time", LX_FSM_FLOAT, "1"), PEV("Finish Event"),
     PT("Real Time", LX_FSM_BOOL, "false", "ignore the time scale, so it still runs while the game is paused"),
     POPT("Store Elapsed", LX_FSM_FLOAT), POPT("Store Progress", LX_FSM_FLOAT)},
    sizeof(FsmTimer), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, sq_wait_enter, sq_wait_update, NULL};

typedef struct { float t, len; bool started; } FsmRandWait;
static void sq_random_wait(LxFsmCtx *c) {
    FsmRandWait *w = c->state;
    if (!w->started) { w->started = true; w->len = lx_random_range(lx_fsm_float(c, 0), lx_fsm_float(c, 1)); }
    w->t += c->dt;
    if (w->t >= w->len) { lx_fsm_finish(c); fsm_send(c, 2); }
}
static const LxFsmActionDesc A_SQ_RANDOM_WAIT = {"Random Wait", "Sequence", "Waits a random time between Min and Max, then finishes.", "wait {0}-{1}s, then {2}",
    {P("Min", LX_FSM_FLOAT, "0.5"), P("Max", LX_FSM_FLOAT, "2"), PEV("Finish Event")},
    sizeof(FsmRandWait), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, sq_random_wait, NULL};

typedef struct { int n; } FsmFrames;
static void sq_wait_frames(LxFsmCtx *c) {
    FsmFrames *f = c->state;
    if (++f->n >= lx_fsm_int(c, 0)) { lx_fsm_finish(c); fsm_send(c, 1); }
}
static const LxFsmActionDesc A_SQ_WAIT_FRAMES = {"Wait For Frames", "Sequence", "Waits a number of frames rather than a length of time.", "wait {0} frames",
    {P("Frames", LX_FSM_INT, "1"), PEV("Finish Event")},
    sizeof(FsmFrames), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, sq_wait_frames, NULL};

static void sq_wait_bool(LxFsmCtx *c) {
    if (lx_fsm_bool(c, 0) == lx_fsm_bool(c, 1)) { lx_fsm_finish(c); fsm_send(c, 2); }
    else if (lx_fsm_float(c, 3) > 0 && lx_fsm_state_time(c) >= lx_fsm_float(c, 3)) { lx_fsm_finish(c); fsm_send(c, 4); }
}
static const LxFsmActionDesc A_SQ_WAIT_BOOL = {"Wait For Bool", "Sequence", "Holds the state until a bool reaches the value you want, with an optional time limit.",
    "wait until {0} == {1}",
    {P("Bool Variable", LX_FSM_BOOL, "false"), P("Wait For", LX_FSM_BOOL, "true"), PEV("Finish Event"),
     PT("Timeout", LX_FSM_FLOAT, "0", "0 = wait forever"), PEV("Timeout Event")},
    0, LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, sq_wait_bool, NULL};

/* ==========================================================================================
 *  Timer and Per Second
 * ========================================================================================== */
typedef struct { float t; bool fired; } FsmCountdown;
static void tm_countdown(LxFsmCtx *c) {
    FsmCountdown *s = c->state;
    float total = lx_fsm_float(c, 0);
    if (!lx_fsm_bool(c, 5)) s->t += lx_fsm_bool(c, 1) ? lx_delta_unscaled(c->scene) : c->dt;
    float left = lx_maxf(0, total - s->t);
    lx_fsm_set_float(c, 2, left);
    lx_fsm_set_float(c, 3, total > 0 ? lx_saturate(s->t / total) : 1.0f);
    if (left <= 0 && !s->fired) { s->fired = true; fsm_send(c, 4); }
}
static const LxFsmActionDesc A_TM_COUNTDOWN = {"Countdown Timer", "Timer", "Counts down and reports the time left and the progress; sends an event when it reaches zero.",
    "countdown {0}s",
    {P("Duration", LX_FSM_FLOAT, "10"), PT("Real Time", LX_FSM_BOOL, "false", "ignore the time scale"),
     POUT("Store Time Left", LX_FSM_FLOAT), POPT("Store Progress", LX_FSM_FLOAT), PEV("Finish Event"),
     PT("Paused", LX_FSM_BOOL, "false", "hold the countdown where it is")},
    sizeof(FsmCountdown), 0, LX_FSM_EVERY_FRAME, tm_countdown, tm_countdown, NULL};

typedef struct { float t; } FsmTick;
static void tm_tick(LxFsmCtx *c) {
    FsmTick *s = c->state;
    float interval = lx_maxf(0.0001f, lx_fsm_float(c, 0));
    s->t += c->dt;
    int fired = 0;
    while (s->t >= interval && fired < 16) { s->t -= interval; fired++; }
    if (fired > 0) {
        lx_fsm_set_int(c, 2, fired);
        fsm_send(c, 1);
    }
}
static const LxFsmActionDesc A_TM_TICK = {"Tick Every", "Timer", "Sends an event at a steady interval for as long as the state runs: damage over time, spawn waves.",
    "every {0}s: {1}",
    {P("Interval", LX_FSM_FLOAT, "1"), PEV("Tick Event"), POPT("Store Ticks This Frame", LX_FSM_INT)},
    sizeof(FsmTick), 0, LX_FSM_EVERY_FRAME, tm_tick, tm_tick, NULL};

static void ps_per_second(LxFsmCtx *c) {
    lx_fsm_set_float(c, 1, lx_fsm_float(c, 0) * c->dt);
}
static const LxFsmActionDesc A_PS_FLOAT = {"Float Per Second", "Timer", "Turns a rate into this frame's share of it, so the result is the same whatever the frame rate.",
    "{0}/s -> {1}",
    {P("Rate", LX_FSM_FLOAT, "1"), POUT("Store This Frame", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, ps_per_second, ps_per_second, NULL};

static void ps_add_per_second(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0) + lx_fsm_float(c, 1) * c->dt;
    if (!lx_fsm_is_none(c, 2) || !lx_fsm_is_none(c, 3)) v = lx_clampf(v, lx_fsm_float(c, 2), lx_fsm_float(c, 3));
    lx_fsm_set_float(c, 0, v);
}
static const LxFsmActionDesc A_PS_ADD = {"Add Per Second", "Timer", "Adds to a Float variable at a steady rate per second: regenerating health, draining stamina.",
    "{0} += {1}/s",
    {POUT("Float Variable", LX_FSM_FLOAT), P("Rate", LX_FSM_FLOAT, "1"), PIN_OPT("Min", LX_FSM_FLOAT), PIN_OPT("Max", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, ps_add_per_second, ps_add_per_second, NULL};

/* ==========================================================================================
 *  Time
 * ========================================================================================== */
static void t_get_time(LxFsmCtx *c) {
    lx_fsm_set_float(c, 0, lx_game_time(c->scene));
    lx_fsm_set_float(c, 1, c->dt);
    lx_fsm_set_float(c, 2, lx_delta_unscaled(c->scene));
    lx_fsm_set_float(c, 3, lx_time_scale());
    lx_fsm_set_float(c, 4, c->dt > 0 ? 1.0f / c->dt : 0);
}
static const LxFsmActionDesc A_T_GET_TIME = {"Get Time Info", "Time", "The game time, the frame time, the time scale and the current frame rate.",
    "time info",
    {POPT("Store Game Time", LX_FSM_FLOAT), POPT("Store Delta Time", LX_FSM_FLOAT), POPT("Store Unscaled Delta", LX_FSM_FLOAT),
     POPT("Store Time Scale", LX_FSM_FLOAT), POPT("Store FPS", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, t_get_time, t_get_time, NULL};

static void t_set_scale(LxFsmCtx *c) { lx_time_set_scale(lx_clampf(lx_fsm_float(c, 0), 0, 100)); }
static const LxFsmActionDesc A_T_SET_SCALE = {"Set Time Scale", "Time", "Speeds the game up or slows it down. 0 pauses it; 1 is normal.", "time scale {0}",
    {P("Time Scale", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, t_set_scale, t_set_scale, NULL};

typedef struct { float t; bool on; } FsmSlowMo;
static void t_slowmo_update(LxFsmCtx *c) {
    FsmSlowMo *s = c->state;
    if (!s->on) { s->on = true; lx_time_set_scale(lx_clampf(lx_fsm_float(c, 0), 0, 4)); }
    s->t += lx_delta_unscaled(c->scene);
    float len = lx_fsm_float(c, 1);
    if (len > 0 && s->t >= len) { lx_time_set_scale(1); s->on = false; lx_fsm_finish(c); fsm_send(c, 2); }
}
static void t_slowmo_exit(LxFsmCtx *c) { FsmSlowMo *s = c->state; if (s->on) lx_time_set_scale(1); }
static const LxFsmActionDesc A_T_SLOWMO = {"Slow Motion", "Time", "Slows the game for a while in real seconds, then puts it back: bullet time, hit pauses.",
    "slow to {0} for {1}s",
    {P("Time Scale", LX_FSM_FLOAT, "0.3"), PT("Real Seconds", LX_FSM_FLOAT, "1", "0 = until the state ends"), PEV("Finish Event")},
    sizeof(FsmSlowMo), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, t_slowmo_update, t_slowmo_exit};

static void t_pause(LxFsmCtx *c) { lx_time_set_scale(lx_fsm_bool(c, 0) ? 0.0f : 1.0f); }
static const LxFsmActionDesc A_T_PAUSE = {"Pause Game", "Time", "Freezes or unfreezes the game by setting the time scale.", "paused: {0}",
    {P("Paused", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_ONCE, t_pause, t_pause, NULL};

static void t_date(LxFsmCtx *c) {
    int d[7];
    lx_date_time(d);
    lx_fsm_set_int(c, 0, d[0]);
    lx_fsm_set_int(c, 1, d[1]);
    lx_fsm_set_int(c, 2, d[2]);
    lx_fsm_set_int(c, 3, d[3]);
    lx_fsm_set_int(c, 4, d[4]);
    lx_fsm_set_int(c, 5, d[5]);
    lx_fsm_set_int(c, 6, d[6]);
}
static const LxFsmActionDesc A_T_DATE = {"Get Date Time", "Time", "The real-world date and time from the player's computer.", "date and time",
    {POPT("Store Year", LX_FSM_INT), POPT("Store Month", LX_FSM_INT), POPT("Store Day", LX_FSM_INT),
     POPT("Store Hour", LX_FSM_INT), POPT("Store Minute", LX_FSM_INT), POPT("Store Second", LX_FSM_INT),
     POPTT("Store Weekday", LX_FSM_INT, "0 is Sunday")},
    0, 0, LX_FSM_ONCE, t_date, t_date, NULL};

/* ==========================================================================================
 *  Debug
 * ========================================================================================== */
static void dbg_log(LxFsmCtx *c) {
    const char *text = lx_fsm_string(c, 0);
    char extra[LX_REF_MAX];
    extra[0] = 0;
    if (!lx_fsm_is_none(c, 1)) {
        LxFsmValue *v = lx_fsm_param_value(c, 1);
        if (v) lx_fsm_value_format(v, extra, sizeof extra, NULL);
    }
    const char *who = lx_entity_name(c->scene, c->self);
    switch (lx_fsm_int(c, 2)) {
    case 1: LX_WARN("[%s] %s %s", who, text, extra); break;
    case 2: LX_ERROR("[%s] %s %s", who, text, extra); break;
    default: LX_INFO("[%s] %s %s", who, text, extra); break;
    }
}
static const LxFsmActionDesc A_DBG_LOG = {"Log", "Debug", "Writes a line to the Console, optionally with the value of a variable of any type.",
    "log \"{0}\" {1}",
    {P("Text", LX_FSM_STRING, "Hello"), PIN_OPT("Value", LX_FSM_ANY), PENUM("Level", "Info|Warning|Error", "0")},
    0, 0, LX_FSM_ONCE, dbg_log, dbg_log, NULL};

static void dbg_log_state(LxFsmCtx *c) {
    LxFsmRun *r = c->run;
    LX_INFO("[%s] graph \"%s\" state \"%s\" for %.2fs (entered by \"%s\")",
            lx_entity_name(c->scene, c->self),
            r && r->graph ? r->graph->name : "?",
            r ? r->state_name : "?",
            (double)lx_fsm_state_time(c),
            r ? r->last_event : "");
}
static const LxFsmActionDesc A_DBG_STATE = {"Log State Info", "Debug", "Writes which graph and state this is, how long it has been here, and what brought it.",
    "log state",
    {{0}}, 0, 0, LX_FSM_ONCE, dbg_log_state, dbg_log_state, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_logic(void) {
    static const LxFsmActionDesc *all[] = {
        &A_SM_FINISH, &A_SM_GET_STATE, &A_SM_CHECK_STATE, &A_SM_PREV_STATE,
        &A_SM_GET_VAR, &A_SM_SET_VAR, &A_SM_GET_GLOBAL, &A_SM_SET_GLOBAL, &A_SM_STATE_TIME,

        &A_EV_SEND, &A_EV_SEND_BY_NAME, &A_EV_NEXT_FRAME, &A_EV_RANDOM, &A_EV_INFO,

        &A_FL_INT_SWITCH, &A_FL_STRING_SWITCH, &A_FL_FLOAT_SWITCH, &A_FL_TAG_SWITCH,
        &A_FL_THRESHOLD, &A_FL_BOOL_CHANGED,

        &A_LG_DISTANCE, &A_LG_ANGLE,

        &A_LP_LOOP, &A_LP_FOR_EACH_CHILD, &A_LP_FOR_EACH_ITEM, &A_LP_FOR_EACH_TAG,

        &A_SQ_WAIT, &A_SQ_RANDOM_WAIT, &A_SQ_WAIT_FRAMES, &A_SQ_WAIT_BOOL,

        &A_TM_COUNTDOWN, &A_TM_TICK, &A_PS_FLOAT, &A_PS_ADD,

        &A_T_GET_TIME, &A_T_SET_SCALE, &A_T_SLOWMO, &A_T_PAUSE, &A_T_DATE,

        &A_DBG_LOG, &A_DBG_STATE,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
