/* Luxora Editor: terrain tools. The Terrain component's inspector (sculpt and paint brushes, chunks, paint
 * layers, grass and scatter, edit layers) and the brush in the Scene view, like Unity's Terrain tools and
 * Unreal's Landscape mode. A brush stroke is one undo step. */
#include "editor.h"

enum {
    TT_RAISE, TT_LOWER, TT_SMOOTH, TT_FLATTEN, TT_SET_HEIGHT, TT_NOISE, TT_ERODE, TT_TERRACE, TT_PAINT, TT_HOLE,
    TT_ERASE_LAYER, TT_RAMP, TT_STAMP, TT_ADD_CHUNK, TT_REMOVE_CHUNK, TT_COUNT
};
static const struct { const char *name; int tool; const char *help; } k_tools[TT_COUNT] = {
    {"Raise", LX_TERRAIN_RAISE, "Click and drag to raise the ground. Hold Shift to lower it."},
    {"Lower", LX_TERRAIN_LOWER, "Click and drag to lower the ground. Hold Shift to raise it."},
    {"Smooth", LX_TERRAIN_SMOOTH, "Drag over bumps to soften them."},
    {"Flatten", LX_TERRAIN_FLATTEN, "Drag to level the ground to the height where the stroke started."},
    {"Set Height", LX_TERRAIN_SET_HEIGHT, "Drag to bring the ground to the Height below. Ctrl-click the terrain to pick it."},
    {"Noise", LX_TERRAIN_NOISE, "Drag to roughen the ground with natural bumps."},
    {"Erode", LX_TERRAIN_ERODE, "Drag to wash material down slopes, carving gullies like rain."},
    {"Terrace", LX_TERRAIN_TERRACE, "Drag to cut the slopes into steps, Spacing metres apart."},
    {"Paint", LX_TERRAIN_PAINT, "Drag to paint the chosen layer. Layers with slope or height rules paint themselves too."},
    {"Hole", LX_TERRAIN_HOLE, "Drag to cut holes (cave mouths, tunnels). Hold Shift to fill them again."},
    {"Erase on Layer", LX_TERRAIN_ERASE_LAYER, "Drag to take away the active edit layer's sculpting under the brush."},
    {"Ramp", -1, "Press where the ramp starts and drag to where it ends: a smooth road or path between them."},
    {"Stamp", -1, "Click to stamp a landform the size of the brush."},
    {"Add Chunk", -1, "Click a square beside the terrain to grow it there. The new chunk's edges blend into its neighbours."},
    {"Remove Chunk", -1, "Click a chunk to remove it."},
};

static int g_tool = -1;   /* -1 = no tool: the normal gizmo and selection */
static LxTerrainBrush g_brush = {LX_TERRAIN_RAISE, 8, 0.5f, 0.6f, 10, 4, 0};
static int g_stamp = LX_STAMP_HILL;
static float g_stamp_height = 30;
static int g_preset = LX_TERRAIN_HILLS;
static float g_gen_height = 60;
static int g_seed = 1;
static struct {
    LxEntity terrain;
    bool stroking, ramping;
    LxVec3 ramp_a;
    bool has_hit;
    LxVec3 hit;
    bool chunk_ok;
    int ccx, ccz;
    bool chunk_valid;       /* Add: an empty square beside the terrain; Remove: a chunk (not the last one) */
} T;

static LxEntity selected_terrain(void) {
    if (!ED.scene || !ED.selected || !lx_entity_valid(ED.scene, ED.selected)) return 0;
    return lx_get_terrain(ED.scene, ED.selected) ? ED.selected : 0;
}
bool ed_terrain_tool_active(void) { return g_tool >= 0 && ED.play == ED_EDIT && selected_terrain() != 0; }

void ed_terrain_frame(void) {
    if (ED.scene && ED.play == ED_EDIT) lx_terrain_stream(ED.scene, ED.cam_pos);   /* chunks around the editor camera */
}

/* a folder for a new terrain's chunks nobody else uses */
void ed_terrain_unique_folder(const char *base, char *out, size_t cap) {
    for (int n = 1; n < 1000; n++) {
        if (n == 1) snprintf(out, cap, "Terrains/%s", base);
        else snprintf(out, cap, "Terrains/%s %d", base, n);
        char path[LX_PATH_MAX];
        lx_asset_path(path, sizeof path, out);
        bool used = lx_dir_exists(path);
        int count = lx_scene_entity_count(ED.scene);
        for (int i = 0; i < count && !used; i++) {
            LxTerrainComponent *t = lx_get_terrain(ED.scene, lx_scene_entity_at(ED.scene, i));
            used = t && !strcmp(t->folder, out);
        }
        if (!used) return;
    }
}
LxEntity ed_terrain_create(void) {
    char folder[LX_REF_MAX];
    ed_terrain_unique_folder("Terrain", folder, sizeof folder);
    LxEntity e = lx_entity_create(ED.scene, "Terrain");
    LxTerrainComponent *t = lx_add_terrain(ED.scene, e, folder);
    float half = t ? t->chunk_size * 0.5f : 32;
    lx_entity_set_position(ED.scene, e, lx_v3(-half, 0, -half));   /* the first chunk centred on the world */
    g_tool = TT_RAISE;
    return e;
}

