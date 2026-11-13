/* Scene viewport: editor camera, picking, transform gizmos, icons and drag-and-drop. */
#include "editor.h"

static struct {
    UiNode *view;
    int nav;               /* 0 none, 1 look, 2 pan, 3 orbit */
    int axis;              /* gizmo axis being dragged: -1 none, 0..2 axes, 3 free */
    int hover_axis;
    LxVec3 drag_start_pos, drag_start_scale, drag_start_euler;
    LxQuat drag_start_rot;
    float drag_t0;
    float drag_angle0;
    LxVec2 press_mouse;
    bool pressed_in_view;
    LxVec3 orbit_pivot;
    float orbit_dist;
    LxMat4 vp, view_m, proj_m;
    LxVec3 eye;
    LxEntity hover_icon;
} V;

/* The Game view looks through the scene's main camera, in edit mode and while playing. */
static bool showing_game(void) { return ED.game_view && ED.scene && lx_scene_main_camera(ED.scene) != 0; }
bool ed_viewport_showing_game(void) { return showing_game(); }

/* ---------------- math helpers ---------------- */
static LxQuat cam_rot(void) { return lx_quat_euler_deg(lx_v3(ED.cam_pitch, ED.cam_yaw, 0)); }

static void editor_view(LxRenderView *rv, int w, int h) {
    memset(rv, 0, sizeof *rv);
    LxMat4 world = lx_mat4_trs(ED.cam_pos, cam_rot(), lx_v3(1, 1, 1));
    rv->view = lx_mat4_inverse(world);
    rv->proj = lx_mat4_perspective(ED.cam_fov * LX_DEG2RAD, h > 0 ? (float)w / (float)h : 1.0f, 0.05f, 2000.0f);
    rv->eye = ED.cam_pos;
    rv->width = w;
    rv->height = h;
    rv->near_clip = 0.05f;
    rv->far_clip = 2000.0f;
    rv->clear_color = lx_v4(0.1f, 0.1f, 0.12f, 1);
}

static bool to_screen(LxVec3 p, LxVec2 *out) {
    LxVec4 c = lx_mat4_v4(V.vp, lx_v4(p.x, p.y, p.z, 1));
    if (c.w <= 1e-4f) return false;
    UiNode *n = V.view;
    out->x = n->x + (c.x / c.w * 0.5f + 0.5f) * n->cw;
    out->y = n->y + (1.0f - (c.y / c.w * 0.5f + 0.5f)) * n->ch;
    return true;
}

static LxRay mouse_ray(void) {
    UiNode *n = V.view;
    LxVec2 m = lx_mouse_pos();
    float nx = (m.x - n->x) / lx_maxf(1, n->cw) * 2.0f - 1.0f, ny = 1.0f - (m.y - n->y) / lx_maxf(1, n->ch) * 2.0f;
    LxMat4 inv = lx_mat4_inverse(V.vp);
    LxVec4 a = lx_mat4_v4(inv, lx_v4(nx, ny, -1, 1)), b = lx_mat4_v4(inv, lx_v4(nx, ny, 1, 1));
    LxVec3 pa = lx_scale3(lx_v3(a.x, a.y, a.z), 1.0f / a.w), pb = lx_scale3(lx_v3(b.x, b.y, b.z), 1.0f / b.w);
    LxRay r = {pa, lx_norm3(lx_sub3(pb, pa))};
    return r;
}

static float dist_point_segment(LxVec2 p, LxVec2 a, LxVec2 b) {
    float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? lx_clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0, 1) : 0;
    float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return sqrtf(ex * ex + ey * ey);
}

/* parameter along the axis line closest to the mouse ray */
static float axis_param(LxVec3 origin, LxVec3 axis, LxRay r) {
    LxVec3 w0 = lx_sub3(origin, r.origin);
    float a = lx_dot3(axis, axis), b = lx_dot3(axis, r.dir), c = lx_dot3(r.dir, r.dir);
    float d = lx_dot3(axis, w0), e = lx_dot3(r.dir, w0);
    float den = a * c - b * b;
    if (fabsf(den) < 1e-6f) return 0;
    return (b * e - c * d) / den;
}

static bool ray_plane(LxRay r, LxVec3 p0, LxVec3 n, LxVec3 *hit) {
    float den = lx_dot3(n, r.dir);
    if (fabsf(den) < 1e-6f) return false;
    float t = lx_dot3(lx_sub3(p0, r.origin), n) / den;
    if (t < 0) return false;
    *hit = lx_madd3(r.origin, r.dir, t);
    return true;
}

static LxVec3 quat_to_euler_deg(LxQuat q) {
    LxMat4 m = lx_mat4_trs(lx_v3(0, 0, 0), q, lx_v3(1, 1, 1));
#define M(r, c) m.m[(c)*4 + (r)]
    float x = asinf(lx_clampf(-M(1, 2), -1, 1));
    float y = atan2f(M(0, 2), M(2, 2));
    float z = atan2f(M(1, 0), M(1, 1));
#undef M
    return lx_v3(x * LX_RAD2DEG, y * LX_RAD2DEG, z * LX_RAD2DEG);
}

static float snapf(float v, float step) { return step > 0 ? roundf(v / step) * step : v; }

/* ---------------- gizmo geometry ---------------- */
static LxVec3 gizmo_axis_dir(int a) {
    LxVec3 base = a == 0 ? lx_v3(1, 0, 0) : a == 1 ? lx_v3(0, 1, 0) : lx_v3(0, 0, 1);
    if (ED.gizmo_local && ED.selected) {
        LxMat4 w = lx_entity_world_matrix(ED.scene, ED.selected);
        return lx_norm3(lx_mat4_dir(w, base));
    }
    return base;
}

static float gizmo_scale(LxVec3 c) { return lx_len3(lx_sub3(c, V.eye)) * 0.16f; }

