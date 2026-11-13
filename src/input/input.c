/* Input manager: named actions/axes bound to keyboard, mouse, gamepads and touch controls.
 * Gamepads: XInput on Windows, the Linux joystick interface (/dev/input/js*), and Android
 * game controllers (fed by the Android platform layer). */
#include "../lx_internal.h"

#if LX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif LX_OS_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#endif

LxPadState g_lx_pads[LX_MAX_PADS];

typedef struct ActionState { bool down, prev; float value; } ActionState;
typedef struct TouchState {
    bool pressed;           /* buttons */
    LxVec2 stick;           /* sticks, -1..1, +Y up */
    LxVec2 look_delta;      /* look areas: pixels this frame */
    LxVec2 last_pos;
    bool tracking;
} TouchState;

static struct {
    LxInputMap map;
    bool map_ready;
    ActionState state[LX_MAX_ACTIONS];
    TouchState touch[LX_MAX_TOUCH_CONTROLS];
    LxInputDevice last_device;
    float rumble_time[LX_MAX_PADS];
    double next_scan;
#if LX_OS_LINUX
    int js_fd[LX_MAX_PADS];
    bool js_init;
#endif
} IN;

/* ======================================================================
 *  names
 * ====================================================================== */
const char *lx_key_name(LxKey k) {
    static char buf[2][8];
    static int slot;
    if ((k >= LX_KEY_A && k <= LX_KEY_Z) || (k >= LX_KEY_0 && k <= LX_KEY_9)) {
        slot ^= 1;
        buf[slot][0] = (char)k;
        buf[slot][1] = 0;
        return buf[slot];
    }
    if (k >= LX_KEY_F1 && k <= LX_KEY_F12) {
        static const char *f[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        return f[k - LX_KEY_F1];
    }
    switch (k) {
    case LX_KEY_SPACE: return "Space";
    case LX_KEY_ESCAPE: return "Escape";
    case LX_KEY_ENTER: return "Enter";
    case LX_KEY_TAB: return "Tab";
    case LX_KEY_BACKSPACE: return "Backspace";
    case LX_KEY_DELETE: return "Delete";
    case LX_KEY_INSERT: return "Insert";
    case LX_KEY_LEFT: return "Left";
    case LX_KEY_RIGHT: return "Right";
    case LX_KEY_UP: return "Up";
    case LX_KEY_DOWN: return "Down";
    case LX_KEY_HOME: return "Home";
    case LX_KEY_END: return "End";
    case LX_KEY_PAGE_UP: return "PageUp";
    case LX_KEY_PAGE_DOWN: return "PageDown";
    case LX_KEY_SHIFT: return "Shift";
    case LX_KEY_CTRL: return "Ctrl";
    case LX_KEY_ALT: return "Alt";
    default: return "None";
    }
}

static const char *PAD_BUTTONS[LX_PAD_BUTTON_COUNT] = {"A", "B", "X", "Y", "LB", "RB", "Back", "Start", "LeftStick", "RightStick",
                                                       "DPadUp", "DPadDown", "DPadLeft", "DPadRight", "Guide"};
static const char *PAD_AXES[LX_PAD_AXIS_COUNT] = {"LeftStickX", "LeftStickY", "RightStickX", "RightStickY", "LeftTrigger", "RightTrigger"};
static const char *MOUSE_BUTTONS[LX_MOUSE_COUNT] = {"LeftButton", "RightButton", "MiddleButton"};
static const char *MOUSE_AXES[3] = {"MouseX", "MouseY", "Wheel"};
static const char *BIND_TYPES[LX_BIND_TYPE_COUNT] = {"key", "keys", "mouse", "mouse_axis", "pad", "pad_axis", "touch", "touch_x", "touch_y"};

const char *lx_pad_button_name(LxGamepadButton b) { return (b >= 0 && b < LX_PAD_BUTTON_COUNT) ? PAD_BUTTONS[b] : "?"; }
const char *lx_pad_axis_name(LxGamepadAxis a) { return (a >= 0 && a < LX_PAD_AXIS_COUNT) ? PAD_AXES[a] : "?"; }
const char *lx_mouse_button_name(LxMouseButton b) { return (b >= 0 && b < LX_MOUSE_COUNT) ? MOUSE_BUTTONS[b] : "?"; }
const char *lx_mouse_axis_name(LxMouseAxis a) { return (a >= 0 && a <= 2) ? MOUSE_AXES[a] : "?"; }
const char *lx_binding_type_name(LxBindingType t) { return (t >= 0 && t < LX_BIND_TYPE_COUNT) ? BIND_TYPES[t] : "?"; }

static int key_from_name(const char *n) {
    for (int k = 1; k < LX_KEY_COUNT; k++)
        if (!strcmp(lx_key_name((LxKey)k), n) && strcmp(n, "None")) return k;
    return 0;
}
static int index_of(const char *const *list, int count, const char *n) {
    for (int i = 0; i < count; i++) if (!strcmp(list[i], n)) return i;
    return 0;
}

void lx_binding_describe(const LxInputMap *map, const LxBinding *b, char *out, size_t cap) {
    const char *touch = (b->code >= 0 && map && b->code < map->touch_count) ? map->touch[b->code].name : "?";
    switch (b->type) {
    case LX_BIND_KEY: snprintf(out, cap, "%s", lx_key_name((LxKey)b->code)); break;
    case LX_BIND_KEY_AXIS: snprintf(out, cap, "%s / %s", lx_key_name((LxKey)b->code2), lx_key_name((LxKey)b->code)); break;
    case LX_BIND_MOUSE_BUTTON: snprintf(out, cap, "Mouse %s", MOUSE_BUTTONS[b->code % LX_MOUSE_COUNT]); break;
    case LX_BIND_MOUSE_AXIS: snprintf(out, cap, "%s", MOUSE_AXES[b->code % 3]); break;
    case LX_BIND_PAD_BUTTON: snprintf(out, cap, "Gamepad %s", lx_pad_button_name((LxGamepadButton)b->code)); break;
    case LX_BIND_PAD_AXIS: snprintf(out, cap, "Gamepad %s", lx_pad_axis_name((LxGamepadAxis)b->code)); break;
    case LX_BIND_TOUCH_BUTTON: snprintf(out, cap, "Touch %s", touch); break;
    case LX_BIND_TOUCH_STICK_X: snprintf(out, cap, "Touch %s X", touch); break;
    case LX_BIND_TOUCH_STICK_Y: snprintf(out, cap, "Touch %s Y", touch); break;
    default: snprintf(out, cap, "?"); break;
    }
}

/* ======================================================================
 *  map editing
 * ====================================================================== */
LxInputMap *lx_input_map(void) {
    if (!IN.map_ready) { lx_input_map_defaults(&IN.map); IN.map_ready = true; }
    return &IN.map;
}

LxInputAction *lx_input_find(LxInputMap *m, const char *name) {
    for (int i = 0; i < m->action_count; i++) if (!strcmp(m->actions[i].name, name)) return &m->actions[i];
    return NULL;
}

LxInputAction *lx_input_add_action(LxInputMap *m, const char *name, bool is_axis) {
    LxInputAction *a = lx_input_find(m, name);
    if (a) return a;
    if (m->action_count >= LX_MAX_ACTIONS) return NULL;
    a = &m->actions[m->action_count++];
    memset(a, 0, sizeof *a);
    lx_strcpy(a->name, sizeof a->name, name);
    a->is_axis = is_axis;
    a->dead_zone = is_axis ? 0.15f : 0.0f;
    a->sensitivity = 1.0f;
    return a;
}

bool lx_input_add_binding(LxInputAction *a, LxBinding b) {
    if (!a || a->binding_count >= LX_MAX_ACTION_BINDINGS) return false;
    if (b.scale == 0) b.scale = 1.0f;
    a->bindings[a->binding_count++] = b;
    return true;
}

static int add_touch(LxInputMap *m, const char *name, LxTouchControlType t, float x, float y, float size, float h) {
    if (m->touch_count >= LX_MAX_TOUCH_CONTROLS) return -1;
    LxTouchControl *c = &m->touch[m->touch_count];
    memset(c, 0, sizeof *c);
    lx_strcpy(c->name, sizeof c->name, name);
    c->type = t;
    c->x = x; c->y = y; c->size = size; c->height = h;
    return m->touch_count++;
}

#define B(t, c, c2, s) ((LxBinding){(t), (c), (c2), (s)})
void lx_input_map_defaults(LxInputMap *m) {
    memset(m, 0, sizeof *m);
    m->trigger_threshold = 0.5f;
    m->touch_controls_mode = 0;
    int move = add_touch(m, "Move Stick", LX_TOUCH_STICK, 0.14f, 0.72f, 0.16f, 0);
    int look = add_touch(m, "Look Area", LX_TOUCH_LOOK_AREA, 0.5f, 0.0f, 0.5f, 1.0f);
    int jump = add_touch(m, "Jump Button", LX_TOUCH_BUTTON, 0.9f, 0.74f, 0.075f, 0);
    int fire = add_touch(m, "Fire Button", LX_TOUCH_BUTTON, 0.79f, 0.86f, 0.065f, 0);

    LxInputAction *a = lx_input_add_action(m, "MoveX", true);
    lx_input_add_binding(a, B(LX_BIND_KEY_AXIS, LX_KEY_D, LX_KEY_A, 1));
    lx_input_add_binding(a, B(LX_BIND_KEY_AXIS, LX_KEY_RIGHT, LX_KEY_LEFT, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_AXIS, LX_PAD_LEFT_X, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_TOUCH_STICK_X, move, 0, 1));
    a = lx_input_add_action(m, "MoveY", true);
    lx_input_add_binding(a, B(LX_BIND_KEY_AXIS, LX_KEY_W, LX_KEY_S, 1));
    lx_input_add_binding(a, B(LX_BIND_KEY_AXIS, LX_KEY_UP, LX_KEY_DOWN, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_AXIS, LX_PAD_LEFT_Y, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_TOUCH_STICK_Y, move, 0, 1));
    a = lx_input_add_action(m, "LookX", true);
    a->dead_zone = 0.1f;
    lx_input_add_binding(a, B(LX_BIND_MOUSE_AXIS, LX_MOUSE_AXIS_X, 0, 0.08f));
    lx_input_add_binding(a, B(LX_BIND_PAD_AXIS, LX_PAD_RIGHT_X, 0, 2.5f));
    lx_input_add_binding(a, B(LX_BIND_TOUCH_STICK_X, look, 0, 0.12f));
    a = lx_input_add_action(m, "LookY", true);
    a->dead_zone = 0.1f;
    lx_input_add_binding(a, B(LX_BIND_MOUSE_AXIS, LX_MOUSE_AXIS_Y, 0, -0.08f));
    lx_input_add_binding(a, B(LX_BIND_PAD_AXIS, LX_PAD_RIGHT_Y, 0, 2.5f));
    lx_input_add_binding(a, B(LX_BIND_TOUCH_STICK_Y, look, 0, 0.12f));
    a = lx_input_add_action(m, "Jump", false);
    lx_input_add_binding(a, B(LX_BIND_KEY, LX_KEY_SPACE, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_BUTTON, LX_PAD_A, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_TOUCH_BUTTON, jump, 0, 1));
    a = lx_input_add_action(m, "Fire", false);
    lx_input_add_binding(a, B(LX_BIND_MOUSE_BUTTON, LX_MOUSE_LEFT, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_AXIS, LX_PAD_RT, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_TOUCH_BUTTON, fire, 0, 1));
    a = lx_input_add_action(m, "Aim", false);
    lx_input_add_binding(a, B(LX_BIND_MOUSE_BUTTON, LX_MOUSE_RIGHT, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_AXIS, LX_PAD_LT, 0, 1));
    a = lx_input_add_action(m, "Interact", false);
    lx_input_add_binding(a, B(LX_BIND_KEY, LX_KEY_E, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_BUTTON, LX_PAD_X, 0, 1));
    a = lx_input_add_action(m, "Sprint", false);
    lx_input_add_binding(a, B(LX_BIND_KEY, LX_KEY_SHIFT, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_BUTTON, LX_PAD_LSTICK, 0, 1));
    a = lx_input_add_action(m, "Pause", false);
    lx_input_add_binding(a, B(LX_BIND_KEY, LX_KEY_ESCAPE, 0, 1));
    lx_input_add_binding(a, B(LX_BIND_PAD_BUTTON, LX_PAD_START, 0, 1));
}
#undef B

/* ======================================================================
 *  save / load  (.lxinput, same key/value format as scenes)
 * ====================================================================== */
static void binding_to_text(const LxInputMap *m, const LxBinding *b, char *out, size_t cap) {
    const char *code = "?", *code2 = "";
    switch (b->type) {
    case LX_BIND_KEY: code = lx_key_name((LxKey)b->code); break;
    case LX_BIND_KEY_AXIS: code = lx_key_name((LxKey)b->code); code2 = lx_key_name((LxKey)b->code2); break;
    case LX_BIND_MOUSE_BUTTON: code = MOUSE_BUTTONS[b->code % LX_MOUSE_COUNT]; break;
    case LX_BIND_MOUSE_AXIS: code = MOUSE_AXES[b->code % 3]; break;
    case LX_BIND_PAD_BUTTON: code = lx_pad_button_name((LxGamepadButton)b->code); break;
    case LX_BIND_PAD_AXIS: code = lx_pad_axis_name((LxGamepadAxis)b->code); break;
    default: code = (b->code >= 0 && b->code < m->touch_count) ? m->touch[b->code].name : "?"; break;
    }
    if (code2[0]) snprintf(out, cap, "%s %s %s %g", lx_binding_type_name(b->type), code, code2, b->scale);
    else snprintf(out, cap, "%s %s %g", lx_binding_type_name(b->type), code, b->scale);
}

bool lx_input_map_save(const LxInputMap *m, const char *path) {
    LxStrBuf sb = {0};
    lx_sb_printf(&sb, "# Luxora input map v1\n[input]\ntrigger_threshold = %g\ntouch_controls = %s\n", m->trigger_threshold,
                 m->touch_controls_mode == 1 ? "always" : m->touch_controls_mode == 2 ? "never" : "auto");
    static const char *tt[] = {"button", "stick", "look_area"};
    for (int i = 0; i < m->touch_count; i++) {
        const LxTouchControl *c = &m->touch[i];
        lx_sb_printf(&sb, "\n[touch]\nname = %s\ntype = %s\nposition = %g %g\nsize = %g\nheight = %g\n", c->name, tt[c->type], c->x, c->y,
                     c->size, c->height);
    }
    for (int i = 0; i < m->action_count; i++) {
        const LxInputAction *a = &m->actions[i];
        lx_sb_printf(&sb, "\n[%s]\nname = %s\ndead_zone = %g\nsensitivity = %g\n", a->is_axis ? "axis" : "action", a->name, a->dead_zone,
                     a->sensitivity);
        for (int k = 0; k < a->binding_count; k++) {
            char t[128];
            binding_to_text(m, &a->bindings[k], t, sizeof t);
            lx_sb_printf(&sb, "bind = %s\n", t);
        }
    }
    bool ok = lx_file_write(path, sb.data, sb.len);
    lx_free(sb.data);
    return ok;
}

static int touch_index(const LxInputMap *m, const char *name) {
    for (int i = 0; i < m->touch_count; i++) if (!strcmp(m->touch[i].name, name)) return i;
    return -1;
}

static bool parse_binding(const LxInputMap *m, const char *text, LxBinding *b) {
    char type[32] = "", a[48] = "", c[48] = "";
    float scale = 1.0f;
    /* touch control names can contain spaces: parse type first, then handle per type */
    const char *p = text;
    int n = 0;
    if (sscanf(p, "%31s%n", type, &n) != 1) return false;
    p += n;
    while (*p == ' ') p++;
    memset(b, 0, sizeof *b);
    b->type = (LxBindingType)index_of(BIND_TYPES, LX_BIND_TYPE_COUNT, type);
    if (strcmp(BIND_TYPES[b->type], type)) return false;
    if (b->type >= LX_BIND_TOUCH_BUTTON) {
        /* everything up to the last token is the control name */
        char rest[128];
        lx_strcpy(rest, sizeof rest, p);
        char *last = strrchr(rest, ' ');
        if (last) { scale = (float)atof(last + 1); *last = 0; }
        b->code = touch_index(m, rest);
        b->scale = scale;
        return b->code >= 0;
    }
    if (b->type == LX_BIND_KEY_AXIS) sscanf(p, "%47s %47s %f", a, c, &scale);
    else sscanf(p, "%47s %f", a, &scale);
    b->scale = scale;
    switch (b->type) {
    case LX_BIND_KEY: b->code = key_from_name(a); return b->code != 0;
    case LX_BIND_KEY_AXIS: b->code = key_from_name(a); b->code2 = key_from_name(c); return b->code || b->code2;
    case LX_BIND_MOUSE_BUTTON: b->code = index_of(MOUSE_BUTTONS, LX_MOUSE_COUNT, a); return true;
    case LX_BIND_MOUSE_AXIS: b->code = index_of(MOUSE_AXES, 3, a); return true;
    case LX_BIND_PAD_BUTTON: b->code = index_of(PAD_BUTTONS, LX_PAD_BUTTON_COUNT, a); return true;
    case LX_BIND_PAD_AXIS: b->code = index_of(PAD_AXES, LX_PAD_AXIS_COUNT, a); return true;
    default: return false;
    }
}

bool lx_input_map_load(LxInputMap *m, const char *path) {
    char *text = lx_file_read(path, NULL);
    if (!text) return false;
    memset(m, 0, sizeof *m);
    m->trigger_threshold = 0.5f;
    const char *cur = text;
    char k[64], v[256], section[32] = "";
    int r;
    LxInputAction *act = NULL;
    LxTouchControl *tc = NULL;
    /* touch controls must exist before bindings reference them, so read the file twice */
    for (int pass = 0; pass < 2; pass++) {
        cur = text;
        act = NULL;
        tc = NULL;
        while ((r = lx_kv_next(&cur, k, sizeof k, v, sizeof v))) {
            if (r == 2) {
                lx_strcpy(section, sizeof section, k);
                act = NULL;
                tc = NULL;
                if (pass == 0 && !strcmp(k, "touch") && m->touch_count < LX_MAX_TOUCH_CONTROLS) {
                    tc = &m->touch[m->touch_count++];
                    memset(tc, 0, sizeof *tc);
                }
                continue;
            }
            if (pass == 0) {
                if (!strcmp(section, "input")) {
                    if (!strcmp(k, "trigger_threshold")) m->trigger_threshold = (float)atof(v);
                    else if (!strcmp(k, "touch_controls")) m->touch_controls_mode = !strcmp(v, "always") ? 1 : !strcmp(v, "never") ? 2 : 0;
                } else if (tc) {
                    if (!strcmp(k, "name")) lx_strcpy(tc->name, sizeof tc->name, v);
                    else if (!strcmp(k, "type")) tc->type = !strcmp(v, "stick") ? LX_TOUCH_STICK : !strcmp(v, "look_area") ? LX_TOUCH_LOOK_AREA : LX_TOUCH_BUTTON;
                    else if (!strcmp(k, "position")) sscanf(v, "%f %f", &tc->x, &tc->y);
                    else if (!strcmp(k, "size")) tc->size = (float)atof(v);
                    else if (!strcmp(k, "height")) tc->height = (float)atof(v);
                }
            } else if (!strcmp(section, "action") || !strcmp(section, "axis")) {
                if (!strcmp(k, "name")) act = lx_input_add_action(m, v, !strcmp(section, "axis"));
                else if (act && !strcmp(k, "dead_zone")) act->dead_zone = (float)atof(v);
                else if (act && !strcmp(k, "sensitivity")) act->sensitivity = (float)atof(v);
                else if (act && !strcmp(k, "bind")) {
                    LxBinding b;
                    if (parse_binding(m, v, &b)) lx_input_add_binding(act, b);
                    else LX_WARN("Input map: could not parse binding '%s' on %s", v, act->name);
                }
            }
        }
    }
    lx_free(text);
    if (m == &IN.map) IN.map_ready = true;
    memset(IN.state, 0, sizeof IN.state);
    return true;
}

/* ======================================================================
 *  gamepad backends
 * ====================================================================== */
#if LX_OS_WINDOWS
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGamepad;
typedef struct { DWORD packet; XGamepad pad; } XState;
typedef struct { WORD left, right; } XVibration;
typedef DWORD(WINAPI *XGetState)(DWORD, XState *);
typedef DWORD(WINAPI *XSetState)(DWORD, XVibration *);
static XGetState g_xget;
static XSetState g_xset;
static bool g_xinit;

static float stick(SHORT v) { return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f; }

static void poll_pads(void) {
    if (!g_xinit) {
        g_xinit = true;
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) {
            g_xget = (XGetState)(void *)GetProcAddress(m, "XInputGetState");
            g_xset = (XSetState)(void *)GetProcAddress(m, "XInputSetState");
        }
    }
    if (!g_xget) return;
    /* polling disconnected slots is slow on some systems: only re-check them every second */
    bool rescan = lx_time() >= IN.next_scan;
    if (rescan) IN.next_scan = lx_time() + 1.0;
    for (int i = 0; i < LX_MAX_PADS; i++) {
        LxPadState *p = &g_lx_pads[i];
        if (!p->connected && !rescan) continue;
        XState st;
        memset(&st, 0, sizeof st);
        if (g_xget((DWORD)i, &st) != 0) { p->connected = false; continue; }
        if (!p->connected) { snprintf(p->name, sizeof p->name, "XInput Controller %d", i + 1); LX_INFO("Gamepad connected: %s", p->name); }
        p->connected = true;
        static const WORD masks[LX_PAD_BUTTON_COUNT] = {0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0020, 0x0010,
                                                        0x0040, 0x0080, 0x0001, 0x0002, 0x0004, 0x0008, 0x0400};
        for (int b = 0; b < LX_PAD_BUTTON_COUNT; b++) p->buttons[b] = (st.pad.buttons & masks[b]) != 0;
        p->axes[LX_PAD_LEFT_X] = stick(st.pad.lx);
        p->axes[LX_PAD_LEFT_Y] = stick(st.pad.ly);
        p->axes[LX_PAD_RIGHT_X] = stick(st.pad.rx);
        p->axes[LX_PAD_RIGHT_Y] = stick(st.pad.ry);
        p->axes[LX_PAD_LT] = st.pad.lt / 255.0f;
        p->axes[LX_PAD_RT] = st.pad.rt / 255.0f;
    }
}
static void set_rumble(int pad, float low, float high) {
    if (!g_xset) return;
    XVibration v = {(WORD)(lx_saturate(low) * 65535), (WORD)(lx_saturate(high) * 65535)};
    g_xset((DWORD)pad, &v);
}
#elif LX_OS_LINUX
/* Linux joystick API (struct js_event from <linux/joystick.h>, declared here to avoid the dependency) */
struct lx_js_event { uint32_t time; int16_t value; uint8_t type; uint8_t number; };
#define LX_JS_EVENT_BUTTON 0x01
#define LX_JS_EVENT_AXIS 0x02
#define LX_JS_EVENT_INIT 0x80
#define LX_JSIOCGNAME(len) (((2u) << 30) | ((unsigned)(len) << 16) | ((unsigned)'j' << 8) | 0x13u)

