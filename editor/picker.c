/* The asset picker: one window for every slot that holds an asset or an object.
 *
 * A slot (ed_slot) shows what it holds - a thumbnail and a name - instead of a text box to type into. Clicking it
 * opens the picker: a scrolling grid of everything that fits (textures, materials, meshes, models, animation
 * clips, prefabs, sounds, scenes, graphs or the scene's objects) with thumbnails and names, a search box, and
 * Apply / Cancel at the bottom. Double-clicking a tile applies it straight away. Slots still take drag and drop.
 *
 * Thumbnails: pictures come from thumbs.c, models / meshes / materials / clips / prefabs are rendered by
 * previews.c; until one is ready the tile shows the asset's icon. */
#include "editor.h"
#include <stdio.h>

#define PICK_MAX 4000
#define TILE 112

typedef struct PickItem {
    char value[LX_REF_MAX];    /* what goes into the slot */
    char label[96];
    char sub[96];              /* folder, model file, ... */
    char file[LX_PATH_MAX];    /* pictures: the file for its thumbnail */
    uint64_t size;
    UiIcon icon;
    char preview[16];          /* rendered thumbnail kind ("" = none) */
} PickItem;

typedef struct Tile { int item; UiNode *tile, *img, *icon; } Tile;

static struct {
    char kind[16];
    char context[LX_REF_MAX];  /* animation slots: the character's model, whose clips come first */
    bool allow_none;
    PickItem *items;
    int count, cap;
    int sel;                   /* item index, -1 none */
    bool sel_none;             /* "None" chosen */
    char search[64];
    UiNode *grid, *status, *apply, *chosen;
    Tile *tiles;
    int tile_count, tile_cap;
    UiNode *target;            /* the slot being filled (NULL once it's gone) */
    EdPickDone done;
    void *user;
} P;

/* ======================================================================
 *  what a kind holds
 * ====================================================================== */
static const char *kind_title(const char *k) {
    if (!strcmp(k, "texture")) return "Texture";
    if (!strcmp(k, "material")) return "Material";
    if (!strcmp(k, "mesh")) return "Mesh";
    if (!strcmp(k, "model")) return "Model";
    if (!strcmp(k, "animation")) return "Animation";
    if (!strcmp(k, "prefab")) return "Prefab";
    if (!strcmp(k, "sound")) return "Sound";
    if (!strcmp(k, "scene")) return "Scene";
    if (!strcmp(k, "graph")) return "State Graph";
    if (!strcmp(k, "object")) return "Game Object";
    return "Asset";
}
static UiIcon kind_icon(const char *k) {
    if (!strcmp(k, "texture")) return UI_ICON_TEXTURE;
    if (!strcmp(k, "material")) return UI_ICON_MATERIAL;
    if (!strcmp(k, "mesh")) return UI_ICON_MESH;
    if (!strcmp(k, "model")) return UI_ICON_MODEL;
    if (!strcmp(k, "animation")) return UI_ICON_ANIMATOR;
    if (!strcmp(k, "prefab")) return UI_ICON_PREFAB;
    if (!strcmp(k, "sound")) return UI_ICON_SOUND;
    if (!strcmp(k, "scene")) return UI_ICON_SCENE;
    if (!strcmp(k, "graph")) return UI_ICON_GRAPH;
    if (!strcmp(k, "object")) return UI_ICON_ENTITY;
    return UI_ICON_FILE;
}
static bool ext_in(const char *name, const char *const *exts) {
    const char *x = lx_path_ext(name);
    for (int i = 0; exts[i]; i++) {
        if (exts[i][0] == '*') return true;
        const char *a = x, *b = exts[i];
        while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
        if (!*a && !*b) return true;
    }
    return false;
}
static const char *const *kind_exts(const char *k) {
    static const char *tex[] = {".png", ".jpg", ".jpeg", ".tga", ".bmp", NULL}, *mat[] = {".lxmat", NULL}, *mesh[] = {".obj", NULL},
                      *model[] = {".fbx", NULL}, *prefab[] = {".lxprefab", NULL}, *sound[] = {".wav", NULL}, *scene[] = {".lxscene", NULL},
                      *graph[] = {".lxgraph", NULL}, *any[] = {"*", NULL}, *none[] = {NULL};
    if (!strcmp(k, "texture")) return tex;
    if (!strcmp(k, "material")) return mat;
    if (!strcmp(k, "mesh")) return mesh;
    if (!strcmp(k, "model") || !strcmp(k, "animation")) return model;
    if (!strcmp(k, "prefab")) return prefab;
    if (!strcmp(k, "sound")) return sound;
    if (!strcmp(k, "scene")) return scene;
    if (!strcmp(k, "graph")) return graph;
    if (!strcmp(k, "file")) return any;
    return none;
}