static int pick_axis(LxVec3 c) {
    LxVec2 m = lx_mouse_pos(), sc;
    if (!to_screen(c, &sc)) return -1;
    float s = gizmo_scale(c), best = 9.0f * ui_scale(ED.ui);
    int axis = -1;
    if (ED.gizmo == ED_GIZMO_ROTATE) {
        for (int a = 0; a < 3; a++) {
            LxVec3 ax = gizmo_axis_dir(a), u = lx_norm3(lx_cross3(ax, fabsf(ax.y) < 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0))), v = lx_cross3(ax, u);
            LxVec2 prev = {0, 0};
            bool have = false;
            for (int i = 0; i <= 48; i++) {
                float ang = (float)i / 48 * 2 * LX_PI;
                LxVec3 p = lx_add3(c, lx_add3(lx_scale3(u, cosf(ang) * s), lx_scale3(v, sinf(ang) * s)));
                LxVec2 q;
                if (!to_screen(p, &q)) { have = false; continue; }
                if (have) {
                    float d = dist_point_segment(m, prev, q);
                    if (d < best) { best = d; axis = a; }
                }
                prev = q;
                have = true;
            }
        }
        return axis;
    }
    for (int a = 0; a < 3; a++) {
        LxVec2 e;
        if (!to_screen(lx_madd3(c, gizmo_axis_dir(a), s), &e)) continue;
        float d = dist_point_segment(m, sc, e);
        if (d < best) { best = d; axis = a; }
    }
    float dc = sqrtf((m.x - sc.x) * (m.x - sc.x) + (m.y - sc.y) * (m.y - sc.y));
    if (dc < 10.0f * ui_scale(ED.ui)) axis = 3;
    return axis;
}

/* ---------------- icons ---------------- */
static bool needs_icon(LxEntity e) {
    uint32_t c = lx_entity_components(ED.scene, e);
    return (c & (LX_COMP_LIGHT | LX_COMP_CAMERA | LX_COMP_AUDIO_SOURCE)) || (!(c & LX_COMP_MESH) && !(c & LX_COMP_CROWD));
}

static LxEntity icon_under_mouse(void) {
    if (!ED.show_icons) return 0;
    LxVec2 m = lx_mouse_pos();
    float r = 13.0f * ui_scale(ED.ui);
    LxEntity best = 0;
    float bd = r;
    int n = lx_scene_entity_count(ED.scene);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        if (!needs_icon(e)) continue;
        LxVec2 s;
        if (!to_screen(lx_entity_world_position(ED.scene, e), &s)) continue;
        float d = sqrtf((m.x - s.x) * (m.x - s.x) + (m.y - s.y) * (m.y - s.y));
        if (d < bd) { bd = d; best = e; }
    }
    return best;
}

/* ---------------- interaction ---------------- */
static void begin_gizmo_drag(int axis) {
    LxScene *s = ED.scene;
    V.axis = axis;
    V.drag_start_pos = lx_entity_position(s, ED.selected);
    V.drag_start_scale = lx_entity_scale(s, ED.selected);
    V.drag_start_euler = lx_entity_euler(s, ED.selected);
    V.drag_start_rot = lx_entity_rotation(s, ED.selected);
    LxVec3 c = lx_entity_world_position(s, ED.selected);
    LxRay r = mouse_ray();
    if (ED.gizmo == ED_GIZMO_ROTATE && axis < 3) {
        LxVec3 ax = gizmo_axis_dir(axis), hit;
        LxVec3 u = lx_norm3(lx_cross3(ax, fabsf(ax.y) < 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0))), v = lx_cross3(ax, u);
        if (ray_plane(r, c, ax, &hit)) {
            LxVec3 d = lx_sub3(hit, c);
            V.drag_angle0 = atan2f(lx_dot3(d, v), lx_dot3(d, u));
        } else V.drag_angle0 = 0;
    } else if (axis < 3) V.drag_t0 = axis_param(c, gizmo_axis_dir(axis), r);
    V.press_mouse = lx_mouse_pos();
}

