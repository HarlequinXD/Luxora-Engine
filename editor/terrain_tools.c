/* Luxora Editor: the terrain workshop. The Terrain component's inspector is split into five modes - Shape,
 * Paint, Plants, Expand and Settings - each with big picture tiles for its tools and plain-language settings,
 * and the brush itself lives in the Scene view. A brush stroke is one undo step. */
#include "editor.h"

enum {
    TT_RAISE, TT_LOWER, TT_SMOOTH, TT_FLATTEN, TT_SET_HEIGHT, TT_NOISE, TT_ERODE, TT_TERRACE, TT_PAINT, TT_HOLE,
    TT_ERASE_LAYER, TT_RAMP, TT_STAMP, TT_ADD_CHUNK, TT_REMOVE_CHUNK, TT_COUNT
};
enum { G_RAISE, G_LOWER, G_SMOOTH, G_LEVEL, G_ROUGHEN, G_ERODE, G_STEPS, G_PATH, G_LANDFORM, G_HOLE, G_ERASE, G_ADD, G_REMOVE,
       G_SHAPE, G_PAINT, G_PLANTS, G_EXPAND, G_SETTINGS };
static const struct { const char *name; int tool, glyph; const char *help; } k_tools[TT_COUNT] = {
    {"Raise", LX_TERRAIN_RAISE, G_RAISE, "Drag over the ground to build it up. Hold Shift to dig down instead."},
    {"Lower", LX_TERRAIN_LOWER, G_LOWER, "Drag to dig the ground down. Hold Shift to build up instead."},
    {"Smooth", LX_TERRAIN_SMOOTH, G_SMOOTH, "Drag over bumps and sharp edges to soften them."},
    {"Level", LX_TERRAIN_FLATTEN, G_LEVEL, "Drag to make the ground flat, at the height where you start."},
    {"Level", LX_TERRAIN_SET_HEIGHT, G_LEVEL, "Drag to make the ground flat at the height below. Ctrl-click the ground to copy its height."},
    {"Roughen", LX_TERRAIN_NOISE, G_ROUGHEN, "Drag to add natural lumps and bumps."},
    {"Erode", LX_TERRAIN_ERODE, G_ERODE, "Drag to let rain wash the hills down, carving gullies and soft slopes."},
    {"Steps", LX_TERRAIN_TERRACE, G_STEPS, "Drag to cut hillsides into flat steps."},
    {"Paint", LX_TERRAIN_PAINT, G_PAINT, "Drag over the ground to paint the chosen texture."},
    {"Hole", LX_TERRAIN_HOLE, G_HOLE, "Drag to cut a hole through the ground for a cave or tunnel. Hold Shift to close it."},
    {"Erase", LX_TERRAIN_ERASE_LAYER, G_ERASE, "Drag to rub out the chosen sculpt layer's work under the brush."},
    {"Path", -1, G_PATH, "Press where the path starts and drag to where it ends: a smooth slope like a road between them."},
    {"Landform", -1, G_LANDFORM, "Click the ground to drop a whole landform, as big as the brush."},
    {"Add", -1, G_ADD, "Click one of the green squares beside your terrain to add more land there. It blends into the land beside it."},
    {"Remove", -1, G_REMOVE, "Click a square of land to remove it."},
};
enum { MODE_SHAPE, MODE_PAINT, MODE_PLANTS, MODE_EXPAND, MODE_SETTINGS, MODE_COUNT };
static const struct { const char *name; int glyph; } k_modes[MODE_COUNT] = {
    {"Shape", G_SHAPE}, {"Paint", G_PAINT}, {"Plants", G_PLANTS}, {"Expand", G_EXPAND}, {"Settings", G_SETTINGS}};

static int g_mode = MODE_SHAPE;
static int g_tool = -1;   /* -1 = no tool: the normal gizmo and selection */
static int g_shape_tool = TT_RAISE;   /* the Shape tool to come back to */
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
    g_mode = MODE_SHAPE;
    g_tool = g_shape_tool = TT_RAISE;
    return e;
}

