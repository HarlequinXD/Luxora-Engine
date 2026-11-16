/* Animator graphs (.lxanim): the asset model shared by the runtime and the editor.
 *
 * An animator graph is a state machine over animation, not over game logic - which is what makes it a
 * different thing from a State Graph. Its states play motion (a clip, or a blend tree that mixes several
 * clips by a parameter), and its transitions are driven by typed parameters and by how far through the
 * clip playback is, not by events. Transitions blend: leaving one state for another cross-fades between
 * their poses over the transition's duration, so animation never snaps.
 *
 * A state poses the whole skeleton: it starts from the model's rest pose and overwrites the bones its
 * clip animates, so a bone no clip in the state touches goes back to where the model has it.
 *
 * Layers let a graph play more than one thing at once - an upper-body aim over a full-body walk - each
 * with its own state machine, its own weight and, through a mask, its own set of bones to write. */
#ifndef LX_ANIM_GRAPH_H
#define LX_ANIM_GRAPH_H

#define LX_ANIM_NAME 64
#define LX_ANIM_MAX_COND 8          /* conditions on one transition */
#define LX_ANIM_MAX_CHILDREN 16     /* motions in one blend tree */
#define LX_ANIM_MAX_LAYERS 8
#define LX_ANIM_MAX_DEPTH 4         /* blend trees nested inside blend trees */

/* ---------------- parameters ---------------- */
/* A trigger is a bool that a transition consumes: setting it makes one transition fire, once. */
typedef enum LxAnimParamType { LX_AP_FLOAT, LX_AP_INT, LX_AP_BOOL, LX_AP_TRIGGER, LX_AP_TYPE_COUNT } LxAnimParamType;

typedef struct LxAnimParam {
    char name[LX_ANIM_NAME];
    LxAnimParamType type;
    float f;            /* starting value (float) */
    int i;              /* starting value (int) */
    bool b;             /* starting value (bool; a trigger always starts unset) */
} LxAnimParam;

/* ---------------- transitions ---------------- */
/* Greater/Less compare floats and ints; Equals/Not Equal compare ints; If/If Not test bools and triggers. */
typedef enum LxAnimCondMode {
    LX_AC_GREATER, LX_AC_LESS, LX_AC_EQUALS, LX_AC_NOT_EQUAL, LX_AC_IF, LX_AC_IF_NOT, LX_AC_MODE_COUNT
} LxAnimCondMode;

typedef struct LxAnimCond {
    char param[LX_ANIM_NAME];
    LxAnimCondMode mode;
    float value;        /* the threshold it is compared against (an int threshold is held here too) */
} LxAnimCond;

/* What may interrupt a transition that is already running. */
typedef enum LxAnimInterrupt { LX_AI_NONE, LX_AI_CURRENT, LX_AI_NEXT, LX_AI_BOTH } LxAnimInterrupt;

typedef struct LxAnimTransition {
    char to[LX_ANIM_NAME];              /* the state it goes to ("" = not connected yet) */
    LxAnimCond cond[LX_ANIM_MAX_COND];
    int cond_count;
    bool has_exit_time;                 /* wait until playback reaches exit_time */
    float exit_time;                    /* in cycles: 0.75 = three quarters through, 1 = the end */
    float duration;                     /* how long the cross-fade takes */
    bool fixed_duration;                /* duration in seconds (else in cycles of the state being left) */
    float offset;                       /* where the state being entered starts, in cycles */
    LxAnimInterrupt interrupt;
    bool to_self;                       /* may transition to the state it starts from */
    bool mute;                          /* ignored */
    bool solo;                          /* while any transition of a state is solo, only solo ones are taken */
} LxAnimTransition;

/* ---------------- motion: a clip, or a blend tree ---------------- */
typedef enum LxAnimMotion { LX_AM_CLIP, LX_AM_TREE } LxAnimMotion;

/* 1D blends along one parameter. The 2D kinds blend over two: Directional treats the children as
 * directions with a magnitude (a locomotion rose: forward, back, strafe), Cartesian treats them as plain
 * positions. Direct gives every child its own parameter as its weight. */
typedef enum LxAnimTreeType {
    LX_AT_1D, LX_AT_2D_DIRECTIONAL, LX_AT_2D_CARTESIAN, LX_AT_DIRECT, LX_AT_TYPE_COUNT
} LxAnimTreeType;

typedef struct LxAnimChild {
    char clip[LX_REF_MAX];              /* the clip it plays, when tree < 0 */
    int tree;                           /* a nested blend tree (index into the graph's trees), -1 = a clip */
    float threshold;                    /* 1D: where on the parameter this child sits */
    float px, py;                       /* 2D: its position */
    float speed;
    char weight_param[LX_ANIM_NAME];    /* Direct: the parameter holding its weight */
    bool mirror;
} LxAnimChild;

typedef struct LxAnimTree {
    LxAnimTreeType type;
    char param_x[LX_ANIM_NAME];
    char param_y[LX_ANIM_NAME];
    LxAnimChild child[LX_ANIM_MAX_CHILDREN];
    int child_count;
} LxAnimTree;

/* ---------------- states ---------------- */
typedef struct LxAnimNode {
    char name[LX_ANIM_NAME];
    char description[256];
    float x, y;                         /* graph position (top-left) */
    int color;                          /* index into the state colour palette */
    LxAnimMotion motion;
    char clip[LX_REF_MAX];              /* LX_AM_CLIP: "Models/Hero.fbx#Run" */
    int tree;                           /* LX_AM_TREE: index into the graph's trees, -1 = none */
    float speed;
    char speed_param[LX_ANIM_NAME];     /* a parameter that multiplies speed ("" = none) */
    bool loop;
    bool mirror;
    float cycle_offset;                 /* where the state starts, in cycles */
    bool foot_ik;                       /* ground the feet while this state plays */
    char tag[LX_ANIM_NAME];             /* a label a graph can ask about ("Attacking") */
    bool breakpoint;                    /* entering this state pauses Play mode */
    LxAnimTransition *trans;
    int trans_count, trans_cap;
} LxAnimNode;

