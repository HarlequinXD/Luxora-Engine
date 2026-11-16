/* Models in the editor: placing imported models, their import settings (rig type, scale, clips),
 * the Animator component, and the Avatar window - a body diagram where each joint can be clicked
 * and mapped to a bone of the rig (filled in automatically from the usual bone naming conventions). */
#include "editor.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ======================================================================
 *  placing
 * ====================================================================== */
LxEntity ed_model_place(const char *ref, LxVec3 pos, LxEntity parent) {
    if (!ED.scene) return 0;
    if (ED.play != ED_EDIT) { ui_notify(ED.ui, "Stop play mode to add objects"); return 0; }
    double t0 = lx_time();
    LxEntity e = lx_model_instantiate(ED.scene, ref, parent ? lx_v3(0, 0, 0) : pos, parent);
    double t1 = lx_time();
    if (!e) { ui_notify(ED.ui, "Could not import %s - see the Console", ref); return 0; }
    ed_select(e);
    ED.hierarchy_hash = 0;
    ed_mark_changed();
    ed_commit();
    LX_INFO("Placed %s: import+instantiate %.0f ms, editor %.0f ms", ref, (t1 - t0) * 1000, (lx_time() - t1) * 1000);
    return e;
}

/* ======================================================================
 *  small widgets
 * ====================================================================== */
static UiNode *row_bool(UiNode *parent, const char *label, bool *v, void (*cb)(UiNode *)) {
    UiNode *r = ed_prop_row(parent, label);
    UiNode *c = ui_checkbox(ED.ui, r, "", v);
    c->on_change = cb;
    return c;
}
static UiNode *row_float(UiNode *parent, const char *label, float *v, float step, float lo, float hi, void (*cb)(UiNode *)) {
    UiNode *r = ed_prop_row(parent, label);
    UiNode *n = ui_number(ED.ui, r, v, step);
    n->grow = 1;
    n->h = 22;
    n->minv = lo;
    n->maxv = hi;
    n->on_change = cb;
    return n;
}
static UiNode *hint(UiNode *parent, const char *text) {
    UiNode *l = ui_label(ED.ui, parent, text);
    l->font = UI_FONT_SMALL;
    l->fg = ui_theme(ED.ui)->text_dim;
    l->wrap = true;
    return l;
}

/* ======================================================================
 *  import settings (asset inspector for .fbx)
 * ====================================================================== */
static LxModelImport g_imp;          /* what's being edited */
static char g_imp_ref[LX_REF_MAX];
static bool g_imp_dirty;
static int g_rig_index;
static const char *RIGS[] = {"None (static model)", "Generic", "Humanoid (avatar)"};

static void imp_changed(UiNode *n) { LX_UNUSED(n); g_imp_dirty = true; }
static int g_lmuv_index;
static const char *LMUVS[] = {"Auto", "Generate", "Model's Own"};
static void lmuv_changed(UiNode *n) { LX_UNUSED(n); g_imp.lightmap_uvs = (LxLightmapUvMode)g_lmuv_index; g_imp_dirty = true; }

static void apply_import(void) {
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, g_imp_ref);
    if (!lx_model_import_save(path, &g_imp)) { ui_notify(ED.ui, "Could not save the import settings"); return; }
    lx_model_reload(g_imp_ref);
    g_imp_dirty = false;
    ED.inspector_dirty = true;
    ui_notify(ED.ui, "Re-imported %s", lx_path_filename(g_imp_ref));
}
static void apply_click(UiNode *n) { LX_UNUSED(n); apply_import(); }
static void revert_click(UiNode *n) {
    LX_UNUSED(n);
    LxModel *m = lx_asset_model(g_imp_ref);
    if (m) g_imp = *lx_model_settings(m);
    g_imp_dirty = false;
    ED.inspector_dirty = true;
}

static void rig_changed(UiNode *n) {
    LX_UNUSED(n);
    g_imp.rig = (LxRigType)g_rig_index;
    if (g_imp.rig == LX_RIG_HUMANOID) {
        bool any = false;
        for (int b = 0; b < LX_HB_COUNT; b++) any |= g_imp.avatar[b][0] != 0;
        if (!any) {   /* first time: map the bones automatically */
            LxModel *m = lx_asset_model(g_imp_ref);
            int k = lx_avatar_automap(m, g_imp.avatar);
            ui_notify(ED.ui, "Avatar: %d bones mapped automatically", k);
        }
    }
    g_imp_dirty = true;
    ED.inspector_dirty = true;
}

static void place_click(UiNode *n) { LX_UNUSED(n); ed_model_place(g_imp_ref, ed_spawn_point(), 0); }
static void avatar_click(UiNode *n) { LX_UNUSED(n); ed_avatar_window(g_imp_ref); }