/* ---------------- drawings for the tiles ---------------- */
static void pline(UiCtx *ui, const float *pts, int n, float x, float y, float s, float th, uint32_t c) {
    for (int i = 0; i + 1 < n; i++) ui_draw_line(ui, x + pts[i * 2] * s, y + pts[i * 2 + 1] * s, x + pts[i * 2 + 2] * s, y + pts[i * 2 + 3] * s, th, c);
}
static void arrow(UiCtx *ui, float x, float y, float s, float cx, float y0, float y1, float th, uint32_t c) {
    ui_draw_line(ui, x + cx * s, y + y0 * s, x + cx * s, y + y1 * s, th, c);
    float d = y1 < y0 ? 1 : -1;   /* head at y1 */
    ui_draw_triangle(ui, x + (cx - 0.12f) * s, y + (y1 + d * 0.12f) * s, x + (cx + 0.12f) * s, y + (y1 + d * 0.12f) * s, x + cx * s, y + (y1 - d * 0.02f) * s, c);
}
/* mound outlines over a ground line */
static void mound(UiCtx *ui, float x, float y, float s, float top, float th, uint32_t c, bool flat) {
    float pts[2 * 13];
    for (int i = 0; i <= 12; i++) {
        float u = (float)i / 12, v = 0.5f - 0.5f * cosf(u * 2 * LX_PI);
        float hgt = flat ? lx_minf(v * 1.6f, 1) : v;
        pts[i * 2] = 0.08f + u * 0.84f;
        pts[i * 2 + 1] = 0.82f - hgt * (0.82f - top);
    }
    pline(ui, pts, 13, x, y, s, th, c);
}
static void glyph(UiCtx *ui, int g, float x, float y, float s, uint32_t c, uint32_t dim) {
    float th = lx_maxf(1.5f, s * 0.07f);
    switch (g) {
    case G_RAISE:
        mound(ui, x, y, s, 0.48f, th, c, false);
        arrow(ui, x, y, s, 0.5f, 0.42f, 0.08f, th, c);
        break;
    case G_LOWER: {
        float pts[] = {0.06f, 0.5f, 0.28f, 0.5f, 0.4f, 0.82f, 0.6f, 0.82f, 0.72f, 0.5f, 0.94f, 0.5f};
        pline(ui, pts, 6, x, y, s, th, c);
        arrow(ui, x, y, s, 0.5f, 0.06f, 0.42f, th, c);
        break;
    }
    case G_SMOOTH: {
        float a[14], b[] = {0.08f, 0.72f, 0.92f, 0.72f};
        for (int i = 0; i < 7; i++) { a[i * 2] = 0.08f + i * 0.14f; a[i * 2 + 1] = i & 1 ? 0.22f : 0.46f; }
        pline(ui, a, 7, x, y, s, th * 0.8f, dim);
        pline(ui, b, 2, x, y, s, th, c);
        ui_draw_triangle(ui, x + 0.42f * s, y + 0.5f * s, x + 0.58f * s, y + 0.5f * s, x + 0.5f * s, y + 0.62f * s, c);
        break;
    }
    case G_LEVEL:
        mound(ui, x, y, s, 0.3f, th, c, true);
        ui_draw_line(ui, x + 0.04f * s, y + 0.3f * s, x + 0.96f * s, y + 0.3f * s, th * 0.6f, dim);
        break;
    case G_ROUGHEN: {
        float pts[] = {0.06f, 0.78f, 0.16f, 0.5f, 0.26f, 0.64f, 0.36f, 0.3f, 0.46f, 0.56f, 0.56f, 0.2f, 0.66f, 0.5f, 0.76f, 0.36f, 0.86f, 0.62f, 0.94f, 0.78f};
        pline(ui, pts, 10, x, y, s, th, c);
        break;
    }
    case G_ERODE:
        mound(ui, x, y, s, 0.22f, th, c, false);
        for (int i = 0; i < 3; i++) {
            float u = 0.32f + i * 0.18f;
            ui_draw_line(ui, x + u * s, y + 0.06f * s, x + (u - 0.04f) * s, y + 0.2f * s, th * 0.7f, dim);
        }
        break;
    case G_STEPS: {
        float pts[] = {0.06f, 0.84f, 0.3f, 0.84f, 0.3f, 0.62f, 0.52f, 0.62f, 0.52f, 0.4f, 0.74f, 0.4f, 0.74f, 0.18f, 0.94f, 0.18f};
        pline(ui, pts, 8, x, y, s, th, c);
        break;
    }
    case G_PATH: {
        ui_draw_line(ui, x + 0.1f * s, y + 0.9f * s, x + 0.42f * s, y + 0.1f * s, th, c);
        ui_draw_line(ui, x + 0.9f * s, y + 0.9f * s, x + 0.58f * s, y + 0.1f * s, th, c);
        for (int i = 0; i < 3; i++) ui_draw_line(ui, x + 0.5f * s, y + (0.78f - i * 0.26f) * s, x + 0.5f * s, y + (0.66f - i * 0.26f) * s, th * 0.7f, dim);
        break;
    }
    case G_LANDFORM:
        ui_draw_triangle(ui, x + 0.5f * s, y + 0.14f * s, x + 0.9f * s, y + 0.78f * s, x + 0.1f * s, y + 0.78f * s, c);
        ui_draw_triangle(ui, x + 0.5f * s, y + 0.14f * s, x + 0.6f * s, y + 0.3f * s, x + 0.4f * s, y + 0.3f * s, UI_HEX(0xf4f6fa));
        ui_draw_line(ui, x + 0.02f * s, y + 0.86f * s, x + 0.98f * s, y + 0.86f * s, th * 0.6f, dim);
        break;
    case G_HOLE:
        ui_draw_line(ui, x + 0.04f * s, y + 0.5f * s, x + 0.96f * s, y + 0.5f * s, th, c);
        ui_draw_circle(ui, x + 0.5f * s, y + 0.5f * s, 0.24f * s, c, 20);
        ui_draw_circle(ui, x + 0.5f * s, y + 0.54f * s, 0.17f * s, UI_HEX(0x101216), 18);
        break;
    case G_ERASE:
        ui_draw_rect(ui, x + 0.22f * s, y + 0.3f * s, 0.56f * s, 0.34f * s, c, 0.06f * s);
        ui_draw_rect(ui, x + 0.22f * s, y + 0.3f * s, 0.2f * s, 0.34f * s, dim, 0.06f * s);
        ui_draw_line(ui, x + 0.1f * s, y + 0.82f * s, x + 0.9f * s, y + 0.82f * s, th * 0.6f, dim);
        break;
    case G_ADD: case G_REMOVE: case G_EXPAND: {
        float q = 0.36f;
        for (int k = 0; k < 4; k++) {
            float gx = 0.1f + (k & 1) * (q + 0.04f), gy = 0.1f + (k >> 1) * (q + 0.04f);
            bool hi = k == 3;
            if (hi && g == G_ADD) ui_draw_border(ui, x + gx * s, y + gy * s, q * s, q * s, c, 0.05f * s, th * 0.6f);
            else ui_draw_rect(ui, x + gx * s, y + gy * s, q * s, q * s, hi && g == G_REMOVE ? ui_alpha(c, 0.35f) : hi ? c : dim, 0.05f * s);
        }
        float cx = 0.1f + 1.5f * q + 0.04f, cy = cx;
        if (g != G_REMOVE) ui_draw_line(ui, x + cx * s, y + (cy - 0.1f) * s, x + cx * s, y + (cy + 0.1f) * s, th, g == G_EXPAND ? UI_HEX(0x101216) : c);
        ui_draw_line(ui, x + (cx - 0.1f) * s, y + cy * s, x + (cx + 0.1f) * s, y + cy * s, th, g == G_EXPAND ? UI_HEX(0x101216) : c);
        break;
    }
    case G_SHAPE:
        ui_draw_icon(ui, UI_ICON_TERRAIN, x, y, s, c);
        break;
    case G_PAINT:
        ui_draw_rect(ui, x + 0.56f * s, y + 0.08f * s, 0.14f * s, 0.46f * s, dim, 0.05f * s);
        ui_draw_rect(ui, x + 0.5f * s, y + 0.5f * s, 0.26f * s, 0.14f * s, c, 0.03f * s);
        ui_draw_triangle(ui, x + 0.5f * s, y + 0.64f * s, x + 0.76f * s, y + 0.64f * s, x + 0.56f * s, y + 0.92f * s, c);
        ui_draw_rect(ui, x + 0.08f * s, y + 0.76f * s, 0.4f * s, 0.12f * s, ui_alpha(c, 0.5f), 0.06f * s);
        break;
    case G_PLANTS:
        for (int i = 0; i < 5; i++) {
            float u = 0.18f + i * 0.16f, lean = (i - 2) * 0.07f, tall = i & 1 ? 0.3f : 0.12f;
            ui_draw_line(ui, x + u * s, y + 0.88f * s, x + (u + lean) * s, y + tall * s, th, i == 2 ? c : ui_alpha(c, 0.75f));
        }
        break;
    case G_SETTINGS:
        ui_draw_icon(ui, UI_ICON_GEAR, x, y, s, c);
        break;
    }
}