/* ---------------- layers ---------------- */
typedef enum LxAnimBlendMode { LX_ABM_OVERRIDE, LX_ABM_ADDITIVE } LxAnimBlendMode;

typedef struct LxAnimLayer {
    char name[LX_ANIM_NAME];
    char start[LX_ANIM_NAME];           /* the state it begins in */
    float weight;                       /* how much of it shows (layer 0 is always the whole base) */
    LxAnimBlendMode blend;
    uint64_t mask;                      /* humanoid bones it writes (with their descendants); 0 = all of them */
    LxAnimNode *states;
    int state_count, state_cap;
    LxAnimTransition *any;              /* Any State transitions: taken from whichever state is playing */
    int any_count, any_cap;
} LxAnimLayer;

typedef struct LxAnimGraph {
    char ref[LX_REF_MAX];
    char name[LX_ANIM_NAME];
    char description[512];
    LxAnimParam *params;
    int param_count, param_cap;
    LxAnimLayer *layers;
    int layer_count, layer_cap;
    LxAnimTree *trees;
    int tree_count, tree_cap;
    uint32_t version;                   /* bumped on every edit: running animators re-bind */
    bool dirty;                         /* edited since saved */
} LxAnimGraph;

/* ---------------- lifetime and text format ---------------- */
LxAnimGraph *lx_anim_graph_new(const char *name);
void lx_anim_graph_free(LxAnimGraph *g);
LxAnimGraph *lx_anim_graph_clone(const LxAnimGraph *g);
char *lx_anim_graph_to_text(const LxAnimGraph *g);
LxAnimGraph *lx_anim_graph_from_text(const char *text);
bool lx_anim_graph_save(LxAnimGraph *g, const char *path);
LxAnimGraph *lx_asset_anim_graph(const char *ref);          /* cached; NULL if missing */
void lx_anim_graph_replace(const char *ref, LxAnimGraph *g);  /* the editor's copy becomes the cached one (undo) */
void lx_anim_graphs_clear(void);
void lx_anim_graph_touch(LxAnimGraph *g);

/* ---------------- editing helpers (keep names unique, fix references) ---------------- */
LxAnimLayer *lx_anim_add_layer(LxAnimGraph *g, const char *name);
void lx_anim_remove_layer(LxAnimGraph *g, int index);
LxAnimNode *lx_anim_add_state(LxAnimGraph *g, LxAnimLayer *l, const char *name, float x, float y);
LxAnimNode *lx_anim_find_state(const LxAnimLayer *l, const char *name);
int lx_anim_state_index(const LxAnimLayer *l, const char *name);
void lx_anim_remove_state(LxAnimLayer *l, int index);
bool lx_anim_rename_state(LxAnimLayer *l, int index, const char *name);
LxAnimTransition *lx_anim_add_transition(LxAnimNode *from, const char *to);
LxAnimTransition *lx_anim_add_any_transition(LxAnimLayer *l, const char *to);
void lx_anim_remove_transition(LxAnimNode *from, int index);
LxAnimCond *lx_anim_add_cond(LxAnimTransition *t, const char *param, LxAnimCondMode mode, float value);
void lx_anim_remove_cond(LxAnimTransition *t, int index);
LxAnimParam *lx_anim_add_param(LxAnimGraph *g, const char *name, LxAnimParamType type);
LxAnimParam *lx_anim_find_param(const LxAnimGraph *g, const char *name);
int lx_anim_param_index(const LxAnimGraph *g, const char *name);
void lx_anim_remove_param(LxAnimGraph *g, int index);
bool lx_anim_rename_param(LxAnimGraph *g, int index, const char *name);
int lx_anim_param_uses(const LxAnimGraph *g, const char *name);
int lx_anim_add_tree(LxAnimGraph *g, LxAnimTreeType type);   /* returns the tree's index */
void lx_anim_unique_name(const LxAnimGraph *g, const LxAnimLayer *l, const char *base, char *out, size_t cap, int kind);
const char *lx_anim_param_type_name(LxAnimParamType t);
const char *lx_anim_cond_mode_name(LxAnimCondMode m);
const char *lx_anim_tree_type_name(LxAnimTreeType t);
/* plain-English line for a transition ("to Run when Speed > 0.1, after 75%") */
void lx_anim_transition_summary(const LxAnimTransition *t, char *out, size_t cap);
/* problems: unconnected transitions, conditions on missing parameters, states with no motion... */
int lx_anim_graph_problems(const LxAnimGraph *g, char (*out)[160], int max);

/* ---------------- blend tree weights (anim_blend.c) ----------------
 * Hands back one weight per child, summing to 1 (all zero when there is nothing to blend). Pure maths on
 * the children's positions and the parameter values, so it is the same whoever asks. */
void lx_anim_tree_weights(const LxAnimTree *t, const float *param_x_y, float *out_weights);
/* Direct trees need a parameter per child, so they take a lookup instead of two values. */
typedef float (*LxAnimParamGet)(void *user, const char *name);
void lx_anim_tree_weights_ex(const LxAnimTree *t, LxAnimParamGet get, void *user, float *out_weights);
#endif
