/* State Graph Finder: one search across every graph in the project - states, actions, the variables they
 * use, where events are sent and where they are received, notes. Click a result to open the graph there
 * (on an object that uses it when the scene has one), inside whichever Sub State it is in. */
#include "graph_editor.h"

void ed_graph_show_tab(int tab);

#define COL_TEXT UI_HEX(0xdfe1e6)
#define COL_DIM UI_HEX(0x8d919a)

static struct {
    UiNode *win, *list, *field, *chips[6];
    char query[64];
    unsigned kinds;
    bool dirty;
    char last[64];
    unsigned last_kinds;
    uint32_t last_versions;
} F = {.kinds = LX_GF_ALL};

static const struct { const char *label; unsigned kinds; } CHIPS[6] = {
    {"All", LX_GF_ALL}, {"States", LX_GF_STATE}, {"Actions", LX_GF_ACTION}, {"Variables", LX_GF_VARIABLE},
    {"Events", LX_GF_EVENT_SENT | LX_GF_EVENT_RECEIVED}, {"Notes", LX_GF_COMMENT}};

static UiIcon kind_icon(LxGraphFindKind k) {
    switch (k) {
    case LX_GF_STATE: return UI_ICON_CUBE;
    case LX_GF_ACTION: return UI_ICON_LIST;
    case LX_GF_VARIABLE: return UI_ICON_INSPECTOR;
    case LX_GF_EVENT_SENT: return UI_ICON_EXTERNAL;
    case LX_GF_EVENT_RECEIVED: return UI_ICON_CHEVRON_RIGHT;
    default: return UI_ICON_INFO;
    }
}
static uint32_t kind_color(LxGraphFindKind k) {
    switch (k) {
    case LX_GF_STATE: return UI_HEX(0x5a92ff);
    case LX_GF_ACTION: return UI_HEX(0x7cc24e);
    case LX_GF_VARIABLE: return UI_HEX(0xc89cff);
    case LX_GF_EVENT_SENT: return UI_HEX(0xffb03a);
    case LX_GF_EVENT_RECEIVED: return UI_HEX(0xff8a5a);
    default: return COL_DIM;
    }
}

/* ---------------- opening a result ---------------- */
static void open_hit(UiNode *n) {
    char ref[LX_REF_MAX];
    lx_strcpy(ref, sizeof ref, n->path);
    LxGraph *g = lx_asset_graph(ref);
    if (!g) return;
    int state = (int)(n->tag >> 16) - 1, action = (int)(n->tag & 0xffff) - 2;
    /* show it on an object that runs it, if the scene has one; otherwise the graph asset */
    bool shown = !strcmp(GE.ref, ref);
    for (int i = 0; !shown && ED.scene && i < lx_scene_entity_count(ED.scene); i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        LxStateGraph *sg = lx_get_state_graph(ED.scene, e);
        for (int k = 0; sg && k < sg->count && !shown; k++)
            if (!strcmp(sg->slots[k].graph, ref)) { ed_graph_edit(e, k); shown = true; }
    }
    if (!shown) ed_graph_open_asset(ref);
    if (ED.view_tabs) ED.view_tabs->tab_index = 1;
    if (state < 0 || state >= g->state_count) {   /* a variable or a graph-wide thing */
        ge_enter_scope(-1);
        if (action == -1) ed_graph_show_tab(3);
        return;
    }
    if (action == -2 && g->states[state].kind == LX_GS_SUB) { ge_enter_scope(state); return; }   /* a note inside it */
    ed_graph_select_state(g->states[state].name);
    GE.framed = false;   /* frame what was found */
}