void ed_model_inspector(UiNode *body, const char *ref) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxModel *m = lx_asset_model(ref);
    if (strcmp(g_imp_ref, ref)) {
        lx_strcpy(g_imp_ref, sizeof g_imp_ref, ref);
        g_imp = m ? *lx_model_settings(m) : lx_model_import_defaults();
        g_imp_dirty = false;
    }
    if (!m) {
        UiNode *e = hint(body, lx_model_error(ref));
        e->fg = t->danger;
        ui_pad(e, 12, 4, 12, 4);
        return;
    }
    char info[200];
    snprintf(info, sizeof info, "%d mesh%s · %d bone%s · %d clip%s", lx_model_mesh_count(m), lx_model_mesh_count(m) == 1 ? "" : "es",
             lx_model_bone_count(m), lx_model_bone_count(m) == 1 ? "" : "s", lx_model_clip_count(m), lx_model_clip_count(m) == 1 ? "" : "s");
    UiNode *il = hint(body, info);
    ui_pad(il, 12, 0, 12, 4);

    static bool open_model = true, open_rig = true, open_anim = true;
    UiNode *sec = ed_section(body, UI_ICON_MODEL, "Model", &open_model, NULL);
    row_float(sec, "Scale", &g_imp.scale, 0.01f, 0.0001f, 10000, imp_changed);
    row_bool(sec, "Import Materials", &g_imp.import_materials, imp_changed);
    row_bool(sec, "Calculate Normals", &g_imp.calculate_normals, imp_changed);
    if (g_imp.calculate_normals) {
        UiNode *r = ed_prop_row(sec, "Smoothing Angle");
        UiNode *sl = ui_slider(ui, r, &g_imp.smoothing_angle, 0, 180);
        sl->grow = 1;
        sl->h = 22;
        sl->on_change = imp_changed;
    }
    g_lmuv_index = (int)g_imp.lightmap_uvs;
    UiNode *lr = ed_prop_row(sec, "Lightmap UVs");
    UiNode *ld = ui_dropdown(ui, lr, &g_lmuv_index, LMUVS, 3);
    ld->grow = 1;
    ld->h = 22;
    ld->on_change = lmuv_changed;
    lx_strcpy(ld->tooltip, sizeof ld->tooltip, "Where baked lighting is stored. Auto: the model's own second UV set, "
                                               "else an automatic unwrap. Generate: always unwrap.");

    sec = ed_section(body, UI_ICON_AVATAR, "Rig", &open_rig, NULL);
    g_rig_index = (int)g_imp.rig;
    UiNode *r = ed_prop_row(sec, "Animation Type");
    UiNode *dd = ui_dropdown(ui, r, &g_rig_index, RIGS, 3);
    dd->grow = 1;
    dd->h = 22;
    dd->on_change = rig_changed;
    if (g_imp.rig == LX_RIG_GENERIC)
        hint(sec, "Generic: the skeleton is used as it is. Its clips play on models whose bones have the same names - no retargeting.");
    else if (g_imp.rig == LX_RIG_HUMANOID) {
        char missing[400];
        bool ok = lx_avatar_is_valid(g_imp.avatar, missing, sizeof missing);
        int mapped = 0;
        for (int b = 0; b < LX_HB_COUNT; b++) mapped += g_imp.avatar[b][0] != 0;
        char line[480];
        if (ok) snprintf(line, sizeof line, "Avatar ready: %d of %d human bones mapped. Humanoid clips retarget onto this model, and its clips onto other humanoids.", mapped, (int)LX_HB_COUNT);
        else snprintf(line, sizeof line, "Avatar incomplete - missing: %s", missing);
        UiNode *st = hint(sec, line);
        st->fg = ok ? t->success : t->warning;
        st->icon = ok ? UI_ICON_SUCCESS : UI_ICON_WARNING;
        st->icon_color = st->fg;
        UiNode *b = ui_button(ui, sec, "Configure Avatar...", avatar_click);
        b->icon = UI_ICON_AVATAR;
        b->icon_color = ed_color_for_icon(UI_ICON_AVATAR);
        b->h = 28;
    } else hint(sec, "None: imported as static meshes, without its skeleton or animation.");

    sec = ed_section(body, UI_ICON_ANIMATOR, "Animation", &open_anim, NULL);
    row_bool(sec, "Import Animations", &g_imp.import_animations, imp_changed);
    if (!lx_model_clip_count(m)) hint(sec, "No animation clips in this file.");
    for (int i = 0; i < lx_model_clip_count(m) && i < g_imp.clip_count; i++) {
        UiNode *cr = ui_box(ui, sec, UI_ROW);
        cr->h = 26;
        cr->gap = 6;
        cr->cross_align = UI_ALIGN_CENTER;
        UiNode *ic = ui_label(ui, cr, "");
        ic->icon = UI_ICON_ANIMATOR;
        ic->icon_color = ed_color_for_icon(UI_ICON_ANIMATOR);
        ic->w = 18;
        UiNode *nm = ui_textfield(ui, cr, g_imp.clips[i].name, sizeof g_imp.clips[i].name);
        nm->grow = 1;
        nm->h = 22;
        nm->on_change = imp_changed;
        UiNode *len = ui_label(ui, cr, "");
        ui_set_text(len, "%.2fs", lx_model_clip_length(m, i));
        len->w = 52;
        len->fg = t->text_dim;
        len->font = UI_FONT_SMALL;
        UiNode *lp = ui_checkbox(ui, cr, "Loop", &g_imp.clips[i].loop);
        lp->on_change = imp_changed;
        lp->w = 64;
        /* What the clip's root does: travel the object, or stay in the pose. Only worth showing for a
           rig that has a root to move. */
        if (g_imp.rig != LX_RIG_NONE) {
            UiNode *rr = ui_box(ui, sec, UI_ROW);
            rr->h = 24;
            rr->gap = 8;
            rr->cross_align = UI_ALIGN_CENTER;
            ui_pad(rr, 24, 0, 0, 0);
            UiNode *lbl = ui_label(ui, rr, "Root motion:");
            lbl->font = UI_FONT_SMALL;
            lbl->fg = t->text_dim;
            lbl->w = 86;
            UiNode *bx = ui_checkbox(ui, rr, "Bake Travel", &g_imp.clips[i].bake_position_xz);
            bx->on_change = imp_changed;
            bx->w = 110;
            lx_strcpy(bx->tooltip, sizeof bx->tooltip,
                      "Keep the clip's forward travel in the pose instead of moving the object with it");
            UiNode *by = ui_checkbox(ui, rr, "Bake Up/Down", &g_imp.clips[i].bake_position_y);
            by->on_change = imp_changed;
            by->w = 124;
            lx_strcpy(by->tooltip, sizeof by->tooltip, "Keep hops and crouches in the pose");
            UiNode *br = ui_checkbox(ui, rr, "Bake Turn", &g_imp.clips[i].bake_rotation);
            br->on_change = imp_changed;
            br->w = 100;
            lx_strcpy(br->tooltip, sizeof br->tooltip, "Keep the clip's turning in the pose instead of turning the object");
        }
    }
    if (lx_model_clip_count(m) && g_imp.rig != LX_RIG_NONE)
        hint(sec, "Unbaked root movement drives the object itself, so the character travels and its collider goes with it - "
                  "switch it on with Root Motion on the Animator.");
    UiNode *btns = ui_box(ui, body, UI_ROW);
    ui_pad(btns, 12, 10, 12, 4);
    btns->h = 46;
    btns->gap = 8;
    UiNode *ap = ui_button(ui, btns, g_imp_dirty ? "Apply *" : "Apply", apply_click);
    ap->grow = 1;
    ap->bg = t->accent;
    ap->fg = t->accent_text;
    lx_strcpy(ap->tooltip, sizeof ap->tooltip, "Save the import settings and re-import the model");
    UiNode *rv = ui_button(ui, btns, "Revert", revert_click);
    rv->w = 80;
    UiNode *pl = ui_button(ui, body, "Add to Scene", place_click);
    pl->icon = UI_ICON_PLUS;
    pl->h = 28;
    ui_pad(pl, 12, 0, 0, 0);
}

/* ======================================================================
 *  Animator component
 * ====================================================================== */
static LxEntity g_anim_entity;
static bool g_preview;
static float g_preview_time;
static UiNode *g_time_label;