/* ---------------- inspector ---------------- */
static void changed_commit(UiNode *n) {
    LX_UNUSED(n);
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}
static void small_hint(UiNode *parent, const char *text) {
    UiNode *h = ui_label(ED.ui, parent, text);
    h->font = UI_FONT_SMALL;
    h->fg = ui_theme(ED.ui)->text_dim;
    h->wrap = true;
}
static UiNode *num_row(UiNode *parent, const char *label, float *v, float step, float minv, float maxv) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_number(ED.ui, row, v, step);
    n->grow = 1;
    n->h = 22;
    n->minv = minv;
    n->maxv = maxv;
    n->on_change = ed_field_changed;
    return n;
}
static UiNode *pair_row(UiNode *parent, const char *label, float *a, float *b, float step, float minv, float maxv) {
    UiNode *row = ed_prop_row(parent, label);
    for (int k = 0; k < 2; k++) {
        UiNode *n = ui_number(ED.ui, row, k ? b : a, step);
        n->grow = 1;
        n->h = 22;
        n->minv = minv;
        n->maxv = maxv;
        n->on_change = ed_field_changed;
    }
    return row;
}
static UiNode *slide_row(UiNode *parent, const char *label, float *v, float minv, float maxv, bool scene_field) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_slider(ED.ui, row, v, minv, maxv);
    n->grow = 1;
    n->h = 22;
    if (scene_field) n->on_change = ed_field_changed;
    return n;
}

static void tool_click(UiNode *n) {
    g_tool = g_tool == (int)n->tag ? -1 : (int)n->tag;   /* clicking the chosen tool puts it away */
    ED.inspector_dirty = true;
}

/* names of the paint layers for dropdowns; [0] is the "none" entry when offset */
static char g_layer_names[LX_TERRAIN_LAYERS + 1][LX_NAME_MAX + 16];
static const char *g_opts_none[LX_TERRAIN_LAYERS + 1], *g_opts_any[LX_TERRAIN_LAYERS + 1];
static void layer_names(LxTerrainComponent *t) {
    g_opts_none[0] = "None";
    g_opts_any[0] = "Anywhere";
    for (int l = 0; l < t->layer_count && l < LX_TERRAIN_LAYERS; l++) {
        char nm[LX_NAME_MAX];
        ed_slot_name("material", t->layers[l].material, nm, sizeof nm);
        snprintf(g_layer_names[l + 1], sizeof g_layer_names[l + 1], "%d  %s", l + 1, nm[0] ? nm : "(no material)");
        g_opts_none[l + 1] = g_opts_any[l + 1] = g_layer_names[l + 1];
    }
}
/* dropdowns over "-1 = none" values go through a proxy index */
static int g_proxy[16];
static int g_proxy_n;
static void proxy_changed(UiNode *n) {
    *(int *)n->user = *n->i - 1;
    ed_mark_changed();
    ed_commit();
}
static UiNode *layer_dropdown(UiNode *row, int *target, int count, const char *const *with_none) {
    UiNode *dd;
    if (with_none) {
        int *p = &g_proxy[g_proxy_n++ & 15];
        *p = *target + 1;
        dd = ui_dropdown(ED.ui, row, p, with_none, count + 1);
        dd->user = target;
        dd->on_change = proxy_changed;
    } else {
        dd = ui_dropdown(ED.ui, row, target, g_opts_none + 1, count);
    }
    dd->grow = 1;
    dd->h = 22;
    return dd;
}