static void update_gizmo_drag(void) {
    LxScene *s = ED.scene;
    LxEntity e = ED.selected;
    if (!lx_entity_valid(s, e)) { V.axis = -1; return; }
    LxRay r = mouse_ray();
    LxVec3 c = lx_add3(V.drag_start_pos, lx_v3(0, 0, 0));
    LxEntity parent = lx_entity_parent(s, e);
    LxMat4 parent_w = parent ? lx_entity_world_matrix(s, parent) : lx_mat4_identity();
    LxVec3 world_start = lx_mat4_point(parent_w, V.drag_start_pos);
    bool snap = ED.snap || lx_key_down(LX_KEY_CTRL);
    if (ED.gizmo == ED_GIZMO_MOVE) {
        LxVec3 new_world = world_start;
        if (V.axis < 3) {
            LxVec3 ax = gizmo_axis_dir(V.axis);
            float t = axis_param(world_start, ax, r) - V.drag_t0;
            if (snap) t = snapf(t, ED.snap_move);
            new_world = lx_madd3(world_start, ax, t);
        } else {
            LxQuat q = cam_rot();
            LxVec3 fwd = lx_quat_rotate(q, lx_v3(0, 0, -1)), hit;
            if (ray_plane(r, world_start, fwd, &hit)) new_world = hit;
            if (snap) { new_world.x = snapf(new_world.x, ED.snap_move); new_world.y = snapf(new_world.y, ED.snap_move); new_world.z = snapf(new_world.z, ED.snap_move); }
        }
        LxMat4 inv = lx_mat4_inverse(parent_w);
        lx_entity_set_position(s, e, lx_mat4_point(inv, new_world));
    } else if (ED.gizmo == ED_GIZMO_ROTATE && V.axis < 3) {
        LxVec3 ax = gizmo_axis_dir(V.axis), hit;
        LxVec3 u = lx_norm3(lx_cross3(ax, fabsf(ax.y) < 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0))), v = lx_cross3(ax, u);
        if (ray_plane(r, world_start, ax, &hit)) {
            LxVec3 d = lx_sub3(hit, world_start);
            float ang = atan2f(lx_dot3(d, v), lx_dot3(d, u)) - V.drag_angle0;
            float deg = ang * LX_RAD2DEG;
            if (snap) deg = snapf(deg, ED.snap_rotate);
            /* axis in parent space */
            LxVec3 local_axis = lx_norm3(lx_mat4_dir(lx_mat4_inverse(parent_w), ax));
            LxQuat q = lx_quat_norm(lx_quat_mul(lx_quat_axis_angle(local_axis, deg * LX_DEG2RAD), V.drag_start_rot));
            lx_entity_set_euler(s, e, quat_to_euler_deg(q));
        }
    } else if (ED.gizmo == ED_GIZMO_SCALE) {
        if (V.axis < 3) {
            LxVec3 ax = gizmo_axis_dir(V.axis);
            float len = gizmo_scale(world_start);
            float t = (axis_param(world_start, ax, r) - V.drag_t0) / lx_maxf(len, 1e-3f);
            LxVec3 sc = V.drag_start_scale;
            float *comp = V.axis == 0 ? &sc.x : V.axis == 1 ? &sc.y : &sc.z;
            *comp = *comp * lx_maxf(0.01f, 1.0f + t);
            if (snap) *comp = lx_maxf(ED.snap_scale, snapf(*comp, ED.snap_scale));
            lx_entity_set_scale(s, e, sc);
        } else {
            float f = lx_maxf(0.01f, 1.0f + (lx_mouse_pos().x - V.press_mouse.x) / (120.0f * ui_scale(ED.ui)));
            LxVec3 sc = lx_scale3(V.drag_start_scale, f);
            if (snap) { sc.x = snapf(sc.x, ED.snap_scale); sc.y = snapf(sc.y, ED.snap_scale); sc.z = snapf(sc.z, ED.snap_scale); }
            lx_entity_set_scale(s, e, sc);
        }
    }
    LX_UNUSED(c);
    ed_mark_changed();
    ED.inspector_dirty = false; /* bound fields read live values; no rebuild needed */
}

static void viewport_frame(UiNode *n) {
    if (!ED.scene) return;
    UiCtx *ui = ED.ui;
    V.view = n;
    LxRenderView rv;
    editor_view(&rv, (int)n->cw, (int)n->ch);
    bool game = showing_game();
    if (game) {
        LxEntity cam = lx_scene_main_camera(ED.scene);
        if (!cam || !lx_camera_view(ED.scene, cam, (int)n->cw, (int)n->ch, &rv)) game = false;
    }
    V.view_m = rv.view;
    V.proj_m = rv.proj;
    V.vp = lx_mat4_mul(rv.proj, rv.view);
    V.eye = rv.eye;
    if (game) return; /* the Game view is what the player sees: no editor camera, picking or gizmos */

    bool inside = ui_is_inside(ui, n) && !ui_has_modal(ui) && !ui_popup_open(ui);
    bool hot = inside && (ui_is_hot(ui, n) || ui_active(ui) == n);
    float dt = ED.dt;
    LxVec2 md = lx_mouse_delta();
    /* begin navigation */
    if (V.nav == 0 && V.axis < 0 && hot) {
        if (lx_mouse_pressed(LX_MOUSE_RIGHT)) V.nav = 1;
        else if (lx_mouse_pressed(LX_MOUSE_MIDDLE)) V.nav = 2;
        else if (lx_mouse_pressed(LX_MOUSE_LEFT) && lx_key_down(LX_KEY_ALT)) {
            V.nav = 3;
            V.orbit_pivot = ED.selected ? lx_entity_world_position(ED.scene, ED.selected) : lx_madd3(ED.cam_pos, lx_quat_rotate(cam_rot(), lx_v3(0, 0, -1)), 10);
            V.orbit_dist = lx_len3(lx_sub3(ED.cam_pos, V.orbit_pivot));
        }
    }
    LxQuat q = cam_rot();
    LxVec3 fwd = lx_quat_rotate(q, lx_v3(0, 0, -1)), right = lx_quat_rotate(q, lx_v3(1, 0, 0)), up = lx_quat_rotate(q, lx_v3(0, 1, 0));
    float look = 0.18f;
    if (V.nav == 1) {
        if (!lx_mouse_down(LX_MOUSE_RIGHT)) V.nav = 0;
        else {
            ED.cam_yaw -= md.x * look;
            ED.cam_pitch = lx_clampf(ED.cam_pitch - md.y * look, -89.5f, 89.5f);
            float speed = ED.cam_speed * (lx_key_down(LX_KEY_SHIFT) ? 3.0f : 1.0f) * dt;
            if (lx_key_down(LX_KEY_W)) ED.cam_pos = lx_madd3(ED.cam_pos, fwd, speed);
            if (lx_key_down(LX_KEY_S)) ED.cam_pos = lx_madd3(ED.cam_pos, fwd, -speed);
            if (lx_key_down(LX_KEY_D)) ED.cam_pos = lx_madd3(ED.cam_pos, right, speed);
            if (lx_key_down(LX_KEY_A)) ED.cam_pos = lx_madd3(ED.cam_pos, right, -speed);
            if (lx_key_down(LX_KEY_E)) ED.cam_pos.y += speed;
            if (lx_key_down(LX_KEY_Q)) ED.cam_pos.y -= speed;
            float wheel = lx_mouse_wheel();
            if (wheel != 0) ED.cam_speed = lx_clampf(ED.cam_speed * (wheel > 0 ? 1.25f : 0.8f), 0.1f, 500.0f);
        }
    } else if (V.nav == 2) {
        if (!lx_mouse_down(LX_MOUSE_MIDDLE)) V.nav = 0;
        else {
            float k = 0.004f * lx_maxf(1.0f, ED.cam_speed);
            ED.cam_pos = lx_madd3(ED.cam_pos, right, -md.x * k);
            ED.cam_pos = lx_madd3(ED.cam_pos, up, md.y * k);
        }
    } else if (V.nav == 3) {
        if (!lx_mouse_down(LX_MOUSE_LEFT)) V.nav = 0;
        else {
            ED.cam_yaw -= md.x * 0.3f;
            ED.cam_pitch = lx_clampf(ED.cam_pitch - md.y * 0.3f, -89.5f, 89.5f);
            LxVec3 f2 = lx_quat_rotate(cam_rot(), lx_v3(0, 0, -1));
            ED.cam_pos = lx_madd3(V.orbit_pivot, f2, -V.orbit_dist);
        }
    } else if (hot) {
        float wheel = lx_mouse_wheel();
        if (wheel != 0) ED.cam_pos = lx_madd3(ED.cam_pos, fwd, wheel * lx_maxf(0.5f, ED.cam_speed * 0.25f));
    }

    /* gizmo / selection */
    bool can_gizmo = ED.selected && ED.gizmo != ED_GIZMO_SELECT && ED.play == ED_EDIT && lx_entity_valid(ED.scene, ED.selected);
    V.hover_axis = -1;
    if (V.axis >= 0) {
        if (!lx_mouse_down(LX_MOUSE_LEFT)) { V.axis = -1; ED.inspector_dirty = true; }
        else update_gizmo_drag();
    } else if (hot && V.nav == 0) {
        if (can_gizmo) V.hover_axis = pick_axis(lx_entity_world_position(ED.scene, ED.selected));
        V.hover_icon = icon_under_mouse();
        if (lx_mouse_pressed(LX_MOUSE_LEFT) && !lx_key_down(LX_KEY_ALT)) {
            if (V.hover_axis >= 0) begin_gizmo_drag(V.hover_axis);
            else { V.pressed_in_view = true; V.press_mouse = lx_mouse_pos(); }
        }
    }
    if (V.pressed_in_view && lx_mouse_released(LX_MOUSE_LEFT)) {
        V.pressed_in_view = false;
        LxVec2 m = lx_mouse_pos();
        if (fabsf(m.x - V.press_mouse.x) < 5 && fabsf(m.y - V.press_mouse.y) < 5 && inside) {
            LxEntity icon = icon_under_mouse();
            if (icon) ed_select(icon);
            else {
                float dist;
                LxEntity hit = lx_scene_raycast(ED.scene, mouse_ray(), 5000.0f, &dist);
                /* clicking a child of a selected hierarchy selects the child; clicking empty space clears */
                ed_select(hit);
            }
        }
    }
}