static void poll_pads(void) {
    if (!IN.js_init) {
        for (int i = 0; i < LX_MAX_PADS; i++) IN.js_fd[i] = -1;
        IN.js_init = true;
    }
    if (lx_time() >= IN.next_scan) {
        IN.next_scan = lx_time() + 2.0;
        for (int i = 0; i < LX_MAX_PADS; i++) {
            if (IN.js_fd[i] >= 0) continue;
            char path[32];
            snprintf(path, sizeof path, "/dev/input/js%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;
            IN.js_fd[i] = fd;
            LxPadState *p = &g_lx_pads[i];
            memset(p, 0, sizeof *p);
            if (ioctl(fd, LX_JSIOCGNAME(sizeof p->name), p->name) < 0) snprintf(p->name, sizeof p->name, "Joystick %d", i + 1);
            p->connected = true;
            LX_INFO("Gamepad connected: %s", p->name);
        }
    }
    for (int i = 0; i < LX_MAX_PADS; i++) {
        int fd = IN.js_fd[i];
        if (fd < 0) continue;
        LxPadState *p = &g_lx_pads[i];
        struct lx_js_event ev;
        ssize_t got;
        while ((got = read(fd, &ev, sizeof ev)) == (ssize_t)sizeof ev) {
            int type = ev.type & ~LX_JS_EVENT_INIT;
            if (type == LX_JS_EVENT_BUTTON) {
                /* xpad / Xbox-style layout used by most controllers through the kernel */
                static const int map[] = {LX_PAD_A, LX_PAD_B, LX_PAD_X, LX_PAD_Y, LX_PAD_LB, LX_PAD_RB, LX_PAD_BACK, LX_PAD_START,
                                          LX_PAD_GUIDE, LX_PAD_LSTICK, LX_PAD_RSTICK};
                if (ev.number < sizeof map / sizeof map[0]) p->buttons[map[ev.number]] = ev.value != 0;
            } else if (type == LX_JS_EVENT_AXIS) {
                float v = ev.value < 0 ? ev.value / 32768.0f : ev.value / 32767.0f;
                switch (ev.number) {
                case 0: p->axes[LX_PAD_LEFT_X] = v; break;
                case 1: p->axes[LX_PAD_LEFT_Y] = -v; break;
                case 2: p->axes[LX_PAD_LT] = (v + 1.0f) * 0.5f; break;
                case 3: p->axes[LX_PAD_RIGHT_X] = v; break;
                case 4: p->axes[LX_PAD_RIGHT_Y] = -v; break;
                case 5: p->axes[LX_PAD_RT] = (v + 1.0f) * 0.5f; break;
                case 6: p->buttons[LX_PAD_LEFT] = v < -0.5f; p->buttons[LX_PAD_RIGHT] = v > 0.5f; break;
                case 7: p->buttons[LX_PAD_UP] = v < -0.5f; p->buttons[LX_PAD_DOWN] = v > 0.5f; break;
                default: break;
                }
            }
        }
        if (got < 0 && errno != EAGAIN) {
            close(fd);
            IN.js_fd[i] = -1;
            LX_INFO("Gamepad disconnected: %s", p->name);
            memset(p, 0, sizeof *p);
        }
    }
}
static void set_rumble(int pad, float low, float high) { LX_UNUSED(pad); LX_UNUSED(low); LX_UNUSED(high); }
#else
static void poll_pads(void) {} /* Android: filled by the platform layer from input events */
static void set_rumble(int pad, float low, float high) { LX_UNUSED(pad); LX_UNUSED(low); LX_UNUSED(high); }
#endif

int lx_gamepad_count(void) {
    int n = 0;
    for (int i = 0; i < LX_MAX_PADS; i++) n += g_lx_pads[i].connected;
    return n;
}
bool lx_gamepad_connected(int pad) { return pad >= 0 && pad < LX_MAX_PADS && g_lx_pads[pad].connected; }
const char *lx_gamepad_name(int pad) { return lx_gamepad_connected(pad) ? g_lx_pads[pad].name : ""; }
bool lx_gamepad_button(int pad, LxGamepadButton b) { return lx_gamepad_connected(pad) && b >= 0 && b < LX_PAD_BUTTON_COUNT && g_lx_pads[pad].buttons[b]; }
float lx_gamepad_axis(int pad, LxGamepadAxis a) { return (lx_gamepad_connected(pad) && a >= 0 && a < LX_PAD_AXIS_COUNT) ? g_lx_pads[pad].axes[a] : 0.0f; }
void lx_gamepad_rumble(int pad, float low, float high, float seconds) {
    if (!lx_gamepad_connected(pad)) return;
    set_rumble(pad, low, high);
    IN.rumble_time[pad] = seconds;
}

/* ======================================================================
 *  touch controls
 * ====================================================================== */
bool lx_input_touch_controls_visible(void) {
    LxInputMap *m = lx_input_map();
    if (m->touch_controls_mode == 1) return true;
    if (m->touch_controls_mode == 2) return false;
#if LX_OS_ANDROID
    return true;
#else
    return IN.last_device == LX_DEVICE_TOUCH;
#endif
}

static void update_touch(void) {
    LxInputMap *m = lx_input_map();
    int w = g_lx_input.width, h = g_lx_input.height;
    float shortest = (float)(w < h ? w : h);
    for (int c = 0; c < m->touch_count; c++) {
        TouchState *ts = &IN.touch[c];
        const LxTouchControl *tc = &m->touch[c];
        ts->pressed = false;
        ts->stick = lx_v2(0, 0);
        ts->look_delta = lx_v2(0, 0);
        bool any = false;
        for (int t = 0; t < g_lx_input.touch_count; t++) {
            LxVec2 p = g_lx_input.touches[t];
            if (tc->type == LX_TOUCH_LOOK_AREA) {
                float x0 = tc->x * (float)w, y0 = tc->y * (float)h, x1 = x0 + tc->size * (float)w, y1 = y0 + tc->height * (float)h;
                if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) continue;
                /* skip touches that belong to another control */
                bool owned = false;
                for (int o = 0; o < m->touch_count && !owned; o++) {
                    if (o == c || m->touch[o].type == LX_TOUCH_LOOK_AREA) continue;
                    float r = m->touch[o].size * shortest * (m->touch[o].type == LX_TOUCH_STICK ? 1.6f : 1.3f);
                    float dx = p.x - m->touch[o].x * (float)w, dy = p.y - m->touch[o].y * (float)h;
                    owned = dx * dx + dy * dy < r * r;
                }
                if (owned) continue;
                if (ts->tracking) ts->look_delta = lx_v2(p.x - ts->last_pos.x, -(p.y - ts->last_pos.y));
                ts->last_pos = p;
                any = true;
                break;
            }
            float cx = tc->x * (float)w, cy = tc->y * (float)h, r = tc->size * shortest;
            float dx = p.x - cx, dy = p.y - cy;
            float zone = tc->type == LX_TOUCH_STICK ? r * 1.6f : r * 1.3f;
            if (dx * dx + dy * dy > zone * zone) continue;
            any = true;
            if (tc->type == LX_TOUCH_BUTTON) ts->pressed = true;
            else {
                LxVec2 v = lx_v2(dx / r, -dy / r);
                float l = sqrtf(v.x * v.x + v.y * v.y);
                if (l > 1) { v.x /= l; v.y /= l; }
                ts->stick = v;
            }
            break;
        }
        ts->tracking = any && tc->type == LX_TOUCH_LOOK_AREA;
        if (tc->type == LX_TOUCH_LOOK_AREA) ts->stick = ts->look_delta;
    }
}

