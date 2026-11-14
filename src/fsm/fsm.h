/* State Graph internals: the graph asset model (shared by the runtime and the editor) and the runtime. */
#ifndef LX_FSM_H
#define LX_FSM_H

#define LX_FSM_NAME 64
#define LX_FSM_MAX_LOOP 1000   /* state changes per object per frame before we decide it's an endless loop */

/* ---------------- graph asset ---------------- */
typedef struct LxGraphParam {
    char value[LX_REF_MAX];   /* literal as text, or the variable's name when is_var */
    bool is_var;
} LxGraphParam;

typedef struct LxGraphAction {
    char type[LX_FSM_NAME];                 /* LxFsmActionDesc name */
    bool enabled;
    bool folded;                            /* editor: collapsed in the State tab */
    LxFsmUpdate update;
    LxGraphParam p[LX_FSM_MAX_PARAMS];
} LxGraphAction;

typedef struct LxGraphTransition {
    char event[LX_FSM_NAME];
    char to[LX_FSM_NAME];                   /* "" = not connected yet */
} LxGraphTransition;

typedef struct LxGraphState {
    char name[LX_FSM_NAME];
    char description[256];
    float x, y;                             /* graph position (top-left) */
    int color;                              /* index into the state colour palette */
    bool sequence;                          /* run actions one after another */
    LxGraphAction *actions;
    int action_count, action_cap;
    LxGraphTransition *trans;
    int trans_count, trans_cap;
} LxGraphState;

typedef struct LxGraphVar {
    char name[LX_FSM_NAME];
    LxFsmType type;
    char value[LX_REF_MAX];                 /* starting value as text */
    bool inspector;                         /* shown (and overridable) on the State Graph component */
    char tooltip[128];
} LxGraphVar;

typedef struct LxGraphEvent { char name[LX_FSM_NAME]; bool global; } LxGraphEvent;

typedef struct LxGraphComment { float x, y, w, h; int color; char text[512]; } LxGraphComment;

typedef struct LxGraph {
    char ref[LX_REF_MAX];
    char name[LX_FSM_NAME];
    char description[512];
    char start[LX_FSM_NAME];
    LxGraphState *states;
    int state_count, state_cap;
    LxGraphEvent *events;
    int event_count, event_cap;
    LxGraphVar *vars;
    int var_count, var_cap;
    LxGraphTransition *globals;             /* global transitions: event -> state, from any state */
    int global_count, global_cap;
    LxGraphComment *comments;
    int comment_count, comment_cap;
    uint32_t version;                       /* bumped on every edit: running graphs re-bind */
    bool dirty;                             /* edited since saved */
} LxGraph;

/* system events the engine sends */
extern const char *const LX_FSM_SYSTEM_EVENTS[];
extern const int LX_FSM_SYSTEM_EVENT_COUNT;
bool lx_fsm_is_system_event(const char *name);

LxGraph *lx_graph_new(const char *name);
void lx_graph_free(LxGraph *g);
LxGraph *lx_graph_clone(const LxGraph *g);
char *lx_graph_to_text(const LxGraph *g);
LxGraph *lx_graph_from_text(const char *text);
bool lx_graph_save(LxGraph *g, const char *path);
LxGraph *lx_asset_graph(const char *ref);   /* cached; NULL if missing */
void lx_graph_replace(const char *ref, LxGraph *g);   /* the editor's copy becomes the cached one (undo) */
void lx_graphs_clear(void);
void lx_graph_touch(LxGraph *g);            /* after an edit: version++, dirty */

/* editing helpers (keep names unique, fix references) */
LxGraphState *lx_graph_add_state(LxGraph *g, const char *name, float x, float y);
LxGraphState *lx_graph_find_state(const LxGraph *g, const char *name);
int lx_graph_state_index(const LxGraph *g, const char *name);
void lx_graph_remove_state(LxGraph *g, int index);
bool lx_graph_rename_state(LxGraph *g, int index, const char *name);
LxGraphAction *lx_graph_add_action(LxGraphState *st, const LxFsmActionDesc *d, int at);   /* at -1 = end */
void lx_graph_remove_action(LxGraphState *st, int index);
void lx_graph_move_action(LxGraphState *st, int from, int to);
LxGraphTransition *lx_graph_add_transition(LxGraph *g, LxGraphState *st, const char *event);
void lx_graph_remove_transition(LxGraphState *st, int index);
LxGraphEvent *lx_graph_add_event(LxGraph *g, const char *name);
LxGraphEvent *lx_graph_find_event(const LxGraph *g, const char *name);
void lx_graph_remove_event(LxGraph *g, int index);
int lx_graph_event_uses(const LxGraph *g, const char *name);
LxGraphVar *lx_graph_add_var(LxGraph *g, const char *name, LxFsmType type);
LxGraphVar *lx_graph_find_var(const LxGraph *g, const char *name);
void lx_graph_remove_var(LxGraph *g, int index);
bool lx_graph_rename_var(LxGraph *g, int index, const char *name);
int lx_graph_var_uses(const LxGraph *g, const char *name);
void lx_graph_unique_name(const LxGraph *g, const char *base, char *out, size_t cap, int kind);   /* kind 0 state, 1 event, 2 var */
const char *lx_fsm_type_name(LxFsmType t);
LxFsmType lx_fsm_type_parse(const char *s);
const char *lx_fsm_default_value(LxFsmType t);
/* plain-English line for an action ("Move self towards Player at 4.5") */
void lx_graph_action_summary(const LxGraphAction *a, const LxFsmActionDesc *d, char *out, size_t cap);
/* problems: unconnected transitions, unknown actions, empty required parameters... (count, text lines) */
int lx_graph_problems(const LxGraph *g, char (*out)[160], int max);