static void anim_changed(UiNode *n) { LX_UNUSED(n); ed_mark_changed(); ed_commit(); }
static void model_slot_changed(UiNode *n) {   /* another skeleton: the clip list and rig line change */
    LX_UNUSED(n);
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}
static void clip_slot_changed(UiNode *n) {
    LX_UNUSED(n);
    LxAnimator *a = lx_get_animator(ED.scene, g_anim_entity);
    if (a && g_preview && a->clip[0]) lx_animator_play(ED.scene, g_anim_entity, a->clip, 0.2f);
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}
static void preview_frame(UiNode *n) {
    LxAnimator *a = lx_get_animator(ED.scene, g_anim_entity);
    if (!a || ED.play != ED_EDIT) { g_preview = false; return; }
    if (g_preview) {
        lx_animator_update(ED.scene, g_anim_entity, ED.dt);
        g_preview_time = lx_animator_time(ED.scene, g_anim_entity);
    }
    if (g_time_label) ui_set_text(g_time_label, "%.2f s", g_preview_time);
    LX_UNUSED(n);
}
static void preview_toggle(UiNode *n) {
    LxAnimator *a = lx_get_animator(ED.scene, g_anim_entity);
    if (!a) return;
    g_preview = !g_preview;
    if (g_preview) {
        if (!lx_animator_playing(ED.scene, g_anim_entity) && a->clip[0]) lx_animator_play(ED.scene, g_anim_entity, a->clip, 0);
    } else {
        lx_animator_reset_pose(ED.scene, g_anim_entity);   /* back to the rest pose: the scene isn't changed */
        g_preview_time = 0;
    }
    ui_set_text(n, g_preview ? "Stop Preview" : "Preview");
    n->icon = g_preview ? UI_ICON_STOP : UI_ICON_PLAY;
}

/* ======================================================================
 *  the Animator component
 *
 *  The component is the animator graph's: the graph is the first thing on it, and what follows -
 *  the parameters, the layers, what each one is playing - is that graph on this object. While the
 *  game runs those are live, so a parameter can be pushed by hand to see what the graph does with it.
 *
 *  An animator with no graph still plays a single clip, which is all a door or a flag needs; those
 *  fields appear only when there is no graph to drive it instead.
 * ====================================================================== */
#define ANIM_BIND_MAX 48
typedef struct AnimBind {
    LxEntity owner;                 /* the object whose animator this is */
    char param[LX_ANIM_NAME];
    int layer;
    LxAnimParamType type;
    float f;
    int i;
    bool b;
} AnimBind;
static AnimBind g_abind[ANIM_BIND_MAX];
static int g_abind_n;

/* the live animator of the object being inspected, or 0 when it is not playing */
static LxEntity anim_live(LxEntity e) {
    if (ED.play == ED_EDIT || !ED.scene || !e || !lx_entity_valid(ED.scene, e)) return 0;
    LxEntity a = lx_find_animator(ED.scene, e);
    return a && lx_animator_layer_count(ED.scene, a) ? a : 0;
}

/* a parameter edited on the component: while playing it goes to the animator, otherwise it becomes the
   graph's own starting value, which is what the object will begin with next time */
static void param_changed(UiNode *n) {
    AnimBind *b = n->user;
    if (!b) return;
    LxEntity live = anim_live(b->owner);
    if (live) {
        switch (b->type) {
        case LX_AP_FLOAT: lx_animator_set_float(ED.scene, live, b->param, b->f); break;
        case LX_AP_INT: lx_animator_set_int(ED.scene, live, b->param, b->i); break;
        default: lx_animator_set_bool(ED.scene, live, b->param, b->b); break;
        }
        return;
    }
    LxAnimator *a = ED.scene ? lx_get_animator(ED.scene, b->owner) : NULL;
    LxAnimGraph *g = a && a->graph[0] ? lx_asset_anim_graph(a->graph) : NULL;
    LxAnimParam *p = g ? lx_anim_find_param(g, b->param) : NULL;
    if (!p) return;
    p->f = b->f;
    p->i = b->i;
    p->b = b->b;
    lx_anim_graph_touch(g);
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, a->graph);
    lx_anim_graph_save(g, path);
}

static void trigger_clicked(UiNode *n) {
    AnimBind *b = n->user;
    LxEntity live = b ? anim_live(b->owner) : 0;
    if (live) lx_animator_set_trigger(ED.scene, live, b->param);
    else ui_notify(ED.ui, "Triggers fire while the game is playing");
}

/* while playing, the rows follow the animator rather than the other way round */
static void param_frame(UiNode *n) {
    AnimBind *b = n->user;
    LxEntity live = b ? anim_live(b->owner) : 0;
    if (!live || ui_active(ED.ui) == n) return;   /* not while it is being typed into */
    switch (b->type) {
    case LX_AP_FLOAT: b->f = lx_animator_get_float(ED.scene, live, b->param); break;
    case LX_AP_INT: b->i = lx_animator_get_int(ED.scene, live, b->param); break;
    default: b->b = lx_animator_get_bool(ED.scene, live, b->param); break;
    }
}

static void layer_weight_changed(UiNode *n) {
    AnimBind *b = n->user;
    LxEntity live = b ? anim_live(b->owner) : 0;
    if (live) { lx_animator_set_layer_weight(ED.scene, live, b->layer, b->f); return; }
    LxAnimator *a = ED.scene && b ? lx_get_animator(ED.scene, b->owner) : NULL;
    LxAnimGraph *g = a && a->graph[0] ? lx_asset_anim_graph(a->graph) : NULL;
    if (!g || !b || b->layer < 0 || b->layer >= g->layer_count) return;
    g->layers[b->layer].weight = b->f;
    lx_anim_graph_touch(g);
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, a->graph);
    lx_anim_graph_save(g, path);
}

static void layer_frame(UiNode *n) {
    AnimBind *b = n->user;
    LxEntity live = b ? anim_live(b->owner) : 0;
    if (!live) { ui_set_text(n, "%s", ""); return; }
    const char *state = lx_animator_state(ED.scene, live, b->layer);
    float t = lx_animator_normalized_time(ED.scene, live, b->layer);
    if (lx_animator_in_transition(ED.scene, live, b->layer)) ui_set_text(n, "%s (blending)", state);
    else ui_set_text(n, "%s  %.0f%%", state, (double)((t - floorf(t)) * 100));
    n->fg = UI_HEX(0xffb03a);
}

static void open_animator_clicked(UiNode *n) { ed_animator_show((LxEntity)n->tag); }
static void new_graph_clicked(UiNode *n) { ed_animator_new_for((LxEntity)n->tag); }

static void graph_slot_changed(UiNode *n) {
    LX_UNUSED(n);
    ed_mark_changed();
    ed_commit();
    ED.inspector_dirty = true;
}