/* ---------------- inspector ---------------- */
static LxEntity g_ui_terrain;
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
static UiNode *card(UiNode *parent, const char *title) {
    const UiTheme *th = ui_theme(ED.ui);
    UiNode *c = ui_box(ED.ui, parent, UI_COL);
    c->bg = th->panel_alt;
    c->border = th->border;
    c->radius = 8;
    c->gap = 4;
    ui_pad(c, 10, 8, 10, 10);
    if (title) {
        UiNode *t = ui_label(ED.ui, c, title);
        t->font = UI_FONT_BOLD;
    }
    return c;
}
static UiNode *num_row(UiNode *parent, const char *label, float *v, float step, float minv, float maxv, bool scene_field) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *n = ui_number(ED.ui, row, v, step);
    n->grow = 1;
    n->h = 22;
    n->minv = minv;
    n->maxv = maxv;
    if (scene_field) n->on_change = ed_field_changed;
    return n;
}
static UiNode *pair_row(UiNode *parent, const char *label, float *a, float *b, float step, float minv, float maxv) {
    UiNode *row = ed_prop_row(parent, label);
    for (int k = 0; k < 2; k++) {
        if (k) {
            UiNode *to = ui_label(ED.ui, row, "to");
            to->font = UI_FONT_SMALL;
            to->fg = ui_theme(ED.ui)->text_dim;
        }
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
static UiNode *dropdown_row(UiNode *parent, const char *label, int *v, const char *const *opts, int count) {
    UiNode *row = ed_prop_row(parent, label);
    UiNode *dd = ui_dropdown(ED.ui, row, v, opts, count);
    dd->grow = 1;
    dd->h = 22;
    return dd;
}

/* a picture tile: drawing on top, name below */
static void tile_draw(UiNode *n, UiCtx *ui) {
    const UiTheme *th = ui_theme(ui);
    float sc = ui_scale(ui);
    bool on = n->selected;
    uint32_t c = on ? th->accent : n->disabled ? th->text_disabled : th->text;
    if (on) ui_draw_border(ui, n->x, n->y, n->cw, n->ch, th->accent, 6 * sc, lx_maxf(1.5f, 1.5f * sc));
    float s = lx_minf(n->cw * 0.5f, n->ch - 22 * sc);
    glyph(ui, (int)n->tag, n->x + (n->cw - s) * 0.5f, n->y + 6 * sc, s, c, ui_alpha(c, 0.45f));
    float tw = ui_text_width(ui, UI_FONT_SMALL, n->text);
    ui_draw_text(ui, UI_FONT_SMALL, n->x + (n->cw - tw) * 0.5f, n->y + n->ch - 17 * sc, n->text, c);
}
static void tile_sel_draw(UiNode *n, UiCtx *ui) {
    n->selected = n->path[0] != 0;
    tile_draw(n, ui);
    n->selected = false;
}
static UiNode *tile(UiNode *parent, const char *name, int glyph_id, bool selected, float w, float h, void (*click)(UiNode *)) {
    const UiTheme *th = ui_theme(ED.ui);
    UiNode *b = ui_box(ED.ui, parent, UI_COL);
    lx_strcpy(b->text, sizeof b->text, name);
    b->tag = glyph_id;
    b->w = w;
    b->h = h;
    b->bg = selected ? ui_alpha(th->accent, 0.16f) : th->field;
    b->bg_hover = selected ? ui_alpha(th->accent, 0.22f) : th->field_hover;
    b->radius = 6;
    b->on_click = click;
    b->on_draw = tile_sel_draw;
    b->path[0] = (char)selected;   /* tile_draw reads selected from here (selected would paint the selection colour) */
    return b;
}

static void mode_click(UiNode *n) {
    int m = (int)n->path[1];
    if (m == g_mode) return;
    g_mode = m;
    if (m == MODE_SHAPE) g_tool = g_shape_tool;
    else if (m == MODE_PAINT) g_tool = TT_PAINT;
    else if (m == MODE_EXPAND) g_tool = TT_ADD_CHUNK;
    else g_tool = -1;
    ED.inspector_dirty = true;
}
static void tool_click(UiNode *n) {
    int t = (int)n->path[1];
    bool same = g_tool == t || (t == TT_FLATTEN && g_tool == TT_SET_HEIGHT);
    g_tool = same ? -1 : t;   /* clicking the chosen tool puts it away */
    if (g_tool >= 0 && g_mode == MODE_SHAPE) g_shape_tool = g_tool;
    ED.inspector_dirty = true;
}
static UiNode *tool_tile(UiNode *grid, int t, float w) {
    bool sel = g_tool == t || (t == TT_FLATTEN && g_tool == TT_SET_HEIGHT);
    UiNode *b = tile(grid, k_tools[t].name, k_tools[t].glyph, sel, w, 62, tool_click);
    b->path[1] = (char)t;
    b->on_draw = tile_sel_draw;
    lx_strcpy(b->tooltip, sizeof b->tooltip, k_tools[t].help);
    return b;
}

/* ---------------- ground textures ---------------- */
static void layer_name(const LxTerrainLayer *L, char *out, size_t cap) {
    const char *ref = L->texture[0] ? L->texture : L->material;
    if (!ref[0]) { lx_strcpy(out, cap, "Grass"); return; }
    if (!strncmp(ref, "builtin:terrain_", 16)) {
        lx_strcpy(out, cap, ref + 16);
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
        return;
    }
    const char *slash = strrchr(ref, '/');
    lx_strcpy(out, cap, slash ? slash + 1 : ref);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    for (char *p = out; *p; p++) if (*p == '_' || *p == '-') *p = ' ';
}
static uint32_t builtin_colour(const char *ref) {
    if (strstr(ref, "rock")) return UI_HEX(0x7a7570);
    if (strstr(ref, "dirt")) return UI_HEX(0x7b5a3c);
    if (strstr(ref, "snow")) return UI_HEX(0xe9eef4);
    if (strstr(ref, "sand")) return UI_HEX(0xd2bd8a);
    return UI_HEX(0x4f7a2a);
}
/* the swatch: the texture's picture, the material's preview, or the built-in ground's colour */
static void swatch_draw(UiCtx *ui, const LxTerrainLayer *L, float x, float y, float s) {
    const UiTheme *th = ui_theme(ui);
    uint32_t tex = 0;
    if (L->texture[0]) {
        char path[LX_PATH_MAX];
        lx_asset_path(path, sizeof path, L->texture);
        float aspect = 1;
        tex = ed_thumb_get(path, lx_file_size(path), 128, &aspect, NULL, NULL);
    } else if (L->material[0] && strncmp(L->material, "builtin:", 8)) tex = ed_preview_get("material", L->material);
    if (tex) ui_draw_image(ui, tex, x, y, s, s);
    else {
        uint32_t c = builtin_colour(L->texture[0] ? "" : L->material);
        ui_draw_rect(ui, x, y, s, s, c, 4 * ui_scale(ui));
        /* a little speckle so it reads as ground, not a flat colour */
        for (int i = 0; i < 7; i++) {
            float u = 0.15f + 0.7f * (float)((i * 37) % 11) / 10.0f, v = 0.15f + 0.7f * (float)((i * 53) % 7) / 6.0f;
            ui_draw_circle(ui, x + u * s, y + v * s, s * 0.05f, ui_alpha(th->window, 0.25f), 6);
        }
    }
    ui_draw_border(ui, x, y, s, s, ui_alpha(th->text, 0.2f), 4 * ui_scale(ui), 1);
}
static void layer_row_draw(UiNode *n, UiCtx *ui) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    int l = (int)n->tag;
    if (!t || l >= t->layer_count) return;
    const UiTheme *th = ui_theme(ui);
    float sc = ui_scale(ui), s = n->ch - 10 * sc;
    bool on = n->path[0] != 0;
    if (on) ui_draw_border(ui, n->x, n->y, n->cw, n->ch, th->accent, 6 * sc, lx_maxf(1.5f, 1.5f * sc));
    swatch_draw(ui, &t->layers[l], n->x + 5 * sc, n->y + 5 * sc, s);
    char name[64];
    layer_name(&t->layers[l], name, sizeof name);
    float tx = n->x + s + 14 * sc;
    ui_draw_text(ui, UI_FONT_BOLD, tx, n->y + 7 * sc, name, on ? th->accent : th->text);
    const LxTerrainLayer *L = &t->layers[l];
    char sub[96];
    bool slope = L->slope_max > L->slope_min, high = L->height_max > L->height_min;
    if (l == 0) snprintf(sub, sizeof sub, "Everywhere else");
    else if (slope && high) snprintf(sub, sizeof sub, "Steep ground up high, and painted");
    else if (slope) snprintf(sub, sizeof sub, "Slopes %.0f-%.0f°, and painted", (double)L->slope_min, (double)L->slope_max);
    else if (high) snprintf(sub, sizeof sub, "Above %.0f m, and painted", (double)L->height_min);
    else snprintf(sub, sizeof sub, "Where you paint it");
    if (l == t->grass_layer) {
        size_t len = strlen(sub);
        snprintf(sub + len, sizeof sub - len, "  ·  grass grows here");
    }
    ui_draw_text(ui, UI_FONT_SMALL, tx, n->y + n->ch - 19 * sc, sub, th->text_dim);
}
static void layer_click(UiNode *n) {
    g_brush.layer = (int)n->tag;
    g_tool = TT_PAINT;
    ED.inspector_dirty = true;
}
static void layer_add(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t || t->layer_count >= LX_TERRAIN_LAYERS) return;
    static const char *next[] = {"builtin:terrain_grass", "builtin:terrain_rock", "builtin:terrain_dirt", "builtin:terrain_snow", "builtin:terrain_sand", "builtin:terrain_dirt"};
    LxTerrainLayer *l = &t->layers[t->layer_count];
    memset(l, 0, sizeof *l);
    lx_strcpy(l->material, sizeof l->material, next[t->layer_count]);
    l->tiling = 4;
    g_brush.layer = t->layer_count++;
    g_tool = TT_PAINT;
    changed_commit(n);
}
static void layer_delete(UiNode *n) {
    lx_terrain_remove_layer(ED.scene, g_ui_terrain, g_brush.layer);
    if (g_brush.layer > 0) g_brush.layer--;
    changed_commit(n);
}
static bool g_auto_slope, g_auto_height;
static void auto_slope_changed(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t || g_brush.layer >= t->layer_count) return;
    LxTerrainLayer *L = &t->layers[g_brush.layer];
    if (g_auto_slope) { L->slope_min = 35; L->slope_max = 90; }
    else L->slope_min = L->slope_max = 0;
    changed_commit(n);
}
static void auto_height_changed(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t || g_brush.layer >= t->layer_count) return;
    LxTerrainLayer *L = &t->layers[g_brush.layer];
    if (g_auto_height) { L->height_min = 150; L->height_max = 100000; }
    else L->height_min = L->height_max = 0;
    changed_commit(n);
}