void lx_input_draw_touch_controls(void) {
    if (!lx_input_touch_controls_visible()) return;
    LxInputMap *m = lx_input_map();
    int w = g_lx_input.width, h = g_lx_input.height;
    float shortest = (float)(w < h ? w : h);
    LxUiVertex *v = NULL;
    uint32_t *idx = NULL;
    int vc = 0, vcap = 0, ic = 0, icap = 0;
#define CIRCLE(cx, cy, r, col)                                                                                  \
    for (int s = 0; s < 32; s++) {                                                                              \
        float a0 = (float)s / 32 * 2 * LX_PI, a1 = (float)(s + 1) / 32 * 2 * LX_PI;                              \
        LxUiVertex q0 = {(cx), (cy), 0.5f, 0.5f, (col)}, q1 = {(cx) + cosf(a0) * (r), (cy) + sinf(a0) * (r), 0.5f, 0.5f, (col)}, \
                   q2 = {(cx) + cosf(a1) * (r), (cy) + sinf(a1) * (r), 0.5f, 0.5f, (col)};                      \
        uint32_t b = (uint32_t)vc;                                                                              \
        LX_VEC_PUSH(v, vc, vcap, q0); LX_VEC_PUSH(v, vc, vcap, q1); LX_VEC_PUSH(v, vc, vcap, q2);                 \
        LX_VEC_PUSH(idx, ic, icap, b); LX_VEC_PUSH(idx, ic, icap, b + 1); LX_VEC_PUSH(idx, ic, icap, b + 2);       \
    }
    for (int c = 0; c < m->touch_count; c++) {
        const LxTouchControl *tc = &m->touch[c];
        if (tc->type == LX_TOUCH_LOOK_AREA) continue;
        float cx = tc->x * (float)w, cy = tc->y * (float)h, r = tc->size * shortest;
        const TouchState *ts = &IN.touch[c];
        uint32_t base = ts->pressed || ts->stick.x != 0 || ts->stick.y != 0 ? 0x70ffffffu : 0x38ffffffu;
        CIRCLE(cx, cy, r, base);
        if (tc->type == LX_TOUCH_STICK) {
            float kx = cx + ts->stick.x * r * 0.6f, ky = cy - ts->stick.y * r * 0.6f;
            CIRCLE(kx, ky, r * 0.45f, 0x90ffffffu);
        }
    }
#undef CIRCLE
    LxUiCmd cmd = {lx_ui_white_texture(), 0, (uint32_t)ic, {0, 0, (float)w, (float)h}};
    LxUiDrawList dl = {v, vc, idx, ic, &cmd, 1, lx_v4(0, 0, 0, 1)};
    lx_renderer_draw_ui(&dl);
    lx_free(v);
    lx_free(idx);
}