void ed_animator_section(UiNode *sec, LxEntity e) {
    UiCtx *ui = ED.ui;
    LxAnimator *a = lx_get_animator(ED.scene, e);
    if (!a) return;
    if (g_anim_entity != e) { g_preview = false; g_preview_time = 0; }
    g_anim_entity = e;
    g_abind_n = 0;
    UiNode *r = ed_prop_row(sec, "Model");
    UiNode *mf = ed_slot(r, a->model, sizeof a->model, "model");
    mf->on_change = model_slot_changed;
    LxModel *m = lx_asset_model(a->model);

    /* ---- the graph ---- */
    r = ed_prop_row(sec, "Animator Graph");
    UiNode *gf = ed_slot(r, a->graph, sizeof a->graph, "animator");
    gf->on_change = graph_slot_changed;
    lx_strcpy(gf->tooltip, sizeof gf->tooltip, "The state machine that decides what this object plays");
    LxAnimGraph *g = a->graph[0] ? lx_asset_anim_graph(a->graph) : NULL;
    UiNode *btns = ui_box(ui, sec, UI_ROW);
    btns->h = 28;
    btns->gap = 6;
    btns->cross_align = UI_ALIGN_CENTER;
    if (g) {
        UiNode *ob = ui_button(ui, btns, "Open in Animator", open_animator_clicked);
        ob->icon = UI_ICON_ANIMATOR;
        ob->h = 24;
        ob->grow = 1;
        ob->tag = (intptr_t)e;
    } else {
        UiNode *nb = ui_button(ui, btns, a->graph[0] ? "Replace With A New Graph" : "New Animator Graph", new_graph_clicked);
        nb->icon = UI_ICON_PLUS;
        nb->h = 24;
        nb->grow = 1;
        nb->tag = (intptr_t)e;
    }

    row_float(sec, "Speed", &a->speed, 0.05f, -10, 10, anim_changed);
    UiNode *rm = row_bool(sec, "Root Motion", &a->root_motion, anim_changed);
    lx_strcpy(rm->tooltip, sizeof rm->tooltip, "Let the clip move the character across the ground (otherwise it animates in place)");
    UiNode *fk = row_bool(sec, "Foot IK", &a->foot_ik, anim_changed);
    lx_strcpy(fk->tooltip, sizeof fk->tooltip, "Put the feet on the ground under them - needs a humanoid rig and something to stand on");
    if (a->foot_ik) row_float(sec, "Foot IK Weight", &a->foot_ik_weight, 0.05f, 0, 1, anim_changed);

    if (!g) {
        /* ---- no graph: the single clip it plays instead ---- */
        if (a->graph[0]) hint(sec, "This animator graph can't be read.");
        r = ed_prop_row(sec, "Clip");
        UiNode *cs = ed_slot(r, a->clip, sizeof a->clip, "animation");
        ed_slot_set_context(cs, a->model);
        cs->on_change = clip_slot_changed;
        row_bool(sec, "Loop", &a->loop, anim_changed);
        row_bool(sec, "Play On Start", &a->play_on_start, anim_changed);
        hint(sec, "One clip at a time. Give it an animator graph for states, blend trees and transitions.");
    } else {
        /* ---- the graph's parameters, live while the game runs ---- */
        LxEntity live = anim_live(e);
        if (g->param_count) {
            UiNode *h = ui_label(ui, sec, "Parameters");
            h->font = UI_FONT_BOLD;
            h->h = 24;
            for (int i = 0; i < g->param_count && g_abind_n < ANIM_BIND_MAX; i++) {
                LxAnimParam *p = &g->params[i];
                AnimBind *b = &g_abind[g_abind_n++];
                memset(b, 0, sizeof *b);
                b->owner = e;
                b->type = p->type;
                lx_strcpy(b->param, sizeof b->param, p->name);
                b->f = live ? lx_animator_get_float(ED.scene, live, p->name) : p->f;
                b->i = live ? lx_animator_get_int(ED.scene, live, p->name) : p->i;
                b->b = live ? lx_animator_get_bool(ED.scene, live, p->name) : p->b;
                UiNode *pr = ed_prop_row(sec, p->name);
                UiNode *w;
                if (p->type == LX_AP_TRIGGER) {
                    w = ui_button(ui, pr, "Fire", trigger_clicked);
                    w->h = 22;
                    w->grow = 1;
                } else if (p->type == LX_AP_BOOL) {
                    w = ui_checkbox(ui, pr, "", &b->b);
                    w->on_change = param_changed;
                } else if (p->type == LX_AP_INT) {
                    w = ui_int(ui, pr, &b->i, -1000000, 1000000);
                    w->grow = 1;
                    w->h = 22;
                    w->on_change = param_changed;
                } else {
                    w = ui_number(ui, pr, &b->f, 0.05f);
                    w->grow = 1;
                    w->h = 22;
                    w->on_change = param_changed;
                }
                w->user = b;
                if (p->type != LX_AP_TRIGGER) w->on_frame = param_frame;
            }
        }
        /* ---- the layers, and what each is playing ---- */
        UiNode *h = ui_label(ui, sec, g->layer_count > 1 ? "Layers" : "Layer");
        h->font = UI_FONT_BOLD;
        h->h = 24;
        for (int i = 0; i < g->layer_count && g_abind_n < ANIM_BIND_MAX; i++) {
            AnimBind *b = &g_abind[g_abind_n++];
            memset(b, 0, sizeof *b);
            b->owner = e;
            b->layer = i;
            b->f = live ? lx_animator_layer_weight(ED.scene, live, i) : g->layers[i].weight;
            UiNode *lr = ed_prop_row(sec, g->layers[i].name);
            if (i == 0) {
                UiNode *base = ui_label(ui, lr, "base");
                base->fg = ui_theme(ui)->text_dim;
                base->font = UI_FONT_SMALL;
                base->grow = 1;
            } else {
                UiNode *sl = ui_slider(ui, lr, &b->f, 0, 1);
                sl->grow = 1;
                sl->h = 22;
                sl->user = b;
                sl->on_change = layer_weight_changed;
            }
            UiNode *st = ui_label(ui, lr, "");
            st->w = 120;
            st->font = UI_FONT_SMALL;
            st->user = b;
            st->on_frame = layer_frame;
        }
        char line[200];
        int states = 0;
        for (int i = 0; i < g->layer_count; i++) states += g->layers[i].state_count;
        snprintf(line, sizeof line, "%s: %d state%s, %d parameter%s", g->name, states, states == 1 ? "" : "s",
                 g->param_count, g->param_count == 1 ? "" : "s");
        hint(sec, line);
        if (ED.play == ED_EDIT) hint(sec, "Press Play to drive the parameters and watch the states change.");
    }

    if (m) {
        char line[160];
        snprintf(line, sizeof line, "%s rig · %d bones%s", lx_model_rig(m) == LX_RIG_HUMANOID ? "Humanoid" : lx_model_rig(m) == LX_RIG_GENERIC ? "Generic" : "No",
                 lx_model_bone_count(m), lx_model_rig(m) == LX_RIG_HUMANOID ? " · plays any humanoid clip" : "");
        hint(sec, line);
    }
    if (!g) {
        UiNode *pr = ui_box(ui, sec, UI_ROW);
        pr->h = 30;
        pr->gap = 8;
        pr->cross_align = UI_ALIGN_CENTER;
        pr->on_frame = preview_frame;
        UiNode *pb = ui_button(ui, pr, g_preview ? "Stop Preview" : "Preview", preview_toggle);
        pb->icon = g_preview ? UI_ICON_STOP : UI_ICON_PLAY;
        pb->w = 140;
        pb->h = 26;
        pb->disabled = ED.play != ED_EDIT || !a->clip[0];
        g_time_label = ui_label(ui, pr, "");
        ui_track(&g_time_label);
        g_time_label->fg = ui_theme(ui)->text_dim;
        g_time_label->grow = 1;
    }
}

