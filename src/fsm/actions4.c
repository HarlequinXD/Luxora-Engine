/* Built-in State Graph actions, part four: the PlayMaker set Luxora was missing. Input the way PlayMaker spells it
 * (Get Key / Get Key Down / Get Button / Get Axis Vector / Mouse Pick / touch and swipe / gamepads), plus logic, maths,
 * text, colour, transform, object, physics, application and graph-to-graph actions with PlayMaker's names, so a
 * PlayMaker user finds what they're looking for in the action browser. */
#include "../lx_internal.h"
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#define P(n, t, d) {n, t, d, 0, NULL, NULL}
#define PT(n, t, d, tip) {n, t, d, 0, NULL, tip}
#define POUT(n, t) {n, t, "", LX_FSM_P_OUT, NULL, NULL}
#define POPT(n, t) {n, t, "", LX_FSM_P_OUT | LX_FSM_P_OPTIONAL, NULL, NULL}
#define PEV(n) {n, LX_FSM_EVENT, "", LX_FSM_P_OPTIONAL, NULL, NULL}
#define PENUM(n, opts, d) {n, LX_FSM_ENUM, d, 0, opts, NULL}
#define PASSET(n, kind) {n, LX_FSM_ASSET, "", 0, kind, NULL}
#define POBJ(n) {n, LX_FSM_OBJECT, "Owner", 0, NULL, NULL}
#define POBJN(n) {n, LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, NULL}
#define KEY_TIP "a key name: A..Z, 0..9, Space, Enter, Escape, Tab, Left, Right, Up, Down, Shift, Ctrl, Alt, F1..F12"
#define PAD_BUTTONS "A|B|X|Y|LB|RB|Back|Start|Left Stick|Right Stick|D-Pad Up|D-Pad Down|D-Pad Left|D-Pad Right|Guide"
#define PAD_AXES "Left Stick X|Left Stick Y|Right Stick X|Right Stick Y|Left Trigger|Right Trigger"
#define MOUSE_BUTTONS "Left|Right|Middle"

static void ev(LxFsmCtx *c, int p) { lx_fsm_send(c, lx_fsm_event(c, p)); }
static LxEntity obj(LxFsmCtx *c, int p) { return lx_fsm_object(c, p); }
static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
}
static LxKey key_of(const char *name) {
    if (!name || !name[0]) return LX_KEY_UNKNOWN;
    for (int k = 1; k < LX_KEY_COUNT; k++) {
        const char *n = lx_key_name((LxKey)k);
        if (n && n[0] && !ci_cmp(n, name)) return (LxKey)k;
    }
    return LX_KEY_UNKNOWN;
}
/* is e the object itself or something under it (a click on a character's sword picks the character) */
static bool under(LxScene *s, LxEntity root, LxEntity e) {
    for (int i = 0; e && i < 64; i++, e = lx_entity_parent(s, e)) if (e == root) return true;
    return false;
}
static LxVec3 flat(LxVec3 v) { return lx_v3(v.x, 0, v.z); }

/* what's under a point of the game UI canvas: the game kit's lx_pick_at (colliders, then plain meshes) */
static bool pick_at(LxScene *s, LxVec2 canvas, float dist, LxRaycastHit *hit) { return lx_pick_at(s, canvas, dist, hit); }

/* ======================================================================
 *  Input: keys. On the frame the state is entered a press or release belongs to the state before
 *  (the press that opened a pause menu must not close it again), so Down / Up only look from the next frame.
 * ====================================================================== */