static LxEntity g_ui_terrain;
static void gen_click(UiNode *n) {
    LX_UNUSED(n);
    if (!lx_entity_valid(ED.scene, g_ui_terrain)) return;
    lx_terrain_generate(ED.scene, g_ui_terrain, (LxTerrainPreset)g_preset, g_gen_height, (uint32_t)g_seed);
    ed_mark_changed();
    ed_commit();
    ui_notify(ED.ui, "Generated every chunk (Undo brings it back)");
}
static void layer_add(UiNode *n) {
    LX_UNUSED(n);
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t || t->layer_count >= LX_TERRAIN_LAYERS) return;
    LxTerrainLayer *l = &t->layers[t->layer_count++];
    memset(l, 0, sizeof *l);
    lx_strcpy(l->material, sizeof l->material, "builtin:terrain_dirt");
    l->tiling = 4;
    changed_commit(n);
}
static void layer_remove_last(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t || t->layer_count <= 1) return;
    t->layer_count--;
    if (t->grass_layer >= t->layer_count) t->grass_layer = -1;
    if (g_brush.layer >= t->layer_count) g_brush.layer = 0;
    for (int k = 0; k < t->scatter_count; k++) if (t->scatter[k].layer >= t->layer_count) t->scatter[k].layer = -1;
    changed_commit(n);
}
static void scatter_add(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t || t->scatter_count >= LX_TERRAIN_SCATTERS) return;
    LxTerrainScatter *sc = &t->scatter[t->scatter_count++];
    memset(sc, 0, sizeof *sc);
    sc->density = 1;
    sc->layer = -1;
    sc->scale_min = 0.8f;
    sc->scale_max = 1.3f;
    sc->max_slope = 30;
    changed_commit(n);
}
static void scatter_remove(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    int k = (int)n->tag;
    if (!t || k < 0 || k >= t->scatter_count) return;
    memmove(&t->scatter[k], &t->scatter[k + 1], sizeof t->scatter[0] * (size_t)(t->scatter_count - k - 1));
    t->scatter_count--;
    changed_commit(n);
}
static void edit_add(UiNode *n) {
    char name[32];
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t) return;
    snprintf(name, sizeof name, "Layer %d", t->edit_layer_count);
    int i = lx_terrain_add_edit_layer(ED.scene, g_ui_terrain, name);
    if (i < 0) { ui_notify(ED.ui, "A terrain has at most %d edit layers", LX_TERRAIN_EDIT_LAYERS); return; }
    t = lx_get_terrain(ED.scene, g_ui_terrain);
    t->active_edit_layer = i;
    changed_commit(n);
}
static void edit_pick(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t) return;
    t->active_edit_layer = (int)n->tag;
    changed_commit(n);
}
static void edit_remove(UiNode *n) {
    lx_terrain_remove_edit_layer(ED.scene, g_ui_terrain, (int)n->tag);
    changed_commit(n);
}
static void edit_collapse(UiNode *n) {
    lx_terrain_collapse_edit_layers(ED.scene, g_ui_terrain);
    changed_commit(n);
    ui_notify(ED.ui, "Edit layers merged into the Base");
}