/* ======================================================================
 *  Avatar window
 * ====================================================================== */
typedef struct Joint { LxHumanBone bone; float x, y; } Joint;
/* the body seen from the front: the character's left is on the right of the picture */
static const Joint BODY_JOINTS[] = {
    {LX_HB_HEAD, 0.5f, 0.085f}, {LX_HB_JAW, 0.5f, 0.125f}, {LX_HB_LEFT_EYE, 0.53f, 0.075f}, {LX_HB_RIGHT_EYE, 0.47f, 0.075f},
    {LX_HB_NECK, 0.5f, 0.165f}, {LX_HB_UPPER_CHEST, 0.5f, 0.225f}, {LX_HB_CHEST, 0.5f, 0.29f}, {LX_HB_SPINE, 0.5f, 0.36f},
    {LX_HB_HIPS, 0.5f, 0.45f},
    {LX_HB_LEFT_SHOULDER, 0.565f, 0.2f}, {LX_HB_RIGHT_SHOULDER, 0.435f, 0.2f},
    {LX_HB_LEFT_UPPER_ARM, 0.65f, 0.215f}, {LX_HB_RIGHT_UPPER_ARM, 0.35f, 0.215f},
    {LX_HB_LEFT_LOWER_ARM, 0.73f, 0.345f}, {LX_HB_RIGHT_LOWER_ARM, 0.27f, 0.345f},
    {LX_HB_LEFT_HAND, 0.8f, 0.46f}, {LX_HB_RIGHT_HAND, 0.2f, 0.46f},
    {LX_HB_LEFT_UPPER_LEG, 0.56f, 0.475f}, {LX_HB_RIGHT_UPPER_LEG, 0.44f, 0.475f},
    {LX_HB_LEFT_LOWER_LEG, 0.575f, 0.685f}, {LX_HB_RIGHT_LOWER_LEG, 0.425f, 0.685f},
    {LX_HB_LEFT_FOOT, 0.585f, 0.885f}, {LX_HB_RIGHT_FOOT, 0.415f, 0.885f},
    {LX_HB_LEFT_TOES, 0.6f, 0.955f}, {LX_HB_RIGHT_TOES, 0.4f, 0.955f},
};
/* the bones drawn between joints */
static const int BODY_LINKS[][2] = {
    {LX_HB_HIPS, LX_HB_SPINE}, {LX_HB_SPINE, LX_HB_CHEST}, {LX_HB_CHEST, LX_HB_UPPER_CHEST}, {LX_HB_UPPER_CHEST, LX_HB_NECK}, {LX_HB_NECK, LX_HB_HEAD},
    {LX_HB_UPPER_CHEST, LX_HB_LEFT_SHOULDER}, {LX_HB_LEFT_SHOULDER, LX_HB_LEFT_UPPER_ARM}, {LX_HB_LEFT_UPPER_ARM, LX_HB_LEFT_LOWER_ARM},
    {LX_HB_LEFT_LOWER_ARM, LX_HB_LEFT_HAND}, {LX_HB_UPPER_CHEST, LX_HB_RIGHT_SHOULDER}, {LX_HB_RIGHT_SHOULDER, LX_HB_RIGHT_UPPER_ARM},
    {LX_HB_RIGHT_UPPER_ARM, LX_HB_RIGHT_LOWER_ARM}, {LX_HB_RIGHT_LOWER_ARM, LX_HB_RIGHT_HAND}, {LX_HB_HIPS, LX_HB_LEFT_UPPER_LEG},
    {LX_HB_LEFT_UPPER_LEG, LX_HB_LEFT_LOWER_LEG}, {LX_HB_LEFT_LOWER_LEG, LX_HB_LEFT_FOOT}, {LX_HB_LEFT_FOOT, LX_HB_LEFT_TOES},
    {LX_HB_HIPS, LX_HB_RIGHT_UPPER_LEG}, {LX_HB_RIGHT_UPPER_LEG, LX_HB_RIGHT_LOWER_LEG}, {LX_HB_RIGHT_LOWER_LEG, LX_HB_RIGHT_FOOT},
    {LX_HB_RIGHT_FOOT, LX_HB_RIGHT_TOES}};

static char g_av_ref[LX_REF_MAX];
static char g_av[LX_HB_COUNT][64];        /* the mapping being edited */
static int g_av_sel = LX_HB_HIPS;
static int g_av_view;                     /* 0 body, 1 left hand, 2 right hand */
static int g_av_idx[LX_HB_COUNT];         /* dropdown choice per human bone */
#define MAX_RIG_BONES 512
static char g_rig_names[MAX_RIG_BONES + 1][64];
static const char *g_rig_opts[MAX_RIG_BONES + 1];
static int g_rig_count;
static UiNode *g_av_list, *g_av_status, *g_av_rows[LX_HB_COUNT];

static uint32_t joint_color(LxHumanBone b) {
    const UiTheme *t = ui_theme(ED.ui);
    if (g_av[b][0]) return t->success;
    return lx_human_bone_required(b) ? t->danger : ui_alpha(t->text_dim, 0.7f);
}

/* hand joints for the hand views: fingers fan out from the wrist */
static int hand_joints(bool left, Joint *out) {
    int n = 0;
    LxHumanBone base = left ? LX_HB_LEFT_THUMB_PROXIMAL : LX_HB_RIGHT_THUMB_PROXIMAL;
    out[n++] = (Joint){left ? LX_HB_LEFT_HAND : LX_HB_RIGHT_HAND, 0.5f, 0.86f};
    static const float fx[5] = {0.22f, 0.36f, 0.5f, 0.64f, 0.77f}, fy[5] = {0.6f, 0.42f, 0.38f, 0.42f, 0.5f};
    for (int f = 0; f < 5; f++)
        for (int j = 0; j < 3; j++) {
            float x = fx[f], y = fy[f] - (float)j * (f == 0 ? 0.1f : 0.11f);
            if (f == 0) x -= (float)j * 0.07f;
            if (!left) x = 1.0f - x;   /* seen from the front, the right hand mirrors the left */
            out[n++] = (Joint){(LxHumanBone)(base + f * 3 + j), x, y};
        }
    return n;
}

static void diagram_rect(UiNode *n, float *x, float *y, float *w, float *h) {
    float k = ui_scale(ED.ui);
    float aw = n->cw - 20 * k, ah = n->ch - 20 * k;
    float side = lx_minf(aw, ah * 0.62f);   /* the figure is taller than wide */
    *w = side;
    *h = side / 0.62f;
    if (*h > ah) { *h = ah; *w = ah * 0.62f; }
    *x = n->x + (n->cw - *w) * 0.5f;
    *y = n->y + (n->ch - *h) * 0.5f;
}

static void draw_limb(UiCtx *ui, float x0, float y0, float x1, float y1, float th, uint32_t c) {
    ui_draw_line(ui, x0, y0, x1, y1, th, c);
    ui_draw_circle(ui, x0, y0, th * 0.5f, c, 16);
    ui_draw_circle(ui, x1, y1, th * 0.5f, c, 16);
}