/* ---------------- drawing ---------------- */
static void draw_arrow(UiCtx *ui, LxVec2 a, LxVec2 b, uint32_t col, float th, int head) {
    ui_draw_line(ui, a.x, a.y, b.x, b.y, th, col);
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
    if (l < 1) return;
    dx /= l;
    dy /= l;
    float s = 10.0f * ui_scale(ui);
    if (head == 1) ui_draw_triangle(ui, b.x + dx * s, b.y + dy * s, b.x - dy * s * 0.5f, b.y + dx * s * 0.5f, b.x + dy * s * 0.5f, b.y - dx * s * 0.5f, col);
    else if (head == 2) ui_draw_rect(ui, b.x - s * 0.45f, b.y - s * 0.45f, s * 0.9f, s * 0.9f, col, 0);
}

static void draw_gizmo(UiCtx *ui) {
    if (!ED.selected || !lx_entity_valid(ED.scene, ED.selected) || ED.play != ED_EDIT) return;
    const UiTheme *t = ui_theme(ui);
    LxVec3 c = lx_entity_world_position(ED.scene, ED.selected);
    LxVec2 sc;
    if (!to_screen(c, &sc)) return;
    float s = gizmo_scale(c), th = 2.4f * ui_scale(ui);
    uint32_t cols[3] = {t->axis_x, t->axis_y, t->axis_z};
    int active = V.axis >= 0 ? V.axis : V.hover_axis;
    if (ED.gizmo == ED_GIZMO_SELECT) {
        ui_draw_ring(ui, sc.x, sc.y, 6 * ui_scale(ui), 2, UI_HEX(0xff8c1a), 16);
        return;
    }
    if (ED.gizmo == ED_GIZMO_ROTATE) {
        for (int a = 0; a < 3; a++) {
            LxVec3 ax = gizmo_axis_dir(a), u = lx_norm3(lx_cross3(ax, fabsf(ax.y) < 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0))), v = lx_cross3(ax, u);
            uint32_t col = a == active ? UI_HEX(0xffe066) : cols[a];
            LxVec2 prev = {0, 0};
            bool have = false;
            for (int i = 0; i <= 64; i++) {
                float ang = (float)i / 64 * 2 * LX_PI;
                LxVec3 p = lx_add3(c, lx_add3(lx_scale3(u, cosf(ang) * s), lx_scale3(v, sinf(ang) * s)));
                /* fade the back half of the ring */
                bool front = lx_dot3(lx_sub3(p, c), lx_sub3(V.eye, c)) > -0.15f * s;
                LxVec2 q;
                if (!to_screen(p, &q)) { have = false; continue; }
                if (have) ui_draw_line(ui, prev.x, prev.y, q.x, q.y, th, front ? col : ui_alpha(col, 0.25f));
                prev = q;
                have = true;
            }
        }
        ui_draw_ring(ui, sc.x, sc.y, 4 * ui_scale(ui), 2, UI_RGBA(255, 255, 255, 180), 12);
        return;
    }
    /* draw axes back to front */
    int order[3] = {0, 1, 2};
    float depth[3];
    for (int a = 0; a < 3; a++) depth[a] = -lx_len3(lx_sub3(lx_madd3(c, gizmo_axis_dir(a), s), V.eye));
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (depth[order[j]] < depth[order[i]]) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
    for (int k = 0; k < 3; k++) {
        int a = order[k];
        LxVec2 e;
        if (!to_screen(lx_madd3(c, gizmo_axis_dir(a), s), &e)) continue;
        uint32_t col = a == active ? UI_HEX(0xffe066) : cols[a];
        draw_arrow(ui, sc, e, col, th, ED.gizmo == ED_GIZMO_SCALE ? 2 : 1);
    }
    uint32_t cc = active == 3 ? UI_HEX(0xffe066) : UI_RGBA(255, 255, 255, 220);
    if (ED.gizmo == ED_GIZMO_SCALE) ui_draw_rect(ui, sc.x - 6 * ui_scale(ui), sc.y - 6 * ui_scale(ui), 12 * ui_scale(ui), 12 * ui_scale(ui), cc, 2);
    else ui_draw_ring(ui, sc.x, sc.y, 7 * ui_scale(ui), 2, cc, 16);
}