void ed_terrain_section(UiNode *sec, LxEntity e) {
    UiCtx *ui = ED.ui;
    const UiTheme *th = ui_theme(ui);
    LxTerrainComponent *t = lx_get_terrain(ED.scene, e);
    if (!t) return;
    g_ui_terrain = e;
    g_proxy_n = 0;
    layer_names(t);
    if (g_brush.layer >= t->layer_count) g_brush.layer = 0;

    /* tools */
    UiNode *grid = ui_box(ui, sec, UI_ROW);
    grid->wrap = true;
    grid->gap = 4;
    ui_pad(grid, 0, 2, 0, 6);
    for (int i = 0; i < TT_COUNT; i++) {
        if (i == TT_ERASE_LAYER && t->edit_layer_count <= 1) continue;
        UiNode *b = ui_button(ui, grid, k_tools[i].name, tool_click);
        b->tag = i;
        b->h = 24;
        b->w = i >= TT_ADD_CHUNK ? 104 : 78;
        b->font = UI_FONT_SMALL;
        b->selected = g_tool == i;
        if (i == TT_ADD_CHUNK) { b->icon = UI_ICON_PLUS; b->icon_color = th->success; }
        if (i == TT_REMOVE_CHUNK) { b->icon = UI_ICON_MINUS; b->icon_color = th->danger; }
    }
    if (g_tool >= 0) {
        small_hint(sec, k_tools[g_tool].help);
        bool sculpt = g_tool != TT_ADD_CHUNK && g_tool != TT_REMOVE_CHUNK;
        if (sculpt) {
            num_row(sec, "Brush Size", &g_brush.radius, 0.25f, 0.5f, 500)->on_change = NULL;
            if (g_tool != TT_STAMP) slide_row(sec, "Strength", &g_brush.strength, 0.01f, 1, false);
            if (g_tool != TT_STAMP) slide_row(sec, "Falloff", &g_brush.falloff, 0, 1, false);
        }
        if (g_tool == TT_SET_HEIGHT) num_row(sec, "Height", &g_brush.target, 0.1f, -10000, 10000)->on_change = NULL;
        if (g_tool == TT_TERRACE) num_row(sec, "Spacing", &g_brush.spacing, 0.1f, 0.25f, 500)->on_change = NULL;
        if (g_tool == TT_PAINT) {
            UiNode *row = ed_prop_row(sec, "Layer");
            layer_dropdown(row, &g_brush.layer, t->layer_count, NULL);
        }
        if (g_tool == TT_STAMP) {
            static const char *shapes[] = {"Hill", "Mountain", "Crater", "Mesa", "Volcano", "Valley"};
            UiNode *row = ed_prop_row(sec, "Shape");
            UiNode *dd = ui_dropdown(ui, row, &g_stamp, shapes, LX_STAMP_COUNT);
            dd->grow = 1;
            dd->h = 22;
            num_row(sec, "Height", &g_stamp_height, 0.5f, -2000, 2000)->on_change = NULL;
        }
        if (sculpt) small_hint(sec, "Ctrl + scroll: brush size · Ctrl + Shift + scroll: strength · Esc: put the tool away");
        if (t->edit_layer_count > 1 && sculpt && g_tool != TT_PAINT) {
            char msg[96];
            snprintf(msg, sizeof msg, "Sculpting on edit layer \"%s\"", t->edit_layers[lx_clampi(t->active_edit_layer, 0, t->edit_layer_count - 1)].name);
            small_hint(sec, msg);
        }
    } else {
        small_hint(sec, "Pick a tool to sculpt, paint or grow the terrain in the Scene view.");
    }

    /* generate */
    static bool open_gen = false;
    UiNode *gs = ed_section(sec, UI_ICON_REFRESH, "Generate", &open_gen, NULL);
    if (open_gen) {
        static const char *presets[] = {"Flat", "Hills", "Mountains", "Island", "Canyons"};
        UiNode *row = ed_prop_row(gs, "Landscape");
        UiNode *dd = ui_dropdown(ui, row, &g_preset, presets, LX_TERRAIN_PRESET_COUNT);
        dd->grow = 1;
        dd->h = 22;
        num_row(gs, "Height", &g_gen_height, 1, 0, 5000)->on_change = NULL;
        row = ed_prop_row(gs, "Seed");
        UiNode *sd = ui_int(ui, row, &g_seed, 0, 1000000);
        sd->grow = 1;
        UiNode *go = ui_button(ui, gs, "Generate All Chunks", gen_click);
        go->icon = UI_ICON_REFRESH;
        go->h = 26;
        small_hint(gs, "Replaces the Base layer of every chunk with a landscape that runs on across chunk borders.");
    }

    /* paint layers */
    static bool open_layers = true;
    UiNode *ls = ed_section(sec, UI_ICON_LAYERS, "Paint Layers", &open_layers, NULL);
    if (open_layers) {
        for (int l = 0; l < t->layer_count; l++) {
            LxTerrainLayer *L = &t->layers[l];
            char title[16];
            snprintf(title, sizeof title, "Layer %d", l + 1);
            UiNode *hdr = ui_label(ui, ls, title);
            hdr->font = UI_FONT_BOLD;
            UiNode *row = ed_prop_row(ls, "Material");
            UiNode *sl = ed_slot(row, L->material, sizeof L->material, "material");
            sl->on_change = changed_commit;
            num_row(ls, "Tiling (m)", &L->tiling, 0.1f, 0.1f, 1000);
            pair_row(ls, "Slope (deg)", &L->slope_min, &L->slope_max, 0.5f, 0, 90);
            pair_row(ls, "Height (m)", &L->height_min, &L->height_max, 0.5f, -10000, 10000);
        }
        small_hint(ls, "Slope and Height paint a layer by themselves where the ground fits (leave max at 0 for off). "
                       "Layer 1 is what's left; layers blend by their height maps.");
        UiNode *row = ui_box(ui, ls, UI_ROW);
        row->gap = 6;
        row->h = 28;
        UiNode *a = ui_button(ui, row, "Add Layer", layer_add);
        a->icon = UI_ICON_PLUS;
        a->grow = 1;
        a->disabled = t->layer_count >= LX_TERRAIN_LAYERS;
        UiNode *r = ui_button(ui, row, "Remove Last", layer_remove_last);
        r->icon = UI_ICON_TRASH;
        r->grow = 1;
        r->disabled = t->layer_count <= 1;
    }

    /* grass & scatter */
    static bool open_grass = true;
    UiNode *gr = ed_section(sec, UI_ICON_TERRAIN, "Grass & Scatter", &open_grass, NULL);
    if (open_grass) {
        UiNode *row = ed_prop_row(gr, "Grass On");
        layer_dropdown(row, &t->grass_layer, t->layer_count, g_opts_none);
        if (t->grass_layer >= 0) {
            num_row(gr, "Density (/m2)", &t->grass_density, 0.1f, 0, 40);
            num_row(gr, "Height (m)", &t->grass_height, 0.02f, 0.05f, 5);
            UiNode *crow = ed_prop_row(gr, "Colour");
            UiNode *c = ui_color(ui, crow, &t->grass_color.x, false);
            c->grow = 1;
            c->h = 22;
            c->on_change = ed_field_changed;
            num_row(gr, "Draw Distance", &t->grass_distance, 1, 5, 1000);
        }
        for (int k = 0; k < t->scatter_count; k++) {
            LxTerrainScatter *sc = &t->scatter[k];
            UiNode *hrow = ui_box(ui, gr, UI_ROW);
            hrow->h = 24;
            char title[24];
            snprintf(title, sizeof title, "Scatter %d", k + 1);
            UiNode *hl = ui_label(ui, hrow, title);
            hl->font = UI_FONT_BOLD;
            hl->grow = 1;
            UiNode *x = ui_icon_button(ui, hrow, UI_ICON_TRASH, "Remove this scatter", scatter_remove);
            x->tag = k;
            row = ed_prop_row(gr, "Prefab");
            UiNode *sl = ed_slot(row, sc->prefab, sizeof sc->prefab, "prefab");
            sl->on_change = changed_commit;
            num_row(gr, "Per 100 m2", &sc->density, 0.05f, 0, 500);
            row = ed_prop_row(gr, "Grows On");
            layer_dropdown(row, &sc->layer, t->layer_count, g_opts_any);
            pair_row(gr, "Scale", &sc->scale_min, &sc->scale_max, 0.02f, 0.01f, 100);
            num_row(gr, "Max Slope", &sc->max_slope, 0.5f, 0, 90);
        }
        UiNode *a = ui_button(ui, gr, "Add Scatter (trees, rocks...)", scatter_add);
        a->icon = UI_ICON_PLUS;
        a->h = 26;
        a->disabled = t->scatter_count >= LX_TERRAIN_SCATTERS;
    }

    /* edit layers */
    static bool open_edit = false;
    UiNode *es = ed_section(sec, UI_ICON_LAYERS, "Edit Layers", &open_edit, NULL);
    if (open_edit) {
        for (int i = 0; i < t->edit_layer_count; i++) {
            LxTerrainEditLayer *L = &t->edit_layers[i];
            UiNode *row = ui_box(ui, es, UI_ROW);
            row->gap = 4;
            row->h = 24;
            UiNode *pick = ui_button(ui, row, i == 0 ? "Base" : "", edit_pick);
            pick->tag = i;
            pick->w = 52;
            pick->font = UI_FONT_SMALL;
            pick->selected = t->active_edit_layer == i;
            if (i > 0) ui_set_text(pick, "%s", pick->selected ? "Active" : "Use");
            if (i > 0) {
                UiNode *nm = ui_textfield(ui, row, L->name, sizeof L->name);
                nm->grow = 1;
                nm->on_change = ed_field_changed;
                UiNode *vis = ui_checkbox(ui, row, "", &L->visible);
                vis->on_change = ed_field_changed;
                lx_strcpy(vis->tooltip, sizeof vis->tooltip, "Show this layer's sculpting");
                UiNode *x = ui_icon_button(ui, row, UI_ICON_TRASH, "Delete this layer and its sculpting", edit_remove);
                x->tag = i;
                slide_row(es, "Strength", &L->strength, 0, 1, true);
            } else {
                UiNode *l = ui_label(ui, row, "Everything sculpted before layers");
                l->font = UI_FONT_SMALL;
                l->fg = th->text_dim;
                l->grow = 1;
            }
        }
        small_hint(es, "Like Unreal's Edit Layers: sculpt a road or a river bed on its own layer, then hide it, fade it "
                       "or erase it without touching the rest.");
        UiNode *row = ui_box(ui, es, UI_ROW);
        row->gap = 6;
        row->h = 28;
        UiNode *a = ui_button(ui, row, "Add Layer", edit_add);
        a->icon = UI_ICON_PLUS;
        a->grow = 1;
        a->disabled = t->edit_layer_count >= LX_TERRAIN_EDIT_LAYERS;
        UiNode *c = ui_button(ui, row, "Collapse All", edit_collapse);
        c->grow = 1;
        c->disabled = t->edit_layer_count <= 1;
    }

    /* chunks & streaming */
    static bool open_chunks = true;
    UiNode *cs = ed_section(sec, UI_ICON_GRID, "Chunks & Streaming", &open_chunks, NULL);
    if (open_chunks) {
        UiNode *info = ui_label(ui, cs, "");
        ui_set_text(info, "%d chunks  ·  %d in full detail  ·  %.0f m, %d x %d points each", lx_terrain_chunk_count(ED.scene, e),
                    lx_terrain_loaded_count(ED.scene, e), (double)t->chunk_size, t->resolution, t->resolution);
        info->font = UI_FONT_SMALL;
        info->fg = th->text_dim;
        UiNode *fr = ed_prop_row(cs, "Folder");
        UiNode *fl = ui_label(ui, fr, t->folder);
        fl->font = UI_FONT_SMALL;
        fl->grow = 1;
        num_row(cs, "Load Radius (m)", &t->load_radius, 5, 0, 100000);
        num_row(cs, "LOD Distance", &t->lod_distance, 0.05f, 0.25f, 8);
        small_hint(cs, "Chunks within the load radius of the player (or the editor camera) are in full detail with "
                       "collision; further ones draw from a light copy. 0 loads everything. Each chunk is its own file, "
                       "saved with the scene.");
    }
}

