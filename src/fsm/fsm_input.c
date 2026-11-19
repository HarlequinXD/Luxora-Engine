/* Input: the Input, Keyboard, Mouse, Touch and Gamepad categories.
 *
 * Two ways in. Named actions and axes ("Jump", "MoveX") go through the project's Input Manager, so one
 * graph works on keyboard, gamepad and touch and the player can rebind it — that is what Get Input
 * Action and Get Input Axis do, and it is what you should reach for. Raw keys, mouse buttons and sticks
 * are there too, for prototyping and for anything the Input Manager does not cover. */
#include "fsm_actions.h"

#define FSM_KEYS "A|B|C|D|E|F|G|H|I|J|K|L|M|N|O|P|Q|R|S|T|U|V|W|X|Y|Z|" \
                "0|1|2|3|4|5|6|7|8|9|Space|Escape|Enter|Tab|Backspace|Delete|Insert|" \
                "Left|Right|Up|Down|Home|End|Page Up|Page Down|Shift|Ctrl|Alt|" \
                "F1|F2|F3|F4|F5|F6|F7|F8|F9|F10|F11|F12"

static LxKey fsm_key(int i) {
    if (i >= 0 && i < 26) return (LxKey)(LX_KEY_A + i);
    if (i >= 26 && i < 36) return (LxKey)(LX_KEY_0 + (i - 26));
    switch (i) {
    case 36: return LX_KEY_SPACE;
    case 37: return LX_KEY_ESCAPE;
    case 38: return LX_KEY_ENTER;
    case 39: return LX_KEY_TAB;
    case 40: return LX_KEY_BACKSPACE;
    case 41: return LX_KEY_DELETE;
    case 42: return LX_KEY_INSERT;
    case 43: return LX_KEY_LEFT;
    case 44: return LX_KEY_RIGHT;
    case 45: return LX_KEY_UP;
    case 46: return LX_KEY_DOWN;
    case 47: return LX_KEY_HOME;
    case 48: return LX_KEY_END;
    case 49: return LX_KEY_PAGE_UP;
    case 50: return LX_KEY_PAGE_DOWN;
    case 51: return LX_KEY_SHIFT;
    case 52: return LX_KEY_CTRL;
    case 53: return LX_KEY_ALT;
    default: break;
    }
    if (i >= 54 && i < 66) return (LxKey)(LX_KEY_F1 + (i - 54));
    return LX_KEY_UNKNOWN;
}
static bool fsm_key_state(LxKey k, int when) {
    return when == 0 ? lx_key_pressed(k) : (when == 1 ? lx_key_released(k) : lx_key_down(k));
}
static bool fsm_mouse_state(LxMouseButton b, int when) {
    return when == 0 ? lx_mouse_pressed(b) : (when == 1 ? lx_mouse_released(b) : lx_mouse_down(b));
}

/* ==========================================================================================
 *  named actions and axes — the Input Manager
 * ========================================================================================== */
