/* State Graph editor (PlayMaker layout): shared state between the graph view (graph_editor.c)
 * and its inspector + Action Browser (graph_inspector.c). */
#ifndef ED_GRAPH_EDITOR_H
#define ED_GRAPH_EDITOR_H
#include "editor.h"

#define GE_MAX_SEL 256
#define GE_UNDO 80
#define GE_STATE_W 150.0f    /* logical px, at zoom 1 */
#define GE_TITLE_H 22.0f
#define GE_ROW_H 18.0f
#define GE_COLORS 8

typedef enum GeDrag { GE_DRAG_NONE, GE_DRAG_PAN, GE_DRAG_MOVE, GE_DRAG_LINK, GE_DRAG_BOX, GE_DRAG_COMMENT, GE_DRAG_COMMENT_SIZE,
                      GE_DRAG_START, GE_DRAG_MINIMAP } GeDrag;

typedef struct GraphEd {
    /* widgets */
    UiNode *panel, *canvas, *insp_tabs, *insp_fsm, *insp_state, *insp_events, *insp_vars;
    UiNode *obj_btn, *graph_btn, *lock_btn, *state_lbl, *zoom_btn, *map_btn, *debug_btn, *hints_btn, *errors_btn, *ab_btn;
    UiNode *ab_win, *ab_list, *ab_details;
    /* what's being edited */
    char ref[LX_REF_MAX];        /* graph asset, "" = none */
    LxEntity obj;                /* object whose State Graph is shown (0 = editing the asset itself) */
    int slot;
    bool locked;
    uint32_t seen_version;       /* graph version the inspector was built for */
    bool insp_dirty;
    /* view */
    float pan_x, pan_y, zoom;    /* graph point at the canvas' top-left corner; zoom 1 = 100% */
    bool framed;                 /* the view was fitted once for this graph */
    /* selection */
    int sel[GE_MAX_SEL];
    int sel_count;
    int sel_trans;               /* transition of the first selected state, -1 = none */
    int sel_comment;
    /* gestures */
    GeDrag drag;
    bool drag_moved;
    LxVec2 press, last;          /* screen */
    float press_gx, press_gy;    /* graph */
    int link_state, link_trans;  /* link being dragged (link_trans -1 = global transition, -2 = start) */
    int link_global;
    double last_click;
    LxVec2 last_click_pos;
    bool focused;                /* keyboard goes to the graph view */
    /* undo (graph text snapshots) */
    char *undo[GE_UNDO];
    int undo_count;
    char *redo[GE_UNDO];
    int redo_count;
    char *last_text;
    const void *last_key;
    double last_commit_time;
    /* options */
    bool debug, hints, minimap;
    /* Action Browser */
    char ab_search[64];
    char ab_sel[64];
    char favs[32][64];
    int fav_count;
    char recent[12][64];
    int recent_count;
    bool ab_dirty;
    char ab_closed_cats[24][32];   /* categories folded by the user */
    int ab_closed_count;
} GraphEd;

extern GraphEd GE;

/* graph_editor.c */
LxGraph *ge_graph(void);                       /* the graph being edited, or NULL */
LxGraphState *ge_sel_state(void);              /* first selected state */
int ge_sel_index(void);
void ge_select_state(int idx, bool add);
void ge_commit(bool structural, const void *key);   /* after any change: undo snapshot, save, re-bind running graphs */
void ge_undo(void);
void ge_redo(void);
LxFsmRun *ge_run(void);                        /* the running copy in play mode, or NULL */
uint32_t ge_state_color(int c);
void ge_open_action_browser(void);
void ge_add_action(const char *type);          /* to the selected state */
void ge_frame_all(void);
void ge_new_graph_for(LxEntity e);             /* creates Graphs/<name>.lxgraph and adds the component */

/* graph_inspector.c */
UiNode *ge_inspector(UiNode *parent);
void ge_inspector_frame(void);
void ge_action_browser_toggle(void);
void ge_prefs_load(void);
void ge_prefs_save(void);
void ge_note_recent(const char *type);
#endif