/* ======================================================================
 *  evaluation
 * ====================================================================== */
static float read_binding(const LxInputMap *m, const LxBinding *b, bool *digital) {
    *digital = false;
    float v = 0;
    bool touching = g_lx_input.touch_count > 0;
    switch (b->type) {
    case LX_BIND_KEY: v = lx_key_down((LxKey)b->code) ? 1.0f : 0.0f; *digital = true; break;
    case LX_BIND_KEY_AXIS: v = (lx_key_down((LxKey)b->code) ? 1.0f : 0.0f) - (lx_key_down((LxKey)b->code2) ? 1.0f : 0.0f); *digital = true; break;
    case LX_BIND_MOUSE_BUTTON: if (!touching) { v = lx_mouse_down((LxMouseButton)b->code) ? 1.0f : 0.0f; *digital = true; } break;
    case LX_BIND_MOUSE_AXIS:
        if (!touching) {
            LxVec2 d = lx_mouse_delta();
            v = b->code == LX_MOUSE_AXIS_X ? d.x : b->code == LX_MOUSE_AXIS_Y ? d.y : lx_mouse_wheel();
        }
        break;
    case LX_BIND_PAD_BUTTON:
        for (int p = 0; p < LX_MAX_PADS; p++) if (g_lx_pads[p].connected && g_lx_pads[p].buttons[b->code % LX_PAD_BUTTON_COUNT]) v = 1.0f;
        *digital = true;
        break;
    case LX_BIND_PAD_AXIS:
        for (int p = 0; p < LX_MAX_PADS; p++) {
            float a = g_lx_pads[p].connected ? g_lx_pads[p].axes[b->code % LX_PAD_AXIS_COUNT] : 0.0f;
            if (fabsf(a) > fabsf(v)) v = a;
        }
        break;
    case LX_BIND_TOUCH_BUTTON: if (b->code >= 0 && b->code < m->touch_count) { v = IN.touch[b->code].pressed ? 1.0f : 0.0f; *digital = true; } break;
    case LX_BIND_TOUCH_STICK_X: if (b->code >= 0 && b->code < m->touch_count) v = IN.touch[b->code].stick.x; break;
    case LX_BIND_TOUCH_STICK_Y: if (b->code >= 0 && b->code < m->touch_count) v = IN.touch[b->code].stick.y; break;
    default: break;
    }
    return v * b->scale;
}