/* ---- collider wireframes for the selection ---- */
static void wire_line(UiCtx *ui, LxVec3 a, LxVec3 b, uint32_t col) {
    LxVec2 sa, sb;
    if (to_screen(a, &sa) && to_screen(b, &sb)) ui_draw_line(ui, sa.x, sa.y, sb.x, sb.y, 1.5f * ui_scale(ui), col);
}
static void wire_circle(UiCtx *ui, LxVec3 c, LxVec3 u, LxVec3 v, float r, float a0, float a1, uint32_t col) {
    const int N = 32;
    LxVec3 prev = lx_add3(c, lx_add3(lx_scale3(u, cosf(a0) * r), lx_scale3(v, sinf(a0) * r)));
    for (int i = 1; i <= N; i++) {
        float a = a0 + (a1 - a0) * (float)i / N;
        LxVec3 p = lx_add3(c, lx_add3(lx_scale3(u, cosf(a) * r), lx_scale3(v, sinf(a) * r)));
        wire_line(ui, prev, p, col);
        prev = p;
    }
}
static void wire_capsule(UiCtx *ui, LxVec3 c, LxVec3 ax, LxVec3 ay, LxVec3 az, float r, float hseg, uint32_t col) {
    LxVec3 top = lx_madd3(c, ay, hseg), bot = lx_madd3(c, ay, -hseg);
    wire_circle(ui, top, ax, az, r, 0, 2 * LX_PI, col);
    wire_circle(ui, bot, ax, az, r, 0, 2 * LX_PI, col);
    LxVec3 sides[4] = {ax, lx_scale3(ax, -1), az, lx_scale3(az, -1)};
    for (int i = 0; i < 4; i++) wire_line(ui, lx_madd3(top, sides[i], r), lx_madd3(bot, sides[i], r), col);
    wire_circle(ui, top, ax, ay, r, 0, LX_PI, col);
    wire_circle(ui, top, az, ay, r, 0, LX_PI, col);
    wire_circle(ui, bot, ax, ay, r, LX_PI, 2 * LX_PI, col);
    wire_circle(ui, bot, az, ay, r, LX_PI, 2 * LX_PI, col);
}
static void draw_collider(UiCtx *ui, LxEntity e) {
    LxMat4 w = lx_entity_world_matrix(ED.scene, e);
    LxVec3 c0 = lx_v3(w.m[0], w.m[1], w.m[2]), c1 = lx_v3(w.m[4], w.m[5], w.m[6]), c2 = lx_v3(w.m[8], w.m[9], w.m[10]);
    LxVec3 sc = lx_v3(lx_len3(c0), lx_len3(c1), lx_len3(c2));
    if (sc.x < 1e-6f || sc.y < 1e-6f || sc.z < 1e-6f) return;
    LxVec3 ax = lx_scale3(c0, 1 / sc.x), ay = lx_scale3(c1, 1 / sc.y), az = lx_scale3(c2, 1 / sc.z);
    LxVec3 pos = lx_v3(w.m[12], w.m[13], w.m[14]);
    LxCharacterController *ch = lx_get_character(ED.scene, e);
    if (ch) {
        float r = ch->radius * lx_maxf(sc.x, sc.z), h = lx_maxf(ch->height * sc.y, 2 * r);
        wire_capsule(ui, lx_madd3(pos, lx_v3(0, 1, 0), h * 0.5f), lx_v3(1, 0, 0), lx_v3(0, 1, 0), lx_v3(0, 0, 1), r, h * 0.5f - r,
                     UI_RGBA(121, 192, 255, 220));
    }
    LxCollider *col = lx_get_collider(ED.scene, e);
    if (!col) return;
    uint32_t color = col->is_trigger ? UI_RGBA(242, 204, 96, 220) : UI_RGBA(126, 231, 135, 220);
    LxVec3 c = lx_mat4_point(w, col->center);
    switch (col->shape) {
    case LX_COLLIDER_BOX: {
        LxVec3 h = lx_v3(col->size.x * sc.x * 0.5f, col->size.y * sc.y * 0.5f, col->size.z * sc.z * 0.5f), v[8];
        for (int i = 0; i < 8; i++)
            v[i] = lx_add3(c, lx_add3(lx_scale3(ax, (i & 1) ? h.x : -h.x), lx_add3(lx_scale3(ay, (i & 2) ? h.y : -h.y), lx_scale3(az, (i & 4) ? h.z : -h.z))));
        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (int i = 0; i < 12; i++) wire_line(ui, v[edges[i][0]], v[edges[i][1]], color);
        break;
    }
    case LX_COLLIDER_SPHERE: {
        float r = col->radius * lx_maxf(sc.x, lx_maxf(sc.y, sc.z));
        wire_circle(ui, c, ax, ay, r, 0, 2 * LX_PI, color);
        wire_circle(ui, c, ax, az, r, 0, 2 * LX_PI, color);
        wire_circle(ui, c, ay, az, r, 0, 2 * LX_PI, color);
        break;
    }
    case LX_COLLIDER_CAPSULE: {
        float r = col->radius * lx_maxf(sc.x, sc.z);
        wire_capsule(ui, c, ax, ay, az, r, lx_maxf(col->height * sc.y * 0.5f - r, 0), color);
        break;
    }
    default: break;   /* mesh colliders match the mesh you already see */
    }
}