/* ---------------- Scene view ---------------- */
static LxRay g_ray;
static bool plane_hit(LxEntity e, LxRay r, LxVec3 *out) {
    LxMat4 w = lx_entity_world_matrix(ED.scene, e);
    LxVec3 o = lx_mat4_point(w, lx_v3(0, 0, 0));
    LxVec3 n = lx_norm3(lx_sub3(lx_mat4_point(w, lx_v3(0, 1, 0)), o));
    float den = lx_dot3(n, r.dir);
    if (fabsf(den) < 1e-6f) return false;
    float tt = lx_dot3(lx_sub3(o, r.origin), n) / den;
    if (tt < 0 || tt > 20000) return false;
    *out = lx_madd3(r.origin, r.dir, tt);
    return true;
}
static bool chunk_neighbour(LxEntity e, int cx, int cz) {
    return lx_terrain_has_chunk(ED.scene, e, cx - 1, cz) || lx_terrain_has_chunk(ED.scene, e, cx + 1, cz) ||
           lx_terrain_has_chunk(ED.scene, e, cx, cz - 1) || lx_terrain_has_chunk(ED.scene, e, cx, cz + 1);
}
static void finish(LxEntity e) {
    lx_terrain_end_stroke(ED.scene, e);
    T.stroking = false;
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}