/* ---------------- plants ---------------- */
static char g_layer_names[LX_TERRAIN_LAYERS][64];
static const char *g_layer_opts[LX_TERRAIN_LAYERS + 1];   /* [0] = "Anywhere" */
static void fill_layer_opts(LxTerrainComponent *t) {
    g_layer_opts[0] = "Anywhere";
    for (int l = 0; l < t->layer_count && l < LX_TERRAIN_LAYERS; l++) {
        layer_name(&t->layers[l], g_layer_names[l], sizeof g_layer_names[l]);
        g_layer_opts[l + 1] = g_layer_names[l];
    }
}
static int g_proxy[LX_TERRAIN_SCATTERS + 2];
static void proxy_changed(UiNode *n) {   /* dropdowns over "-1 = anywhere" values */
    *(int *)n->user = *n->i - 1;
    changed_commit(n);
}
static bool g_grass_on;
static int g_grass_pick;
static void grass_on_changed(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t) return;
    t->grass_layer = g_grass_on ? 0 : -1;
    if (g_grass_on && t->grass_density <= 0.01f) t->grass_density = 3;
    changed_commit(n);
}
static void grass_layer_changed(UiNode *n) {
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t) return;
    t->grass_layer = g_grass_pick;
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

/* ---------------- sculpt layers and new landscapes ---------------- */
static void gen_click(UiNode *n) {
    LX_UNUSED(n);
    if (!lx_entity_valid(ED.scene, g_ui_terrain)) return;
    lx_terrain_generate(ED.scene, g_ui_terrain, (LxTerrainPreset)g_preset, g_gen_height, (uint32_t)g_seed);
    ed_mark_changed();
    ed_commit();
    ui_notify(ED.ui, "New landscape made (Undo brings back the old one)");
}
static void seed_click(UiNode *n) {
    g_seed = (int)(((uint32_t)g_seed * 1103515245u + 12345u) % 999999u) + 1;
    gen_click(n);
    ED.inspector_dirty = true;
}
static void edit_add(UiNode *n) {
    char name[32];
    LxTerrainComponent *t = lx_get_terrain(ED.scene, g_ui_terrain);
    if (!t) return;
    snprintf(name, sizeof name, "Layer %d", t->edit_layer_count);
    int i = lx_terrain_add_edit_layer(ED.scene, g_ui_terrain, name);
    if (i < 0) { ui_notify(ED.ui, "A terrain has at most %d sculpt layers", LX_TERRAIN_EDIT_LAYERS); return; }
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
    ui_notify(ED.ui, "Sculpt layers merged into the ground");
}