static void avatar_draw(UiNode *n, UiCtx *ui) {
    const UiTheme *t = ui_theme(ui);
    float k = ui_scale(ui);
    ui_draw_rect_gradient(ui, n->x, n->y, n->cw, n->ch, UI_HEX(0x1f2023), UI_HEX(0x17181b), 8 * k, false);
    float X, Y, W, H;
    diagram_rect(n, &X, &Y, &W, &H);
    uint32_t skin = UI_RGBA(120, 64, 50, 150), skin_edge = UI_RGBA(200, 110, 80, 90);
    if (g_av_view == 0) {
        /* silhouette: head, torso, limbs */
        #define PX(u) (X + (u) * W)
        #define PY(v) (Y + (v) * H)
        ui_draw_circle(ui, PX(0.5f), PY(0.09f), W * 0.085f, skin, 28);
        ui_draw_ring(ui, PX(0.5f), PY(0.09f), W * 0.085f, k, skin_edge, 28);
        ui_draw_rect(ui, PX(0.4f), PY(0.185f), W * 0.2f, H * 0.29f, skin, W * 0.06f);
        draw_limb(ui, PX(0.5f), PY(0.14f), PX(0.5f), PY(0.2f), W * 0.07f, skin);   /* neck */
        draw_limb(ui, PX(0.42f), PY(0.205f), PX(0.58f), PY(0.205f), W * 0.09f, skin);   /* shoulders */
        for (int side = 0; side < 2; side++) {
            float sx = side ? 1.0f : -1.0f;
            #define SX(u) PX(0.5f + sx * ((u) - 0.5f))
            draw_limb(ui, SX(0.65f), PY(0.215f), SX(0.73f), PY(0.345f), W * 0.075f, skin);
            draw_limb(ui, SX(0.73f), PY(0.345f), SX(0.8f), PY(0.46f), W * 0.06f, skin);
            ui_draw_circle(ui, SX(0.815f), PY(0.49f), W * 0.04f, skin, 16);
            draw_limb(ui, SX(0.56f), PY(0.48f), SX(0.575f), PY(0.685f), W * 0.1f, skin);
            draw_limb(ui, SX(0.575f), PY(0.685f), SX(0.585f), PY(0.885f), W * 0.08f, skin);
            draw_limb(ui, SX(0.585f), PY(0.9f), SX(0.6f), PY(0.955f), W * 0.06f, skin);
            #undef SX
        }
        ui_draw_rect(ui, PX(0.42f), PY(0.41f), W * 0.16f, H * 0.09f, skin, W * 0.05f);   /* pelvis */
        /* bones */
        for (size_t i = 0; i < sizeof BODY_LINKS / sizeof BODY_LINKS[0]; i++) {
            const Joint *a = NULL, *b = NULL;
            for (size_t j = 0; j < sizeof BODY_JOINTS / sizeof BODY_JOINTS[0]; j++) {
                if ((int)BODY_JOINTS[j].bone == BODY_LINKS[i][0]) a = &BODY_JOINTS[j];
                if ((int)BODY_JOINTS[j].bone == BODY_LINKS[i][1]) b = &BODY_JOINTS[j];
            }
            if (!a || !b) continue;
            bool on = g_av[a->bone][0] && g_av[b->bone][0];
            ui_draw_line(ui, PX(a->x), PY(a->y), PX(b->x), PY(b->y), 2 * k, on ? ui_alpha(t->success, 0.7f) : ui_alpha(t->text_dim, 0.35f));
        }
        for (size_t j = 0; j < sizeof BODY_JOINTS / sizeof BODY_JOINTS[0]; j++) {
            const Joint *jt = &BODY_JOINTS[j];
            float r = (jt->bone == LX_HB_LEFT_EYE || jt->bone == LX_HB_RIGHT_EYE || jt->bone == LX_HB_JAW) ? 5 * k : 7.5f * k;
            uint32_t c = joint_color(jt->bone);
            if ((int)jt->bone == g_av_sel) ui_draw_circle(ui, PX(jt->x), PY(jt->y), r + 6 * k, ui_alpha(t->glow, 0.45f), 24);
            ui_draw_circle(ui, PX(jt->x), PY(jt->y), r, c, 20);
            ui_draw_ring(ui, PX(jt->x), PY(jt->y), r, 1.5f * k, (int)jt->bone == g_av_sel ? UI_HEX(0xffffff) : UI_RGBA(0, 0, 0, 160), 20);
        }
        ui_draw_text(ui, UI_FONT_SMALL, n->x + 10 * k, n->y + 8 * k, "Right", t->text_dim);
        float lw = ui_text_width(ui, UI_FONT_SMALL, "Left");
        ui_draw_text(ui, UI_FONT_SMALL, n->x + n->cw - lw - 10 * k, n->y + 8 * k, "Left", t->text_dim);
        #undef PX
        #undef PY
    } else {
        bool left = g_av_view == 1;
        Joint js[16];
        int nj = hand_joints(left, js);
        float hx = X, hy = Y + H * 0.15f, hw = W, hh = H * 0.7f;
        #define HX(u) (hx + (u) * hw)
        #define HY(v) (hy + (v) * hh)
        ui_draw_rect(ui, HX(0.3f), HY(0.55f), hw * 0.4f, hh * 0.38f, skin, hw * 0.08f);
        for (int f = 0; f < 5; f++) {
            const Joint *a = &js[1 + f * 3], *c = &js[1 + f * 3 + 2];
            draw_limb(ui, HX(a->x), HY(a->y), HX(c->x), HY(c->y - 0.08f), hw * 0.075f, skin);
            for (int j = 0; j < 3; j++) {
                const Joint *p = j == 0 ? &js[0] : &js[1 + f * 3 + j - 1], *q = &js[1 + f * 3 + j];
                bool on = g_av[p->bone][0] && g_av[q->bone][0];
                ui_draw_line(ui, HX(p->x), HY(p->y), HX(q->x), HY(q->y), 2 * k, on ? ui_alpha(t->success, 0.7f) : ui_alpha(t->text_dim, 0.35f));
            }
        }
        for (int j = 0; j < nj; j++) {
            float r = (j == 0 ? 8.0f : 6.0f) * k;
            if ((int)js[j].bone == g_av_sel) ui_draw_circle(ui, HX(js[j].x), HY(js[j].y), r + 6 * k, ui_alpha(t->glow, 0.45f), 24);
            ui_draw_circle(ui, HX(js[j].x), HY(js[j].y), r, joint_color(js[j].bone), 20);
            ui_draw_ring(ui, HX(js[j].x), HY(js[j].y), r, 1.5f * k, (int)js[j].bone == g_av_sel ? UI_HEX(0xffffff) : UI_RGBA(0, 0, 0, 160), 20);
        }
        ui_draw_text(ui, UI_FONT_SMALL, n->x + 10 * k, n->y + 8 * k, left ? "Left hand (palm towards you)" : "Right hand (palm towards you)", t->text_dim);
        #undef HX
        #undef HY
    }
    /* the selected bone's name under the figure */
    char cap[160];
    snprintf(cap, sizeof cap, "%s  ->  %s", lx_human_bone_name((LxHumanBone)g_av_sel), g_av[g_av_sel][0] ? g_av[g_av_sel] : "(not mapped)");
    float cw = ui_text_width(ui, UI_FONT_BOLD, cap);
    ui_draw_text(ui, UI_FONT_BOLD, n->x + (n->cw - cw) * 0.5f, n->y + n->ch - 26 * k, cap, t->text);
}