void ed_terrain_viewport(LxRay ray, bool hot, bool inside) {
    LxEntity e = selected_terrain();
    g_ray = ray;
    T.has_hit = T.chunk_ok = false;
    if (!e) return;
    if (T.terrain != e) { T.terrain = e; T.stroking = T.ramping = false; }
    LxScene *s = ED.scene;
    if (hot && lx_key_pressed(LX_KEY_ESCAPE)) {
        if (T.stroking) finish(e);
        T.ramping = false;
        g_tool = -1;
        ED.inspector_dirty = true;
        return;
    }
    /* brush size / strength on the wheel */
    if (hot && lx_key_down(LX_KEY_CTRL)) {
        float wheel = lx_mouse_wheel();
        if (wheel != 0) {
            if (lx_key_down(LX_KEY_SHIFT)) g_brush.strength = lx_clampf(g_brush.strength + (wheel > 0 ? 0.05f : -0.05f), 0.01f, 1);
            else g_brush.radius = lx_clampf(g_brush.radius * (wheel > 0 ? 1.12f : 0.89f), 0.5f, 500);
        }
    }
    T.has_hit = (hot || T.stroking || T.ramping) && lx_terrain_raycast(s, e, ray, 20000, &T.hit);
    bool chunk_tool = g_tool == TT_ADD_CHUNK || g_tool == TT_REMOVE_CHUNK;
    if (chunk_tool) {
        LxVec3 p;
        bool got = g_tool == TT_REMOVE_CHUNK && T.has_hit ? (p = T.hit, true) : plane_hit(e, ray, &p);
        if (hot && got && lx_terrain_chunk_at(s, e, p, &T.ccx, &T.ccz)) {
            T.chunk_ok = true;
            bool has = lx_terrain_has_chunk(s, e, T.ccx, T.ccz);
            T.chunk_valid = g_tool == TT_ADD_CHUNK ? !has && chunk_neighbour(e, T.ccx, T.ccz) : has && lx_terrain_chunk_count(s, e) > 1;
            if (lx_mouse_pressed(LX_MOUSE_LEFT) && !lx_key_down(LX_KEY_ALT) && T.chunk_valid) {
                bool ok = g_tool == TT_ADD_CHUNK ? lx_terrain_add_chunk(s, e, T.ccx, T.ccz) : lx_terrain_remove_chunk(s, e, T.ccx, T.ccz);
                if (ok) { ed_mark_changed(); ed_commit(); ED.inspector_dirty = true; }
            }
        }
        return;
    }
    LxTerrainComponent *t = lx_get_terrain(s, e);
    if (!t) return;
    bool shift = lx_key_down(LX_KEY_SHIFT), ctrl = lx_key_down(LX_KEY_CTRL);
    /* Ctrl-click picks the height for Set Height */
    if (hot && ctrl && T.has_hit && lx_mouse_pressed(LX_MOUSE_LEFT) && (g_tool == TT_SET_HEIGHT || g_tool == TT_FLATTEN)) {
        g_brush.target = T.hit.y;
        ED.inspector_dirty = true;
        return;
    }
    if (g_tool == TT_STAMP) {
        if (hot && T.has_hit && lx_mouse_pressed(LX_MOUSE_LEFT) && !lx_key_down(LX_KEY_ALT)) {
            lx_terrain_begin_stroke(s, e);
            lx_terrain_stamp(s, e, T.hit, (LxTerrainStamp)g_stamp, g_brush.radius, g_stamp_height);
            finish(e);
        }
        return;
    }
    if (g_tool == TT_RAMP) {
        if (!T.ramping && hot && T.has_hit && lx_mouse_pressed(LX_MOUSE_LEFT) && !lx_key_down(LX_KEY_ALT)) {
            T.ramping = true;
            T.ramp_a = T.hit;
        } else if (T.ramping && !lx_mouse_down(LX_MOUSE_LEFT)) {
            T.ramping = false;
            if (T.has_hit && lx_len3(lx_sub3(T.hit, T.ramp_a)) > 0.5f) {
                lx_terrain_begin_stroke(s, e);
                lx_terrain_ramp(s, e, T.ramp_a, T.hit, g_brush.radius, g_brush.falloff);
                finish(e);
            }
        }
        return;
    }
    /* painted strokes: one undo step from press to release */
    if (!T.stroking && hot && inside && T.has_hit && lx_mouse_pressed(LX_MOUSE_LEFT) && !lx_key_down(LX_KEY_ALT)) {
        T.stroking = true;
        lx_terrain_begin_stroke(s, e);
        if (g_tool == TT_FLATTEN) g_brush.target = T.hit.y;
    }
    if (T.stroking) {
        if (!lx_mouse_down(LX_MOUSE_LEFT)) { finish(e); return; }
        if (T.has_hit) {
            LxTerrainBrush b = g_brush;
            b.tool = k_tools[g_tool].tool;
            if (g_tool == TT_FLATTEN) b.tool = LX_TERRAIN_SET_HEIGHT;   /* to the height where the stroke began */
            if (shift && b.tool == LX_TERRAIN_RAISE) b.tool = LX_TERRAIN_LOWER;
            else if (shift && b.tool == LX_TERRAIN_LOWER) b.tool = LX_TERRAIN_RAISE;
            else if (shift && b.tool == LX_TERRAIN_HOLE) b.tool = LX_TERRAIN_FILL_HOLE;
            lx_terrain_brush(s, e, T.hit, &b, ED.dt);
        }
    }
}