/* ---------------- global variables / events (Assets/Globals.lxglobals) ---------------- */
typedef struct LxGlobals {
    LxGraphVar *vars;
    int var_count, var_cap;
    LxGraphEvent *events;
    int event_count, event_cap;
    bool dirty;
} LxGlobals;
LxGlobals *lx_fsm_globals(void);          /* loaded from the project on first use */
void lx_fsm_globals_reload(void);
bool lx_fsm_globals_save(void);
int lx_fsm_globals_index(const char *name);
LxGraphVar *lx_fsm_globals_find_var(const char *name);
struct LxFsmValue *lx_fsm_global_value(int i);
void lx_fsm_globals_reset_values(void);   /* play starts: runtime values from their starting values */

/* ---------------- runtime ---------------- */
typedef struct LxFsmValue {
    LxFsmType type;
    float f;
    int i;
    bool b;
    LxVec3 v;
    LxVec4 c;
    LxEntity e;
    char s[LX_REF_MAX];      /* string / asset / object lookup ("Owner", "name:X", "tag:X") */
} LxFsmValue;

typedef struct LxFsmBound {   /* one action as it runs */
    const LxFsmActionDesc *desc;
    const LxGraphAction *src;
    int var[LX_FSM_MAX_PARAMS];      /* local variable index, -2 - global index, or -1 = literal */
    LxFsmValue lit[LX_FSM_MAX_PARAMS];
    void *state;
    bool finished, entered;
} LxFsmBound;

typedef struct LxFsmRun {
    LxGraph *graph;
    uint32_t bound_version;
    LxEntity self;
    int slot;
    int state;                 /* -1 = not started / stopped */
    int prev_state;
    float state_time;
    LxFsmBound **bound;        /* per state: action_count entries */
    int *bound_counts;
    int bound_states;
    char state_name[LX_FSM_NAME];
    LxFsmValue *vars;          /* local variables */
    char (*var_names)[LX_FSM_NAME];
    int var_count;
    bool started, finished_sent, error;
    int pending;               /* index of a requested transition target, -1 none */
    char pending_event[LX_FSM_NAME];
    float pend_value;
    LxEntity pend_sender, pend_other;
    int in_action;             /* >0 while actions run: events are queued, not taken immediately */
    int loops;                 /* state changes this frame */
    /* the event that caused the last transition */
    char last_event[LX_FSM_NAME];
    float event_value;
    LxEntity event_sender, event_other;
    LxEntity last_other;       /* the other object of the latest collision / trigger */
    char last_system_event[LX_FSM_NAME];
    /* debugging */
    double enter_time;
    int history_count;
    struct { int state; char event[LX_FSM_NAME]; float time; } history[64];
} LxFsmRun;

typedef struct LxFsmInstance {   /* per object: one run per slot */
    LxFsmRun runs[LX_FSM_GRAPHS_PER_OBJECT];
    int count;
} LxFsmInstance;

void lx_fsm_scene_update(LxScene *s, float dt, LxFsmUpdate phase);   /* EVERY_FRAME / FIXED / LATE */
void lx_fsm_entity_free(LxEntityData *d);           /* the object goes away / loses the component */
void lx_fsm_scene_stop(LxScene *s);
void lx_fsm_system_event(LxScene *s, LxEntity self, const char *event, LxEntity other, float value);
LxFsmRun *lx_fsm_run_of(LxScene *s, LxEntity e, int slot);
bool lx_fsm_value_parse(LxFsmValue *v, LxFsmType t, const char *text, const char *options);
void lx_fsm_value_format(const LxFsmValue *v, char *out, size_t cap, const char *options);
LxStateGraph *lx_state_graph_alloc(void);
#endif