static void in_action(LxFsmCtx *c) {
    char name[32];
    lx_strcpy(name, sizeof name, lx_fsm_string(c, 0));
    int when = lx_fsm_int(c, 1);
    bool on = when == 0 ? lx_action_pressed(name) : (when == 1 ? lx_action_released(name) : lx_action_down(name));
    lx_fsm_set_bool(c, 2, on);
    if (on) fsm_send(c, 3);
    else fsm_send(c, 4);
}
static const LxFsmActionDesc A_IN_ACTION = {"Get Input Action", "Input", "Reads a named action from the Input Manager, so it works on keyboard, gamepad and touch and the player can rebind it.",
    "{0} {1}?",
    {PT("Action", LX_FSM_STRING, "Jump", "a name from the Input Manager window"),
     PENUMT("When", "Pressed|Released|Held", "0", "Pressed fires once on the frame the button goes down"),
     POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_action, in_action, NULL};

static void in_axis(LxFsmCtx *c) {
    char x[32], y[32];
    lx_strcpy(x, sizeof x, lx_fsm_string(c, 0));
    lx_strcpy(y, sizeof y, lx_fsm_string(c, 1));
    float fx = x[0] ? lx_axis(x) : 0.0f;
    float fy = y[0] ? lx_axis(y) : 0.0f;
    if (x[0] && y[0]) {
        LxVec2 v = lx_axis2(x, y);    /* a round dead zone, so diagonals are not faster */
        fx = v.x;
        fy = v.y;
    }
    float scale = lx_fsm_float(c, 2);
    fx *= scale;
    fy *= scale;
    lx_fsm_set_float(c, 3, fx);
    lx_fsm_set_float(c, 4, fy);
    lx_fsm_set_vec3(c, 5, lx_v3(fx, fy, 0));
    /* also as a flat world direction, which is what you usually feed into movement.
       Luxora faces +Z with +Y up, so the player's right is -X: pushing the stick right gives -X. */
    lx_fsm_set_vec3(c, 6, lx_v3(-fx, 0, fy));
    float mag = sqrtf(fx * fx + fy * fy);
    lx_fsm_set_float(c, 7, mag);
    fsm_send(c, mag > 0.0001f ? 8 : 9);
}
static const LxFsmActionDesc A_IN_AXIS = {"Get Input Axis", "Input", "Reads one or two named axes from the Input Manager. With both set it applies a round dead zone, so diagonals are not faster than straight lines.",
    "axes {0}, {1}",
    {PT("X Axis", LX_FSM_STRING, "MoveX", "a name from the Input Manager window"),
     PT("Y Axis", LX_FSM_STRING, "MoveY", "leave empty to read the X axis alone"),
     PT("Multiplier", LX_FSM_FLOAT, "1", "scales both axes"),
     POPT("Store X", LX_FSM_FLOAT), POPT("Store Y", LX_FSM_FLOAT), POPT("Store Vector2", LX_FSM_VEC2),
     POPTT("Store Flat Vector3", LX_FSM_VEC3, "x and y laid out on the ground as x and z"),
     POPT("Store Magnitude", LX_FSM_FLOAT), PEV("Moving Event"), PEV("Idle Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_axis, in_axis, NULL};

static void in_move_direction(LxFsmCtx *c) {
    char x[32], y[32];
    lx_strcpy(x, sizeof x, lx_fsm_string(c, 0));
    lx_strcpy(y, sizeof y, lx_fsm_string(c, 1));
    LxVec2 v = lx_axis2(x, y);
    LxEntity rel = fsm_obj(c, 2);
    LxVec3 dir;
    if (rel) {
        /* steer relative to something, almost always the camera: the usual third-person control scheme.
           The right-hand direction is forward x up, so it matches which way the player sees "right". */
        LxVec3 fwd = lx_entity_forward(c->scene, rel);
        fwd.y = 0;
        fwd = lx_norm3(fwd);
        if (lx_len3sq(fwd) < 1e-8f) fwd = lx_v3(0, 0, 1);
        LxVec3 right = lx_norm3(lx_cross3(fwd, lx_v3(0, 1, 0)));
        dir = lx_add3(lx_scale3(right, v.x), lx_scale3(fwd, v.y));
    } else {
        dir = lx_v3(-v.x, 0, v.y);   /* facing +Z with +Y up, the player's right is -X */
    }
    float mag = lx_len3(dir);
    if (mag > 1.0f) dir = lx_scale3(dir, 1.0f / mag);
    lx_fsm_set_vec3(c, 3, dir);
    lx_fsm_set_float(c, 4, lx_minf(mag, 1.0f));
    fsm_send(c, mag > 0.0001f ? 5 : 6);
}
static const LxFsmActionDesc A_IN_MOVE_DIR = {"Get Move Direction", "Input", "Turns the movement sticks into a world direction, measured from the camera: the standard third-person control scheme.",
    "move direction -> {3}",
    {P("X Axis", LX_FSM_STRING, "MoveX"), P("Y Axis", LX_FSM_STRING, "MoveY"),
     POBJNT("Relative To", "usually the camera; None steers along the world axes"),
     POUT("Store Direction", LX_FSM_VEC3), POPTT("Store Amount", LX_FSM_FLOAT, "0 to 1: how far the stick is pushed"),
     PEV("Moving Event"), PEV("Idle Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_move_direction, in_move_direction, NULL};

/* ==========================================================================================
 *  raw keyboard and mouse
 * ========================================================================================== */
static void in_key(LxFsmCtx *c) {
    bool on = fsm_key_state(fsm_key(lx_fsm_int(c, 0)), lx_fsm_int(c, 1));
    lx_fsm_set_bool(c, 2, on);
    fsm_send(c, on ? 3 : 4);
}
static const LxFsmActionDesc A_IN_KEY = {"Get Key", "Keyboard", "Reads a key directly. Good for prototyping; named actions are better for a finished game, because players can rebind those.",
    "key {0} {1}?",
    {PENUM("Key", FSM_KEYS, "36"), PENUM("When", "Pressed|Released|Held", "0"),
     POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_key, in_key, NULL};

static void in_any_key(LxFsmCtx *c) {
    bool any = false;
    for (int i = 0; i < 66 && !any; i++) any = lx_key_pressed(fsm_key(i));
    if (!any) for (int b = 0; b < LX_MOUSE_COUNT && !any; b++) any = lx_mouse_pressed((LxMouseButton)b);
    lx_fsm_set_bool(c, 0, any);
    if (any) fsm_send(c, 1);
}
static const LxFsmActionDesc A_IN_ANY_KEY = {"Any Key Pressed", "Keyboard", "True on the frame any key or mouse button goes down: \"press any key to continue\".",
    "any key?",
    {POPT("Store Result", LX_FSM_BOOL), PEV("Pressed Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_any_key, in_any_key, NULL};

static void in_text(LxFsmCtx *c) {
    int n = 0;
    const uint32_t *cp = lx_text_input(&n);
    char buf[LX_REF_MAX];
    int at = 0;
    for (int i = 0; i < n && at < (int)sizeof buf - 1; i++) {
        if (cp[i] >= 32 && cp[i] < 127) buf[at++] = (char)cp[i];   /* plain ASCII only */
    }
    buf[at] = 0;
    lx_fsm_set_string(c, 0, buf);
    lx_fsm_set_int(c, 1, at);
    if (at > 0) fsm_send(c, 2);
}
static const LxFsmActionDesc A_IN_TEXT = {"Get Typed Text", "Keyboard", "The characters typed this frame: name entry, chat, cheat codes.",
    "typed -> {0}",
    {POUT("Store Text", LX_FSM_STRING), POPT("Store Count", LX_FSM_INT), PEV("Typed Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_text, in_text, NULL};

static void in_mouse_button(LxFsmCtx *c) {
    bool on = fsm_mouse_state((LxMouseButton)lx_fsm_int(c, 0), lx_fsm_int(c, 1));
    lx_fsm_set_bool(c, 2, on);
    fsm_send(c, on ? 3 : 4);
}
static const LxFsmActionDesc A_IN_MOUSE_BUTTON = {"Get Mouse Button", "Mouse", "Reads a mouse button.", "mouse {0} {1}?",
    {PENUM("Button", "Left|Right|Middle", "0"), PENUM("When", "Pressed|Released|Held", "0"),
     POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_mouse_button, in_mouse_button, NULL};

static void in_mouse(LxFsmCtx *c) {
    LxVec2 p = lx_mouse_pos(), d = lx_mouse_delta();
    LxVec2 canvas = lx_ui_screen_to_canvas(p);
    lx_fsm_set_vec3(c, 0, lx_v3(p.x, p.y, 0));
    lx_fsm_set_vec3(c, 1, lx_v3(canvas.x, canvas.y, 0));
    lx_fsm_set_vec3(c, 2, lx_v3(d.x, d.y, 0));
    lx_fsm_set_float(c, 3, lx_mouse_wheel());
    lx_fsm_set_bool(c, 4, lx_mouse_locked());
}
static const LxFsmActionDesc A_IN_MOUSE = {"Get Mouse", "Mouse", "Where the pointer is, how far it moved this frame, and the wheel. The delta is what mouse-look needs.",
    "mouse info",
    {POPTT("Store Window Position", LX_FSM_VEC2, "in window pixels"),
     POPTT("Store Canvas Position", LX_FSM_VEC2, "in the UI's canvas units"),
     POPTT("Store Delta", LX_FSM_VEC2, "movement this frame; works while the pointer is locked"),
     POPT("Store Wheel", LX_FSM_FLOAT), POPT("Store Is Locked", LX_FSM_BOOL)},
    0, 0, LX_FSM_EVERY_FRAME, in_mouse, in_mouse, NULL};

/* ==========================================================================================
 *  gamepad
 * ========================================================================================== */
static void in_pad_button(LxFsmCtx *c) {
    int pad = lx_fsm_int(c, 0);
    bool on = lx_gamepad_connected(pad) && lx_gamepad_button(pad, (LxGamepadButton)lx_fsm_int(c, 1));
    lx_fsm_set_bool(c, 2, on);
    fsm_send(c, on ? 3 : 4);
}
static const LxFsmActionDesc A_IN_PAD_BUTTON = {"Get Gamepad Button", "Gamepad", "Reads a gamepad button directly. Held only — use a named action if you need the press moment.",
    "pad {0} button {1}?",
    {PT("Pad", LX_FSM_INT, "0", "0 to 3"),
     PENUM("Button", "A|B|X|Y|LB|RB|Back|Start|Left Stick|Right Stick|D-Pad Up|D-Pad Down|D-Pad Left|D-Pad Right|Guide", "0"),
     POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_pad_button, in_pad_button, NULL};

static void in_pad_axis(LxFsmCtx *c) {
    int pad = lx_fsm_int(c, 0);
    if (!lx_gamepad_connected(pad)) { fsm_send(c, 6); return; }
    float lx_ = lx_gamepad_axis(pad, LX_PAD_LEFT_X), ly = lx_gamepad_axis(pad, LX_PAD_LEFT_Y);
    float rx = lx_gamepad_axis(pad, LX_PAD_RIGHT_X), ry = lx_gamepad_axis(pad, LX_PAD_RIGHT_Y);
    float dead = lx_saturate(lx_fsm_float(c, 1));
    if (sqrtf(lx_ * lx_ + ly * ly) < dead) { lx_ = 0; ly = 0; }
    if (sqrtf(rx * rx + ry * ry) < dead) { rx = 0; ry = 0; }
    lx_fsm_set_vec3(c, 2, lx_v3(lx_, ly, 0));
    lx_fsm_set_vec3(c, 3, lx_v3(rx, ry, 0));
    lx_fsm_set_float(c, 4, lx_gamepad_axis(pad, LX_PAD_LT));
    lx_fsm_set_float(c, 5, lx_gamepad_axis(pad, LX_PAD_RT));
}
static const LxFsmActionDesc A_IN_PAD_AXIS = {"Get Gamepad Sticks", "Gamepad", "Both sticks and both triggers of a gamepad, with a round dead zone applied.",
    "pad {0} sticks",
    {PT("Pad", LX_FSM_INT, "0", "0 to 3"), PT("Dead Zone", LX_FSM_FLOAT, "0.2", "stick movement smaller than this reads as nothing"),
     POPT("Store Left Stick", LX_FSM_VEC2), POPT("Store Right Stick", LX_FSM_VEC2),
     POPT("Store Left Trigger", LX_FSM_FLOAT), POPT("Store Right Trigger", LX_FSM_FLOAT), PEV("Not Connected Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_pad_axis, in_pad_axis, NULL};

static void in_pad_rumble(LxFsmCtx *c) {
    lx_gamepad_rumble(lx_fsm_int(c, 0), lx_saturate(lx_fsm_float(c, 1)), lx_saturate(lx_fsm_float(c, 2)),
                      lx_maxf(0, lx_fsm_float(c, 3)));
}
static const LxFsmActionDesc A_IN_PAD_RUMBLE = {"Gamepad Rumble", "Gamepad", "Shakes the controller: hits, explosions, engine rumble.",
    "rumble pad {0}",
    {P("Pad", LX_FSM_INT, "0"), PT("Low Frequency", LX_FSM_FLOAT, "0.5", "the heavy motor: thuds and explosions"),
     PT("High Frequency", LX_FSM_FLOAT, "0.5", "the light motor: texture and detail"), P("Seconds", LX_FSM_FLOAT, "0.2")},
    0, 0, LX_FSM_ONCE, in_pad_rumble, in_pad_rumble, NULL};

static void in_device(LxFsmCtx *c) {
    LxInputDevice d = lx_input_last_device();
    lx_fsm_set_int(c, 0, (int)d);
    lx_fsm_set_int(c, 1, lx_gamepad_count());
    fsm_send(c, d == LX_DEVICE_GAMEPAD ? 3 : (d == LX_DEVICE_TOUCH ? 4 : 2));
}
static const LxFsmActionDesc A_IN_DEVICE = {"Get Input Device", "Input", "Which kind of input the player last used, so you can show the right button prompts.",
    "last device -> {0}",
    {POPTT("Store Device", LX_FSM_INT, "0 keyboard and mouse, 1 gamepad, 2 touch"),
     POPT("Store Gamepad Count", LX_FSM_INT), PEV("Keyboard Event"), PEV("Gamepad Event"), PEV("Touch Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_device, in_device, NULL};

/* ==========================================================================================
 *  touch
 * ========================================================================================== */
static void in_touch(LxFsmCtx *c) {
    int n = lx_touch_count();
    lx_fsm_set_int(c, 0, n);
    int i = lx_fsm_int(c, 1);
    if (i < 0 || i >= n) { fsm_send(c, 6); return; }
    LxVec2 p = lx_touch_pos(i), d = lx_touch_delta(i);
    LxVec2 canvas = lx_ui_screen_to_canvas(p);
    lx_fsm_set_vec3(c, 2, lx_v3(canvas.x, canvas.y, 0));
    lx_fsm_set_vec3(c, 3, lx_v3(d.x, d.y, 0));
    LxTouchPhase phase = lx_touch_phase(i);
    lx_fsm_set_int(c, 4, (int)phase);
    lx_fsm_set_bool(c, 5, phase != LX_TOUCH_NONE);
    if (phase == LX_TOUCH_BEGAN) fsm_send(c, 7);
    else if (phase == LX_TOUCH_ENDED) fsm_send(c, 8);
}
static const LxFsmActionDesc A_IN_TOUCH = {"Get Touch", "Touch", "Where a finger is on screen, how it is moving, and whether it has just landed or lifted.",
    "touch {1}",
    {POPT("Store Touch Count", LX_FSM_INT), PT("Finger Index", LX_FSM_INT, "0", "0 is the first finger down"),
     POPTT("Store Canvas Position", LX_FSM_VEC2, "in the UI's canvas units"), POPT("Store Delta", LX_FSM_VEC2),
     POPTT("Store Phase", LX_FSM_INT, "0 none, 1 began, 2 moved, 3 still, 4 ended"), POPT("Store Is Touching", LX_FSM_BOOL),
     PEV("No Touch Event"), PEV("Began Event"), PEV("Ended Event")},
    0, 0, LX_FSM_EVERY_FRAME, in_touch, in_touch, NULL};

typedef struct { LxVec2 start; bool down; float t; } FsmSwipe;
static void in_swipe(LxFsmCtx *c) {
    FsmSwipe *s = c->state;
    bool touching = lx_touch_count() > 0;
    LxVec2 p = touching ? lx_touch_pos(0) : lx_mouse_pos();
    bool down = touching || lx_mouse_down(LX_MOUSE_LEFT);
    if (down && !s->down) { s->down = true; s->start = p; s->t = 0; return; }
    if (down) { s->t += c->dt; return; }
    if (!s->down) return;
    s->down = false;
    LxVec2 d = lx_v2(p.x - s->start.x, p.y - s->start.y);
    float len = sqrtf(d.x * d.x + d.y * d.y);
    float min_len = lx_fsm_float(c, 0);
    float max_time = lx_fsm_float(c, 1);
    if (len < min_len || (max_time > 0 && s->t > max_time)) { fsm_send(c, 7); return; }
    lx_fsm_set_vec3(c, 2, lx_v3(d.x, d.y, 0));
    lx_fsm_set_float(c, 3, len);
    /* screen y grows downwards, so an upward swipe has a negative y */
    if (fabsf(d.x) > fabsf(d.y)) fsm_send(c, d.x > 0 ? 4 : 5);
    else fsm_send(c, d.y < 0 ? 6 : 8);
}
static const LxFsmActionDesc A_IN_SWIPE = {"Detect Swipe", "Touch", "Watches for a flick of a finger (or the mouse) and reports which way it went.",
    "swipe",
    {PT("Min Distance", LX_FSM_FLOAT, "80", "window pixels the finger must travel"),
     PT("Max Time", LX_FSM_FLOAT, "0.5", "seconds; slower than this is a drag, not a swipe; 0 = no limit"),
     POPT("Store Delta", LX_FSM_VEC2), POPT("Store Distance", LX_FSM_FLOAT),
     PEV("Swipe Right Event"), PEV("Swipe Left Event"), PEV("Swipe Up Event"), PEV("Tap Event"), PEV("Swipe Down Event")},
    sizeof(FsmSwipe), 0, LX_FSM_EVERY_FRAME, in_swipe, in_swipe, NULL};

/* ==========================================================================================
 *  looking around
 * ========================================================================================== */
/* Mouse look is two rotations on two objects, and the only part worth any thought is that the pitch
 * has to be remembered: it is an angle the player has arrived at, not something read from the input,
 * and reading it back off the head each frame would let it drift and lose its limits. */
typedef struct { float pitch; bool started; } FsmLook;

static float look_wrap_deg(float a) {
    a = fmodf(a, 360.0f);
    if (a > 180.0f) a -= 360.0f;
    if (a < -180.0f) a += 360.0f;
    return a;
}

static void in_look(LxFsmCtx *c) {
    FsmLook *st = c->state;
    LxEntity body = fsm_obj(c, 0);
    LxEntity head = fsm_obj(c, 1);
    if (head == body) head = 0;   /* one object cannot be both pivots: the pitch would fight the yaw */
    if (!body && !head) return;
    int source = lx_fsm_int(c, 2);
    float sens = lx_fsm_float(c, 3);
    float vsens = lx_fsm_float(c, 4);
    if (!(vsens > 0)) vsens = sens;
    float down = lx_fsm_float(c, 6), up = lx_fsm_float(c, 7);
    if (down > up) { float t = down; down = up; up = t; }

    /* The mouse reports how far it moved, a stick reports how far it is pushed. One is already a
       movement and the other is a rate, so a stick is turned into degrees by the clock. */
    LxVec2 d = lx_v2(0, 0);
    if (source == 1) {
        d = lx_v2(lx_gamepad_axis(0, LX_PAD_RIGHT_X), lx_gamepad_axis(0, LX_PAD_RIGHT_Y));
        d = lx_v2(d.x * 60.0f * c->dt, -d.y * 60.0f * c->dt);   /* a stick's +Y is up, the mouse's is down */
    } else if (source == 2) {
        const char *xa = lx_fsm_string(c, 8), *ya = lx_fsm_string(c, 9);
        d = lx_v2(xa && xa[0] ? lx_axis(xa) : 0.0f, ya && ya[0] ? lx_axis(ya) : 0.0f);
        d = lx_v2(d.x * 60.0f * c->dt, -d.y * 60.0f * c->dt);
    } else {
        if (lx_fsm_bool(c, 10)) lx_mouse_lock(true);
        d = lx_mouse_delta();
        if (!lx_mouse_locked() && lx_fsm_bool(c, 10)) d = lx_v2(0, 0);   /* not held yet: no jump as it catches */
    }

    if (body) {
        /* left and right, about the body's own up: the same turn whether it stands on a slope or not */
        float yaw = d.x * sens;
        if (yaw != 0 && yaw == yaw) {
            LxQuat r = lx_entity_rotation(c->scene, body);
            lx_entity_set_rotation(c->scene, body, lx_quat_norm(lx_quat_mul(r, lx_quat_axis_angle(lx_v3(0, 1, 0), yaw * LX_DEG2RAD))));
        }
        lx_fsm_set_float(c, 12, look_wrap_deg(lx_entity_euler(c->scene, body).y));
    }
    if (head) {
        if (!st->started) {
            st->started = true;
            st->pitch = lx_clampf(look_wrap_deg(lx_entity_euler(c->scene, head).x), down, up);
        }
        float pitch = d.y * vsens * (lx_fsm_bool(c, 5) ? 1.0f : -1.0f);
        if (pitch == pitch) st->pitch = lx_clampf(st->pitch + pitch, down, up);
        lx_entity_set_euler(c->scene, head, lx_v3(st->pitch, 0, 0));
        lx_fsm_set_float(c, 11, st->pitch);
    }
}
static const LxFsmActionDesc A_IN_LOOK = {"Mouse Look", "Mouse",
    "Turns a character with the mouse, a stick or a pair of axes: the body left and right, a head or camera pivot up and down, "
    "with the pitch kept inside its limits. This is the looking half of a first or third person controller.",
    "{0} looks around",
    {POBJT("Body Object", "turned left and right - usually the character itself"),
     POBJNT("Head Object", "tilted up and down - usually the camera's pivot under the body; None leaves the pitch alone"),
     PENUMT("Source", "Mouse|Gamepad Right Stick|Input Axes", "0", "where the looking comes from"),
     PT("Sensitivity", LX_FSM_FLOAT, "0.2", "degrees per pixel of mouse movement; for a stick, degrees per second at 60"),
     PT("Vertical Sensitivity", LX_FSM_FLOAT, "0", "0 uses Sensitivity for both"),
     PT("Invert Y", LX_FSM_BOOL, "false", "push up to look down"),
     PT("Down Limit", LX_FSM_FLOAT, "-80", "how far down it can look, in degrees"),
     PT("Up Limit", LX_FSM_FLOAT, "80", "how far up it can look, in degrees"),
     PT("X Axis", LX_FSM_STRING, "LookX", "Input Axes: the axis that turns left and right"),
     PT("Y Axis", LX_FSM_STRING, "LookY", "Input Axes: the axis that looks up and down"),
     PT("Hold The Pointer", LX_FSM_BOOL, "true", "Mouse: hide it and keep it in the window, so looking never runs out of screen"),
     POPTT("Store Pitch", LX_FSM_FLOAT, "how far up or down it is looking, in degrees"),
     POPTT("Store Yaw", LX_FSM_FLOAT, "which way the body faces, in degrees")},
    sizeof(FsmLook), 0, LX_FSM_EVERY_FRAME, NULL, in_look, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_input(void) {
    static const LxFsmActionDesc *all[] = {
        &A_IN_ACTION, &A_IN_AXIS, &A_IN_MOVE_DIR,
        &A_IN_KEY, &A_IN_ANY_KEY, &A_IN_TEXT,
        &A_IN_MOUSE_BUTTON, &A_IN_MOUSE, &A_IN_LOOK,
        &A_IN_PAD_BUTTON, &A_IN_PAD_AXIS, &A_IN_PAD_RUMBLE, &A_IN_DEVICE,
        &A_IN_TOUCH, &A_IN_SWIPE,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