static PickItem *add(const char *value, const char *label, const char *sub, UiIcon icon) {
    if (P.count >= PICK_MAX) return NULL;
    PickItem it;
    memset(&it, 0, sizeof it);
    lx_strcpy(it.value, sizeof it.value, value);
    lx_strcpy(it.label, sizeof it.label, label);
    lx_strcpy(it.sub, sizeof it.sub, sub ? sub : "");
    it.icon = icon;
    LX_VEC_PUSH(P.items, P.count, P.cap, it);
    return &P.items[P.count - 1];
}

/* "Folder/Name.ext" -> "Name" and "Folder" */
static void split_ref(const char *ref, char *name, size_t ncap, char *folder, size_t fcap) {
    const char *slash = strrchr(ref, '/');
    lx_strcpy(name, ncap, slash ? slash + 1 : ref);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    if (folder) {
        lx_strcpy(folder, fcap, ref);
        char *s = strrchr(folder, '/');
        if (s) *s = 0;
        else folder[0] = 0;
    }
}

/* the name a slot shows for a value */
void ed_slot_name(const char *kind, const char *value, char *out, size_t cap) {
    if (!value[0]) { lx_strcpy(out, cap, "None"); return; }
    if (!strncmp(value, "builtin:", 8)) {
        lx_strcpy(out, cap, value + 8);
        if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
        return;
    }
    if (!strncmp(value, "name:", 5)) { lx_strcpy(out, cap, value + 5); return; }
    if (!strncmp(value, "tag:", 4)) { snprintf(out, cap, "Tag %s", value + 4); return; }
    const char *h = strchr(value, '#');
    if (h && !strcmp(kind, "animation")) { lx_strcpy(out, cap, h + 1); return; }
    if (h == value) { lx_strcpy(out, cap, value + 1); return; }
    split_ref(value, out, cap, NULL, 0);
}

static void walk(const char *dir, const char *rel, const char *const *exts, bool clips) {
    LxDirEntry *e;
    int n = lx_dir_list(dir, &e);
    for (int i = 0; i < n && P.count < PICK_MAX; i++) {
        if (e[i].name[0] == '.') continue;
        char full[LX_PATH_MAX], r[LX_REF_MAX];
        lx_path_join(full, sizeof full, dir, e[i].name);
        if (rel[0]) snprintf(r, sizeof r, "%s/%s", rel, e[i].name);
        else lx_strcpy(r, sizeof r, e[i].name);
        if (e[i].is_dir) { walk(full, r, exts, clips); continue; }
        if (!ext_in(e[i].name, exts)) continue;
        char name[96], folder[96];
        split_ref(r, name, sizeof name, folder, sizeof folder);
        if (clips) {
            if (P.context[0] && !strcmp(r, P.context)) continue;   /* already listed first */
            LxModel *m = lx_asset_model(r);
            for (int c = 0; m && c < lx_model_clip_count(m); c++) {
                char v[LX_REF_MAX];
                snprintf(v, sizeof v, "%s#%s", r, lx_model_clip_name(m, c));
                PickItem *it = add(v, lx_model_clip_name(m, c), e[i].name, UI_ICON_ANIMATOR);
                if (it) lx_strcpy(it->preview, sizeof it->preview, "animation");
            }
            continue;
        }
        PickItem *it = add(r, name, folder, ed_icon_for_file(e[i].name, false));
        if (!it) break;
        if (!strcmp(P.kind, "texture")) { lx_strcpy(it->file, sizeof it->file, full); it->size = e[i].size; }
        else if (ed_preview_supported(P.kind)) lx_strcpy(it->preview, sizeof it->preview, P.kind);
    }
    lx_free(e);
}