/* ---------------- the list ---------------- */
static void rebuild(void) {
    if (!F.list) return;
    UiCtx *ui = ED.ui;
    ui_clear(ui, F.list);
    lx_strcpy(F.last, sizeof F.last, F.query);
    F.last_kinds = F.kinds;
    if (!F.query[0] || (!strncmp(F.query, "event:", 6) && !F.query[6])) {
        UiNode *h = ui_label(ui, F.list, "Type something to find: a state, an action, a variable, an event...");
        h->fg = COL_DIM;
        h->font = UI_FONT_SMALL;
        UiNode *h2 = ui_label(ui, F.list, "event:NAME finds exactly where that event is sent and received.");
        h2->fg = COL_DIM;
        h2->font = UI_FONT_SMALL;
        return;
    }
    ge_load_all_graphs();
    LxGraph *graphs[256];
    int ng = lx_graphs_loaded(graphs, 256), total = 0, shown_graphs = 0;
    static LxGraphHit hits[400];
    for (int gi = 0; gi < ng && total < 400; gi++) {
        LxGraph *g = graphs[gi];
        int nh = lx_graph_find(g, F.query, F.kinds, hits, 400);
        if (nh > 400) nh = 400;
        if (!nh) continue;
        shown_graphs++;
        char head[200];
        snprintf(head, sizeof head, "%s   (%s, %d)", g->name, g->ref, nh);
        UiNode *hl = ui_label(ui, F.list, head);
        hl->font = UI_FONT_BOLD;
        hl->fg = COL_TEXT;
        hl->icon = UI_ICON_GRAPH;
        hl->icon_color = COL_DIM;
        hl->h = 24;
        for (int k = 0; k < nh && total < 400; k++, total++) {
            UiNode *it = ui_menu_item(ui, F.list, hits[k].what, NULL, open_hit);
            it->menu_item = false;   /* a list row: clicking it keeps the Finder open */
            it->icon = kind_icon(hits[k].kind);
            it->icon_color = kind_color(hits[k].kind);
            lx_strcpy(it->path, sizeof it->path, g->ref);
            it->tag = ((uint32_t)(hits[k].state + 1) << 16) | (uint32_t)((hits[k].action + 2) & 0xffff);
        }
    }
    if (!total) {
        UiNode *h = ui_label(ui, F.list, "Nothing found in this project's graphs.");
        h->fg = COL_DIM;
    } else {
        char foot[96];
        snprintf(foot, sizeof foot, "%d found in %d graph%s", total, shown_graphs, shown_graphs == 1 ? "" : "s");
        UiNode *f = ui_label(ui, F.list, foot);
        f->fg = COL_DIM;
        f->font = UI_FONT_SMALL;
    }
}
static uint32_t versions_sum(void) {
    LxGraph *graphs[256];
    int ng = lx_graphs_loaded(graphs, 256);
    uint32_t v = (uint32_t)ng;
    for (int i = 0; i < ng; i++) v = v * 31u + graphs[i]->version;
    return v;
}
static void finder_frame(UiNode *n) {
    LX_UNUSED(n);
    uint32_t v = versions_sum();
    if (F.dirty || strcmp(F.last, F.query) || F.last_kinds != F.kinds || v != F.last_versions) {
        F.dirty = false;
        F.last_versions = v;
        rebuild();
    }
    for (int i = 0; i < 6; i++) if (F.chips[i]) F.chips[i]->selected = F.kinds == CHIPS[i].kinds;
}
static void chip_click(UiNode *n) { F.kinds = CHIPS[n->tag].kinds; }
static void query_enter(UiNode *n) {
    LX_UNUSED(n);
    if (!lx_key_pressed(LX_KEY_ENTER) || !F.list) return;
    for (UiNode *c = F.list->first; c; c = c->next)   /* Enter: the first result */
        if (c->on_click) { c->on_click(c); return; }
}
static void finder_closed(UiNode *w) { LX_UNUSED(w); F.win = NULL; F.list = NULL; F.field = NULL; for (int i = 0; i < 6; i++) F.chips[i] = NULL; }

void ge_finder_open(const char *query) {
    UiCtx *ui = ED.ui;
    if (query) lx_strcpy(F.query, sizeof F.query, query);
    if (query && !strncmp(query, "event:", 6)) F.kinds = LX_GF_EVENT_SENT | LX_GF_EVENT_RECEIVED;
    F.dirty = true;
    if (F.win) { ui_window_front(ui, F.win); if (F.field) ui_focus(ui, F.field); return; }
    float s = ui_scale(ui);
    float x = GE.canvas ? GE.canvas->x + 40 * s : 200, y = GE.canvas ? GE.canvas->y + 40 * s : 120;
    UiNode *body = ui_window(ui, "Find in Graphs", x, y, 520, 560);
    if (!body) return;
    F.win = body->parent;
    F.win->on_change = finder_closed;
    ui_track(&F.win);
    body->on_frame = finder_frame;
    ui_pad(body, 8, 8, 8, 8);
    body->gap = 6;
    UiNode *tf = ui_textfield(ui, body, F.query, sizeof F.query);
    tf->h = 28;
    tf->icon = UI_ICON_SEARCH;
    tf->on_change = query_enter;
    lx_strcpy(tf->hint, sizeof tf->hint, "Find states, actions, variables, events (event:NAME for one event)");
    F.field = tf;
    ui_track(&F.field);
    UiNode *row = ui_box(ui, body, UI_ROW);
    row->h = 26;
    row->gap = 4;
    for (int i = 0; i < 6; i++) {
        UiNode *b = ui_button(ui, row, CHIPS[i].label, chip_click);
        b->h = 24;
        b->w = i == 0 ? 50 : i == 3 ? 86 : 72;
        b->tag = i;
        F.chips[i] = b;
        ui_track(&F.chips[i]);
    }
    UiNode *list = ui_box(ui, body, UI_COL);
    list->grow = 1;
    list->scroll = true;
    list->gap = 1;
    F.list = list;
    ui_track(&F.list);
    ui_focus(ui, tf);
}
void ge_finder_toggle(void) {
    if (F.win) { UiNode *w = F.win; finder_closed(w); ui_remove(ED.ui, w); return; }
    ge_finder_open(NULL);
}