/* the influence box of a selected reflection probe */
static void draw_probe_box(UiCtx *ui, LxEntity e, LxVec3 c) {
    LxReflectionProbe *p = lx_get_reflection_probe(ED.scene, e);
    if (!p) return;
    LxVec3 sc = lx_entity_scale(ED.scene, e);
    LxVec3 h = lx_v3(fabsf(p->size.x * sc.x) * 0.5f, fabsf(p->size.y * sc.y) * 0.5f, fabsf(p->size.z * sc.z) * 0.5f);
    LxVec3 v[8];
    for (int i = 0; i < 8; i++) v[i] = lx_v3(c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z));
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    uint32_t col = UI_RGBA(139, 233, 253, 200);
    for (int i = 0; i < 12; i++) {
        LxVec2 a, b;
        if (to_screen(v[edges[i][0]], &a) && to_screen(v[edges[i][1]], &b)) ui_draw_line(ui, a.x, a.y, b.x, b.y, 1.5f * ui_scale(ui), col);
    }
}

static void draw_icons(UiCtx *ui) {
    if (ED.selected && lx_entity_valid(ED.scene, ED.selected) &&
        (lx_entity_components(ED.scene, ED.selected) & (LX_COMP_COLLIDER | LX_COMP_CHARACTER)))
        draw_collider(ui, ED.selected);
    if (!ED.show_icons) return;
    int n = lx_scene_entity_count(ED.scene);
    float sz = 22 * ui_scale(ui);
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(ED.scene, i);
        if (!needs_icon(e) || !lx_entity_active(ED.scene, e)) continue;
        LxVec3 wp = lx_entity_world_position(ED.scene, e);
        LxVec2 s;
        if (!to_screen(wp, &s)) continue;
        UiIcon icon = ed_icon_for_entity(ED.scene, e);
        bool sel = e == ED.selected, hov = e == V.hover_icon;
        ui_draw_circle(ui, s.x, s.y, sz * 0.62f, sel ? UI_RGBA(255, 140, 26, 200) : UI_RGBA(22, 13, 12, hov ? 220 : 160), 20);
        ui_draw_icon(ui, icon, s.x - sz * 0.4f, s.y - sz * 0.4f, sz * 0.8f, ed_color_for_icon(icon));
        /* direction hint for selected lights and cameras */
        uint32_t c = lx_entity_components(ED.scene, e);
        if (sel && (c & LX_COMP_REFLECTION_PROBE)) draw_probe_box(ui, e, wp);
        if (sel && (c & (LX_COMP_LIGHT | LX_COMP_CAMERA))) {
            LxVec3 f = lx_entity_forward(ED.scene, e);
            LxVec2 tip;
            if (to_screen(lx_madd3(wp, f, 1.5f), &tip)) draw_arrow(ui, s, tip, UI_HEX(0xf2cc60), 2 * ui_scale(ui), 1);
            LxLight *l = lx_get_light(ED.scene, e);
            if (l && l->type == LX_LIGHT_POINT) {
                LxVec2 edge;
                if (to_screen(lx_madd3(wp, lx_quat_rotate(cam_rot(), lx_v3(1, 0, 0)), l->range), &edge))
                    ui_draw_ring(ui, s.x, s.y, sqrtf((edge.x - s.x) * (edge.x - s.x) + (edge.y - s.y) * (edge.y - s.y)), 1.5f, UI_RGBA(242, 204, 96, 120), 48);
            }
        }
    }
}

static void draw_axis_triad(UiCtx *ui, UiNode *n) {
    float r = 30 * ui_scale(ui);
    float cx = n->x + n->cw - r - 16 * ui_scale(ui), cy = n->y + r + 16 * ui_scale(ui);
    ui_draw_circle(ui, cx, cy, r + 8 * ui_scale(ui), UI_RGBA(22, 13, 12, 120), 28);
    const UiTheme *t = ui_theme(ui);
    uint32_t cols[3] = {t->axis_x, t->axis_y, t->axis_z};
    const char *labels[3] = {"X", "Y", "Z"};
    for (int a = 0; a < 3; a++) {
        LxVec3 ax = a == 0 ? lx_v3(1, 0, 0) : a == 1 ? lx_v3(0, 1, 0) : lx_v3(0, 0, 1);
        LxVec3 v = lx_mat4_dir(V.view_m, ax);
        float ex = cx + v.x * r, ey = cy - v.y * r;
        ui_draw_line(ui, cx, cy, ex, ey, 2 * ui_scale(ui), cols[a]);
        ui_draw_circle(ui, ex, ey, 7 * ui_scale(ui), cols[a], 14);
        ui_draw_text(ui, UI_FONT_SMALL, ex - 3.5f * ui_scale(ui), ey - ui_line_height(ui, UI_FONT_SMALL) * 0.5f, labels[a], UI_HEX(0x120a09));
    }
}