static void gather(LxEntity owner) {
    P.count = 0;
    const char *k = P.kind;
    if (!strcmp(k, "mesh")) {
        static const char *b[] = {"builtin:cube", "builtin:sphere", "builtin:plane", "builtin:cylinder", "builtin:capsule"};
        for (int i = 0; i < 5; i++) {
            char nm[32];
            ed_slot_name(k, b[i], nm, sizeof nm);
            PickItem *it = add(b[i], nm, "Built-in", UI_ICON_MESH);
            if (it) lx_strcpy(it->preview, sizeof it->preview, "mesh");
        }
    } else if (!strcmp(k, "material")) {
        static const char *b[] = {"builtin:default", "builtin:metal", "builtin:red", "builtin:green", "builtin:blue", "builtin:emissive", "builtin:concrete", "builtin:fabric", "builtin:grid"};
        for (int i = 0; i < 9; i++) {
            char nm[32];
            ed_slot_name(k, b[i], nm, sizeof nm);
            PickItem *it = add(b[i], nm, "Built-in", UI_ICON_MATERIAL);
            if (it) lx_strcpy(it->preview, sizeof it->preview, "material");
        }
    } else if (!strcmp(k, "animation")) {
        /* the character's own clips first */
        LxModel *m = P.context[0] ? lx_asset_model(P.context) : NULL;
        char file[96], folder[96];
        split_ref(P.context, file, sizeof file, folder, sizeof folder);
        for (int c = 0; m && c < lx_model_clip_count(m); c++) {
            char v[LX_REF_MAX], sub[96];
            snprintf(v, sizeof v, "%s#%s", P.context, lx_model_clip_name(m, c));
            snprintf(sub, sizeof sub, "%s (this character)", file);
            PickItem *it = add(v, lx_model_clip_name(m, c), sub, UI_ICON_ANIMATOR);
            if (it) lx_strcpy(it->preview, sizeof it->preview, "animation");
        }
    } else if (!strcmp(k, "object")) {
        if (owner || ED.scene) {
            add("Owner", "Owner", "this object", UI_ICON_GRAPH);
            add("None", "None", "no object", UI_ICON_CLOSE);
        }
        for (int i = 0; ED.scene && i < lx_scene_entity_count(ED.scene) && P.count < PICK_MAX; i++) {
            LxEntity e = lx_scene_entity_at(ED.scene, i);
            const char *nm = lx_entity_name(ED.scene, e);
            if (!nm || !nm[0]) continue;
            char v[LX_REF_MAX], sub[96] = "";
            snprintf(v, sizeof v, "name:%s", nm);
            LxEntity p = lx_entity_parent(ED.scene, e);
            if (p) snprintf(sub, sizeof sub, "in %s", lx_entity_name(ED.scene, p));
            add(v, nm, sub, ed_icon_for_entity(ED.scene, e));
        }
        return;
    }
    if (ED.has_project) walk(ED.project.assets, "", kind_exts(k), !strcmp(k, "animation"));
}

/* ======================================================================
 *  the window
 * ====================================================================== */
