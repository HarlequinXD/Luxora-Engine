/* What you see: the Camera, Renderer, Material, Light, Screen, Environment, Cursor and Graphics
 * categories.
 *
 * A material reference points at a shared asset, so changing one through these actions changes it for
 * every object using it — the same bargain Unity's sharedMaterial makes. Set Renderer swaps which
 * material an object uses, which is what you usually want. */
#include "fsm_actions.h"

/* ==========================================================================================
 *  Camera
 * ========================================================================================== */
static LxEntity fsm_camera(LxFsmCtx *c, int p) {
    LxEntity e = lx_fsm_is_none(c, p) ? 0 : fsm_obj(c, p);
    if (e && lx_get_camera(c->scene, e)) return e;
    return lx_scene_main_camera(c->scene);
}

static void cam_set(LxFsmCtx *c) {
    LxEntity e = fsm_camera(c, 0);
    LxCamera *cam = e ? lx_get_camera(c->scene, e) : NULL;
    if (!cam) return;
    if (!lx_fsm_is_none(c, 1)) cam->fov_deg = lx_clampf(lx_fsm_float(c, 1), 1, 179);
    if (!lx_fsm_is_none(c, 2)) cam->near_clip = lx_maxf(0.001f, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) cam->far_clip = lx_maxf(cam->near_clip + 0.01f, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) cam->clear_color = lx_fsm_color(c, 4);
    if (!lx_fsm_is_none(c, 5)) cam->orthographic = lx_fsm_bool(c, 5);
    if (!lx_fsm_is_none(c, 6)) cam->ortho_size = lx_maxf(0.01f, lx_fsm_float(c, 6));
    if (!lx_fsm_is_none(c, 7)) cam->priority = lx_fsm_int(c, 7);
}
static const LxFsmActionDesc A_CAM_SET = {"Set Camera", "Camera", "Changes a camera's field of view, clipping planes, background and projection. None leaves a setting alone.",
    "camera {0}",
    {POBJNT("Camera", "None uses the scene's main camera"), PT("Field Of View", LX_FSM_FLOAT, "", "degrees; smaller zooms in"),
     PIN_OPT("Near Clip", LX_FSM_FLOAT), PIN_OPT("Far Clip", LX_FSM_FLOAT), PIN_OPT("Background Color", LX_FSM_COLOR),
     PT("Orthographic", LX_FSM_BOOL, "", "flat projection with no perspective"), PT("Ortho Size", LX_FSM_FLOAT, "", "half the view height when orthographic"),
     PT("Priority", LX_FSM_INT, "", "the highest priority camera is the one that renders")},
    0, 0, LX_FSM_ONCE, cam_set, cam_set, NULL};

static void cam_get(LxFsmCtx *c) {
    LxEntity e = fsm_camera(c, 0);
    LxCamera *cam = e ? lx_get_camera(c->scene, e) : NULL;
    lx_fsm_set_object(c, 1, e);
    if (!cam) return;
    lx_fsm_set_float(c, 2, cam->fov_deg);
    lx_fsm_set_color(c, 3, cam->clear_color);
    lx_fsm_set_bool(c, 4, cam->orthographic);
    lx_fsm_set_vec3(c, 5, lx_entity_forward(c->scene, e));
    lx_fsm_set_vec3(c, 6, lx_entity_world_position(c->scene, e));
}
static const LxFsmActionDesc A_CAM_GET = {"Get Camera", "Camera", "The main camera (or a given one) and its settings, facing and position.", "camera info",
    {POBJNT("Camera", "None uses the scene's main camera"), POPT("Store Camera", LX_FSM_OBJECT),
     POPT("Store Field Of View", LX_FSM_FLOAT), POPT("Store Background Color", LX_FSM_COLOR),
     POPT("Store Is Orthographic", LX_FSM_BOOL), POPT("Store Forward", LX_FSM_VEC3), POPT("Store Position", LX_FSM_VEC3)},
    0, 0, LX_FSM_EVERY_FRAME, cam_get, cam_get, NULL};

typedef struct { float from, t; bool started; } FsmZoom;
static void cam_zoom(LxFsmCtx *c) {
    FsmZoom *z = c->state;
    LxEntity e = fsm_camera(c, 0);
    LxCamera *cam = e ? lx_get_camera(c->scene, e) : NULL;
    if (!cam) { lx_fsm_finish(c); return; }
    if (!z->started) { z->started = true; z->from = cam->fov_deg; }
    float len = lx_fsm_float(c, 2);
    z->t += c->dt;
    float k = len > 0 ? lx_saturate(z->t / len) : 1.0f;
    cam->fov_deg = lx_lerpf(z->from, lx_fsm_float(c, 1), lx_ease(fsm_ease(lx_fsm_int(c, 3)), k));
    if (k >= 1.0f) { lx_fsm_finish(c); fsm_send(c, 4); }
}
static const LxFsmActionDesc A_CAM_ZOOM = {"Zoom Camera", "Camera", "Eases a camera's field of view to a new value: aiming down sights, dramatic push-ins.",
    "zoom to {1} over {2}s",
    {POBJN("Camera"), P("Target Field Of View", LX_FSM_FLOAT, "40"), P("Duration", LX_FSM_FLOAT, "0.25"),
     PENUM("Ease", FSM_EASES, "3"), PEV("Finish Event")},
    sizeof(FsmZoom), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, cam_zoom, cam_zoom, NULL};