static void note_device(void) {
    if (g_lx_input.touch_count > 0) { IN.last_device = LX_DEVICE_TOUCH; return; }
    for (int p = 0; p < LX_MAX_PADS; p++) {
        if (!g_lx_pads[p].connected) continue;
        for (int b = 0; b < LX_PAD_BUTTON_COUNT; b++) if (g_lx_pads[p].buttons[b]) { IN.last_device = LX_DEVICE_GAMEPAD; return; }
        for (int a = 0; a < LX_PAD_AXIS_COUNT; a++) if (fabsf(g_lx_pads[p].axes[a]) > 0.4f) { IN.last_device = LX_DEVICE_GAMEPAD; return; }
    }
    LxVec2 d = lx_mouse_delta();
    bool key = false;
    for (int k = 1; k < LX_KEY_COUNT && !key; k++) key = g_lx_input.keys[k];
    if (key || fabsf(d.x) + fabsf(d.y) > 2 || g_lx_input.mouse[0] || g_lx_input.mouse[1]) IN.last_device = LX_DEVICE_KEYBOARD_MOUSE;
}

void lx_input_update(float dt) {
    LxInputMap *m = lx_input_map();
    for (int p = 0; p < LX_MAX_PADS; p++) memcpy(g_lx_pads[p].prev_buttons, g_lx_pads[p].buttons, sizeof g_lx_pads[p].buttons);
    poll_pads();
    for (int p = 0; p < LX_MAX_PADS; p++)
        if (IN.rumble_time[p] > 0 && (IN.rumble_time[p] -= dt) <= 0) set_rumble(p, 0, 0);
    update_touch();
    note_device();
    for (int i = 0; i < m->action_count; i++) {
        const LxInputAction *a = &m->actions[i];
        ActionState *st = &IN.state[i];
        st->prev = st->down;
        float best = 0;
        bool down = false;
        for (int k = 0; k < a->binding_count; k++) {
            bool digital;
            float v = read_binding(m, &a->bindings[k], &digital);
            if (fabsf(v) > fabsf(best)) best = v;
            if (digital ? v != 0 : fabsf(v) >= m->trigger_threshold) down = true;
        }
        if (a->is_axis) {
            float dz = a->dead_zone;
            float mag = fabsf(best);
            /* rescale outside the dead zone so small movements still start from 0 */
            if (mag < dz) best = 0;
            else if (mag <= 1.0f) best = (best > 0 ? 1.0f : -1.0f) * (mag - dz) / (1.0f - dz);
            best *= a->sensitivity == 0 ? 1.0f : a->sensitivity;
            st->down = fabsf(best) >= m->trigger_threshold;
        } else st->down = down;
        st->value = best;
    }
}