static bool matches(const PickItem *it) {
    if (!P.search[0]) return true;
    char a[200], b[64];
    snprintf(a, sizeof a, "%s %s", it->label, it->sub);
    lx_strcpy(b, sizeof b, P.search);
    for (char *c = a; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    for (char *c = b; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    return strstr(a, b) != NULL;
}

static void update_footer(void) {
    if (P.chosen) {
        if (P.sel_none) ui_set_text(P.chosen, "None");
        else if (P.sel >= 0 && P.sel < P.count) ui_set_text(P.chosen, "%s%s%s", P.items[P.sel].label, P.items[P.sel].sub[0] ? "  ·  " : "", P.items[P.sel].sub);
        else ui_set_text(P.chosen, "Nothing chosen");
    }
    if (P.apply) P.apply->disabled = !P.sel_none && (P.sel < 0 || P.sel >= P.count);
}

static void apply(void) {
    if (!P.sel_none && (P.sel < 0 || P.sel >= P.count)) return;
    char value[LX_REF_MAX];
    lx_strcpy(value, sizeof value, P.sel_none ? "" : P.items[P.sel].value);
    UiNode *t = P.target;
    EdPickDone done = P.done;
    void *user = P.user;
    char kind[16];
    lx_strcpy(kind, sizeof kind, P.kind);
    ui_close_modal(ED.ui);
    if (t && t->s) {
        lx_strcpy(t->s, (size_t)t->s_cap, value);
        if (!strcmp(kind, "mesh") || !strcmp(kind, "material")) lx_scene_mark_static_dirty(ED.scene);
        if (t->on_change) t->on_change(t);
    }
    if (done) done(user, value);
}
static void apply_click(UiNode *n) { LX_UNUSED(n); apply(); }
static void cancel_click(UiNode *n) { LX_UNUSED(n); ui_close_modal(ED.ui); }
static void none_click(UiNode *n) {
    LX_UNUSED(n);
    P.sel = -1;
    P.sel_none = true;
    for (int i = 0; i < P.tile_count; i++) P.tiles[i].tile->selected = false;
    apply();
}

static void tile_click(UiNode *n) {
    P.sel = (int)n->tag;
    P.sel_none = false;
    for (int i = 0; i < P.tile_count; i++) P.tiles[i].tile->selected = P.tiles[i].item == P.sel;
    update_footer();
}
static void tile_double(UiNode *n) { tile_click(n); apply(); }

/* thumbnails arrive over time: swap the icon for the picture */
static void grid_frame(UiNode *n) {
    LX_UNUSED(n);
    ed_thumbs_mark_visible();
    for (int i = 0; i < P.tile_count; i++) {
        Tile *t = &P.tiles[i];
        if (t->img->texture) continue;
        const PickItem *it = &P.items[t->item];
        uint32_t tex = 0;
        float aspect = 1;
        if (it->file[0]) tex = ed_thumb_get(it->file, it->size, 128, &aspect, NULL, NULL);
        else if (it->preview[0]) tex = ed_preview_get(it->preview, it->value);
        if (!tex) continue;
        t->img->texture = tex;
        t->img->tex_aspect = aspect;
        t->img->hidden = false;
        t->icon->hidden = true;
    }
}

static void build_grid(void) {
    UiCtx *ui = ED.ui;
    const UiTheme *th = ui_theme(ui);
    if (!P.grid) return;
    ui_clear(ui, P.grid);
    P.tile_count = 0;
    int shown = 0;
    for (int i = 0; i < P.count; i++) {
        const PickItem *it = &P.items[i];
        if (!matches(it)) continue;
        UiNode *tl = ui_box(ui, P.grid, UI_COL);
        tl->w = TILE;
        tl->h = TILE + 34;
        ui_pad(tl, 6, 6, 6, 4);
        tl->gap = 2;
        tl->radius = 6;
        tl->bg_hover = th->hover;
        tl->tag = i;
        tl->on_click = tile_click;
        tl->on_double_click = tile_double;
        tl->selected = i == P.sel;
        snprintf(tl->tooltip, sizeof tl->tooltip, "%s%s%s", it->label, it->sub[0] ? "\n" : "", it->sub);
        UiNode *img = ui_image(ui, tl, 0);
        img->h = TILE - 24;
        img->hidden = true;
        UiNode *ic = ui_label(ui, tl, "");
        ic->icon = it->icon;
        ic->icon_size = (TILE - 24) * 0.5f;
        ic->icon_color = ed_color_for_icon(it->icon);
        ic->center_text = true;
        ic->h = TILE - 24;
        UiNode *nm = ui_label(ui, tl, it->label);
        nm->font = UI_FONT_SMALL;
        nm->center_text = true;
        nm->h = 16;
        nm->w = TILE - 12;
        nm->clip = true;
        UiNode *sb = ui_label(ui, tl, it->sub);
        sb->font = UI_FONT_SMALL;
        sb->fg = th->text_dim;
        sb->center_text = true;
        sb->h = 14;
        sb->w = TILE - 12;
        sb->clip = true;
        Tile t = {i, tl, img, ic};
        LX_VEC_PUSH(P.tiles, P.tile_count, P.tile_cap, t);
        shown++;
    }
    if (!shown) {
        UiNode *msg = ui_label(ui, P.grid, P.search[0] ? "Nothing matches your search." :
                                           !strcmp(P.kind, "animation") ? "No animation clips: import a model (.fbx) with animations into Assets." :
                                           "Nothing of this kind in the project's Assets yet.");
        msg->fg = th->text_dim;
        msg->w = 520;
        msg->h = 40;
        ui_pad(msg, 10, 0, 0, 0);
    }
    if (P.status) ui_set_text(P.status, "%d of %d", shown, P.count);
    grid_frame(NULL);
}
static void search_changed(UiNode *n) { LX_UNUSED(n); build_grid(); }

void ed_picker_open(const char *kind, const char *current, const char *context_model, UiNode *target, EdPickDone done, void *user) {
    UiCtx *ui = ED.ui;
    const UiTheme *th = ui_theme(ui);
    lx_strcpy(P.kind, sizeof P.kind, kind);
    lx_strcpy(P.context, sizeof P.context, context_model ? context_model : "");
    char ctx[LX_REF_MAX];   /* a clip ref's model: "Models/Hero.fbx#Run" -> "Models/Hero.fbx" */
    lx_strcpy(ctx, sizeof ctx, P.context);
    char *h = strchr(ctx, '#');
    if (h) *h = 0;
    lx_strcpy(P.context, sizeof P.context, ctx);
    P.done = done;
    P.user = user;
    P.target = target;
    P.search[0] = 0;
    P.sel = -1;
    P.sel_none = false;
    P.allow_none = strcmp(kind, "object") != 0;
    gather(0);
    for (int i = 0; current && current[0] && i < P.count; i++)
        if (!strcmp(P.items[i].value, current)) P.sel = i;

    char title[64];
    snprintf(title, sizeof title, "Select %s", kind_title(kind));
    UiNode *body = ui_modal(ui, title, 800, 600);
    if (!body) return;
    if (target) ui_track(&P.target);   /* the slot may go away while the window is open */
    UiNode *bar = ui_box(ui, body, UI_ROW);
    bar->h = 30;
    bar->gap = 8;
    bar->cross_align = UI_ALIGN_CENTER;
    UiNode *sf = ui_textfield(ui, bar, P.search, sizeof P.search);
    sf->grow = 1;
    sf->h = 28;
    sf->icon = UI_ICON_SEARCH;
    lx_strcpy(sf->hint, sizeof sf->hint, "Search");
    sf->on_change = search_changed;
    P.status = ui_label(ui, bar, "");
    P.status->fg = th->text_dim;
    P.status->font = UI_FONT_SMALL;
    P.status->w = 90;
    ui_track(&P.status);
    if (!strcmp(kind, "animation") && P.context[0]) {
        char nm[96];
        split_ref(P.context, nm, sizeof nm, NULL, 0);
        UiNode *hint = ui_label(ui, body, "");
        ui_set_text(hint, "Clips of %s first; humanoid clips from other models play on it too.", nm);
        hint->fg = th->text_dim;
        hint->font = UI_FONT_SMALL;
        hint->h = 16;
    }
    UiNode *frame = ui_box(ui, body, UI_COL);
    frame->grow = 1;
    frame->bg = th->panel_alt;
    frame->radius = 6;
    frame->border = th->border;
    ui_pad(frame, 6, 6, 6, 6);
    P.grid = ui_box(ui, frame, UI_ROW);
    P.grid->grow = 1;
    P.grid->wrap = true;
    P.grid->scroll = true;
    P.grid->gap = 6;
    P.grid->on_frame = grid_frame;
    ui_track(&P.grid);
    UiNode *foot = ui_box(ui, body, UI_ROW);
    foot->h = 34;
    foot->gap = 8;
    foot->cross_align = UI_ALIGN_CENTER;
    UiNode *ki = ui_label(ui, foot, "");
    ki->icon = kind_icon(kind);
    ki->icon_color = ed_color_for_icon(ki->icon);
    ki->w = 22;
    P.chosen = ui_label(ui, foot, "");
    P.chosen->grow = 1;
    P.chosen->clip = true;
    ui_track(&P.chosen);
    if (P.allow_none) {
        UiNode *nb = ui_button(ui, foot, "None", none_click);
        nb->w = 80;
        lx_strcpy(nb->tooltip, sizeof nb->tooltip, "Empty the slot");
    }
    UiNode *cb = ui_button(ui, foot, "Cancel", cancel_click);
    cb->w = 100;
    P.apply = ui_button(ui, foot, "Apply", apply_click);
    P.apply->w = 110;
    P.apply->bg = th->accent;
    P.apply->bg_hover = th->accent_hover;
    P.apply->fg = th->accent_text;
    P.apply->icon = UI_ICON_CHECK;
    ui_track(&P.apply);
    build_grid();
    update_footer();
}

/* ======================================================================
 *  slots
 * ====================================================================== */
/* slot fields: s/s_cap = the value, hint = the kind, drag_payload = the context model (animation slots),
 * edit_backup = the value it last showed */
static void slot_refresh(UiNode *n) {
    if (!n->s || !strcmp(n->edit_backup, n->s)) return;
    lx_strcpy(n->edit_backup, sizeof n->edit_backup, n->s);
    UiNode *thumb = n->first, *icon = thumb ? thumb->next : NULL, *label = icon ? icon->next : NULL;
    if (!label) return;
    char nm[128];
    ed_slot_name(n->hint, n->s, nm, sizeof nm);
    ui_set_text(label, "%s", nm);
    label->fg = n->s[0] ? ui_theme(ED.ui)->text : ui_theme(ED.ui)->text_dim;
    thumb->texture = 0;
    thumb->hidden = true;
    icon->hidden = false;
    snprintf(n->tooltip, sizeof n->tooltip, "%s\nClick to choose, or drop one here", n->s[0] ? n->s : "Empty");
}
static void slot_frame(UiNode *n) {
    slot_refresh(n);
    UiNode *thumb = n->first, *icon = thumb ? thumb->next : NULL;
    if (!icon || thumb->texture || !n->s || !n->s[0]) return;
    uint32_t tex = 0;
    float aspect = 1;
    if (!strcmp(n->hint, "texture")) {
        char full[LX_PATH_MAX];
        lx_asset_path(full, sizeof full, n->s);
        if (lx_file_exists(full)) tex = ed_thumb_get(full, lx_file_size(full), 128, &aspect, NULL, NULL);
    } else if (ed_preview_supported(n->hint)) tex = ed_preview_get(n->hint, n->s);
    if (!tex) return;
    thumb->texture = tex;
    thumb->tex_aspect = aspect;
    thumb->hidden = false;
    icon->hidden = true;
}
static void slot_click(UiNode *n) {
    if (n->disabled) return;
    ed_picker_open(n->hint, n->s, n->drag_payload, n, NULL, NULL);
}
static void slot_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(type);
    if (!n->s) return;
    lx_strcpy(n->s, (size_t)n->s_cap, payload);
    if (!strcmp(n->hint, "mesh") || !strcmp(n->hint, "material")) lx_scene_mark_static_dirty(ED.scene);
    if (n->on_change) n->on_change(n);
}

UiNode *ed_slot(UiNode *parent, char *buf, int cap, const char *kind) {
    UiCtx *ui = ED.ui;
    const UiTheme *th = ui_theme(ui);
    UiNode *n = ui_box(ui, parent, UI_ROW);
    n->grow = 1;
    n->h = 24;
    n->gap = 6;
    n->cross_align = UI_ALIGN_CENTER;
    ui_pad(n, 3, 2, 6, 2);
    n->bg = th->field;
    n->bg_hover = th->field_hover;
    n->border = th->border;
    n->radius = 4;
    n->s = buf;
    n->s_cap = cap;
    lx_strcpy(n->hint, sizeof n->hint, kind);
    n->accept = !strcmp(kind, "object") ? "entity" : !strcmp(kind, "animation") ? "animation" : kind;
    n->on_click = slot_click;
    n->on_drop = slot_drop;
    n->on_frame = slot_frame;
    UiNode *thumb = ui_image(ui, n, 0);
    thumb->w = 20;
    thumb->h = 20;
    thumb->radius = 3;
    thumb->hidden = true;
    UiNode *icon = ui_label(ui, n, "");
    icon->icon = kind_icon(kind);
    icon->icon_color = ed_color_for_icon(icon->icon);
    icon->w = 20;
    icon->center_text = true;
    UiNode *label = ui_label(ui, n, "");
    label->grow = 1;
    label->clip = true;
    UiNode *more = ui_label(ui, n, "");
    more->icon = UI_ICON_DOTS;
    more->icon_color = th->text_dim;
    more->w = 16;
    n->edit_backup[0] = 1;   /* nothing shown yet */
    n->edit_backup[1] = 0;
    slot_refresh(n);
    return n;
}

void ed_slot_set_context(UiNode *slot, const char *model_ref) {
    lx_strcpy(slot->drag_payload, sizeof slot->drag_payload, model_ref ? model_ref : "");
}

bool ed_picker_open_now(void) { return ui_has_modal(ED.ui) && P.grid != NULL; }

/* ---- for the smoke test ---- */
int ed_picker_item_count(void) { return ed_picker_open_now() ? P.count : -1; }
int ed_picker_tile_count(void) { return ed_picker_open_now() ? P.tile_count : -1; }
/* choose a tile by its value and press Apply (false when no tile has it) */
bool ed_picker_choose(const char *value) {
    if (!ed_picker_open_now()) return false;
    for (int i = 0; i < P.tile_count; i++)
        if (!strcmp(P.items[P.tiles[i].item].value, value)) {
            tile_click(P.tiles[i].tile);
            apply_click(P.apply);
            return true;
        }
    return false;
}
void ed_picker_search(const char *text) {
    if (!ed_picker_open_now()) return;
    lx_strcpy(P.search, sizeof P.search, text);
    build_grid();
}