static void cam_world_to_screen(LxFsmCtx *c) {
    LxEntity t = fsm_obj(c, 0);
    LxVec3 world = t ? lx_entity_world_position(c->scene, t) : lx_fsm_vec3(c, 1);
    world = lx_add3(world, lx_fsm_vec3(c, 2));
    LxVec2 out = {0, 0};
    bool on = lx_ui_world_to_canvas(c->scene, world, &out);
    lx_fsm_set_vec3(c, 3, lx_v3(out.x, out.y, 0));
    lx_fsm_set_bool(c, 4, on);
    fsm_send(c, on ? 5 : 6);
}
static const LxFsmActionDesc A_CAM_WORLD_TO_SCREEN = {"World To Screen Point", "Camera", "Where a world position lands on screen: health bars over heads, waypoint markers.",
    "{0} on screen -> {3}",
    {POBJNT("Object", "None uses the position below"), P("World Position", LX_FSM_VEC3, "0 0 0"),
     PT("Offset", LX_FSM_VEC3, "0 2 0", "added to the world position, e.g. above the head"),
     POUT("Store Screen Position", LX_FSM_VEC2), POPT("Store Is On Screen", LX_FSM_BOOL),
     PEV("On Screen Event"), PEV("Off Screen Event")},
    0, 0, LX_FSM_EVERY_FRAME, cam_world_to_screen, cam_world_to_screen, NULL};

static void cam_is_visible(LxFsmCtx *c) {
    LxEntity t = fsm_obj(c, 0);
    if (!t) { fsm_send(c, 3); return; }
    LxVec2 out = {0, 0};
    bool on = lx_ui_world_to_canvas(c->scene, lx_entity_world_position(c->scene, t), &out);
    lx_fsm_set_bool(c, 1, on);
    fsm_send(c, on ? 2 : 3);
}
static const LxFsmActionDesc A_CAM_IS_VISIBLE = {"Is Visible To Camera", "Camera", "True when an object's position is somewhere on screen.", "{0} visible?",
    {POBJ("Object"), POPT("Store Result", LX_FSM_BOOL), PEV("Visible Event"), PEV("Not Visible Event")},
    0, 0, LX_FSM_EVERY_FRAME, cam_is_visible, cam_is_visible, NULL};

/* ==========================================================================================
 *  Renderer
 * ========================================================================================== */
static void rn_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxMeshRenderer *m = e ? lx_get_mesh(c->scene, e) : NULL;
    if (!m) return;
    const char *mat = lx_fsm_asset(c, 1);
    if (mat && mat[0]) lx_strcpy(m->material, sizeof m->material, mat);
    const char *mesh = lx_fsm_asset(c, 2);
    if (mesh && mesh[0]) lx_strcpy(m->mesh, sizeof m->mesh, mesh);
    if (!lx_fsm_is_none(c, 3)) m->cast_shadows = lx_fsm_bool(c, 3);
    if (!lx_fsm_is_none(c, 4)) {
        bool want = lx_fsm_bool(c, 4);
        if (want != m->static_geometry) { m->static_geometry = want; lx_scene_mark_static_dirty(c->scene); }
    }
}
static const LxFsmActionDesc A_RN_SET = {"Set Renderer", "Renderer", "Swaps an object's material or mesh and controls whether it casts shadows.",
    "renderer of {0}",
    {POBJ("Object"), PASSETT("Material", "material", "leave empty to keep the current one"),
     PASSETT("Mesh", "mesh", "leave empty to keep the current one"),
     PIN_OPT("Cast Shadows", LX_FSM_BOOL), PT("Static Geometry", LX_FSM_BOOL, "", "takes part in baked lighting and acoustics")},
    0, 0, LX_FSM_ONCE, rn_set, rn_set, NULL};