static int action_index(const char *name) {
    LxInputMap *m = lx_input_map();
    for (int i = 0; i < m->action_count; i++) if (!strcmp(m->actions[i].name, name)) return i;
    static char warned[16][32];
    static int wn;
    for (int i = 0; i < wn; i++) if (!strcmp(warned[i], name)) return -1;
    if (wn < 16) { lx_strcpy(warned[wn++], 32, name); LX_WARN("Input action '%s' is not defined in the Input Manager", name); }
    return -1;
}

bool lx_action_down(const char *n) { int i = action_index(n); return i >= 0 && IN.state[i].down; }
bool lx_action_pressed(const char *n) { int i = action_index(n); return i >= 0 && IN.state[i].down && !IN.state[i].prev; }
bool lx_action_released(const char *n) { int i = action_index(n); return i >= 0 && !IN.state[i].down && IN.state[i].prev; }
float lx_axis(const char *n) {
    int i = action_index(n);
    if (i < 0) return 0.0f;
    LxInputAction *a = &lx_input_map()->actions[i];
    return a->is_axis ? IN.state[i].value : (IN.state[i].down ? 1.0f : 0.0f);
}
LxVec2 lx_axis2(const char *x, const char *y) {
    LxVec2 v = lx_v2(lx_axis(x), lx_axis(y));
    float l = sqrtf(v.x * v.x + v.y * v.y);
    if (l > 1.0f) { v.x /= l; v.y /= l; } /* keep diagonals from being faster */
    return v;
}
LxInputDevice lx_input_last_device(void) { return IN.last_device; }