typedef struct { bool entered; } EdgeState;
static bool edge_ready(LxFsmCtx *c) {   /* false on the first call after entering */
    EdgeState *s = c->state;
    bool ready = s->entered;
    s->entered = true;
    return ready;
}
static void get_key(LxFsmCtx *c) { LxKey k = key_of(lx_fsm_string(c, 0)); lx_fsm_set_bool(c, 1, k && lx_key_down(k)); }
static const LxFsmActionDesc A_GET_KEY = {"Get Key", "Input", "Stores whether a key is held down.", "{1} = {0} held",
    {PT("Key", LX_FSM_STRING, "Space", KEY_TIP), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, get_key, get_key, NULL};
static void key_edge(LxFsmCtx *c, bool down) {
    bool ready = edge_ready(c);
    LxKey k = key_of(lx_fsm_string(c, 0));
    bool hit = ready && k && (down ? lx_key_pressed(k) : lx_key_released(k));
    lx_fsm_set_bool(c, 2, hit);
    if (hit) ev(c, 1);
}
static void get_key_down(LxFsmCtx *c) { key_edge(c, true); }
static void get_key_up(LxFsmCtx *c) { key_edge(c, false); }
static const LxFsmActionDesc A_GET_KEY_DOWN = {"Get Key Down", "Input", "Sends an event on the frame a key is pressed.", "{0} pressed: {1}",
    {PT("Key", LX_FSM_STRING, "Space", KEY_TIP), PEV("Send Event"), POPT("Store Result", LX_FSM_BOOL)}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, get_key_down, get_key_down, NULL};
static const LxFsmActionDesc A_GET_KEY_UP = {"Get Key Up", "Input", "Sends an event on the frame a key is let go.", "{0} released: {1}",
    {PT("Key", LX_FSM_STRING, "Space", KEY_TIP), PEV("Send Event"), POPT("Store Result", LX_FSM_BOOL)}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, get_key_up, get_key_up, NULL};

/* Input Manager buttons (keyboard, gamepad and touch buttons at once) */
#define BUTTON_TIP "an action from the Input Manager (Edit > Input Manager): Jump, Fire, Interact..."
static void get_button(LxFsmCtx *c) { lx_fsm_set_bool(c, 1, lx_action_down(lx_fsm_string(c, 0))); }
static const LxFsmActionDesc A_GET_BUTTON = {"Get Button", "Input", "Stores whether an Input Manager button is held (keyboard, gamepad or touch).", "{1} = {0} held",
    {PT("Button", LX_FSM_STRING, "Jump", BUTTON_TIP), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, get_button, get_button, NULL};
static void button_edge(LxFsmCtx *c, bool down) {
    bool ready = edge_ready(c);
    const char *a = lx_fsm_string(c, 0);
    bool hit = ready && (down ? lx_action_pressed(a) : lx_action_released(a));
    lx_fsm_set_bool(c, 2, hit);
    if (hit) ev(c, 1);
}
static void get_button_down(LxFsmCtx *c) { button_edge(c, true); }
static void get_button_up(LxFsmCtx *c) { button_edge(c, false); }
static const LxFsmActionDesc A_GET_BUTTON_DOWN = {"Get Button Down", "Input", "Sends an event on the frame an Input Manager button is pressed.", "{0} pressed: {1}",
    {PT("Button", LX_FSM_STRING, "Jump", BUTTON_TIP), PEV("Send Event"), POPT("Store Result", LX_FSM_BOOL)}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, get_button_down, get_button_down, NULL};
static const LxFsmActionDesc A_GET_BUTTON_UP = {"Get Button Up", "Input", "Sends an event on the frame an Input Manager button is let go.", "{0} released: {1}",
    {PT("Button", LX_FSM_STRING, "Jump", BUTTON_TIP), PEV("Send Event"), POPT("Store Result", LX_FSM_BOOL)}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, get_button_up, get_button_up, NULL};

/* mouse buttons (the first finger counts as the left button) */
static void get_mouse_button(LxFsmCtx *c) { lx_fsm_set_bool(c, 1, lx_mouse_down((LxMouseButton)lx_fsm_int(c, 0))); }
static const LxFsmActionDesc A_GET_MOUSE_BUTTON = {"Get Mouse Button", "Input", "Stores whether a mouse button is held (a finger counts as Left).", "{1} = {0} button held",
    {PENUM("Button", MOUSE_BUTTONS, "Left"), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_EVERY_FRAME, get_mouse_button, get_mouse_button, NULL};
static void mouse_edge(LxFsmCtx *c, bool down) {
    bool ready = edge_ready(c);
    LxMouseButton b = (LxMouseButton)lx_fsm_int(c, 0);
    bool hit = ready && (down ? lx_mouse_pressed(b) : lx_mouse_released(b));
    if (hit && lx_fsm_bool(c, 3) && lx_ui_pointer_over()) hit = false;   /* the click was on a game UI button */
    lx_fsm_set_bool(c, 2, hit);
    if (hit) ev(c, 1);
}
static void get_mouse_button_down(LxFsmCtx *c) { mouse_edge(c, true); }
static void get_mouse_button_up(LxFsmCtx *c) { mouse_edge(c, false); }
#define IGNORE_UI PT("Ignore UI Clicks", LX_FSM_BOOL, "true", "a click on a game UI button doesn't count")
static const LxFsmActionDesc A_GET_MOUSE_BUTTON_DOWN = {"Get Mouse Button Down", "Input", "Sends an event on the frame a mouse button is clicked (or the screen tapped).", "{0} click: {1}",
    {PENUM("Button", MOUSE_BUTTONS, "Left"), PEV("Send Event"), POPT("Store Result", LX_FSM_BOOL), IGNORE_UI}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, get_mouse_button_down, get_mouse_button_down, NULL};
static const LxFsmActionDesc A_GET_MOUSE_BUTTON_UP = {"Get Mouse Button Up", "Input", "Sends an event on the frame a mouse button is let go (or a finger lifted).", "{0} released: {1}",
    {PENUM("Button", MOUSE_BUTTONS, "Left"), PEV("Send Event"), POPT("Store Result", LX_FSM_BOOL), IGNORE_UI}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, get_mouse_button_up, get_mouse_button_up, NULL};

/* axes */
static void get_axis_value(LxFsmCtx *c) {
    float v = lx_axis(lx_fsm_string(c, 0));
    if (lx_fsm_bool(c, 2)) v = -v;
    lx_fsm_set_float(c, 3, v * lx_fsm_float(c, 1));
}
static const LxFsmActionDesc A_GET_AXIS_VALUE = {"Get Axis Value", "Input", "Reads one Input Manager axis (-1..1) into a number, like PlayMaker's Get Axis.", "{3} = {0} × {1}",
    {PT("Axis", LX_FSM_STRING, "MoveX", "an axis from the Input Manager: MoveX, MoveY, LookX, LookY..."), P("Multiplier", LX_FSM_FLOAT, "1"), P("Invert", LX_FSM_BOOL, "false"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, get_axis_value, get_axis_value, NULL};

static void get_axis_vector(LxFsmCtx *c) {
    LxVec2 a = lx_axis2(lx_fsm_string(c, 0), lx_fsm_string(c, 1));
    float k = lx_fsm_float(c, 2);
    LxVec3 right = lx_v3(-1, 0, 0), fwd = lx_v3(0, 0, 1), up = lx_v3(0, 1, 0);   /* +Z forward: right is -X */
    int plane = lx_fsm_int(c, 3), rel = lx_fsm_int(c, 4);
    if (rel != 0) {
        LxEntity e = rel == 1 ? (lx_fsm_is_none(c, 5) ? lx_scene_main_camera(c->scene) : obj(c, 5)) : obj(c, 5);
        if (e) {
            LxQuat q = lx_entity_world_rotation(c->scene, e);
            LxVec3 r = lx_quat_rotate(q, lx_v3(-1, 0, 0)), f = lx_quat_rotate(q, lx_v3(0, 0, 1)), u = lx_quat_rotate(q, lx_v3(0, 1, 0));
            if (plane == 0) {   /* on the ground: the camera's forward flattened (looking down still walks forward) */
                LxVec3 ff = flat(f);
                if (lx_len3sq(ff) < 1e-6f) ff = flat(u);
                LxVec3 rr = flat(r);
                if (lx_len3sq(ff) > 1e-8f) fwd = lx_norm3(ff);
                if (lx_len3sq(rr) > 1e-8f) right = lx_norm3(rr);
            } else { right = r; up = u; }
        }
    }
    LxVec3 v = plane == 0 ? lx_add3(lx_scale3(right, a.x), lx_scale3(fwd, a.y)) : lx_add3(lx_scale3(right, a.x), lx_scale3(up, a.y));
    v = lx_scale3(v, k);
    lx_fsm_set_vec3(c, 6, v);
    lx_fsm_set_float(c, 7, lx_len3(v));
}
static const LxFsmActionDesc A_GET_AXIS_VECTOR = {"Get Axis Vector", "Input",
    "Two Input Manager axes as a movement direction, on the ground (X/Z) or upright (X/Y), relative to the world, the camera or an object. "
    "Feed it to Character Move or Translate.",
    "{6} = ({0}, {1}) × {2} relative to {4}",
    {P("Horizontal Axis", LX_FSM_STRING, "MoveX"), P("Vertical Axis", LX_FSM_STRING, "MoveY"), P("Multiplier", LX_FSM_FLOAT, "1"),
     PENUM("Map To Plane", "Ground (XZ)|Upright (XY)", "Ground (XZ)"), PENUM("Relative To", "World|Camera|Object", "Camera"),
     {"Object", LX_FSM_OBJECT, "None", LX_FSM_P_OPTIONAL, NULL, "Camera: None = the main camera"}, POPT("Store Vector", LX_FSM_VEC3), POPT("Store Magnitude", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, get_axis_vector, get_axis_vector, NULL};

static void get_mouse_x(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_mouse_delta().x * lx_fsm_float(c, 1)); }
static void get_mouse_y(LxFsmCtx *c) { lx_fsm_set_float(c, 0, -lx_mouse_delta().y * lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_GET_MOUSE_X = {"Get Mouse X", "Input", "How far the mouse moved left/right this frame (pixels × multiplier; right is +).", "{0} = mouse X × {1}",
    {POUT("Store Result", LX_FSM_FLOAT), P("Multiplier", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_EVERY_FRAME, get_mouse_x, get_mouse_x, NULL};
static const LxFsmActionDesc A_GET_MOUSE_Y = {"Get Mouse Y", "Input", "How far the mouse moved up/down this frame (pixels × multiplier; up is +).", "{0} = mouse Y × {1}",
    {POUT("Store Result", LX_FSM_FLOAT), P("Multiplier", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_EVERY_FRAME, get_mouse_y, get_mouse_y, NULL};

/* ======================================================================
 *  Input: picking objects with the mouse or a finger
 * ====================================================================== */
static void mouse_pick(LxFsmCtx *c) {
    LxRaycastHit h;
    bool got = !lx_ui_pointer_over() && pick_at(c->scene, lx_ui_pointer(), lx_fsm_float(c, 0), &h);
    lx_fsm_set_bool(c, 1, got);
    lx_fsm_set_object(c, 2, got ? h.entity : 0);
    if (got) { lx_fsm_set_vec3(c, 3, h.point); lx_fsm_set_vec3(c, 4, h.normal); lx_fsm_set_float(c, 5, h.distance); }
}
static const LxFsmActionDesc A_MOUSE_PICK = {"Mouse Pick", "Input", "What's under the mouse pointer (or finger): the object, the point and how far, through the main camera.",
    "{2} = object under the pointer",
    {P("Distance", LX_FSM_FLOAT, "100"), POPT("Store Did Pick", LX_FSM_BOOL), POPT("Store Object", LX_FSM_OBJECT), POPT("Store Point", LX_FSM_VEC3),
     POPT("Store Normal", LX_FSM_VEC3), POPT("Store Distance", LX_FSM_FLOAT)}, 0, 0, LX_FSM_EVERY_FRAME, mouse_pick, mouse_pick, NULL};

typedef struct { bool entered, over, pressed_on; } PickEvState;
static void mouse_pick_event(LxFsmCtx *c) {
    PickEvState *s = c->state;
    bool first = !s->entered;
    s->entered = true;
    LxEntity target = obj(c, 0);
    LxRaycastHit h;
    bool over = target && !lx_ui_pointer_over() && pick_at(c->scene, lx_ui_pointer(), lx_fsm_float(c, 1), &h) && under(c->scene, target, h.entity);
    bool was = s->over;
    s->over = over;
    lx_fsm_set_bool(c, 8, over);
    if (first) return;   /* what the pointer did on the entering frame belonged to the state before */
    bool down = lx_mouse_pressed(LX_MOUSE_LEFT), up = lx_mouse_released(LX_MOUSE_LEFT);
    if (over && down) s->pressed_on = true;
    if (over && !was) ev(c, 3);
    if (!over && was) ev(c, 4);
    if (over && down) ev(c, 5);
    if (up) {
        if (over) ev(c, 6);
        if (s->pressed_on && over) ev(c, 7);
        s->pressed_on = false;
    }
    if (over) ev(c, 2);
}
static const LxFsmActionDesc A_MOUSE_PICK_EVENT = {"Mouse Pick Event", "Input",
    "Events when the mouse pointer (or a finger) is over, enters, leaves, clicks or releases on an object or its children. Click = pressed and released on it.",
    "{0}: over {2}, down {5}, click {7}",
    {POBJ("Object"), P("Distance", LX_FSM_FLOAT, "100"), PEV("Mouse Over"), PEV("Mouse Enter"), PEV("Mouse Exit"), PEV("Mouse Down"), PEV("Mouse Up"),
     PEV("Mouse Click"), POPT("Store Is Over", LX_FSM_BOOL)}, sizeof(PickEvState), 0, LX_FSM_EVERY_FRAME, mouse_pick_event, mouse_pick_event, NULL};

/* ======================================================================
 *  Input: touch
 * ====================================================================== */
static LxVec2 canvas_of(LxVec2 px) { return lx_ui_screen_to_canvas(px); }
static void touch_count(LxFsmCtx *c) { lx_fsm_set_int(c, 0, lx_touch_count()); }
static const LxFsmActionDesc A_TOUCH_COUNT = {"Get Touch Count", "Input", "How many fingers are on the screen.", "{0} = fingers down",
    {POUT("Store Count", LX_FSM_INT)}, 0, 0, LX_FSM_EVERY_FRAME, touch_count, touch_count, NULL};

static const char *phase_name(LxTouchPhase p) {
    static const char *n[] = {"None", "Began", "Moved", "Stationary", "Ended"};
    return (int)p >= 0 && (int)p < 5 ? n[p] : "None";
}
static void touch_info(LxFsmCtx *c) {
    int f = lx_fsm_int(c, 0);
    LxTouchPhase ph = lx_touch_phase(f);
    LxVec2 p = canvas_of(ph == LX_TOUCH_ENDED ? lx_touch_last_pos(f) : lx_touch_pos(f));
    LxVec2 d = lx_touch_delta(f);
    float k = lx_ui_screen_to_canvas(lx_v2(1, 0)).x - lx_ui_screen_to_canvas(lx_v2(0, 0)).x;
    lx_fsm_set_bool(c, 1, ph != LX_TOUCH_NONE && ph != LX_TOUCH_ENDED);
    lx_fsm_set_vec3(c, 2, lx_v3(p.x, p.y, 0));
    lx_fsm_set_vec3(c, 3, lx_v3(d.x * k, d.y * k, 0));
    lx_fsm_set_string(c, 4, phase_name(ph));
}
static const LxFsmActionDesc A_TOUCH_INFO = {"Get Touch Info", "Input", "Where a finger is on the game UI canvas, how far it moved this frame and its phase (Began, Moved, Stationary, Ended).",
    "finger {0}: {2}",
    {PT("Finger", LX_FSM_INT, "0", "0 = the first finger down"), POPT("Store Is Touching", LX_FSM_BOOL), POPT("Store Position", LX_FSM_VEC3), POPT("Store Movement", LX_FSM_VEC3),
     POPT("Store Phase", LX_FSM_STRING)}, 0, 0, LX_FSM_EVERY_FRAME, touch_info, touch_info, NULL};

static void touch_event(LxFsmCtx *c) {
    if (!edge_ready(c)) return;
    int want = lx_fsm_int(c, 0), phase = lx_fsm_int(c, 1) + 1;   /* Began.. */
    for (int f = 0; f < 10; f++) {
        if (want >= 0 && f != want) continue;
        LxTouchPhase ph = lx_touch_phase(f);
        if ((int)ph != phase) continue;
        LxVec2 p = canvas_of(ph == LX_TOUCH_ENDED ? lx_touch_last_pos(f) : lx_touch_pos(f));
        lx_fsm_set_int(c, 3, f);
        lx_fsm_set_vec3(c, 4, lx_v3(p.x, p.y, 0));
        ev(c, 2);
        return;
    }
}
static const LxFsmActionDesc A_TOUCH_EVENT = {"Touch Event", "Input", "Sends an event when a finger touches, moves, holds still or lifts.", "finger {0} {1}: {2}",
    {PT("Finger", LX_FSM_INT, "-1", "-1 = any finger"), PENUM("Phase", "Began|Moved|Stationary|Ended", "Began"), PEV("Send Event"), POPT("Store Finger", LX_FSM_INT),
     POPT("Store Position", LX_FSM_VEC3)}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, touch_event, touch_event, NULL};

static void touch_object_event(LxFsmCtx *c) {
    if (!edge_ready(c)) return;
    LxEntity target = obj(c, 0);
    if (!target) return;
    for (int f = 0; f < 10; f++) {
        LxTouchPhase ph = lx_touch_phase(f);
        if (ph == LX_TOUCH_NONE) continue;
        LxRaycastHit h;
        if (!pick_at(c->scene, canvas_of(ph == LX_TOUCH_ENDED ? lx_touch_last_pos(f) : lx_touch_pos(f)), lx_fsm_float(c, 1), &h) || !under(c->scene, target, h.entity)) continue;
        lx_fsm_set_int(c, 6, f);
        lx_fsm_set_vec3(c, 7, h.point);
        ev(c, 1 + (int)ph);   /* Began = 2 .. Ended = 5 */
    }
}
static const LxFsmActionDesc A_TOUCH_OBJECT_EVENT = {"Touch Object Event", "Input", "Events when a finger touches, drags over, holds or lifts off an object (or its children).",
    "touching {0}: {2}",
    {POBJ("Object"), P("Distance", LX_FSM_FLOAT, "100"), PEV("Touch Began"), PEV("Touch Moved"), PEV("Touch Stationary"), PEV("Touch Ended"), POPT("Store Finger", LX_FSM_INT),
     POPT("Store Hit Point", LX_FSM_VEC3)}, sizeof(EdgeState), 0, LX_FSM_EVERY_FRAME, touch_object_event, touch_object_event, NULL};

typedef struct { bool entered, down, blocked; LxVec2 start, last; float t; } SwipeState;
static void swipe(LxFsmCtx *c) {
    SwipeState *s = c->state;
    bool touching = lx_touch_count() > 0 || (lx_fsm_bool(c, 7) && lx_mouse_down(LX_MOUSE_LEFT));
    LxVec2 p = lx_ui_pointer();
    if (!s->entered) { s->entered = true; s->blocked = touching; return; }   /* a finger already down started before this state */
    if (s->blocked) { s->blocked = touching; return; }
    if (touching && !s->down) { s->down = true; s->start = s->last = p; s->t = 0; return; }
    if (touching) { s->last = p; s->t += lx_delta_unscaled(c->scene); return; }
    if (!s->down) return;
    s->down = false;
    float dx = s->last.x - s->start.x, dy = s->last.y - s->start.y, len = sqrtf(dx * dx + dy * dy);
    float max_t = lx_fsm_float(c, 1);
    if (len < lx_fsm_float(c, 0) || (max_t > 0 && s->t > max_t)) return;
    lx_fsm_set_vec3(c, 6, lx_v3(dx / len, -dy / len, 0));   /* up is + */
    if (fabsf(dx) >= fabsf(dy)) ev(c, dx < 0 ? 2 : 3);
    else ev(c, dy < 0 ? 4 : 5);
}
static const LxFsmActionDesc A_SWIPE = {"Swipe Gesture Event", "Input", "Sends an event when the player swipes left, right, up or down (finger, or a mouse drag).",
    "Swipe: ← {2} → {3} ↑ {4} ↓ {5}",
    {PT("Min Distance", LX_FSM_FLOAT, "120", "in game UI units (the screen is 1080 tall)"), PT("Max Time", LX_FSM_FLOAT, "0.6", "seconds; 0 = any"), PEV("Swipe Left"),
     PEV("Swipe Right"), PEV("Swipe Up"), PEV("Swipe Down"), POPT("Store Direction", LX_FSM_VEC3), PT("Mouse Too", LX_FSM_BOOL, "true", "a left-button drag counts too (testing on a PC)")},
    sizeof(SwipeState), 0, LX_FSM_EVERY_FRAME, swipe, swipe, NULL};

/* ======================================================================
 *  Input: gamepads, device, typing
 * ====================================================================== */
typedef struct { bool entered, was; } PadState;
static void pad_button(LxFsmCtx *c) {
    PadState *s = c->state;
    int pad = lx_fsm_int(c, 0);
    bool down = lx_gamepad_button(pad, (LxGamepadButton)lx_fsm_int(c, 1));
    int when = lx_fsm_int(c, 2);
    bool hit = s->entered && (when == 0 ? down && !s->was : when == 1 ? !down && s->was : down);
    if (!s->entered && when == 2) hit = down;
    s->entered = true;
    s->was = down;
    lx_fsm_set_bool(c, 4, down);
    if (hit) ev(c, 3);
}
static const LxFsmActionDesc A_PAD_BUTTON = {"Get Gamepad Button", "Input", "A gamepad button directly (for menus that always use A/B, whatever the Input Manager says).",
    "pad {0} {1} {2}: {3}",
    {PT("Gamepad", LX_FSM_INT, "0", "0..3"), PENUM("Button", PAD_BUTTONS, "A"), PENUM("When", "Pressed|Released|Held", "Pressed"), PEV("Send Event"), POPT("Store Is Down", LX_FSM_BOOL)},
    sizeof(PadState), 0, LX_FSM_EVERY_FRAME, pad_button, pad_button, NULL};
static void pad_axis(LxFsmCtx *c) { lx_fsm_set_float(c, 2, lx_gamepad_axis(lx_fsm_int(c, 0), (LxGamepadAxis)lx_fsm_int(c, 1)) * lx_fsm_float(c, 3)); }
static const LxFsmActionDesc A_PAD_AXIS = {"Get Gamepad Axis", "Input", "A gamepad stick (-1..1, up is +) or trigger (0..1) directly.", "{2} = pad {0} {1}",
    {PT("Gamepad", LX_FSM_INT, "0", "0..3"), PENUM("Axis", PAD_AXES, "Left Stick X"), POUT("Store Result", LX_FSM_FLOAT), P("Multiplier", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_EVERY_FRAME, pad_axis, pad_axis, NULL};
static void pad_info(LxFsmCtx *c) {
    int pad = lx_fsm_int(c, 0);
    bool on = lx_gamepad_connected(pad);
    lx_fsm_set_int(c, 1, lx_gamepad_count());
    lx_fsm_set_bool(c, 2, on);
    const char *n = on ? lx_gamepad_name(pad) : "";
    lx_fsm_set_string(c, 3, n ? n : "");
    ev(c, on ? 4 : 5);
}
static const LxFsmActionDesc A_PAD_INFO = {"Get Gamepad Info", "Input", "How many gamepads are plugged in, and whether one is connected (and its name).", "pad {0} connected → {2}",
    {PT("Gamepad", LX_FSM_INT, "0", "0..3"), POPT("Store Count", LX_FSM_INT), POPT("Store Connected", LX_FSM_BOOL), POPT("Store Name", LX_FSM_STRING), PEV("Connected Event"),
     PEV("Not Connected Event")}, 0, 0, LX_FSM_ONCE, pad_info, pad_info, NULL};

static void input_device(LxFsmCtx *c) {
    static const char *names[] = {"Keyboard & Mouse", "Gamepad", "Touch"};
    int d = (int)lx_input_last_device();
    if (d < 0 || d > 2) d = 0;
    lx_fsm_set_int(c, 0, d);
    lx_fsm_set_string(c, 1, names[d]);
    ev(c, 2 + d);
}
static const LxFsmActionDesc A_INPUT_DEVICE = {"Get Input Device", "Input", "What the player used last (for showing keyboard, gamepad or touch button prompts).",
    "{1} = last input device",
    {POPT("Store Index", LX_FSM_INT), POPT("Store Name", LX_FSM_STRING), PEV("Keyboard & Mouse"), PEV("Gamepad"), PEV("Touch")}, 0, 0, LX_FSM_EVERY_FRAME, input_device, input_device, NULL};

static void typed_text(LxFsmCtx *c) {
    int n = 0;
    const uint32_t *cp = lx_text_input(&n);
    char buf[LX_REF_MAX];
    lx_strcpy(buf, sizeof buf, lx_fsm_string(c, 0));
    size_t len = strlen(buf);
    int max = lx_fsm_int(c, 1);
    if (max <= 0 || max > (int)sizeof buf - 5) max = (int)sizeof buf - 5;
    bool changed = false;
    if (lx_key_pressed(LX_KEY_BACKSPACE) && len) {
        do len--; while (len && ((unsigned char)buf[len] & 0xC0) == 0x80);   /* a whole UTF-8 character */
        buf[len] = 0;
        changed = true;
    }
    for (int i = 0; i < n; i++) {
        uint32_t u = cp[i];
        if (u < 32 || u == 127) continue;
        int chars = 0;
        for (size_t k = 0; k < len; k++) chars += ((unsigned char)buf[k] & 0xC0) != 0x80;
        if (chars >= max) break;
        char e[4];
        int m = u < 0x80 ? (e[0] = (char)u, 1) : u < 0x800 ? (e[0] = (char)(0xC0 | u >> 6), e[1] = (char)(0x80 | (u & 63)), 2)
              : u < 0x10000 ? (e[0] = (char)(0xE0 | u >> 12), e[1] = (char)(0x80 | ((u >> 6) & 63)), e[2] = (char)(0x80 | (u & 63)), 3)
                            : (e[0] = (char)(0xF0 | u >> 18), e[1] = (char)(0x80 | ((u >> 12) & 63)), e[2] = (char)(0x80 | ((u >> 6) & 63)), e[3] = (char)(0x80 | (u & 63)), 4);
        if (len + (size_t)m >= sizeof buf) break;
        memcpy(buf + len, e, (size_t)m);
        len += (size_t)m;
        buf[len] = 0;
        changed = true;
    }
    if (changed) lx_fsm_set_string(c, 0, buf);
    if (lx_key_pressed(LX_KEY_ENTER)) ev(c, 2);
}
static const LxFsmActionDesc A_TYPED_TEXT = {"Get Typed Text", "Input", "Adds what the player types to a text variable (Backspace deletes) - name entry, cheat codes. Enter sends an event.",
    "{0} += typed text",
    {POUT("Text Variable", LX_FSM_STRING), PT("Max Length", LX_FSM_INT, "16", "characters; 0 = no limit"), PEV("Enter Event")}, 0, 0, LX_FSM_EVERY_FRAME, NULL, typed_text, NULL};

/* ======================================================================
 *  Logic
 * ====================================================================== */
static void bool_op(LxFsmCtx *c) {
    bool a = lx_fsm_bool(c, 0), b = lx_fsm_bool(c, 1), r;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = a && b; break;
    case 1: r = a || b; break;
    case 2: r = a != b; break;
    case 3: r = !(a && b); break;
    default: r = !(a || b); break;
    }
    lx_fsm_set_bool(c, 3, r);
}
static const LxFsmActionDesc A_BOOL_OP = {"Bool Operator", "Logic", "AND, OR, XOR, NAND or NOR of two true/false values.", "{3} = {0} {2} {1}",
    {P("Bool 1", LX_FSM_BOOL, "false"), P("Bool 2", LX_FSM_BOOL, "false"), PENUM("Operation", "AND|OR|XOR|NAND|NOR", "AND"), POUT("Store Result", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, bool_op, bool_op, NULL};

static void float_switch(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    for (int i = 0; i < 4; i++) {
        if (lx_fsm_is_none(c, 1 + i * 2)) continue;
        if (v < lx_fsm_float(c, 1 + i * 2)) { ev(c, 2 + i * 2); return; }
    }
    ev(c, 9);
}
#define LT(n) {"Less Than " n, LX_FSM_FLOAT, "", LX_FSM_P_OPTIONAL, NULL, "checked in order: the first one the value is under wins"}
static const LxFsmActionDesc A_FLOAT_SWITCH = {"Float Switch", "Logic", "Sends the event of the first range the number is under (health below 25 → Danger, below 50 → Hurt...).",
    "Switch on {0}",
    {P("Float", LX_FSM_FLOAT, "0"), LT("1"), PEV("Event 1"), LT("2"), PEV("Event 2"), LT("3"), PEV("Event 3"), LT("4"), PEV("Event 4"), PEV("Otherwise")},
    0, 0, LX_FSM_ONCE, float_switch, float_switch, NULL};

static void float_sign(LxFsmCtx *c) { float v = lx_fsm_float(c, 0); ev(c, v > 0 ? 1 : v < 0 ? 2 : 3); }
static const LxFsmActionDesc A_FLOAT_SIGN = {"Float Sign Test", "Logic", "Different events for positive, negative and zero.", "{0}: + {1}, − {2}, 0 {3}",
    {P("Float", LX_FSM_FLOAT, "0"), PEV("Positive"), PEV("Negative"), PEV("Zero")}, 0, 0, LX_FSM_ONCE, float_sign, float_sign, NULL};

/* pick one of up to six filled-in values */
static int pick_filled(LxFsmCtx *c, int first, int count) {
    int ok[8], n = 0;
    for (int i = 0; i < count; i++) if (!lx_fsm_is_none(c, first + i)) ok[n++] = first + i;
    return n ? ok[lx_random_int(0, n - 1)] : -1;
}
#define OPT(n, t) {n, t, "", LX_FSM_P_OPTIONAL, NULL, NULL}
static void sel_float(LxFsmCtx *c) { int p = pick_filled(c, 1, 6); if (p >= 0) lx_fsm_set_float(c, 0, lx_fsm_float(c, p)); }
static const LxFsmActionDesc A_SEL_FLOAT = {"Select Random Float", "Math", "Picks one of up to six numbers at random.", "{0} = one of {1}, {2}, {3}...",
    {POUT("Store Result", LX_FSM_FLOAT), OPT("Float 1", LX_FSM_FLOAT), OPT("Float 2", LX_FSM_FLOAT), OPT("Float 3", LX_FSM_FLOAT), OPT("Float 4", LX_FSM_FLOAT), OPT("Float 5", LX_FSM_FLOAT),
     OPT("Float 6", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, sel_float, sel_float, NULL};
static void sel_string(LxFsmCtx *c) { int p = pick_filled(c, 1, 6); if (p >= 0) { char t[LX_REF_MAX]; lx_strcpy(t, sizeof t, lx_fsm_string(c, p)); lx_fsm_set_string(c, 0, t); } }
static const LxFsmActionDesc A_SEL_STRING = {"Select Random String", "Variables", "Picks one of up to six texts at random (barks, hints, names).", "{0} = one of \"{1}\", \"{2}\"...",
    {POUT("Store Result", LX_FSM_STRING), OPT("String 1", LX_FSM_STRING), OPT("String 2", LX_FSM_STRING), OPT("String 3", LX_FSM_STRING), OPT("String 4", LX_FSM_STRING),
     OPT("String 5", LX_FSM_STRING), OPT("String 6", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, sel_string, sel_string, NULL};
static void sel_object(LxFsmCtx *c) {
    int ok[6], n = 0;
    for (int i = 1; i <= 6; i++) if (obj(c, i)) ok[n++] = i;
    lx_fsm_set_object(c, 0, n ? obj(c, ok[lx_random_int(0, n - 1)]) : 0);
}
static const LxFsmActionDesc A_SEL_OBJECT = {"Select Random Object", "Objects", "Picks one of up to six objects at random (spawn points, targets).", "{0} = one of {1}, {2}...",
    {POUT("Store Result", LX_FSM_OBJECT), POBJN("Object 1"), POBJN("Object 2"), POBJN("Object 3"), POBJN("Object 4"), POBJN("Object 5"), POBJN("Object 6")},
    0, 0, LX_FSM_ONCE, sel_object, sel_object, NULL};

/* ======================================================================
 *  Math: vectors, trigonometry, ranges
 * ====================================================================== */
static void vec_clamp_mag(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    float m = lx_maxf(lx_fsm_float(c, 1), 0), l = lx_len3(v);
    lx_fsm_set_vec3(c, 0, l > m && l > 0 ? lx_scale3(v, m / l) : v);
}
static const LxFsmActionDesc A_VEC_CLAMP = {"Vector3 Clamp Magnitude", "Math/Vector3", "Shortens a vector to a maximum length (top speed).", "{0} at most {1} long",
    {POUT("Vector Variable", LX_FSM_VEC3), P("Max Length", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, vec_clamp_mag, vec_clamp_mag, NULL};
static void vec_length(LxFsmCtx *c) { lx_fsm_set_float(c, 1, lx_len3(lx_fsm_vec3(c, 0))); }
static const LxFsmActionDesc A_VEC_LENGTH = {"Get Vector Length", "Math/Vector3", "How long a vector is (a velocity's speed).", "{1} = length of {0}",
    {P("Vector", LX_FSM_VEC3, "0 0 0"), POUT("Store Length", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, vec_length, vec_length, NULL};
static void vec_invert(LxFsmCtx *c) { lx_fsm_set_vec3(c, 0, lx_scale3(lx_fsm_vec3(c, 0), -1)); }
static const LxFsmActionDesc A_VEC_INVERT = {"Vector3 Invert", "Math/Vector3", "Flips a vector to point the other way.", "{0} = -{0}",
    {POUT("Vector Variable", LX_FSM_VEC3)}, 0, 0, LX_FSM_ONCE, vec_invert, vec_invert, NULL};
static void vec_per_second(LxFsmCtx *c) { lx_fsm_set_vec3(c, 0, lx_scale3(lx_fsm_vec3(c, 0), c->dt)); }
static const LxFsmActionDesc A_VEC_PER_SECOND = {"Vector3 Per Second", "Math/Vector3", "Multiplies a vector by this frame's time (a speed per second → a move this frame).",
    "{0} ×= frame time", {POUT("Vector Variable", LX_FSM_VEC3)}, 0, 0, LX_FSM_EVERY_FRAME, vec_per_second, vec_per_second, NULL};
static void vec_rotate_towards(LxFsmCtx *c) {
    LxVec3 a = lx_fsm_vec3(c, 0), b = lx_fsm_vec3(c, 1);
    float la = lx_len3(a), lb = lx_len3(b);
    if (la < 1e-6f || lb < 1e-6f) { lx_fsm_set_vec3(c, 0, b); return; }
    LxVec3 na = lx_scale3(a, 1 / la), nb = lx_scale3(b, 1 / lb);
    float ang = acosf(lx_clampf(lx_dot3(na, nb), -1, 1)), step = lx_fsm_float(c, 2) * LX_DEG2RAD * c->dt;
    float len = la + lx_clampf(lb - la, -lx_fsm_float(c, 3) * c->dt, lx_fsm_float(c, 3) * c->dt);
    if (ang <= step || ang < 1e-5f) { lx_fsm_set_vec3(c, 0, lx_scale3(nb, len)); return; }
    LxVec3 axis = lx_cross3(na, nb);
    if (lx_len3sq(axis) < 1e-10f) axis = fabsf(na.y) < 0.9f ? lx_cross3(na, lx_v3(0, 1, 0)) : lx_cross3(na, lx_v3(1, 0, 0));
    LxVec3 r = lx_quat_rotate(lx_quat_axis_angle(lx_norm3(axis), step), na);
    lx_fsm_set_vec3(c, 0, lx_scale3(r, len));
}
static const LxFsmActionDesc A_VEC_ROTATE_TOWARDS = {"Vector3 Rotate Towards", "Math/Vector3", "Turns a direction towards another at a set speed (homing missiles, slow turns).",
    "turn {0} towards {1} at {2}°/s",
    {POUT("Vector Variable", LX_FSM_VEC3), P("Target Direction", LX_FSM_VEC3, "0 0 1"), P("Degrees Per Second", LX_FSM_FLOAT, "90"),
     PT("Length Change Per Second", LX_FSM_FLOAT, "1", "how fast its length moves to the target's length")}, 0, 0, LX_FSM_EVERY_FRAME, vec_rotate_towards, vec_rotate_towards, NULL};

typedef struct { float t; bool have; LxVec3 from; LxVec4 cfrom; } InterpState;
static float ease(int e, float k) {
    switch (e) {
    case 1: return k * k;
    case 2: return 1 - (1 - k) * (1 - k);
    case 3: return k < 0.5f ? 2 * k * k : 1 - 2 * (1 - k) * (1 - k);
    default: return k;
    }
}
static void vec_interp(LxFsmCtx *c) {
    InterpState *s = c->state;
    if (!s->have) { s->from = lx_fsm_is_none(c, 1) ? lx_fsm_vec3(c, 0) : lx_fsm_vec3(c, 1); s->have = true; }
    s->t += lx_fsm_bool(c, 6) ? lx_delta_unscaled(c->scene) : c->dt;
    float len = lx_fsm_float(c, 3), k = len > 0 ? lx_clampf(s->t / len, 0, 1) : 1;
    lx_fsm_set_vec3(c, 0, lx_lerp3(s->from, lx_fsm_vec3(c, 2), ease(lx_fsm_int(c, 4), k)));
    if (k >= 1) { lx_fsm_finish(c); ev(c, 5); }
}
#define EASE4 "Linear|Ease In|Ease Out|Ease In Out"
static const LxFsmActionDesc A_VEC_INTERP = {"Vector3 Interpolate", "Math/Vector3", "Moves a vector variable from one value to another over time.", "{0}: → {2} in {3} s",
    {POUT("Vector Variable", LX_FSM_VEC3), {"From", LX_FSM_VEC3, "", LX_FSM_P_OPTIONAL, NULL, "None = where the variable is now"}, P("To", LX_FSM_VEC3, "0 0 0"), P("Time", LX_FSM_FLOAT, "1"),
     PENUM("Easing", EASE4, "Linear"), PEV("Finish Event"), PT("Real Time", LX_FSM_BOOL, "false", "ignore the time scale")},
    sizeof(InterpState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, vec_interp, NULL};

typedef struct { LxVec3 v; bool have; } FilterState;
static void vec_low_pass(LxFsmCtx *c) {
    FilterState *s = c->state;
    LxVec3 in = lx_fsm_vec3(c, 0);
    if (!s->have) { s->v = in; s->have = true; }
    float k = lx_clampf(lx_fsm_float(c, 1), 0, 1);
    s->v = lx_lerp3(s->v, in, k);
    if (lx_fsm_int(c, 2) == 0) lx_fsm_set_vec3(c, 0, s->v);
    else lx_fsm_set_vec3(c, 0, lx_sub3(in, s->v));
}
static const LxFsmActionDesc A_VEC_FILTER = {"Vector3 Filter", "Math/Vector3", "Low pass smooths jittery input (tilt, aim); high pass keeps only quick changes (shakes).",
    "{2} filter {0}",
    {POUT("Vector Variable", LX_FSM_VEC3), PT("Amount", LX_FSM_FLOAT, "0.1", "0..1: lower = smoother"), PENUM("Filter", "Low Pass|High Pass", "Low Pass")},
    sizeof(FilterState), 0, LX_FSM_EVERY_FRAME, vec_low_pass, vec_low_pass, NULL};

static void trig(LxFsmCtx *c) {
    float x = lx_fsm_float(c, 0), r = 0;
    bool deg = lx_fsm_bool(c, 2);
    float a = deg ? x * LX_DEG2RAD : x;
    switch (lx_fsm_int(c, 1)) {
    case 0: r = sinf(a); break;
    case 1: r = cosf(a); break;
    case 2: r = tanf(a); break;
    case 3: r = asinf(lx_clampf(x, -1, 1)); break;
    case 4: r = acosf(lx_clampf(x, -1, 1)); break;
    default: r = atanf(x); break;
    }
    if (lx_fsm_int(c, 1) >= 3 && deg) r *= LX_RAD2DEG;
    lx_fsm_set_float(c, 3, r);
}
static const LxFsmActionDesc A_TRIG = {"Trigonometry", "Math", "Sin, Cos, Tan and their inverses (Asin, Acos, Atan), in degrees or radians.", "{3} = {1}({0})",
    {P("Value", LX_FSM_FLOAT, "0"), PENUM("Function", "Sin|Cos|Tan|Asin|Acos|Atan", "Sin"), PT("Degrees", LX_FSM_BOOL, "true", "angles in degrees (off = radians)"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, trig, trig, NULL};
static void atan2_fn(LxFsmCtx *c) {
    float r = atan2f(lx_fsm_float(c, 0), lx_fsm_float(c, 1));
    lx_fsm_set_float(c, 3, lx_fsm_bool(c, 2) ? r * LX_RAD2DEG : r);
}
static const LxFsmActionDesc A_ATAN2 = {"Get Atan2", "Math", "The angle of a direction (Y, X) - which way a stick or a vector points.", "{3} = atan2({0}, {1})",
    {P("Y", LX_FSM_FLOAT, "0"), P("X", LX_FSM_FLOAT, "1"), P("Degrees", LX_FSM_BOOL, "true"), POUT("Store Angle", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, atan2_fn, atan2_fn, NULL};
static void float_remap(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0), a = lx_fsm_float(c, 1), b = lx_fsm_float(c, 2), x = lx_fsm_float(c, 3), y = lx_fsm_float(c, 4);
    float k = b != a ? (v - a) / (b - a) : 0;
    if (lx_fsm_bool(c, 5)) k = lx_clampf(k, 0, 1);
    lx_fsm_set_float(c, 6, x + (y - x) * k);
}
static const LxFsmActionDesc A_FLOAT_REMAP = {"Float Remap", "Math", "Maps a number from one range to another (speed 0..10 → pitch 0.8..1.4).", "{6} = {0} from {1}..{2} to {3}..{4}",
    {P("Float", LX_FSM_FLOAT, "0"), P("From Min", LX_FSM_FLOAT, "0"), P("From Max", LX_FSM_FLOAT, "1"), P("To Min", LX_FSM_FLOAT, "0"), P("To Max", LX_FSM_FLOAT, "1"),
     P("Clamp", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, float_remap, float_remap, NULL};

/* ======================================================================
 *  Text
 * ====================================================================== */
static size_t utf8_offset(const char *s, int chars) {   /* byte offset of the chars-th character */
    size_t i = 0;
    for (int n = 0; s[i] && n < chars; n++) do i++; while (s[i] && ((unsigned char)s[i] & 0xC0) == 0x80);
    return i;
}
static void substring(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    int start = lx_fsm_int(c, 1), count = lx_fsm_int(c, 2);
    if (start < 0) start = 0;
    char out[LX_REF_MAX];
    size_t a = utf8_offset(s, start), b = count < 0 ? strlen(s) : a + utf8_offset(s + a, count);
    size_t n = b - a < sizeof out - 1 ? b - a : sizeof out - 1;
    memcpy(out, s + a, n);
    out[n] = 0;
    lx_fsm_set_string(c, 3, out);
}
static const LxFsmActionDesc A_SUBSTRING = {"Get Substring", "Variables", "Part of a text: from a character, so many characters long.", "{3} = {0}[{1}, {2}]",
    {P("String", LX_FSM_STRING, ""), PT("Start", LX_FSM_INT, "0", "0 = the first character"), PT("Length", LX_FSM_INT, "-1", "-1 = to the end"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, substring, substring, NULL};
static void string_replace(LxFsmCtx *c) {
    char src[LX_REF_MAX], what[LX_REF_MAX], with[LX_REF_MAX], out[LX_REF_MAX];
    lx_strcpy(src, sizeof src, lx_fsm_string(c, 0));
    lx_strcpy(what, sizeof what, lx_fsm_string(c, 1));
    lx_strcpy(with, sizeof with, lx_fsm_string(c, 2));
    size_t o = 0, wl = strlen(what), rl = strlen(with);
    for (const char *p = src; *p && o < sizeof out - 1;) {
        if (wl && !strncmp(p, what, wl)) {
            for (size_t k = 0; k < rl && o < sizeof out - 1; k++) out[o++] = with[k];
            p += wl;
        } else out[o++] = *p++;
    }
    out[o] = 0;
    lx_fsm_set_string(c, 3, out);
}
static const LxFsmActionDesc A_STRING_REPLACE = {"String Replace", "Variables", "Replaces every copy of one piece of text with another.", "{3} = {0} with \"{1}\" → \"{2}\"",
    {P("String", LX_FSM_STRING, ""), P("Replace", LX_FSM_STRING, ""), P("With", LX_FSM_STRING, ""), POUT("Store Result", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, string_replace, string_replace, NULL};
static void string_case(LxFsmCtx *c) {
    char s[LX_REF_MAX];
    lx_strcpy(s, sizeof s, lx_fsm_string(c, 0));
    int m = lx_fsm_int(c, 1);
    bool start = true;
    for (char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch >= 0x80) { start = false; continue; }
        if (m == 0) *p = (char)toupper(ch);
        else if (m == 1) *p = (char)tolower(ch);
        else { *p = (char)(start ? toupper(ch) : tolower(ch)); }
        start = isspace(ch) != 0;
    }
    lx_fsm_set_string(c, 2, s);
}
static const LxFsmActionDesc A_STRING_CASE = {"String Case", "Variables", "UPPER CASE, lower case or Title Case.", "{2} = {1} of {0}",
    {P("String", LX_FSM_STRING, ""), PENUM("Case", "Upper|Lower|Title", "Upper"), POUT("Store Result", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, string_case, string_case, NULL};
static void string_part(LxFsmCtx *c) {
    char s[LX_REF_MAX], sep[32];
    lx_strcpy(s, sizeof s, lx_fsm_string(c, 0));
    lx_strcpy(sep, sizeof sep, lx_fsm_string(c, 1));
    int want = lx_fsm_int(c, 2), n = 0, count = 1;
    size_t sl = strlen(sep);
    char *p = s, *found = NULL;
    for (;;) {
        char *e = sl ? strstr(p, sep) : NULL;
        if (n == want) { if (e) *e = 0; found = p; }
        if (!e) break;
        p = e + sl;
        n++;
        count++;
    }
    lx_fsm_set_string(c, 3, found ? found : "");
    lx_fsm_set_int(c, 4, count);
}
static const LxFsmActionDesc A_STRING_PART = {"String Split", "Variables", "Splits a text at a separator and takes one part (\"a,b,c\" part 1 = \"b\").", "{3} = part {2} of {0} split by \"{1}\"",
    {P("String", LX_FSM_STRING, ""), P("Separator", LX_FSM_STRING, ","), PT("Part", LX_FSM_INT, "0", "0 = the first part"), POUT("Store Result", LX_FSM_STRING), POPT("Store Part Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, string_part, string_part, NULL};
static void string_to_number(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    char *end = NULL;
    double v = strtod(s, &end);
    bool ok = end && end != s;
    while (ok && *end && isspace((unsigned char)*end)) end++;
    ok = ok && !*end && isfinite(v);
    if (!ok) v = 0;
    lx_fsm_set_float(c, 1, (float)v);
    lx_fsm_set_int(c, 2, (int)lround(lx_clampf((float)v, -2147483520.0f, 2147483520.0f)));
    ev(c, ok ? 3 : 4);
}
static const LxFsmActionDesc A_STRING_TO_NUMBER = {"String To Number", "Variables", "Reads a number out of a text (\"42\", \"3.5\").", "{1} = number in {0}",
    {P("String", LX_FSM_STRING, "0"), POPT("Store Float", LX_FSM_FLOAT), POPT("Store Int", LX_FSM_INT), PEV("Success Event"), PEV("Not A Number Event")},
    0, 0, LX_FSM_ONCE, string_to_number, string_to_number, NULL};

/* ======================================================================
 *  Colour
 * ====================================================================== */
static void get_rgba(LxFsmCtx *c) {
    LxVec4 v = lx_fsm_color(c, 0);
    lx_fsm_set_float(c, 1, v.x); lx_fsm_set_float(c, 2, v.y); lx_fsm_set_float(c, 3, v.z); lx_fsm_set_float(c, 4, v.w);
}
static const LxFsmActionDesc A_GET_RGBA = {"Get Color RGBA", "Color", "Splits a colour into red, green, blue and alpha (0..1).", "split {0}",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), POPT("Store Red", LX_FSM_FLOAT), POPT("Store Green", LX_FSM_FLOAT), POPT("Store Blue", LX_FSM_FLOAT), POPT("Store Alpha", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, get_rgba, get_rgba, NULL};
static void set_rgba(LxFsmCtx *c) {
    LxVec4 v = lx_fsm_color(c, 0);
    if (!lx_fsm_is_none(c, 1)) v.x = lx_fsm_float(c, 1);
    if (!lx_fsm_is_none(c, 2)) v.y = lx_fsm_float(c, 2);
    if (!lx_fsm_is_none(c, 3)) v.z = lx_fsm_float(c, 3);
    if (!lx_fsm_is_none(c, 4)) v.w = lx_fsm_float(c, 4);
    lx_fsm_set_color(c, 0, v);
}
static const LxFsmActionDesc A_SET_RGBA = {"Set Color RGBA", "Color", "Changes some channels of a colour variable (None = keep).", "{0}: R {1} G {2} B {3} A {4}",
    {POUT("Color Variable", LX_FSM_COLOR), OPT("Red", LX_FSM_FLOAT), OPT("Green", LX_FSM_FLOAT), OPT("Blue", LX_FSM_FLOAT), OPT("Alpha", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, set_rgba, set_rgba, NULL};
static LxVec4 lerp4(LxVec4 a, LxVec4 b, float k) { return lx_v4(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k); }
static void color_interp(LxFsmCtx *c) {
    InterpState *s = c->state;
    if (!s->have) { s->cfrom = lx_fsm_is_none(c, 1) ? lx_fsm_color(c, 0) : lx_fsm_color(c, 1); s->have = true; }
    s->t += lx_fsm_bool(c, 6) ? lx_delta_unscaled(c->scene) : c->dt;
    float len = lx_fsm_float(c, 3), k = len > 0 ? lx_clampf(s->t / len, 0, 1) : 1;
    lx_fsm_set_color(c, 0, lerp4(s->cfrom, lx_fsm_color(c, 2), ease(lx_fsm_int(c, 4), k)));
    if (k >= 1) { lx_fsm_finish(c); ev(c, 5); }
}
static const LxFsmActionDesc A_COLOR_INTERP = {"Color Interpolate", "Color", "Fades a colour variable to another colour over time.", "{0} → {2} in {3} s",
    {POUT("Color Variable", LX_FSM_COLOR), {"From", LX_FSM_COLOR, "", LX_FSM_P_OPTIONAL, NULL, "None = the colour it is now"}, P("To", LX_FSM_COLOR, "1 1 1 1"), P("Time", LX_FSM_FLOAT, "1"),
     PENUM("Easing", EASE4, "Linear"), PEV("Finish Event"), PT("Real Time", LX_FSM_BOOL, "false", "ignore the time scale")},
    sizeof(InterpState), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, color_interp, NULL};
static void sel_color(LxFsmCtx *c) { int p = pick_filled(c, 1, 6); if (p >= 0) lx_fsm_set_color(c, 0, lx_fsm_color(c, p)); }
static const LxFsmActionDesc A_SEL_COLOR = {"Select Random Color", "Color", "Picks one of up to six colours at random.", "{0} = one of {1}, {2}...",
    {POUT("Store Result", LX_FSM_COLOR), OPT("Color 1", LX_FSM_COLOR), OPT("Color 2", LX_FSM_COLOR), OPT("Color 3", LX_FSM_COLOR), OPT("Color 4", LX_FSM_COLOR), OPT("Color 5", LX_FSM_COLOR),
     OPT("Color 6", LX_FSM_COLOR)}, 0, 0, LX_FSM_ONCE, sel_color, sel_color, NULL};
static void color_ramp(LxFsmCtx *c) {
    LxVec4 cols[4];
    int n = 0;
    for (int i = 0; i < 4; i++) if (!lx_fsm_is_none(c, 1 + i)) cols[n++] = lx_fsm_color(c, 1 + i);
    if (!n) return;
    float k = lx_clampf(lx_fsm_float(c, 0), 0, 1) * (float)(n - 1);
    int a = (int)floorf(k);
    if (a >= n - 1) { lx_fsm_set_color(c, 5, cols[n - 1]); return; }
    lx_fsm_set_color(c, 5, lerp4(cols[a], cols[a + 1], k - (float)a));
}
static const LxFsmActionDesc A_COLOR_RAMP = {"Color Ramp", "Color", "A colour along a gradient of up to four colours (health 0..1 → red..yellow..green).", "{5} = ramp at {0}",
    {PT("Value", LX_FSM_FLOAT, "0", "0..1 along the colours"), OPT("Color 1", LX_FSM_COLOR), OPT("Color 2", LX_FSM_COLOR), OPT("Color 3", LX_FSM_COLOR), OPT("Color 4", LX_FSM_COLOR),
     POUT("Store Color", LX_FSM_COLOR)}, 0, 0, LX_FSM_EVERY_FRAME, color_ramp, color_ramp, NULL};

/* ======================================================================
 *  Time
 * ====================================================================== */
static void system_time(LxFsmCtx *c) {
    time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    lx_fsm_set_int(c, 0, t.tm_hour);
    lx_fsm_set_int(c, 1, t.tm_min);
    lx_fsm_set_int(c, 2, t.tm_sec);
    lx_fsm_set_int(c, 3, t.tm_mday);
    lx_fsm_set_int(c, 4, t.tm_mon + 1);
    lx_fsm_set_int(c, 5, t.tm_year + 1900);
    lx_fsm_set_int(c, 6, t.tm_wday);
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    lx_fsm_set_string(c, 7, buf);
}
static const LxFsmActionDesc A_SYSTEM_TIME = {"Get System Date Time", "Time", "The real-world clock and date (day/night cycles that follow real time, daily rewards).",
    "{7} = now",
    {POPT("Store Hour", LX_FSM_INT), POPT("Store Minute", LX_FSM_INT), POPT("Store Second", LX_FSM_INT), POPT("Store Day", LX_FSM_INT), POPT("Store Month", LX_FSM_INT),
     POPT("Store Year", LX_FSM_INT), POPT("Store Weekday", LX_FSM_INT), POPT("Store Text", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, system_time, system_time, NULL};

/* ======================================================================
 *  Transform
 * ====================================================================== */
static void random_rotation(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 r = lx_entity_euler(c->scene, e);
    if (lx_fsm_bool(c, 1)) r.x = lx_random_range(0, 360);
    if (lx_fsm_bool(c, 2)) r.y = lx_random_range(0, 360);
    if (lx_fsm_bool(c, 3)) r.z = lx_random_range(0, 360);
    lx_entity_set_euler(c->scene, e, r);
}
static const LxFsmActionDesc A_RANDOM_ROTATION = {"Set Random Rotation", "Transform", "Turns an object to a random angle (scattered rocks, debris).", "random rotation for {0}",
    {POBJ("Object"), P("X", LX_FSM_BOOL, "false"), P("Y", LX_FSM_BOOL, "true"), P("Z", LX_FSM_BOOL, "false")}, 0, 0, LX_FSM_ONCE, random_rotation, random_rotation, NULL};

static void angle_to_target(LxFsmCtx *c) {
    LxEntity e = obj(c, 0), t = obj(c, 1);
    if (!e) return;
    LxVec3 p = lx_entity_world_position(c->scene, e);
    LxVec3 tp = t ? lx_add3(lx_entity_world_position(c->scene, t), lx_fsm_vec3(c, 2)) : lx_fsm_vec3(c, 2);
    LxVec3 f = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), lx_v3(0, 0, 1)), to = lx_sub3(tp, p);
    float ang;
    if (lx_fsm_bool(c, 3)) {
        f = flat(f); to = flat(to);
        if (lx_len3sq(f) < 1e-10f || lx_len3sq(to) < 1e-10f) ang = 0;
        else ang = -atan2f(lx_cross3(f, to).y, lx_dot3(f, to)) * LX_RAD2DEG;   /* + = to the right */
    } else {
        float lf = lx_len3(f), lt = lx_len3(to);
        ang = lf > 1e-6f && lt > 1e-6f ? acosf(lx_clampf(lx_dot3(f, to) / (lf * lt), -1, 1)) * LX_RAD2DEG : 0;
    }
    lx_fsm_set_float(c, 4, ang);
    lx_fsm_set_float(c, 5, fabsf(ang));
}
static const LxFsmActionDesc A_ANGLE_TO_TARGET = {"Get Angle To Target", "Transform",
    "How far an object would have to turn to face a target: + to the right, − to the left (ignoring height), or the plain angle in 3D.",
    "{4} = angle from {0} to {1}",
    {POBJ("Object"), POBJN("Target"), PT("Target Position", LX_FSM_VEC3, "0 0 0", "Target None: this world position; otherwise an offset from it"),
     PT("Ignore Height", LX_FSM_BOOL, "true", "turning on the spot: + right, − left"), POPT("Store Angle", LX_FSM_FLOAT), POPT("Store Absolute Angle", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, angle_to_target, angle_to_target, NULL};

static void transform_point(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxMat4 m = lx_entity_world_matrix(c->scene, e);
    LxVec3 v = lx_fsm_vec3(c, 1), r;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_mat4_point(m, v); break;
    case 1: r = lx_mat4_point(lx_mat4_inverse(m), v); break;
    case 2: r = lx_quat_rotate(lx_entity_world_rotation(c->scene, e), v); break;
    default: r = lx_quat_rotate(lx_quat_conj(lx_entity_world_rotation(c->scene, e)), v); break;
    }
    lx_fsm_set_vec3(c, 3, r);
}
static const LxFsmActionDesc A_TRANSFORM_POINT = {"Transform Point", "Transform", "Converts a point or direction between an object's own space and the world (\"2 m in front of the player\").",
    "{3} = {1} {2} of {0}",
    {POBJ("Object"), P("Vector", LX_FSM_VEC3, "0 0 2"),
     PENUM("Convert", "Local Point To World|World Point To Local|Local Direction To World|World Direction To Local", "Local Point To World"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_EVERY_FRAME, transform_point, transform_point, NULL};


/* PlayMaker's Smooth Look At Direction: turn to face a direction (a velocity, an input vector) rather than a point */
static void smooth_look_dir(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    LxVec3 d = lx_fsm_vec3(c, 1);
    if (lx_fsm_int(c, 5) == 0) {   /* Self: the direction is in the object's own space */
        LxEntity par = lx_entity_parent(c->scene, e);
        if (par) d = lx_quat_rotate(lx_entity_world_rotation(c->scene, par), d);
    }
    if (lx_fsm_bool(c, 4)) d.y = 0;
    float min = lx_maxf(lx_fsm_float(c, 2), 1e-4f);
    if (!(lx_len3sq(d) >= min * min)) return;   /* too short to mean a direction (a stick at rest): keep facing */
    LxVec3 up = lx_fsm_vec3(c, 3);
    if (!(lx_len3sq(up) > 1e-8f)) up = lx_v3(0, 1, 0);
    up = lx_norm3(up);
    LxVec3 f = lx_norm3(d);
    if (fabsf(lx_dot3(f, up)) > 0.999f) up = fabsf(f.x) < 0.9f ? lx_v3(1, 0, 0) : lx_v3(0, 0, 1);   /* straight along up */
    LxQuat want = lx_quat_look(f, up), cur = lx_entity_world_rotation(c->scene, e);
    if (cur.x * want.x + cur.y * want.y + cur.z * want.z + cur.w * want.w < 0) { want.x = -want.x; want.y = -want.y; want.z = -want.z; want.w = -want.w; }
    float speed = lx_fsm_float(c, 6);
    float k = !(speed > 0) ? 1 : c->dt > 0 ? lx_clampf(1 - expf(-speed * c->dt), 0, 1) : 0;   /* 0 = at once */
    LxQuat r = {cur.x + (want.x - cur.x) * k, cur.y + (want.y - cur.y) * k, cur.z + (want.z - cur.z) * k, cur.w + (want.w - cur.w) * k};
    if (r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w < 1e-12f) r = want;
    r = lx_quat_norm(r);
    lx_entity_set_world_pose(c->scene, e, lx_entity_world_position(c->scene, e), r);
    /* close enough: send Finish (once per visit) */
    float dot = fabsf(r.x * want.x + r.y * want.y + r.z * want.z + r.w * want.w);
    float angle = 2 * acosf(lx_clampf(dot, -1, 1)) * LX_RAD2DEG;
    lx_fsm_set_float(c, 9, angle);
    bool *sent = c->state;
    if (angle <= lx_fsm_float(c, 7) && !*sent) { *sent = true; ev(c, 8); }
    else if (angle > lx_fsm_float(c, 7) * 2 + 1) *sent = false;
}
static const LxFsmActionDesc A_SMOOTH_LOOK_DIR = {"Smooth Look At Direction", "Transform",
    "Turns an object smoothly to face a direction - where it's moving, or a stick's vector from Get Axis Vector. "
    "Directions shorter than Min Magnitude are ignored, so it keeps facing the last way when the stick is let go.",
    "Turn {0} to face {1} (speed {6})",
    {POBJ("Object"), P("Target Direction", LX_FSM_VEC3, "0 0 1"), PT("Min Magnitude", LX_FSM_FLOAT, "0.1", "shorter directions are ignored"),
     PT("Up Vector", LX_FSM_VEC3, "0 1 0", "which way is up for the object"), PT("Keep Vertical", LX_FSM_BOOL, "true", "only turn left and right (ignores the direction's height)"),
     PENUM("Space", "Self|World", "World"), PT("Speed", LX_FSM_FLOAT, "8", "how fast it turns; 0 = at once"), PT("Finish Tolerance", LX_FSM_FLOAT, "1", "degrees"),
     PEV("Finish Event"), POPT("Store Angle Left", LX_FSM_FLOAT)},
    sizeof(bool), 0, LX_FSM_EVERY_FRAME, smooth_look_dir, smooth_look_dir, NULL};

/* ======================================================================
 *  Objects
 * ====================================================================== */
static int children(LxScene *s, LxEntity e, LxEntity *out, int max) {
    int n = 0;
    for (LxEntity k = e ? lx_entity_first_child(s, e) : 0; k && n < max; k = lx_entity_next_sibling(s, k)) out[n++] = k;
    return n;
}
static void destroy_children(LxFsmCtx *c) {
    LxEntity kids[512];
    int n = children(c->scene, obj(c, 0), kids, 512);
    for (int i = 0; i < n; i++) if (lx_entity_valid(c->scene, kids[i])) lx_entity_destroy(c->scene, kids[i]);
}
static const LxFsmActionDesc A_DESTROY_CHILDREN = {"Destroy Children", "Objects", "Removes everything under an object (clear a spawned list, empty a container).", "destroy children of {0}",
    {POBJ("Object")}, 0, 0, LX_FSM_ONCE, destroy_children, NULL, NULL};
static void child_at(LxFsmCtx *c) {
    LxEntity kids[512];
    int n = children(c->scene, obj(c, 0), kids, 512), i = lx_fsm_int(c, 1);
    lx_fsm_set_object(c, 2, i >= 0 && i < n ? kids[i] : 0);
    lx_fsm_set_int(c, 3, n);
}
static const LxFsmActionDesc A_CHILD_AT = {"Get Child At Index", "Objects", "A child by its place in the Hierarchy (0 = the first), and how many there are.", "{2} = child {1} of {0}",
    {POBJ("Object"), P("Index", LX_FSM_INT, "0"), POUT("Store Child", LX_FSM_OBJECT), POPT("Store Child Count", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, child_at, child_at, NULL};
static void random_child(LxFsmCtx *c) {
    LxEntity kids[512];
    int n = children(c->scene, obj(c, 0), kids, 512);
    lx_fsm_set_object(c, 1, n ? kids[lx_random_int(0, n - 1)] : 0);
}
static const LxFsmActionDesc A_RANDOM_CHILD = {"Get Random Child", "Objects", "Picks one of an object's children at random (a random spawn point under a group).", "{1} = random child of {0}",
    {POBJ("Object"), POUT("Store Child", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, random_child, random_child, NULL};
static void get_root(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    for (int i = 0; e && lx_entity_parent(c->scene, e) && i < 64; i++) e = lx_entity_parent(c->scene, e);
    lx_fsm_set_object(c, 1, e);
}
static const LxFsmActionDesc A_GET_ROOT = {"Get Root", "Objects", "The top object of a hierarchy (the character a hand bone belongs to).", "{1} = root of {0}",
    {POBJ("Object"), POUT("Store Root", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, get_root, get_root, NULL};

/* loops: the Index variable remembers where it got to, so entering the state again takes the next one */
static void next_child(LxFsmCtx *c) {
    LxEntity kids[512];
    int n = children(c->scene, obj(c, 0), kids, 512), i = lx_fsm_int(c, 1);
    if (i < 0) i = 0;
    if (i >= n) { lx_fsm_set_int(c, 1, 0); lx_fsm_set_object(c, 2, 0); ev(c, 4); return; }
    lx_fsm_set_object(c, 2, kids[i]);
    lx_fsm_set_int(c, 1, i + 1);
    ev(c, 3);
}
#define LOOP_TIP "a variable that keeps the place between visits: set it to 0 to start again"
static const LxFsmActionDesc A_NEXT_CHILD = {"Get Next Child", "Objects",
    "Goes through an object's children one per visit: stores the next one and sends Loop, then Finished when there are none left (and starts over).",
    "{2} = next child of {0}",
    {POBJ("Object"), {"Index", LX_FSM_INT, "", LX_FSM_P_OUT, NULL, LOOP_TIP}, POUT("Store Child", LX_FSM_OBJECT), PEV("Loop Event"), PEV("Finished Event")},
    0, 0, LX_FSM_ONCE, next_child, NULL, NULL};
static void next_with_tag(LxFsmCtx *c) {
    char tag[LX_NAME_MAX];
    lx_strcpy(tag, sizeof tag, lx_fsm_string(c, 0));
    int want = lx_fsm_int(c, 1), seen = 0, n = lx_scene_entity_count(c->scene);
    if (want < 0) want = 0;
    for (int i = 0; i < n; i++) {
        LxEntity e = lx_scene_entity_at(c->scene, i);
        if (!e || !lx_entity_has_tag(c->scene, e, tag)) continue;
        if (seen++ == want) { lx_fsm_set_object(c, 2, e); lx_fsm_set_int(c, 1, want + 1); ev(c, 3); return; }
    }
    lx_fsm_set_int(c, 1, 0);
    lx_fsm_set_object(c, 2, 0);
    ev(c, 4);
}
static const LxFsmActionDesc A_NEXT_WITH_TAG = {"Get Next With Tag", "Objects",
    "Goes through every object with a tag one per visit: stores the next one and sends Loop, then Finished at the end (and starts over).",
    "{2} = next with tag {0}",
    {P("Tag", LX_FSM_STRING, "Enemy"), {"Index", LX_FSM_INT, "", LX_FSM_P_OUT, NULL, LOOP_TIP}, POUT("Store Object", LX_FSM_OBJECT), PEV("Loop Event"), PEV("Finished Event")},
    0, 0, LX_FSM_ONCE, next_with_tag, NULL, NULL};

/* ======================================================================
 *  Physics
 * ====================================================================== */
static void rb_props(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxRigidbody *rb = e ? lx_get_rigidbody(c->scene, e) : NULL;
    if (!rb) return;
    if (!lx_fsm_is_none(c, 1)) rb->mass = lx_maxf(lx_fsm_float(c, 1), 0.001f);
    if (!lx_fsm_is_none(c, 2)) rb->linear_damping = lx_maxf(lx_fsm_float(c, 2), 0);
    if (!lx_fsm_is_none(c, 3)) rb->angular_damping = lx_maxf(lx_fsm_float(c, 3), 0);
    int fr = lx_fsm_int(c, 4);
    if (fr) rb->freeze_rotation = fr == 1;
    lx_rigidbody_wake(c->scene, e);
}
static const LxFsmActionDesc A_RB_PROPS = {"Set Mass And Drag", "Physics", "Changes a Rigidbody's mass, drag and whether it can tip over (None = keep).", "{0}: mass {1}, drag {2}",
    {POBJ("Object"), OPT("Mass", LX_FSM_FLOAT), OPT("Drag", LX_FSM_FLOAT), OPT("Angular Drag", LX_FSM_FLOAT), PENUM("Freeze Rotation", "Don't Change|Freeze|Free", "Don't Change")},
    0, 0, LX_FSM_ONCE, rb_props, rb_props, NULL};
static void wake_up(LxFsmCtx *c) { LxEntity e = obj(c, 0); if (e) lx_rigidbody_wake(c->scene, e); lx_fsm_set_bool(c, 1, e && lx_rigidbody_sleeping(c->scene, e)); }
static const LxFsmActionDesc A_WAKE_UP = {"Wake Up", "Physics", "Wakes a resting Rigidbody so it reacts again (after you move what it rests on).", "wake {0}",
    {POBJ("Object"), POPT("Store Is Sleeping", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, wake_up, wake_up, NULL};

/* ======================================================================
 *  Audio
 * ====================================================================== */
static void set_audio_clip(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    LxAudioSource *a = e ? lx_get_audio_source(c->scene, e) : NULL;
    if (!a) return;
    const char *clip = lx_fsm_asset(c, 1);
    if (clip[0]) lx_strcpy(a->clip, sizeof a->clip, clip);
    int loop = lx_fsm_int(c, 2);
    if (loop) a->loop = loop == 1;
    if (lx_fsm_bool(c, 3)) { lx_audio_stop(c->scene, e); lx_audio_play(c->scene, e); }
}
static const LxFsmActionDesc A_SET_AUDIO_CLIP = {"Set Audio Clip", "Audio", "Swaps the sound on an Audio Source (music tracks, a radio), sets looping and plays it.", "{0} plays {1}",
    {POBJ("Object"), PASSET("Clip", "sound"), PENUM("Loop", "Don't Change|Loop|Once", "Don't Change"), P("Play Now", LX_FSM_BOOL, "true")}, 0, 0, LX_FSM_ONCE, set_audio_clip, NULL, NULL};
/* ======================================================================
 *  Application
 * ====================================================================== */
static void screen_size(LxFsmCtx *c) {
    int w = 0, h = 0;
    lx_window_size(&w, &h);
    LxVec2 cs = lx_ui_canvas_size();
    lx_fsm_set_int(c, 0, w);
    lx_fsm_set_int(c, 1, h);
    lx_fsm_set_float(c, 2, cs.x);
    lx_fsm_set_float(c, 3, cs.y);
    lx_fsm_set_bool(c, 4, h > w);
}
static const LxFsmActionDesc A_SCREEN_SIZE = {"Get Screen Size", "Application", "The window's size in pixels, the game UI canvas size, and whether the phone is held upright.",
    "{0} × {1} pixels",
    {POPT("Store Width", LX_FSM_INT), POPT("Store Height", LX_FSM_INT), POPT("Store Canvas Width", LX_FSM_FLOAT), POPT("Store Canvas Height", LX_FSM_FLOAT), POPT("Store Is Portrait", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, screen_size, screen_size, NULL};
static void target_fps(LxFsmCtx *c) { int f = lx_fsm_int(c, 0); lx_graphics_settings()->target_fps = f > 0 ? (f < 10 ? 10 : f) : 0; }
static const LxFsmActionDesc A_TARGET_FPS = {"Set Target Frame Rate", "Application", "Caps the frame rate (30 saves battery on phones; 0 = as fast as it goes).", "frame rate cap {0}",
    {P("Frames Per Second", LX_FSM_INT, "60")}, 0, 0, LX_FSM_ONCE, target_fps, NULL, NULL};
static void get_platform(LxFsmCtx *c) {
    LxPlatform g = lx_game_platform();
    int p = g == LX_PLATFORM_ANDROID ? 2 : g == LX_PLATFORM_WINDOWS ? 0 : 1;
    static const char *names[] = {"Windows", "Linux", "Android"};
    lx_fsm_set_string(c, 0, names[p]);
    lx_fsm_set_bool(c, 1, p == 2);
    ev(c, 2 + p);
}
static const LxFsmActionDesc A_PLATFORM = {"Get Platform", "Application", "Which system the game is running on (show touch controls only on phones). In the editor, Play with Android as the Build Settings target counts as Android.", "{0} = platform",
    {POPT("Store Name", LX_FSM_STRING), POPT("Store Is Mobile", LX_FSM_BOOL), PEV("Windows"), PEV("Linux"), PEV("Android")}, 0, 0, LX_FSM_ONCE, get_platform, get_platform, NULL};

/* ======================================================================
 *  State machine: other graphs and global variables
 * ====================================================================== */
static void graph_get_float(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (!e) return;
    lx_fsm_set_float(c, 3, lx_fsm_get_var_float(c->scene, e, lx_fsm_int(c, 1), lx_fsm_string(c, 2)));
}
#define SLOT_TIP "which State Graph on the object: 0 = the first"
static const LxFsmActionDesc A_GRAPH_GET_FLOAT = {"Get Graph Variable", "State Machine", "Reads a number variable from another object's State Graph (an enemy's Health).",
    "{3} = {0}.{2}",
    {POBJ("Object"), PT("Graph", LX_FSM_INT, "0", SLOT_TIP), P("Variable", LX_FSM_STRING, "Health"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, graph_get_float, graph_get_float, NULL};
static void graph_set_float(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    if (e) lx_fsm_set_var_float(c->scene, e, lx_fsm_int(c, 1), lx_fsm_string(c, 2), lx_fsm_float(c, 3));
}
static const LxFsmActionDesc A_GRAPH_SET_FLOAT = {"Set Graph Variable", "State Machine", "Sets a number (or Int / Bool) variable in another object's State Graph.", "{0}.{2} = {3}",
    {POBJ("Object"), PT("Graph", LX_FSM_INT, "0", SLOT_TIP), P("Variable", LX_FSM_STRING, "Health"), P("Value", LX_FSM_FLOAT, "0")}, 0, 0, LX_FSM_ONCE, graph_set_float, graph_set_float, NULL};
static void graph_state(LxFsmCtx *c) {
    LxEntity e = obj(c, 0);
    char st[LX_FSM_NAME];
    lx_strcpy(st, sizeof st, e ? lx_fsm_current_state(c->scene, e, lx_fsm_int(c, 1)) : "");
    lx_fsm_set_string(c, 2, st);
    const char *want = lx_fsm_string(c, 3);
    if (want[0]) ev(c, strcmp(st, want) ? 5 : 4);
}
static const LxFsmActionDesc A_GRAPH_STATE = {"Get Current State", "State Machine", "Which state another object's State Graph is in, optionally compared with a name.",
    "{2} = state of {0}",
    {POBJ("Object"), PT("Graph", LX_FSM_INT, "0", SLOT_TIP), POPT("Store State", LX_FSM_STRING), PT("Compare With", LX_FSM_STRING, "", "a state name; empty = just store it"),
     PEV("Is That State"), PEV("Is Another State")}, 0, 0, LX_FSM_ONCE, graph_state, graph_state, NULL};
static void global_get(LxFsmCtx *c) { lx_fsm_set_float(c, 1, lx_fsm_global_float(lx_fsm_string(c, 0))); }
static const LxFsmActionDesc A_GLOBAL_GET = {"Get Global Float", "State Machine", "Reads a number from the project's global variables (Assets/Globals.lxglobals).", "{1} = global {0}",
    {P("Name", LX_FSM_STRING, "Score"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, global_get, global_get, NULL};
static void global_set(LxFsmCtx *c) { lx_fsm_set_global_float(lx_fsm_string(c, 0), lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_GLOBAL_SET = {"Set Global Float", "State Machine", "Sets a number in the project's global variables (shared by every graph and scene).", "global {0} = {1}",
    {P("Name", LX_FSM_STRING, "Score"), P("Value", LX_FSM_FLOAT, "0")}, 0, 0, LX_FSM_ONCE, global_set, global_set, NULL};

/* ======================================================================
 *  Debug
 * ====================================================================== */
static void log_value(LxFsmCtx *c) {
    char text[LX_REF_MAX];
    lx_strcpy(text, sizeof text, lx_fsm_string(c, 0));
    const char *who = lx_entity_name(c->scene, c->self);
    switch (lx_fsm_int(c, 1)) {
    case 0: LX_INFO("[%s] %s %d", who, text, lx_fsm_int(c, 2)); break;
    case 1: LX_INFO("[%s] %s %s", who, text, lx_fsm_bool(c, 3) ? "true" : "false"); break;
    case 2: { LxVec3 v = lx_fsm_vec3(c, 4); LX_INFO("[%s] %s (%g, %g, %g)", who, text, (double)v.x, (double)v.y, (double)v.z); break; }
    case 3: { LxEntity e = obj(c, 5); LX_INFO("[%s] %s %s", who, text, e ? lx_entity_name(c->scene, e) : "None"); break; }
    default: { char s[LX_REF_MAX]; lx_strcpy(s, sizeof s, lx_fsm_string(c, 6)); LX_INFO("[%s] %s \"%s\"", who, text, s); break; }
    }
}
static const LxFsmActionDesc A_LOG_VALUE = {"Log Value", "Debug", "Writes a line with an Int, Bool, Vector3, Object or String to the Console (Log does numbers).", "Log \"{0}\" {1}",
    {P("Text", LX_FSM_STRING, "Value:"), PENUM("Type", "Int|Bool|Vector3|Object|String", "Int"), OPT("Int", LX_FSM_INT), OPT("Bool", LX_FSM_BOOL), OPT("Vector3", LX_FSM_VEC3),
     POBJN("Object"), OPT("String", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, log_value, log_value, NULL};

/* ======================================================================
 *  registration
 * ====================================================================== */
void lx_fsm_register_playmaker_actions(void) {
    static const LxFsmActionDesc *all[] = {
        &A_GET_KEY, &A_GET_KEY_DOWN, &A_GET_KEY_UP, &A_GET_BUTTON, &A_GET_BUTTON_DOWN, &A_GET_BUTTON_UP,
        &A_GET_MOUSE_BUTTON, &A_GET_MOUSE_BUTTON_DOWN, &A_GET_MOUSE_BUTTON_UP, &A_GET_AXIS_VALUE, &A_GET_AXIS_VECTOR, &A_GET_MOUSE_X, &A_GET_MOUSE_Y,
        &A_MOUSE_PICK, &A_MOUSE_PICK_EVENT, &A_TOUCH_COUNT, &A_TOUCH_INFO, &A_TOUCH_EVENT, &A_TOUCH_OBJECT_EVENT, &A_SWIPE,
        &A_PAD_BUTTON, &A_PAD_AXIS, &A_PAD_INFO, &A_INPUT_DEVICE, &A_TYPED_TEXT,
        &A_BOOL_OP, &A_FLOAT_SWITCH, &A_FLOAT_SIGN, &A_SEL_FLOAT, &A_SEL_STRING, &A_SEL_OBJECT,
        &A_VEC_CLAMP, &A_VEC_LENGTH, &A_VEC_INVERT, &A_VEC_PER_SECOND, &A_VEC_ROTATE_TOWARDS, &A_VEC_INTERP, &A_VEC_FILTER, &A_TRIG, &A_ATAN2, &A_FLOAT_REMAP,
        &A_SUBSTRING, &A_STRING_REPLACE, &A_STRING_CASE, &A_STRING_PART, &A_STRING_TO_NUMBER,
        &A_GET_RGBA, &A_SET_RGBA, &A_COLOR_INTERP, &A_SEL_COLOR, &A_COLOR_RAMP,
        &A_SYSTEM_TIME, &A_RANDOM_ROTATION, &A_ANGLE_TO_TARGET, &A_TRANSFORM_POINT, &A_SMOOTH_LOOK_DIR,
        &A_DESTROY_CHILDREN, &A_CHILD_AT, &A_RANDOM_CHILD, &A_GET_ROOT, &A_NEXT_CHILD, &A_NEXT_WITH_TAG,
        &A_RB_PROPS, &A_WAKE_UP, &A_SET_AUDIO_CLIP,
        &A_SCREEN_SIZE, &A_TARGET_FPS, &A_PLATFORM,
        &A_GRAPH_GET_FLOAT, &A_GRAPH_SET_FLOAT, &A_GRAPH_STATE, &A_GLOBAL_GET, &A_GLOBAL_SET, &A_LOG_VALUE,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