static void rn_get(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxMeshRenderer *m = e ? lx_get_mesh(c->scene, e) : NULL;
    lx_fsm_set_bool(c, 3, m != NULL);
    if (!m) return;
    lx_fsm_set_string(c, 1, m->material);
    lx_fsm_set_string(c, 2, m->mesh);
    lx_fsm_set_bool(c, 4, m->cast_shadows);
}
static const LxFsmActionDesc A_RN_GET = {"Get Renderer", "Renderer", "Which material and mesh an object is drawn with.", "renderer of {0}",
    {POBJ("Object"), POPT("Store Material", LX_FSM_STRING), POPT("Store Mesh", LX_FSM_STRING),
     POPT("Store Has Renderer", LX_FSM_BOOL), POPT("Store Casts Shadows", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, rn_get, rn_get, NULL};

static void rn_visible(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    bool show = lx_fsm_int(c, 1) == 0 ? true : (lx_fsm_int(c, 1) == 1 ? false : !lx_entity_active(c->scene, e));
    lx_entity_set_active(c->scene, e, show);
}
static const LxFsmActionDesc A_RN_VISIBLE = {"Set Renderer Visible", "Renderer", "Shows or hides an object by switching it on or off.", "{0} visible: {1}",
    {POBJ("Object"), PENUM("Set", "Show|Hide|Toggle", "0")}, 0, 0, LX_FSM_ONCE, rn_visible, rn_visible, NULL};

/* ==========================================================================================
 *  Material
 * ========================================================================================== */
static void mt_set(LxFsmCtx *c) {
    const char *ref = lx_fsm_asset(c, 0);
    LxMaterial *m = (ref && ref[0]) ? lx_asset_material(ref) : NULL;
    if (!m) {
        /* no material named: fall back to the one the object is drawn with */
        LxEntity e = fsm_obj(c, 1);
        LxMeshRenderer *r = e ? lx_get_mesh(c->scene, e) : NULL;
        if (r && r->material[0]) m = lx_asset_material(r->material);
    }
    if (!m) return;
    if (!lx_fsm_is_none(c, 2)) m->albedo = lx_fsm_color(c, 2);
    if (!lx_fsm_is_none(c, 3)) m->metallic = lx_saturate(lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) m->roughness = lx_saturate(lx_fsm_float(c, 4));
    if (!lx_fsm_is_none(c, 5)) { LxVec4 k = lx_fsm_color(c, 5); m->emissive = lx_v3(k.x, k.y, k.z); }
    if (!lx_fsm_is_none(c, 6)) m->emissive_strength = lx_maxf(0, lx_fsm_float(c, 6));
    if (!lx_fsm_is_none(c, 7)) m->uv_scale = lx_fsm_float(c, 7);
}
static const LxFsmActionDesc A_MT_SET = {"Set Material Properties", "Material", "Changes a material's colour, metalness, roughness and glow. The material is shared, so every object using it changes.",
    "material {0}",
    {PASSETT("Material", "material", "leave empty to use the object's own material"),
     POBJNT("Object", "used when no Material is named"),
     PIN_OPT("Color", LX_FSM_COLOR), PT("Metallic", LX_FSM_FLOAT, "", "0 is plastic, 1 is bare metal"),
     PT("Roughness", LX_FSM_FLOAT, "", "0 is a mirror, 1 is chalk"),
     PT("Emissive Color", LX_FSM_COLOR, "", "the colour it glows"), PT("Emissive Strength", LX_FSM_FLOAT, "", "how brightly it glows"),
     PT("UV Scale", LX_FSM_FLOAT, "", "how many times the texture repeats")},
    0, 0, LX_FSM_ONCE, mt_set, mt_set, NULL};

static void mt_get(LxFsmCtx *c) {
    const char *ref = lx_fsm_asset(c, 0);
    LxMaterial *m = (ref && ref[0]) ? lx_asset_material(ref) : NULL;
    if (!m) {
        LxEntity e = fsm_obj(c, 1);
        LxMeshRenderer *r = e ? lx_get_mesh(c->scene, e) : NULL;
        if (r && r->material[0]) m = lx_asset_material(r->material);
    }
    lx_fsm_set_bool(c, 5, m != NULL);
    if (!m) return;
    lx_fsm_set_color(c, 2, m->albedo);
    lx_fsm_set_float(c, 3, m->metallic);
    lx_fsm_set_float(c, 4, m->roughness);
}
static const LxFsmActionDesc A_MT_GET = {"Get Material Properties", "Material", "Reads a material's colour, metalness and roughness.", "material {0}",
    {PASSET("Material", "material"), POBJN("Object"), POPT("Store Color", LX_FSM_COLOR),
     POPT("Store Metallic", LX_FSM_FLOAT), POPT("Store Roughness", LX_FSM_FLOAT), POPT("Store Found", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, mt_get, mt_get, NULL};

typedef struct { LxVec4 from; float t; bool started; } FsmMatFade;
static void mt_fade_color(LxFsmCtx *c) {
    FsmMatFade *f = c->state;
    LxEntity e = fsm_obj(c, 0);
    LxMeshRenderer *r = e ? lx_get_mesh(c->scene, e) : NULL;
    LxMaterial *m = (r && r->material[0]) ? lx_asset_material(r->material) : NULL;
    if (!m) { lx_fsm_finish(c); return; }
    if (!f->started) { f->started = true; f->from = m->albedo; }
    float len = lx_fsm_float(c, 2);
    f->t += c->dt;
    float k = len > 0 ? lx_saturate(f->t / len) : 1.0f;
    k = lx_ease(fsm_ease(lx_fsm_int(c, 3)), k);
    LxVec4 to = lx_fsm_color(c, 1);
    m->albedo = lx_v4(lx_lerpf(f->from.x, to.x, k), lx_lerpf(f->from.y, to.y, k),
                      lx_lerpf(f->from.z, to.z, k), lx_lerpf(f->from.w, to.w, k));
    if (f->t >= len) { lx_fsm_finish(c); fsm_send(c, 4); }
}
static const LxFsmActionDesc A_MT_FADE = {"Fade Material Color", "Material", "Blends an object's material colour to a new one over time: damage flashes, highlights.",
    "fade {0} to {1} over {2}s",
    {POBJ("Object"), P("Target Color", LX_FSM_COLOR, "1 0 0 1"), P("Duration", LX_FSM_FLOAT, "0.2"),
     PENUM("Ease", FSM_EASES, "0"), PEV("Finish Event")},
    sizeof(FsmMatFade), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, mt_fade_color, mt_fade_color, NULL};

/* ==========================================================================================
 *  Light
 * ========================================================================================== */
static void li_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) return;
    if (!lx_fsm_is_none(c, 1)) { LxVec4 k = lx_fsm_color(c, 1); l->color = lx_v3(k.x, k.y, k.z); }
    if (!lx_fsm_is_none(c, 2)) l->intensity = lx_maxf(0, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) l->range = lx_maxf(0, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) l->spot_angle_deg = lx_clampf(lx_fsm_float(c, 4), 1, 179);
    if (!lx_fsm_is_none(c, 5)) l->cast_shadows = lx_fsm_bool(c, 5);
}
static const LxFsmActionDesc A_LI_SET = {"Set Light", "Light", "Changes a light's colour, brightness, reach, cone and shadows. None leaves a setting alone.",
    "light {0}",
    {POBJ("Light"), PIN_OPT("Color", LX_FSM_COLOR), PIN_OPT("Intensity", LX_FSM_FLOAT),
     PT("Range", LX_FSM_FLOAT, "", "point and spot lights: how far the light reaches"),
     PT("Spot Angle", LX_FSM_FLOAT, "", "spot lights: how wide the cone is"), PIN_OPT("Cast Shadows", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, li_set, li_set, NULL};

static void li_get(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    lx_fsm_set_bool(c, 4, l != NULL);
    if (!l) return;
    lx_fsm_set_color(c, 1, lx_v4(l->color.x, l->color.y, l->color.z, 1));
    lx_fsm_set_float(c, 2, l->intensity);
    lx_fsm_set_float(c, 3, l->range);
}
static const LxFsmActionDesc A_LI_GET = {"Get Light", "Light", "Reads a light's colour, brightness and reach.", "light {0}",
    {POBJ("Light"), POPT("Store Color", LX_FSM_COLOR), POPT("Store Intensity", LX_FSM_FLOAT),
     POPT("Store Range", LX_FSM_FLOAT), POPT("Store Has Light", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, li_get, li_get, NULL};

typedef struct { float t; float base; bool started; } FsmLightAnim;
static void li_flicker(LxFsmCtx *c) {
    FsmLightAnim *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) return;
    if (!s->started) { s->started = true; s->base = l->intensity; }
    s->t += c->dt * lx_maxf(0.0001f, lx_fsm_float(c, 2));
    /* two sine waves of different speeds read as an irregular flicker */
    float n = sinf(s->t * 6.2831853f) * 0.6f + sinf(s->t * 11.17f) * 0.4f;
    float amount = lx_saturate(lx_fsm_float(c, 1));
    l->intensity = lx_maxf(0, s->base * (1.0f - amount * 0.5f + n * amount * 0.5f));
}
static void li_flicker_exit(LxFsmCtx *c) {
    FsmLightAnim *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (l && s->started) l->intensity = s->base;
}
static const LxFsmActionDesc A_LI_FLICKER = {"Flicker Light", "Light", "Wobbles a light's brightness: torches, fires, failing strip lights. Its old brightness comes back when the state ends.",
    "flicker {0}",
    {POBJ("Light"), PT("Amount", LX_FSM_FLOAT, "0.4", "0 is steady, 1 is wild"), P("Speed", LX_FSM_FLOAT, "8")},
    sizeof(FsmLightAnim), 0, LX_FSM_EVERY_FRAME, li_flicker, li_flicker, li_flicker_exit};

static void li_fade(LxFsmCtx *c) {
    FsmLightAnim *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) { lx_fsm_finish(c); return; }
    if (!s->started) { s->started = true; s->base = l->intensity; }
    float len = lx_fsm_float(c, 2);
    s->t += c->dt;
    float k = len > 0 ? lx_saturate(s->t / len) : 1.0f;
    l->intensity = lx_lerpf(s->base, lx_maxf(0, lx_fsm_float(c, 1)), k);
    if (k >= 1.0f) { lx_fsm_finish(c); fsm_send(c, 3); }
}
static const LxFsmActionDesc A_LI_FADE = {"Fade Light", "Light", "Brings a light up or down over time: dawn, a lamp switching on, a power cut.",
    "fade {0} to {1} over {2}s",
    {POBJ("Light"), P("Target Intensity", LX_FSM_FLOAT, "0"), P("Duration", LX_FSM_FLOAT, "1"), PEV("Finish Event")},
    sizeof(FsmLightAnim), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, li_fade, li_fade, NULL};

static void li_blink(LxFsmCtx *c) {
    FsmLightAnim *s = c->state;
    LxEntity e = fsm_obj(c, 0);
    LxLight *l = e ? lx_get_light(c->scene, e) : NULL;
    if (!l) return;
    if (!s->started) { s->started = true; s->base = l->intensity; }
    float period = lx_maxf(0.0001f, lx_fsm_float(c, 1));
    s->t += c->dt;
    float phase = s->t / period;
    bool on = (phase - floorf(phase)) < lx_saturate(lx_fsm_float(c, 2));
    l->intensity = on ? s->base : 0.0f;
}
static const LxFsmActionDesc A_LI_BLINK = {"Blink Light", "Light", "Switches a light on and off at a steady rate: alarms, beacons, warning lamps.",
    "blink {0} every {1}s",
    {POBJ("Light"), P("Period", LX_FSM_FLOAT, "0.5"), PT("On Fraction", LX_FSM_FLOAT, "0.5", "how much of each period it is lit")},
    sizeof(FsmLightAnim), 0, LX_FSM_EVERY_FRAME, li_blink, li_blink, li_flicker_exit};

/* ==========================================================================================
 *  Screen, cursor and environment
 * ========================================================================================== */
static void sc_get(LxFsmCtx *c) {
    int w = 0, h = 0;
    lx_window_size(&w, &h);
    LxVec2 canvas = lx_ui_canvas_size();
    lx_fsm_set_int(c, 0, w);
    lx_fsm_set_int(c, 1, h);
    lx_fsm_set_float(c, 2, h > 0 ? (float)w / (float)h : 1.0f);
    lx_fsm_set_vec3(c, 3, lx_v3(canvas.x, canvas.y, 0));
    lx_fsm_set_float(c, 4, lx_dpi_scale());
}
static const LxFsmActionDesc A_SC_GET = {"Get Screen Info", "Screen", "The window size in pixels, its shape, and the UI canvas size that layouts are measured in.",
    "screen info",
    {POPT("Store Width", LX_FSM_INT), POPT("Store Height", LX_FSM_INT), POPT("Store Aspect Ratio", LX_FSM_FLOAT),
     POPT("Store Canvas Size", LX_FSM_VEC2), POPT("Store DPI Scale", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, sc_get, sc_get, NULL};

static void cu_set(LxFsmCtx *c) {
    int mode = lx_fsm_int(c, 0);
    lx_mouse_lock(mode == 2);
    lx_mouse_show(mode == 0);
    const char *tex = lx_fsm_asset(c, 1);
    lx_mouse_set_image(tex && tex[0] ? tex : NULL, lx_fsm_float(c, 2), lx_fsm_float(c, 3), lx_fsm_float(c, 4));   /* empty: the arrow again */
}
static const LxFsmActionDesc A_CU_SET = {"Set Cursor", "Screen", "Shows, hides or locks the mouse pointer, and can give it a picture. Locked is what mouse-look needs.",
    "cursor {0}",
    {PENUMT("Mode", "Visible|Hidden|Locked", "0", "Locked hides it and holds it in the game view for mouse look"),
     PASSETT("Image", "texture", "a picture to draw as the pointer; empty = the normal arrow"),
     PT("Hotspot X", LX_FSM_FLOAT, "0", "0..1 across the image: where it actually points"),
     P("Hotspot Y", LX_FSM_FLOAT, "0"), PT("Size", LX_FSM_FLOAT, "32", "in canvas units")},
    0, 0, LX_FSM_ONCE, cu_set, cu_set, NULL};

static void en_set(LxFsmCtx *c) {
    LxEnvironment *e = lx_scene_environment(c->scene);
    if (!e) return;
    if (!lx_fsm_is_none(c, 0)) e->fog_density = lx_maxf(0, lx_fsm_float(c, 0));
    if (!lx_fsm_is_none(c, 1)) { LxVec4 k = lx_fsm_color(c, 1); e->fog_color = lx_v3(k.x, k.y, k.z); }
    if (!lx_fsm_is_none(c, 2)) e->ambient_intensity = lx_maxf(0, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) e->exposure = lx_maxf(0, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) { LxVec4 k = lx_fsm_color(c, 4); e->sky_zenith = lx_v3(k.x, k.y, k.z); }
    if (!lx_fsm_is_none(c, 5)) { LxVec4 k = lx_fsm_color(c, 5); e->sky_horizon = lx_v3(k.x, k.y, k.z); }
}
static const LxFsmActionDesc A_EN_SET = {"Set Environment", "Rendering", "Changes the fog, ambient light, exposure and sky colours: caves, storms, nightfall. None leaves a setting alone.",
    "environment",
    {PT("Fog Density", LX_FSM_FLOAT, "", "0 is clear air"), PIN_OPT("Fog Color", LX_FSM_COLOR),
     PT("Ambient", LX_FSM_FLOAT, "", "how much light comes from the sky itself"),
     PT("Exposure", LX_FSM_FLOAT, "", "overall brightness of the image"),
     PIN_OPT("Sky Top Color", LX_FSM_COLOR), PIN_OPT("Sky Horizon Color", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, en_set, en_set, NULL};

static void en_day_night(LxFsmCtx *c) {
    LxEntity sun = fsm_obj(c, 0);
    float hour = lx_fsm_float(c, 1);
    float per_day = lx_fsm_float(c, 2);
    if (per_day > 0) {
        hour += c->dt * (24.0f / per_day);
        hour = hour - floorf(hour / 24.0f) * 24.0f;
        lx_fsm_set_float(c, 1, hour);
    }
    if (lx_scene_environment(c->scene)->sky_mode == LX_SKY_DAY_NIGHT) lx_sky_set_time(c->scene, hour);   /* the real sky follows */
    else lx_day_night_apply(c->scene, sun, hour);
    bool night = hour < 6.0f || hour >= 18.0f;
    lx_fsm_set_bool(c, 5, night);
    int *was = c->state;
    int now = night ? 2 : 1;
    if (*was && *was != now) fsm_send(c, night ? 4 : 3);
    *was = now;
}
static const LxFsmActionDesc A_EN_DAY_NIGHT = {"Day Night Cycle", "Rendering", "Swings the sun through the day and colours the sky to match; the Hour keeps counting on its own.",
    "{0} at {1}h",
    {POBJT("Sun", "a directional light"), POUTT("Hour", LX_FSM_FLOAT, "a Float variable, 0..24; it keeps counting"),
     PT("Seconds Per Day", LX_FSM_FLOAT, "240", "0 = hold at this hour"), PEV("Morning Event"), PEV("Evening Event"),
     POPT("Store Is Night", LX_FSM_BOOL)},
    sizeof(int), 0, LX_FSM_EVERY_FRAME, en_day_night, en_day_night, NULL};

/* ---- the Day-night sky and weather (Rendering panel: Sky Mode Day-night) ---- */
static void sk_set_time(LxFsmCtx *c) {
    LxEnvironment *e = lx_scene_environment(c->scene);
    if (!lx_fsm_is_none(c, 0)) lx_sky_set_time(c->scene, lx_fsm_float(c, 0));
    if (!lx_fsm_is_none(c, 1)) e->day_length = lx_maxf(0.0f, lx_fsm_float(c, 1));
}
static const LxFsmActionDesc A_SK_SET_TIME = {"Set Time of Day", "Sky", "Sets the clock of the Day-night sky, and how fast it runs. The sun, moon and sky follow at once.",
    "time {0}h",
    {PT("Hour", LX_FSM_FLOAT, "12", "0..24: 6 is about sunrise, 18 about sunset"),
     PT("Day Length", LX_FSM_FLOAT, "", "real minutes per day; 0 stops the clock; None leaves it")},
    0, 0, LX_FSM_ONCE, sk_set_time, NULL, NULL};

static void sk_get_time(LxFsmCtx *c) {
    lx_fsm_set_float(c, 0, lx_sky_time(c->scene));
    lx_fsm_set_bool(c, 1, lx_sky_is_day(c->scene));
    lx_fsm_set_vec3(c, 2, lx_sky_sun_direction(c->scene));
    lx_fsm_set_int(c, 3, (int)lx_sky_weather(c->scene));
    lx_fsm_set_float(c, 4, lx_sky_wetness(c->scene));
}
static const LxFsmActionDesc A_SK_GET_TIME = {"Get Time of Day", "Sky", "The clock, whether the sun is up, where it is, the weather and how wet the ground is.",
    "time of day",
    {POPTT("Store Hour", LX_FSM_FLOAT, "0..24"), POPT("Store Is Day", LX_FSM_BOOL), POPTT("Store Sun Direction", LX_FSM_VEC3, "towards the sun"),
     POPTT("Store Weather", LX_FSM_INT, "0 Clear, 1 Cloudy, 2 Rain, 3 Storm, 4 Snow, 5 Fog, 6 Custom"), POPTT("Store Wetness", LX_FSM_FLOAT, "0..1")},
    0, 0, LX_FSM_EVERY_FRAME, sk_get_time, sk_get_time, NULL};

static void sk_at_time(LxFsmCtx *c) {
    float *last = c->state;
    float now = lx_sky_time(c->scene), at = lx_fsm_float(c, 0);
    at = at - floorf(at / 24.0f) * 24.0f;
    if (*last >= 0) {   /* the clock moved forward a little since last frame (across midnight too): did it pass the hour? */
        float step = fmodf(now - *last + 24.0f, 24.0f), to = fmodf(at - *last + 24.0f, 24.0f);
        if (step > 0 && step < 6.0f && to > 0 && to <= step) fsm_send(c, 1);
    }
    *last = now;
}
static void sk_at_time_enter(LxFsmCtx *c) { *(float *)c->state = -1.0f; sk_at_time(c); }
static const LxFsmActionDesc A_SK_AT_TIME = {"At Time of Day", "Sky", "Sends an event when the clock passes an hour: street lamps at dusk, shops opening at 9.",
    "at {0}h send {1}",
    {PT("Hour", LX_FSM_FLOAT, "20", "0..24"), PEV("Event")},
    sizeof(float), 0, LX_FSM_EVERY_FRAME, sk_at_time_enter, sk_at_time, NULL};

static void sk_weather(LxFsmCtx *c) {
    float blend = lx_fsm_is_none(c, 1) ? -1.0f : lx_maxf(0.0f, lx_fsm_float(c, 1));
    lx_sky_set_weather(c->scene, (LxWeather)lx_fsm_int(c, 0), blend);
}
static const LxFsmActionDesc A_SK_WEATHER = {"Set Weather", "Sky", "Changes the weather to a preset: the clouds, rain, snow, lightning, mist and wind all move towards it together.",
    "weather {0}",
    {PENUM("Weather", "Clear|Cloudy|Rain|Storm|Snow|Fog", "2"),
     PT("Blend Seconds", LX_FSM_FLOAT, "", "how long the change takes; 0 at once; None uses the scene's Blend Time")},
    0, 0, LX_FSM_ONCE, sk_weather, NULL, NULL};

static void sk_clouds(LxFsmCtx *c) {
    LxEnvironment *e = lx_scene_environment(c->scene);
    if (!lx_fsm_is_none(c, 0)) e->cloud_coverage = lx_saturate(lx_fsm_float(c, 0));
    if (!lx_fsm_is_none(c, 1)) e->cloud_density = lx_saturate(lx_fsm_float(c, 1));
    if (!lx_fsm_is_none(c, 2)) e->cloud_type = lx_fsm_int(c, 2);
    if (!lx_fsm_is_none(c, 3)) e->wind_speed = lx_maxf(0.0f, lx_fsm_float(c, 3));
    if (!lx_fsm_is_none(c, 4)) e->wind_direction = lx_fsm_float(c, 4);
    e->weather = LX_WEATHER_CUSTOM;
}
static const LxFsmActionDesc A_SK_CLOUDS = {"Set Clouds", "Sky", "Changes the clouds and the wind. They move there over the scene's weather Blend Time. None leaves a setting alone.",
    "clouds {0}",
    {PT("Coverage", LX_FSM_FLOAT, "", "0..1 of the sky"), PT("Density", LX_FSM_FLOAT, "", "0..1: thin .. thick and dark"),
     {"Type", LX_FSM_ENUM, "", LX_FSM_P_OPTIONAL, "Cumulus|Stratus|Storm", NULL},
     PT("Wind Speed", LX_FSM_FLOAT, "", "m/s"), PT("Wind Direction", LX_FSM_FLOAT, "", "degrees, 0 = +X")},
    0, 0, LX_FSM_ONCE, sk_clouds, NULL, NULL};

static void sk_precip(LxFsmCtx *c) {
    LxEnvironment *e = lx_scene_environment(c->scene);
    if (!lx_fsm_is_none(c, 0)) e->rain = lx_saturate(lx_fsm_float(c, 0));
    if (!lx_fsm_is_none(c, 1)) e->snow = lx_saturate(lx_fsm_float(c, 1));
    if (!lx_fsm_is_none(c, 2)) e->lightning = lx_saturate(lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) e->mist = lx_saturate(lx_fsm_float(c, 3));
    e->weather = LX_WEATHER_CUSTOM;
}
static const LxFsmActionDesc A_SK_PRECIP = {"Set Rain and Snow", "Sky", "Changes the rain, snow, lightning and mist. They move there over the scene's weather Blend Time. None leaves a setting alone.",
    "rain {0} snow {1}",
    {PT("Rain", LX_FSM_FLOAT, "", "0..1"), PT("Snow", LX_FSM_FLOAT, "", "0..1"), PT("Lightning", LX_FSM_FLOAT, "", "0..1: how often it strikes"),
     PT("Mist", LX_FSM_FLOAT, "", "0..1: weather fog")},
    0, 0, LX_FSM_ONCE, sk_precip, NULL, NULL};

static void sk_strike(LxFsmCtx *c) { lx_sky_strike_lightning(c->scene); }
static const LxFsmActionDesc A_SK_STRIKE = {"Strike Lightning", "Sky", "A lightning bolt now, somewhere around the horizon, with thunder that arrives as late as sound would.",
    "lightning", {{0}}, 0, 0, LX_FSM_ONCE, sk_strike, NULL, NULL};

static void gf_quality(LxFsmCtx *c) {
    LxGraphicsSettings *g = lx_graphics_settings();
    if (!g) return;
    int q = lx_fsm_int(c, 0);
    lx_graphics_apply_preset(g, q);
    g->quality_preset = q;
    if (!lx_fsm_is_none(c, 1)) g->vsync = lx_fsm_bool(c, 1);
    if (!lx_fsm_is_none(c, 2)) g->target_fps = lx_fsm_int(c, 2);
    if (!lx_fsm_is_none(c, 3)) g->render_scale = lx_clampf(lx_fsm_float(c, 3), 0.5f, 2.0f);
}
static const LxFsmActionDesc A_GF_QUALITY = {"Set Graphics Quality", "Rendering", "Applies a quality preset and the frame rate settings: exactly what an options menu needs.",
    "quality {0}",
    {PENUM("Quality", "Low|Medium|High|Ultra", "2"), PIN_OPT("VSync", LX_FSM_BOOL),
     PT("Target FPS", LX_FSM_INT, "", "0 = unlimited"), PT("Render Scale", LX_FSM_FLOAT, "", "0.5 to 2.0")},
    0, 0, LX_FSM_ONCE, gf_quality, gf_quality, NULL};

static void gf_stats(LxFsmCtx *c) {
    const LxRenderStats *s = lx_render_stats();
    if (!s) return;
    lx_fsm_set_int(c, 0, s->draw_calls);
    lx_fsm_set_int(c, 1, s->instances_drawn);
    lx_fsm_set_float(c, 2, s->frame_ms);
    lx_fsm_set_float(c, 3, s->frame_ms > 0 ? 1000.0f / s->frame_ms : 0);
}
static const LxFsmActionDesc A_GF_STATS = {"Get Render Stats", "Rendering", "Draw calls, objects drawn and frame time: for on-screen performance readouts.",
    "render stats",
    {POPT("Store Draw Calls", LX_FSM_INT), POPT("Store Instances Drawn", LX_FSM_INT),
     POPT("Store Frame Ms", LX_FSM_FLOAT), POPT("Store FPS", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, gf_stats, gf_stats, NULL};

static void gf_refresh(LxFsmCtx *c) {
    (void)c;
    lx_reflections_refresh();
    lx_gi_reset();
}
static const LxFsmActionDesc A_GF_REFRESH = {"Refresh Lighting", "Rendering", "Recaptures the reflection probes and restarts the bounced-light accumulation after the scene changes a lot.",
    "refresh lighting",
    {{0}}, 0, 0, LX_FSM_ONCE, gf_refresh, NULL, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_render(void) {
    static const LxFsmActionDesc *all[] = {
        &A_CAM_SET, &A_CAM_GET, &A_CAM_ZOOM, &A_CAM_WORLD_TO_SCREEN, &A_CAM_IS_VISIBLE,
        &A_RN_SET, &A_RN_GET, &A_RN_VISIBLE,
        &A_MT_SET, &A_MT_GET, &A_MT_FADE,
        &A_LI_SET, &A_LI_GET, &A_LI_FLICKER, &A_LI_FADE, &A_LI_BLINK,
        &A_SC_GET, &A_CU_SET, &A_EN_SET, &A_EN_DAY_NIGHT, &A_GF_QUALITY, &A_GF_STATS, &A_GF_REFRESH,
        &A_SK_SET_TIME, &A_SK_GET_TIME, &A_SK_AT_TIME, &A_SK_WEATHER, &A_SK_CLOUDS, &A_SK_PRECIP, &A_SK_STRIKE,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