static void tool_tip(UiNode *parent, const char *text) {
    const UiTheme *th = ui_theme(ED.ui);
    UiNode *tip = ui_box(ED.ui, parent, UI_ROW);
    tip->bg = ui_alpha(th->accent, 0.1f);
    tip->radius = 6;
    tip->gap = 6;
    ui_pad(tip, 8, 6, 8, 6);
    UiNode *ic = ui_label(ED.ui, tip, "");
    ic->icon = UI_ICON_INFO;
    ic->icon_color = th->accent;
    ic->w = 16;
    UiNode *l = ui_label(ED.ui, tip, text);
    l->wrap = true;
    l->grow = 1;
    l->font = UI_FONT_SMALL;
}
static UiNode *grid_of(UiNode *parent) {
    UiNode *g = ui_box(ED.ui, parent, UI_ROW);
    g->wrap = true;
    g->gap = 6;
    return g;
}
static void brush_card(UiNode *parent, bool strength) {
    UiNode *c = card(parent, "Brush");
    num_row(c, "Size (m)", &g_brush.radius, 0.25f, 0.5f, 500, false);
    if (strength) slide_row(c, "Strength", &g_brush.strength, 0.01f, 1, false);
    if (strength) slide_row(c, "Softness", &g_brush.falloff, 0, 1, false);
    small_hint(c, "Ctrl + mouse wheel changes the size, Ctrl + Shift + wheel the strength. Esc puts the brush away.");
}