static void line3(UiCtx *ui, LxVec3 a, LxVec3 b, uint32_t col, float th) {
    LxVec2 sa, sb;
    if (ed_viewport_to_screen(a, &sa) && ed_viewport_to_screen(b, &sb)) ui_draw_line(ui, sa.x, sa.y, sb.x, sb.y, th * ui_scale(ui), col);
}
static float ground(float x, float z, float fallback) {
    float y;
    return lx_terrain_height(ED.scene, x, z, &y) ? y + 0.05f : fallback;
}
static void draw_square(UiCtx *ui, LxMat4 w, float size, int cx, int cz, uint32_t col, float th) {
    LxVec3 c[4] = {lx_mat4_point(w, lx_v3(cx * size, 0, cz * size)), lx_mat4_point(w, lx_v3((cx + 1) * size, 0, cz * size)),
                   lx_mat4_point(w, lx_v3((cx + 1) * size, 0, (cz + 1) * size)), lx_mat4_point(w, lx_v3(cx * size, 0, (cz + 1) * size))};
    for (int k = 0; k < 4; k++) {
        LxVec3 a = c[k], b = c[(k + 1) & 3];
        const int N = 8;   /* follow the ground along existing chunks */
        LxVec3 prev = lx_v3(a.x, ground(a.x, a.z, a.y), a.z);
        for (int i = 1; i <= N; i++) {
            float f = (float)i / N;
            LxVec3 p = lx_lerp3(a, b, f);
            p.y = ground(p.x, p.z, p.y);
            line3(ui, prev, p, col, th);
            prev = p;
        }
    }
}