static void select_bone(int b) {
    g_av_sel = b;
    for (int i = 0; i < LX_HB_COUNT; i++) if (g_av_rows[i]) g_av_rows[i]->selected = i == b;
    if (g_av_list && g_av_rows[b]) {   /* bring its row into view */
        float k = ui_scale(ED.ui);
        float top = g_av_rows[b]->y - g_av_list->y + g_av_list->scroll_pos;
        if (top < g_av_list->scroll_pos || top > g_av_list->scroll_pos + g_av_list->ch - 30 * k) g_av_list->scroll_pos = lx_maxf(0, top - 60 * k);
    }
}

static void avatar_frame(UiNode *n) {
    if (!ui_is_hot(ED.ui, n) || !lx_mouse_pressed(LX_MOUSE_LEFT)) return;
    LxVec2 mp = lx_mouse_pos();
    float X, Y, W, H, k = ui_scale(ED.ui);
    diagram_rect(n, &X, &Y, &W, &H);
    float best = 22 * k;
    int pick = -1;
    if (g_av_view == 0) {
        for (size_t j = 0; j < sizeof BODY_JOINTS / sizeof BODY_JOINTS[0]; j++) {
            float dx = X + BODY_JOINTS[j].x * W - mp.x, dy = Y + BODY_JOINTS[j].y * H - mp.y, d = sqrtf(dx * dx + dy * dy);
            if (d < best) { best = d; pick = BODY_JOINTS[j].bone; }
        }
    } else {
        Joint js[16];
        int nj = hand_joints(g_av_view == 1, js);
        float hx = X, hy = Y + H * 0.15f, hw = W, hh = H * 0.7f;
        for (int j = 0; j < nj; j++) {
            float dx = hx + js[j].x * hw - mp.x, dy = hy + js[j].y * hh - mp.y, d = sqrtf(dx * dx + dy * dy);
            if (d < best) { best = d; pick = js[j].bone; }
        }
    }
    if (pick >= 0) select_bone(pick);
}

void ed_avatar_refresh_dots(void);
static void status_update(void) {
    ed_avatar_refresh_dots();
    if (!g_av_status) return;
    char missing[400];
    int mapped = 0;
    for (int b = 0; b < LX_HB_COUNT; b++) mapped += g_av[b][0] != 0;
    if (lx_avatar_is_valid(g_av, missing, sizeof missing)) {
        ui_set_text(g_av_status, "Valid avatar: %d of %d bones mapped", mapped, (int)LX_HB_COUNT);
        g_av_status->fg = ui_theme(ED.ui)->success;
        g_av_status->icon = UI_ICON_SUCCESS;
    } else {
        ui_set_text(g_av_status, "Missing required: %s", missing);
        g_av_status->fg = ui_theme(ED.ui)->warning;
        g_av_status->icon = UI_ICON_WARNING;
    }
    g_av_status->icon_color = g_av_status->fg;
}

static void sync_indices(void) {
    for (int b = 0; b < LX_HB_COUNT; b++) {
        g_av_idx[b] = 0;
        for (int i = 1; i <= g_rig_count; i++) if (!strcmp(g_rig_names[i], g_av[b])) { g_av_idx[b] = i; break; }
    }
}

static void bone_choice_changed(UiNode *n) {
    int b = (int)n->tag;
    int i = g_av_idx[b];
    lx_strcpy(g_av[b], 64, i > 0 && i <= g_rig_count ? g_rig_names[i] : "");
    select_bone(b);
    status_update();
}
static void row_pick(UiNode *n) {
    int b = (int)n->tag;
    select_bone(b);
    /* hand bones switch the picture to that hand */
    if (b >= LX_HB_LEFT_THUMB_PROXIMAL && b <= LX_HB_LEFT_LITTLE_DISTAL) g_av_view = 1;
    else if (b >= LX_HB_RIGHT_THUMB_PROXIMAL && b <= LX_HB_RIGHT_LITTLE_DISTAL) g_av_view = 2;
    else g_av_view = 0;
}

static void av_automap(UiNode *n) {
    LX_UNUSED(n);
    int k = lx_avatar_automap(lx_asset_model(g_av_ref), g_av);
    sync_indices();
    status_update();
    ui_notify(ED.ui, "Automap: %d bones mapped", k);
}
static void av_clear(UiNode *n) {
    LX_UNUSED(n);
    for (int b = 0; b < LX_HB_COUNT; b++) g_av[b][0] = 0;
    sync_indices();
    status_update();
}
static void av_view(UiNode *n) { g_av_view = (int)n->tag; }
static void av_cancel(UiNode *n) { LX_UNUSED(n); ui_close_modal(ED.ui); g_av_list = NULL; g_av_status = NULL; memset(g_av_rows, 0, sizeof g_av_rows); }
static void av_done(UiNode *n) {
    char path[LX_PATH_MAX];
    lx_asset_path(path, sizeof path, g_av_ref);
    LxModelImport imp;
    if (!lx_model_import_load(path, &imp)) {
        LxModel *m = lx_asset_model(g_av_ref);
        imp = m ? *lx_model_settings(m) : lx_model_import_defaults();
    }
    imp.rig = LX_RIG_HUMANOID;
    memcpy(imp.avatar, g_av, sizeof g_av);
    if (lx_model_import_save(path, &imp)) {
        lx_model_reload(g_av_ref);
        ui_notify(ED.ui, "Avatar saved for %s", lx_path_filename(g_av_ref));
    }
    if (!strcmp(g_imp_ref, g_av_ref)) { g_imp = imp; g_imp_dirty = false; }
    ED.inspector_dirty = true;
    av_cancel(n);
}

static const struct { const char *title; int first, last; } GROUPS[] = {
    {"Body", LX_HB_HIPS, -1}, {"Head", -2, -1}, {"Left Arm", -3, -1}, {"Right Arm", -4, -1}, {"Left Leg", -5, -1}, {"Right Leg", -6, -1},
    {"Left Hand", LX_HB_LEFT_THUMB_PROXIMAL, LX_HB_LEFT_LITTLE_DISTAL}, {"Right Hand", LX_HB_RIGHT_THUMB_PROXIMAL, LX_HB_RIGHT_LITTLE_DISTAL}};