static void level_mode_changed(UiNode *n) {
    g_tool = g_shape_tool = *n->i ? TT_SET_HEIGHT : TT_FLATTEN;
    ED.inspector_dirty = true;
}

static void shape_mode(UiNode *sec, LxTerrainComponent *t) {
    UiCtx *ui = ED.ui;
    const UiTheme *th = ui_theme(ui);
    UiNode *g = grid_of(sec);
    static const int order[] = {TT_RAISE, TT_LOWER, TT_SMOOTH, TT_FLATTEN, TT_NOISE, TT_ERODE, TT_TERRACE, TT_RAMP, TT_STAMP, TT_HOLE, TT_ERASE_LAYER};
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        if (order[i] == TT_ERASE_LAYER && t->edit_layer_count <= 1) continue;
        tool_tile(g, order[i], 66);
    }
    if (g_tool >= 0 && g_tool != TT_PAINT) {
        tool_tip(sec, k_tools[g_tool].help);
        if (g_tool == TT_FLATTEN || g_tool == TT_SET_HEIGHT) {
            UiNode *c = card(sec, "Level to");
            static int level_mode;
            level_mode = g_tool == TT_SET_HEIGHT;
            static const char *opts[] = {"Where I start dragging", "A height I choose"};
            UiNode *dd = dropdown_row(c, "Height", &level_mode, opts, 2);
            dd->on_change = level_mode_changed;   /* the two ways are two tools */
            if (level_mode) num_row(c, "Metres", &g_brush.target, 0.1f, -10000, 10000, false);
        }
        if (g_tool == TT_TERRACE) {
            UiNode *c = card(sec, "Steps");
            num_row(c, "Step height (m)", &g_brush.spacing, 0.1f, 0.25f, 500, false);
        }
        if (g_tool == TT_STAMP) {
            UiNode *c = card(sec, "Landform");
            static const char *shapes[] = {"Hill", "Mountain", "Crater", "Plateau", "Volcano", "Valley"};
            dropdown_row(c, "Kind", &g_stamp, shapes, LX_STAMP_COUNT);
            num_row(c, "Height (m)", &g_stamp_height, 0.5f, -2000, 2000, false);
        }
        if (g_tool == TT_RAMP) {
            UiNode *c = card(sec, "Path");
            num_row(c, "Half width (m)", &g_brush.radius, 0.25f, 0.5f, 500, false);
            slide_row(c, "Edge softness", &g_brush.falloff, 0, 1, false);
        } else {
            brush_card(sec, g_tool != TT_STAMP);
        }
    } else {
        small_hint(sec, "Pick a tool, then drag over the terrain in the Scene view.");
    }

    /* sculpt layers */
    static bool open_layers = false;
    UiNode *ls = ed_section(sec, UI_ICON_LAYERS, "Sculpt Layers", &open_layers, NULL);
    if (open_layers) {
        small_hint(ls, "Keep work like a road or a river bed on its own layer, so you can hide it, fade it or rub it out "
                       "later without touching the rest. The tools shape the layer that's switched on.");
        for (int i = 0; i < t->edit_layer_count; i++) {
            LxTerrainEditLayer *L = &t->edit_layers[i];
            UiNode *row = ui_box(ui, ls, UI_ROW);
            row->gap = 4;
            row->h = 26;
            UiNode *pick = ui_icon_button(ui, row, t->active_edit_layer == i ? UI_ICON_CHECK : UI_ICON_NONE,
                                          "Shape this layer", edit_pick);
            pick->tag = i;
            pick->selected = t->active_edit_layer == i;
            if (i == 0) {
                UiNode *l = ui_label(ui, row, "Ground");
                l->grow = 1;
                continue;
            }
            UiNode *nm = ui_textfield(ui, row, L->name, sizeof L->name);
            nm->grow = 1;
            nm->on_change = ed_field_changed;
            UiNode *vis = ui_checkbox(ui, row, "", &L->visible);
            vis->on_change = ed_field_changed;
            lx_strcpy(vis->tooltip, sizeof vis->tooltip, "Show this layer");
            UiNode *x = ui_icon_button(ui, row, UI_ICON_TRASH, "Delete this layer and its work", edit_remove);
            x->tag = i;
            slide_row(ls, "Amount", &L->strength, 0, 1, true);
        }
        UiNode *row = ui_box(ui, ls, UI_ROW);
        row->gap = 6;
        row->h = 28;
        UiNode *a = ui_button(ui, row, "New Layer", edit_add);
        a->icon = UI_ICON_PLUS;
        a->grow = 1;
        a->disabled = t->edit_layer_count >= LX_TERRAIN_EDIT_LAYERS;
        UiNode *c = ui_button(ui, row, "Merge All", edit_collapse);
        c->grow = 1;
        c->disabled = t->edit_layer_count <= 1;
    }

    /* a new landscape */
    static bool open_gen = false;
    UiNode *gs = ed_section(sec, UI_ICON_REFRESH, "Make a Landscape", &open_gen, NULL);
    if (open_gen) {
        static const char *presets[] = {"Flat", "Rolling hills", "Mountains", "Island", "Canyons"};
        dropdown_row(gs, "Style", &g_preset, presets, LX_TERRAIN_PRESET_COUNT);
        num_row(gs, "Height (m)", &g_gen_height, 1, 0, 5000, false);
        UiNode *row = ed_prop_row(gs, "Variation");
        UiNode *sd = ui_int(ui, row, &g_seed, 0, 1000000);
        sd->grow = 1;
        UiNode *bt = ui_box(ui, gs, UI_ROW);
        bt->gap = 6;
        bt->h = 28;
        UiNode *go = ui_button(ui, bt, "Make It", gen_click);
        go->icon = UI_ICON_CHECK;
        go->grow = 1;
        UiNode *rn = ui_button(ui, bt, "Surprise Me", seed_click);
        rn->icon = UI_ICON_REFRESH;
        rn->grow = 1;
        small_hint(gs, "Replaces the shape of all your land. Paint and plants stay. Undo brings the old shape back.");
    }
    LX_UNUSED(th);
}