static void viewport_draw(UiNode *n, UiCtx *ui) {
    if (!ED.viewport_rt) return;
    ui_push_clip(ui, n->x, n->y, n->cw, n->ch);
    ui_draw_image(ui, lx_render_target_texture(ED.viewport_rt), n->x, n->y, n->cw, n->ch);
    if (ED.play != ED_EDIT) {
        /* the game's own UI (HUD, menus), exactly as the player draws it */
        LxGameUiGeometry g;
        lx_game_ui_geometry(&g);
        ui_draw_mesh(ui, g.vertices, g.vertex_count, g.indices, g.index_count, g.cmds, g.cmd_count);
    }
    bool game = showing_game();
    if (!game) {
        draw_icons(ui);
        draw_gizmo(ui);
        draw_axis_triad(ui, n);
    }
    const UiTheme *t = ui_theme(ui);
    float k = ui_scale(ui);
    if (ED.game_view && ED.play == ED_EDIT) {
        /* badge naming the camera we're looking through, or why we can't */
        LxEntity cam = lx_scene_main_camera(ED.scene);
        char msg[LX_NAME_MAX + 48];
        if (cam) snprintf(msg, sizeof msg, "GAME VIEW  ·  %s", lx_entity_name(ED.scene, cam));
        else snprintf(msg, sizeof msg, "No camera in this scene - add one with Create > Camera");
        float tw = ui_text_width(ui, UI_FONT_BOLD, msg) + 44 * k;
        float bx = n->x + (n->cw - tw) * 0.5f, by = n->y + 8 * k;
        ui_draw_rect(ui, bx, by, tw, 26 * k, cam ? UI_RGBA(22, 13, 12, 210) : ui_alpha(t->warning, 0.92f), 13 * k);
        ui_draw_icon(ui, cam ? UI_ICON_CAMERA : UI_ICON_WARNING, bx + 12 * k, by + 5 * k, 16 * k, cam ? ed_color_for_icon(UI_ICON_CAMERA) : UI_HEX(0x120a09));
        ui_draw_text(ui, UI_FONT_BOLD, bx + 34 * k, by + (26 * k - ui_line_height(ui, UI_FONT_BOLD)) * 0.5f, msg, cam ? t->text : UI_HEX(0x120a09));
    }
    if (ED.play != ED_EDIT) {
        ui_draw_border(ui, n->x, n->y, n->cw, n->ch, ED.play == ED_PAUSED ? t->warning : t->success, 0, 2 * ui_scale(ui));
        const char *msg = ED.play == ED_PAUSED ? "PAUSED" : (game ? "PLAYING · Game view" : "PLAYING · Scene view");
        float tw = ui_text_width(ui, UI_FONT_BOLD, msg) + 20 * ui_scale(ui);
        ui_draw_rect(ui, n->x + (n->cw - tw) * 0.5f, n->y + 8 * ui_scale(ui), tw, 24 * ui_scale(ui), ui_alpha(ED.play == ED_PAUSED ? t->warning : t->success, 0.9f), 12 * ui_scale(ui));
        ui_draw_text(ui, UI_FONT_BOLD, n->x + (n->cw - tw) * 0.5f + 10 * ui_scale(ui), n->y + 8 * ui_scale(ui) + (24 * ui_scale(ui) - ui_line_height(ui, UI_FONT_BOLD)) * 0.5f, msg, UI_HEX(0x120a09));
    }
    if (V.nav == 1) {
        char buf[64];
        snprintf(buf, sizeof buf, "Fly · speed %.1f (scroll to change)", (double)ED.cam_speed);
        ui_draw_rect(ui, n->x + 8 * ui_scale(ui), n->y + n->ch - 30 * ui_scale(ui), ui_text_width(ui, UI_FONT_SMALL, buf) + 16 * ui_scale(ui), 22 * ui_scale(ui), UI_RGBA(22, 13, 12, 200), 4 * ui_scale(ui));
        ui_draw_text(ui, UI_FONT_SMALL, n->x + 16 * ui_scale(ui), n->y + n->ch - 30 * ui_scale(ui) + (22 * ui_scale(ui) - ui_line_height(ui, UI_FONT_SMALL)) * 0.5f, buf, t->text);
    }
    if (ED.show_stats) {
        const LxRenderStats *st = lx_render_stats();
        char buf[256];
        snprintf(buf, sizeof buf, "%.0f fps   %d draws   %lld tris   %d instances (%d culled)   GI %d photons %.1f ms", (double)ED.fps, st->draw_calls,
                 (long long)st->triangles, st->instances_drawn, st->instances_culled, st->gi_photons_traced, (double)st->gi_ms);
        ui_draw_rect(ui, n->x + 8 * ui_scale(ui), n->y + 8 * ui_scale(ui), ui_text_width(ui, UI_FONT_MONO, buf) + 16 * ui_scale(ui), 24 * ui_scale(ui), UI_RGBA(22, 13, 12, 200), 4 * ui_scale(ui));
        ui_draw_text(ui, UI_FONT_MONO, n->x + 16 * ui_scale(ui), n->y + 8 * ui_scale(ui) + (24 * ui_scale(ui) - ui_line_height(ui, UI_FONT_MONO)) * 0.5f, buf, t->text);
    }
    ui_pop_clip(ui);
}

