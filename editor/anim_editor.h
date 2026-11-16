/* Animator editor: the Animator tab's shared state, between the graph view (anim_editor.c) and its
 * inspector (anim_inspector.c). */
#ifndef ED_ANIM_EDITOR_H
#define ED_ANIM_EDITOR_H
#include "editor.h"

#define AE_UNDO 60
#define AE_STATE_W 164.0f    /* logical px, at zoom 1 */
#define AE_STATE_H 42.0f
#define AE_COLORS 8
#define AE_ANY (-2)          /* the Any State node, where a state index is wanted */
#define AE_ENTRY (-3)        /* the Entry node */

typedef enum AeDrag { AE_DRAG_NONE, AE_DRAG_PAN, AE_DRAG_MOVE, AE_DRAG_LINK, AE_DRAG_ENTRY } AeDrag;

typedef struct AnimEd {
    /* widgets */
    UiNode *panel, *canvas, *insp_tabs, *insp_params, *insp_state, *insp_trans, *insp_layers;
    UiNode *obj_btn, *graph_btn, *layer_btn, *zoom_btn, *state_lbl, *problems_btn;
    /* what is being edited */
    char ref[LX_REF_MAX];        /* the .lxanim asset, "" = none */
    LxEntity obj;                /* the object whose animator is shown (0 = the asset on its own) */
    int layer;                   /* which layer's state machine the view shows */
    uint32_t seen_version;
    bool insp_dirty;
    bool locked;
    /* view */
    float pan_x, pan_y, zoom;
    bool framed;
    /* selection */
    int sel;                     /* state index, AE_ANY, or -1 for nothing */
    int sel_trans;               /* transition of the selected state, -1 = none */
    /* gestures */
    AeDrag drag;
    bool drag_moved;
    LxVec2 press, last;
    float press_gx, press_gy;
    int link_from;               /* the state a transition is being dragged from (or AE_ANY) */
    double last_click;
    LxVec2 last_click_pos;
    bool focused;
    /* undo (graph text snapshots) */
    char *undo[AE_UNDO];
    int undo_count;
    char *redo[AE_UNDO];
    int redo_count;
    char *last_text;
    const void *last_key;
    double last_commit_time;
} AnimEd;

extern AnimEd AE;

/* anim_editor.c */
LxAnimGraph *ae_graph(void);                 /* the graph being edited, or NULL */
LxAnimLayer *ae_layer(void);                 /* the layer the view shows, or NULL */
LxAnimNode *ae_sel_state(void);              /* the selected state, or NULL */
LxAnimTransition *ae_sel_transition(void);   /* the selected transition, or NULL */
int ae_sel_index(void);
void ae_select(int state, int trans);
void ae_commit(bool structural, const void *key);   /* after any change: undo snapshot, save, re-bind */
void ae_undo(void);
void ae_redo(void);
void ae_frame_all(void);
uint32_t ae_state_color(int c);
void ae_new_graph_for(LxEntity e);           /* creates Animators/<name>.lxanim and points the Animator at it */
/* which state each layer is playing right now, for the live highlight (-1 = not playing) */
int ae_live_state(int layer);

/* anim_inspector.c */
UiNode *ae_inspector(UiNode *parent);
void ae_inspector_frame(void);
#endif