static void paint_mode(UiNode *sec, LxTerrainComponent *t) {
    UiCtx *ui = ED.ui;
    if (g_brush.layer >= t->layer_count) g_brush.layer = 0;
    small_hint(sec, "Click a ground texture, then paint it onto the terrain. To change how one looks, pick a new image for it below.");
    for (int l = 0; l < t->layer_count; l++) {
        const UiTheme *th = ui_theme(ui);
        UiNode *r = ui_box(ui, sec, UI_ROW);
        r->h = 50;
        r->tag = l;
        bool sel = g_brush.layer == l;
        r->bg = sel ? ui_alpha(th->accent, 0.14f) : th->field;
        r->bg_hover = sel ? ui_alpha(th->accent, 0.2f) : th->field_hover;
        r->radius = 6;
        r->path[0] = (char)sel;
        r->on_click = layer_click;
        r->on_draw = layer_row_draw;
    }
    UiNode *row = ui_box(ui, sec, UI_ROW);
    row->gap = 6;
    row->h = 28;
    UiNode *a = ui_button(ui, row, "Add Texture", layer_add);
    a->icon = UI_ICON_PLUS;
    a->grow = 1;
    a->disabled = t->layer_count >= LX_TERRAIN_LAYERS;

    /* the chosen texture */
    LxTerrainLayer *L = &t->layers[g_brush.layer];
    char name[64], title[96];
    layer_name(L, name, sizeof name);
    snprintf(title, sizeof title, "%s", name);
    UiNode *c = card(sec, title);
    UiNode *ir = ed_prop_row(c, "Image");
    UiNode *sl = ed_slot(ir, L->texture, sizeof L->texture, "texture");
    sl->on_change = changed_commit;
    if (L->texture[0]) {
        UiNode *nr = ed_prop_row(c, "Bumps (optional)");
        UiNode *ns = ed_slot(nr, L->normal_map, sizeof L->normal_map, "texture");
        ns->on_change = changed_commit;
    } else {
        UiNode *mr = ed_prop_row(c, "Or a Material");
        UiNode *ms = ed_slot(mr, L->material, sizeof L->material, "material");
        ms->on_change = changed_commit;
    }
    num_row(c, "Pattern size (m)", &L->tiling, 0.1f, 0.1f, 1000, true);
    if (g_brush.layer > 0) {
        g_auto_slope = L->slope_max > L->slope_min;
        g_auto_height = L->height_max > L->height_min;
        UiNode *cb = ui_checkbox(ui, c, "Covers slopes by itself", &g_auto_slope);
        cb->on_change = auto_slope_changed;
        if (g_auto_slope) pair_row(c, "Slopes (°)", &L->slope_min, &L->slope_max, 0.5f, 0, 90);
        cb = ui_checkbox(ui, c, "Covers high ground by itself", &g_auto_height);
        cb->on_change = auto_height_changed;
        if (g_auto_height) num_row(c, "Above (m)", &L->height_min, 1, -10000, 10000, true);
        UiNode *d = ui_button(ui, c, "Delete This Texture", layer_delete);
        d->icon = UI_ICON_TRASH;
        d->h = 26;
        lx_strcpy(d->tooltip, sizeof d->tooltip, "Where it was painted goes back to the first texture");
    } else {
        small_hint(c, "The first texture covers all the ground nothing else does.");
    }
    if (g_tool == TT_PAINT) brush_card(sec, true);
}