static void viewport_drop(UiNode *n, const char *type, const char *payload) {
    LX_UNUSED(n);
    if (ED.play != ED_EDIT) return;
    LxRay r = mouse_ray();
    if (!strcmp(type, "mesh")) {
        float dist;
        LxEntity hit = lx_scene_raycast(ED.scene, r, 1000, &dist);
        LxVec3 p;
        if (hit) p = lx_madd3(r.origin, r.dir, dist);
        else if (!ray_plane(r, lx_v3(0, 0, 0), lx_v3(0, 1, 0), &p)) p = lx_madd3(r.origin, r.dir, 10);
        LxEntity e = lx_entity_create(ED.scene, lx_path_filename(payload));
        lx_entity_set_position(ED.scene, e, p);
        lx_add_mesh(ED.scene, e, payload, "builtin:default");
        ed_select(e);
        ed_mark_changed();
        ed_commit();
    } else if (!strcmp(type, "prefab")) {
        float dist;
        LxEntity hit = lx_scene_raycast(ED.scene, r, 1000, &dist);
        LxVec3 p;
        if (hit) p = lx_madd3(r.origin, r.dir, dist);
        else if (!ray_plane(r, lx_v3(0, 0, 0), lx_v3(0, 1, 0), &p)) p = lx_madd3(r.origin, r.dir, 10);
        ed_prefab_place(payload, p, 0);
    } else if (!strcmp(type, "model")) {
        float dist;
        LxEntity hit = lx_scene_raycast(ED.scene, r, 1000, &dist);
        LxVec3 p;
        if (hit) p = lx_madd3(r.origin, r.dir, dist);
        else if (!ray_plane(r, lx_v3(0, 0, 0), lx_v3(0, 1, 0), &p)) p = lx_madd3(r.origin, r.dir, 10);
        ed_model_place(payload, p, 0);
    } else if (!strcmp(type, "material")) {
        float dist;
        LxEntity hit = lx_scene_raycast(ED.scene, r, 1000, &dist);
        LxMeshRenderer *mr = hit ? lx_get_mesh(ED.scene, hit) : NULL;
        if (mr) {
            lx_strcpy(mr->material, sizeof mr->material, payload);
            ed_select(hit);
            ED.inspector_dirty = true;
            lx_scene_mark_static_dirty(ED.scene);
            ed_mark_changed();
            ed_commit();
        }
    } else if (!strcmp(type, "scene")) {
        extern bool ed_open_scene(const char *ref);
        if (!ED.dirty) ed_open_scene(payload);
        else ui_notify(ED.ui, "Save or discard changes first (File > New Scene)");
    }
}

static void vb_view(UiNode *n) { ED.game_view = n->tag == 1; }
static void vb_icons(UiNode *n) { LX_UNUSED(n); ED.show_icons = !ED.show_icons; ed_save_prefs(); }
static void vb_stats(UiNode *n) { LX_UNUSED(n); ED.show_stats = !ED.show_stats; }
static void vb_frame(UiNode *bar) {
    for (UiNode *b = bar->first; b; b = b->next) {
        if (b->on_click == vb_view) b->selected = (b->tag == 1) == ED.game_view;
        else if (b->on_click == vb_icons) b->selected = ED.show_icons;
        else if (b->on_click == vb_stats) b->selected = ED.show_stats;
    }
}

UiNode *ed_viewport_panel(UiNode *parent) {
    UiCtx *ui = ED.ui;
    const UiTheme *t = ui_theme(ui);
    UiNode *p = ui_box(ui, parent, UI_COL);
    p->bg = t->panel;
    UiNode *bar = ui_box(ui, p, UI_ROW);
    bar->h = 32;
    bar->bg = t->header;
    ui_pad(bar, 6, 3, 6, 3);
    bar->gap = 4;
    bar->cross_align = UI_ALIGN_CENTER;
    bar->on_frame = vb_frame;
    UiNode *b = ui_button(ui, bar, "Scene", vb_view);
    b->icon = UI_ICON_VIEWPORT;
    b->tag = 0;
    b->h = 26;
    b->bg_active = t->selection;
    b = ui_button(ui, bar, "Game", vb_view);
    b->icon = UI_ICON_CAMERA;
    b->tag = 1;
    b->h = 26;
    b->bg_active = t->selection;
    lx_strcpy(b->tooltip, sizeof b->tooltip, "See exactly what the player sees through the scene's main camera");
    ui_separator(ui, bar)->w = 10;
    UiNode *spd = ui_label(ui, bar, "Speed");
    spd->font = UI_FONT_SMALL;
    spd->fg = t->text_dim;
    UiNode *s = ui_slider(ui, bar, &ED.cam_speed, 0.5f, 60);
    s->w = 110;
    s->h = 22;
    s->precision = 1;
    UiNode *fov = ui_label(ui, bar, "FOV");
    fov->font = UI_FONT_SMALL;
    fov->fg = t->text_dim;
    UiNode *fs = ui_slider(ui, bar, &ED.cam_fov, 20, 120);
    fs->w = 80;
    fs->h = 22;
    fs->precision = 0;
    ui_spacer(ui, bar);
    UiNode *ic = ui_icon_button(ui, bar, UI_ICON_LIGHT, "Show object icons", vb_icons);
    ic->bg_active = t->selection;
    UiNode *st = ui_icon_button(ui, bar, UI_ICON_STATS, "Show statistics overlay", vb_stats);
    st->bg_active = t->selection;
    UiNode *v = ui_new(ui, p, UI_CUSTOM);
    v->grow = 1;
    v->on_frame = viewport_frame;
    v->on_draw = viewport_draw;
    v->accept = "mesh,material,scene,prefab,model";
    v->on_drop = viewport_drop;
    V.view = v;
    V.axis = -1;
    return p;
}

void ed_render(void) {
    if (!ED.scene || !V.view || V.view->cw < 8 || V.view->ch < 8) return;
    int w = (int)V.view->cw, h = (int)V.view->ch;
    if (!ED.viewport_rt) ED.viewport_rt = lx_render_target_create(w, h);
    else lx_render_target_resize(ED.viewport_rt, w, h);
    LxRenderView rv;
    bool game = showing_game();
    if (!(game && lx_camera_view(ED.scene, lx_scene_main_camera(ED.scene), w, h, &rv))) {
        editor_view(&rv, w, h);
        rv.draw_grid = ED.show_grid;
        rv.highlight = ED.play == ED_EDIT ? ED.selected : 0;
    }
    lx_render_scene(ED.scene, &rv, ED.viewport_rt);
}