void ed_terrain_draw(UiCtx *ui) {
    if (!ed_terrain_tool_active()) return;
    LxEntity e = selected_terrain();
    LxTerrainComponent *t = lx_get_terrain(ED.scene, e);
    if (!t) return;
    const UiTheme *th = ui_theme(ui);
    if (g_tool == TT_ADD_CHUNK || g_tool == TT_REMOVE_CHUNK) {
        LxMat4 w = lx_entity_world_matrix(ED.scene, e);
        /* every place a chunk could go, and the one under the mouse */
        int cmin = -64, cmax = 64;
        for (int cz = cmin; cz <= cmax; cz++)
            for (int cx = cmin; cx <= cmax; cx++) {
                bool has = lx_terrain_has_chunk(ED.scene, e, cx, cz);
                if (g_tool == TT_ADD_CHUNK && !has && chunk_neighbour(e, cx, cz)) draw_square(ui, w, t->chunk_size, cx, cz, ui_alpha(th->success, 0.35f), 1);
                if (g_tool == TT_REMOVE_CHUNK && has) draw_square(ui, w, t->chunk_size, cx, cz, ui_alpha(th->text, 0.3f), 1);
            }
        if (T.chunk_ok)
            draw_square(ui, w, t->chunk_size, T.ccx, T.ccz,
                        !T.chunk_valid ? ui_alpha(th->text_dim, 0.6f) : g_tool == TT_ADD_CHUNK ? th->success : th->danger, 2.5f);
        return;
    }
    if (!T.has_hit) return;
    bool shift = lx_key_down(LX_KEY_SHIFT);
    uint32_t col = th->accent;
    if (g_tool == TT_LOWER || g_tool == TT_HOLE || g_tool == TT_ERASE_LAYER || (g_tool == TT_RAISE && shift)) col = th->danger;
    else if (g_tool == TT_PAINT) col = th->success;
    /* the brush follows the ground */
    const int N = 48;
    for (int ring = 0; ring < 2; ring++) {
        float r = ring == 0 ? g_brush.radius : g_brush.radius * (1.0f - lx_clampf(g_brush.falloff, 0, 1) * 0.7f);
        if (ring == 1 && (g_tool == TT_STAMP || g_tool == TT_RAMP)) break;
        LxVec3 prev = lx_v3(0, 0, 0);
        for (int i = 0; i <= N; i++) {
            float a = (float)i / N * 6.2831853f;
            LxVec3 p = lx_v3(T.hit.x + cosf(a) * r, 0, T.hit.z + sinf(a) * r);
            p.y = ground(p.x, p.z, T.hit.y);
            if (i) line3(ui, prev, p, ring == 0 ? col : ui_alpha(col, 0.45f), ring == 0 ? 2 : 1.2f);
            prev = p;
        }
    }
    LxVec2 c;
    if (ed_viewport_to_screen(T.hit, &c)) ui_draw_circle(ui, c.x, c.y, 2.5f * ui_scale(ui), col, 10);
    if (g_tool == TT_RAMP && T.ramping) {
        LxVec3 a = T.ramp_a, b = T.hit;
        LxVec3 d = lx_sub3(b, a);
        d.y = 0;
        float len = lx_len3(d);
        if (len > 0.01f) {
            LxVec3 side = lx_scale3(lx_v3(-d.z / len, 0, d.x / len), g_brush.radius);
            line3(ui, lx_add3(a, side), lx_add3(b, side), col, 1.5f);
            line3(ui, lx_sub3(a, side), lx_sub3(b, side), col, 1.5f);
            line3(ui, a, b, ui_alpha(col, 0.6f), 1);
        }
    }
    if (g_tool == TT_SET_HEIGHT || g_tool == TT_FLATTEN) {
        char buf[48];
        snprintf(buf, sizeof buf, "%.1f m", (double)(g_tool == TT_FLATTEN && !T.stroking ? T.hit.y : g_brush.target));
        if (ed_viewport_to_screen(T.hit, &c)) ui_draw_text(ui, UI_FONT_SMALL, c.x + 10 * ui_scale(ui), c.y - 18 * ui_scale(ui), buf, th->text);
    }
}