static int group_of(int b) {
    switch (b) {
    case LX_HB_HIPS: case LX_HB_SPINE: case LX_HB_CHEST: case LX_HB_UPPER_CHEST: return 0;
    case LX_HB_NECK: case LX_HB_HEAD: case LX_HB_LEFT_EYE: case LX_HB_RIGHT_EYE: case LX_HB_JAW: return 1;
    case LX_HB_LEFT_SHOULDER: case LX_HB_LEFT_UPPER_ARM: case LX_HB_LEFT_LOWER_ARM: case LX_HB_LEFT_HAND: return 2;
    case LX_HB_RIGHT_SHOULDER: case LX_HB_RIGHT_UPPER_ARM: case LX_HB_RIGHT_LOWER_ARM: case LX_HB_RIGHT_HAND: return 3;
    case LX_HB_LEFT_UPPER_LEG: case LX_HB_LEFT_LOWER_LEG: case LX_HB_LEFT_FOOT: case LX_HB_LEFT_TOES: return 4;
    case LX_HB_RIGHT_UPPER_LEG: case LX_HB_RIGHT_LOWER_LEG: case LX_HB_RIGHT_FOOT: case LX_HB_RIGHT_TOES: return 5;
    default: return b >= LX_HB_LEFT_THUMB_PROXIMAL && b <= LX_HB_LEFT_LITTLE_DISTAL ? 6 : 7;
    }
}

void ed_avatar_window(const char *ref) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    LxModel *m = lx_asset_model(ref);
    if (!m) return;
    lx_strcpy(g_av_ref, sizeof g_av_ref, ref);
    /* start from the settings being edited (or the saved ones) */
    if (!strcmp(g_imp_ref, ref)) memcpy(g_av, g_imp.avatar, sizeof g_av);
    else memcpy(g_av, lx_model_settings(m)->avatar, sizeof g_av);
    bool any = false;
    for (int b = 0; b < LX_HB_COUNT; b++) any |= g_av[b][0] != 0;
    if (!any) lx_avatar_automap(m, g_av);
    g_rig_count = 0;
    lx_strcpy(g_rig_names[0], 64, "None");
    g_rig_opts[0] = g_rig_names[0];
    for (int i = 0; i < lx_model_bone_count(m) && g_rig_count < MAX_RIG_BONES; i++) {
        g_rig_count++;
        lx_strcpy(g_rig_names[g_rig_count], 64, lx_model_bone_name(m, i));
        g_rig_opts[g_rig_count] = g_rig_names[g_rig_count];
    }
    sync_indices();
    memset(g_av_rows, 0, sizeof g_av_rows);

    char title[LX_NAME_MAX + 32];
    snprintf(title, sizeof title, "Avatar - %s", lx_path_filename(ref));
    UiNode *body = ui_modal(ui, title, 980, 700);
    UiNode *top = ui_box(ui, body, UI_ROW);
    top->h = 32;
    top->gap = 6;
    top->cross_align = UI_ALIGN_CENTER;
    static const char *views[] = {"Body", "Left Hand", "Right Hand"};
    for (int v = 0; v < 3; v++) {
        UiNode *b = ui_button(ui, top, views[v], av_view);
        b->tag = v;
        b->w = 100;
        b->h = 26;
    }
    ui_spacer(ui, top);
    UiNode *am = ui_button(ui, top, "Automap", av_automap);
    am->icon = UI_ICON_AVATAR;
    am->w = 110;
    am->h = 26;
    lx_strcpy(am->tooltip, sizeof am->tooltip, "Map bones from their names: Unity, Mixamo, Unreal, Blender/Rigify, 3ds Max Biped");
    UiNode *cl = ui_button(ui, top, "Clear", av_clear);
    cl->w = 80;
    cl->h = 26;

    UiNode *split = ui_box(ui, body, UI_ROW);
    split->grow = 1;
    split->gap = 12;
    UiNode *diagram = ui_new(ui, split, UI_CUSTOM);
    diagram->w = 400;
    diagram->on_draw = avatar_draw;
    diagram->on_frame = avatar_frame;
    lx_strcpy(diagram->tooltip, sizeof diagram->tooltip, "Click a joint to choose its bone");
    UiNode *list = ui_box(ui, split, UI_COL);
    list->grow = 1;
    list->scroll = true;
    list->gap = 2;
    ui_pad(list, 4, 4, 8, 4);
    list->bg = t->panel_alt;
    list->radius = 6;
    g_av_list = list;
    ui_track(&g_av_list);
    for (int g = 0; g < 8; g++) {
        UiNode *gh = ui_label(ui, list, GROUPS[g].title);
        gh->font = UI_FONT_SMALL;
        gh->fg = t->accent;
        gh->h = 22;
        ui_pad(gh, 6, 6, 0, 0);
        for (int b = 0; b < LX_HB_COUNT; b++) {
            if (group_of(b) != g) continue;
            UiNode *row = ui_box(ui, list, UI_ROW);
            row->h = 26;
            row->gap = 6;
            row->cross_align = UI_ALIGN_CENTER;
            row->radius = 4;
            row->bg_hover = t->hover;
            row->tag = b;
            row->on_click = row_pick;
            row->selected = b == g_av_sel;
            ui_pad(row, 6, 0, 4, 0);
            UiNode *dot = ui_label(ui, row, "");
            dot->icon = UI_ICON_AVATAR;
            dot->icon_size = 12;
            dot->w = 16;
            dot->icon_color = joint_color((LxHumanBone)b);
            char label[80];
            snprintf(label, sizeof label, "%s%s", lx_human_bone_name((LxHumanBone)b), lx_human_bone_required((LxHumanBone)b) ? " *" : "");
            UiNode *nm = ui_label(ui, row, label);
            nm->w = 190;
            UiNode *dd = ui_dropdown(ui, row, &g_av_idx[b], g_rig_opts, g_rig_count + 1);
            dd->grow = 1;
            dd->h = 22;
            dd->tag = b;
            dd->on_change = bone_choice_changed;
            g_av_rows[b] = row;
            ui_track(&g_av_rows[b]);
        }
    }
    UiNode *bottom = ui_box(ui, body, UI_ROW);
    bottom->h = 34;
    bottom->gap = 8;
    bottom->cross_align = UI_ALIGN_CENTER;
    g_av_status = ui_label(ui, bottom, "");
    ui_track(&g_av_status);
    g_av_status->grow = 1;
    status_update();
    UiNode *cancel = ui_button(ui, bottom, "Cancel", av_cancel);
    cancel->w = 90;
    UiNode *done = ui_button(ui, bottom, "Done", av_done);
    done->w = 110;
    done->bg = t->accent;
    done->fg = t->accent_text;
}

/* the joint dots follow the mapping as it changes */
void ed_avatar_refresh_dots(void) {
    for (int b = 0; b < LX_HB_COUNT; b++)
        if (g_av_rows[b] && g_av_rows[b]->first) g_av_rows[b]->first->icon_color = joint_color((LxHumanBone)b);
}