static void plants_mode(UiNode *sec, LxTerrainComponent *t) {
    UiCtx *ui = ED.ui;
    fill_layer_opts(t);
    UiNode *c = card(sec, "Grass");
    g_grass_on = t->grass_layer >= 0;
    UiNode *cb = ui_checkbox(ui, c, "Grow grass", &g_grass_on);
    cb->on_change = grass_on_changed;
    if (g_grass_on) {
        g_grass_pick = t->grass_layer;
        UiNode *dd = dropdown_row(c, "Grows on", &g_grass_pick, g_layer_opts + 1, t->layer_count);
        dd->on_change = grass_layer_changed;
        UiNode *crow = ed_prop_row(c, "Colour");
        UiNode *col = ui_color(ui, crow, &t->grass_color.x, false);
        col->grow = 1;
        col->h = 22;
        col->on_change = ed_field_changed;
        num_row(c, "Height (m)", &t->grass_height, 0.02f, 0.05f, 5, true);
        slide_row(c, "Thickness", &t->grass_density, 0.2f, 16, true);
        num_row(c, "Seen up to (m)", &t->grass_distance, 1, 5, 1000, true);
        small_hint(c, "Grass grows wherever its ground texture is painted. To change the ground under it, go to Paint "
                      "and pick a new image for that texture.");
    }

    for (int k = 0; k < t->scatter_count; k++) {
        LxTerrainScatter *sc = &t->scatter[k];
        char title[48];
        char nm[LX_NAME_MAX];
        ed_slot_name("prefab", sc->prefab, nm, sizeof nm);
        snprintf(title, sizeof title, "%s", nm[0] ? nm : "Trees, rocks...");
        UiNode *pc = card(sec, NULL);
        UiNode *hrow = ui_box(ui, pc, UI_ROW);
        hrow->h = 24;
        UiNode *hl = ui_label(ui, hrow, title);
        hl->font = UI_FONT_BOLD;
        hl->grow = 1;
        UiNode *x = ui_icon_button(ui, hrow, UI_ICON_TRASH, "Remove", scatter_remove);
        x->tag = k;
        UiNode *row = ed_prop_row(pc, "Object");
        UiNode *sl = ed_slot(row, sc->prefab, sizeof sc->prefab, "prefab");
        sl->on_change = changed_commit;
        int *p = &g_proxy[k];
        *p = sc->layer + 1;
        UiNode *dd = dropdown_row(pc, "Grows on", p, g_layer_opts, t->layer_count + 1);
        dd->user = &sc->layer;
        dd->on_change = proxy_changed;
        num_row(pc, "How many", &sc->density, 0.05f, 0, 500, true);
        pair_row(pc, "Size", &sc->scale_min, &sc->scale_max, 0.02f, 0.01f, 100);
        num_row(pc, "Not steeper than (°)", &sc->max_slope, 0.5f, 0, 90, true);
    }
    UiNode *a = ui_button(ui, sec, "Add Trees, Rocks or Bushes", scatter_add);
    a->icon = UI_ICON_PLUS;
    a->h = 28;
    a->disabled = t->scatter_count >= LX_TERRAIN_SCATTERS;
    small_hint(sec, "Pick any prefab or model: it's spread over the land with random turns and sizes. \"How many\" is per 100 m².");
}

static void expand_mode(UiNode *sec, LxEntity e, LxTerrainComponent *t) {
    UiCtx *ui = ED.ui;
    UiNode *g = grid_of(sec);
    tool_tile(g, TT_ADD_CHUNK, 100);
    tool_tile(g, TT_REMOVE_CHUNK, 100);
    if (g_tool == TT_ADD_CHUNK || g_tool == TT_REMOVE_CHUNK) tool_tip(sec, k_tools[g_tool].help);
    UiNode *c = card(sec, NULL);
    UiNode *info = ui_label(ui, c, "");
    int n = lx_terrain_chunk_count(ED.scene, e);
    ui_set_text(info, "%d square%s of land, %.0f × %.0f m each", n, n == 1 ? "" : "s", (double)t->chunk_size, (double)t->chunk_size);
    info->font = UI_FONT_BOLD;
    small_hint(c, "Your terrain is made of squares. Add as many as you like in any direction: the world only loads "
                  "the ones near the player, so it can grow very large.");
}

static void settings_mode(UiNode *sec, LxEntity e, LxTerrainComponent *t) {
    UiCtx *ui = ED.ui;
    UiNode *c = card(sec, "Performance");
    num_row(c, "Full detail within (m)", &t->load_radius, 5, 0, 100000, true);
    small_hint(c, "Land closer than this to the player is in full detail and can be walked on. Further away it's drawn "
                  "simpler. 0 keeps everything in full detail.");
    slide_row(c, "Distant detail", &t->lod_distance, 0.25f, 4, true);
    small_hint(c, "Higher keeps faraway land smoother, lower is faster on phones.");
    UiNode *f = card(sec, "Files");
    UiNode *info = ui_label(ui, f, "");
    ui_set_text(info, "%d of %d squares loaded  ·  %d × %d height points each", lx_terrain_loaded_count(ED.scene, e),
                lx_terrain_chunk_count(ED.scene, e), t->resolution, t->resolution);
    info->font = UI_FONT_SMALL;
    UiNode *fr = ed_prop_row(f, "Saved in");
    UiNode *fl = ui_label(ui, fr, t->folder);
    fl->font = UI_FONT_SMALL;
    fl->grow = 1;
    small_hint(f, "Each square is its own file, saved when you save the scene.");
}

void ed_terrain_section(UiNode *sec, LxEntity e) {
    UiCtx *ui = ED.ui;
    const UiTheme *th = ui_theme(ui);
    LxTerrainComponent *t = lx_get_terrain(ED.scene, e);
    if (!t) return;
    g_ui_terrain = e;
    sec->gap = lx_maxf(sec->gap, 8);

    /* the mode bar */
    UiNode *bar = ui_box(ui, sec, UI_ROW);
    bar->gap = 4;
    bar->bg = th->field;
    bar->radius = 8;
    ui_pad(bar, 4, 4, 4, 4);
    for (int m = 0; m < MODE_COUNT; m++) {
        UiNode *b = tile(bar, k_modes[m].name, k_modes[m].glyph, g_mode == m, 0, 54, mode_click);
        b->grow = 1;
        b->bg = g_mode == m ? th->panel : 0;
        b->bg_hover = th->field_hover;
        b->path[1] = (char)m;
        b->on_draw = tile_sel_draw;
    }
    switch (g_mode) {
    case MODE_SHAPE: shape_mode(sec, t); break;
    case MODE_PAINT: paint_mode(sec, t); break;
    case MODE_PLANTS: plants_mode(sec, t); break;
    case MODE_EXPAND: expand_mode(sec, e, t); break;
    default: settings_mode(sec, e, t); break;
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
        g_tool = g_shape_tool = TT_SET_HEIGHT;   /* copying a height means levelling to it */
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