bool lx_input_capture(LxBinding *out, bool want_axis) {
    memset(out, 0, sizeof *out);
    out->scale = 1.0f;
    for (int k = 1; k < LX_KEY_COUNT; k++)
        if (lx_key_pressed((LxKey)k) && k != LX_KEY_ESCAPE) {
            out->type = want_axis ? LX_BIND_KEY_AXIS : LX_BIND_KEY;
            out->code = k;
            return true;
        }
    for (int b = 0; b < LX_MOUSE_COUNT; b++)
        if (lx_mouse_pressed((LxMouseButton)b) && !want_axis) { out->type = LX_BIND_MOUSE_BUTTON; out->code = b; return true; }
    for (int p = 0; p < LX_MAX_PADS; p++) {
        if (!g_lx_pads[p].connected) continue;
        for (int b = 0; b < LX_PAD_BUTTON_COUNT; b++)
            if (g_lx_pads[p].buttons[b] && !g_lx_pads[p].prev_buttons[b]) { out->type = LX_BIND_PAD_BUTTON; out->code = b; return true; }
        for (int a = 0; a < LX_PAD_AXIS_COUNT; a++)
            if (fabsf(g_lx_pads[p].axes[a]) > 0.6f) { out->type = LX_BIND_PAD_AXIS; out->code = a; return true; }
    }
    if (want_axis) {
        LxVec2 d = lx_mouse_delta();
        if (fabsf(d.x) > 25) { out->type = LX_BIND_MOUSE_AXIS; out->code = LX_MOUSE_AXIS_X; out->scale = 0.08f; return true; }
        if (fabsf(d.y) > 25) { out->type = LX_BIND_MOUSE_AXIS; out->code = LX_MOUSE_AXIS_Y; out->scale = -0.08f; return true; }
    }
    return false;
}
