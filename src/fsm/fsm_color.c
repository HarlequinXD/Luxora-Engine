/* Colour: the Color, Color32 and Color Utility categories.
 *
 * A Luxora Color variable holds four floats, 0..1 (red, green, blue, alpha). Color32's 0..255 bytes are
 * the same colours written differently, so both sit in one set of actions with a 0..255 switch where it
 * matters. Vector4 maths has no separate variable type in Luxora — these actions cover it, since a
 * Color is exactly a four-component value. */
#include "fsm_actions.h"

static float fsm_c_comp(LxVec4 c, int i) { return i == 0 ? c.x : (i == 1 ? c.y : (i == 2 ? c.z : c.w)); }
static LxVec4 fsm_c_with(LxVec4 c, int i, float v) {
    switch (i) { case 0: c.x = v; break; case 1: c.y = v; break; case 2: c.z = v; break; default: c.w = v; break; }
    return c;
}
static LxVec4 fsm_c_sat(LxVec4 c) {
    return lx_v4(lx_saturate(c.x), lx_saturate(c.y), lx_saturate(c.z), lx_saturate(c.w));
}
static void fsm_rgb_to_hsv(LxVec4 c, float *h, float *s, float *v) {
    float r = c.x, g = c.y, b = c.z;
    float mx = lx_maxf(r, lx_maxf(g, b)), mn = lx_minf(r, lx_minf(g, b));
    float d = mx - mn;
    *v = mx;
    *s = mx > 1e-6f ? d / mx : 0;
    if (d < 1e-6f) { *h = 0; return; }
    float hh;
    if (mx == r) hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else hh = (r - g) / d + 4.0f;
    *h = hh / 6.0f;
}
static LxVec4 fsm_hsv_to_rgb(float h, float s, float v, float a) {
    h = h - floorf(h);
    s = lx_saturate(s);
    v = lx_saturate(v);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch ((int)i % 6) {
    case 0: return lx_v4(v, t, p, a);
    case 1: return lx_v4(q, v, p, a);
    case 2: return lx_v4(p, v, t, a);
    case 3: return lx_v4(p, q, v, a);
    case 4: return lx_v4(t, p, v, a);
    default: return lx_v4(v, p, q, a);
    }
}
static int fsm_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* ==========================================================================================
 *  making and reading colours
 * ========================================================================================== */
static void c_set(LxFsmCtx *c) { lx_fsm_set_color(c, 1, lx_fsm_color(c, 0)); }
static const LxFsmActionDesc A_C_SET = {"Color Set Value", "Color", "Stores a colour in a Color variable.", "{1} = {0}",
    {P("Value", LX_FSM_COLOR, "1 1 1 1"), POUT("Store Result", LX_FSM_COLOR)}, 0, 0, LX_FSM_ONCE, c_set, c_set, NULL};

static void c_from_rgba(LxFsmCtx *c) {
    float scale = lx_fsm_bool(c, 4) ? 1.0f / 255.0f : 1.0f;
    LxVec4 col = lx_v4(lx_fsm_float(c, 0) * scale, lx_fsm_float(c, 1) * scale,
                       lx_fsm_float(c, 2) * scale, lx_fsm_float(c, 3) * scale);
    lx_fsm_set_color(c, 5, fsm_c_sat(col));
}
static const LxFsmActionDesc A_C_FROM_RGBA = {"Color From RGBA", "Color", "Builds a colour from red, green, blue and alpha, either as 0..1 or as 0..255.",
    "rgba({0}, {1}, {2}, {3}) -> {5}",
    {P("Red", LX_FSM_FLOAT, "1"), P("Green", LX_FSM_FLOAT, "1"), P("Blue", LX_FSM_FLOAT, "1"), P("Alpha", LX_FSM_FLOAT, "1"),
     PT("Use 0-255", LX_FSM_BOOL, "false", "the values are bytes, as Color32 uses"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_from_rgba, c_from_rgba, NULL};

static void c_get_rgba(LxFsmCtx *c) {
    LxVec4 col = lx_fsm_color(c, 0);
    float scale = lx_fsm_bool(c, 1) ? 255.0f : 1.0f;
    lx_fsm_set_float(c, 2, col.x * scale);
    lx_fsm_set_float(c, 3, col.y * scale);
    lx_fsm_set_float(c, 4, col.z * scale);
    lx_fsm_set_float(c, 5, col.w * scale);
}
static const LxFsmActionDesc A_C_GET_RGBA = {"Color Get RGBA", "Color", "Reads the red, green, blue and alpha out of a colour.", "{0} -> r, g, b, a",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), PT("Use 0-255", LX_FSM_BOOL, "false", "report the values as bytes"),
     POPT("Store Red", LX_FSM_FLOAT), POPT("Store Green", LX_FSM_FLOAT), POPT("Store Blue", LX_FSM_FLOAT), POPT("Store Alpha", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, c_get_rgba, c_get_rgba, NULL};

static void c_get_channel(LxFsmCtx *c) {
    LxVec4 col = lx_fsm_color(c, 0);
    float scale = lx_fsm_bool(c, 2) ? 255.0f : 1.0f;
    lx_fsm_set_float(c, 3, fsm_c_comp(col, lx_fsm_int(c, 1)) * scale);
}
static const LxFsmActionDesc A_C_GET_CHANNEL = {"Color Get Channel", "Color", "Reads one channel of a colour.", "{0}.{1} -> {3}",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), PENUM("Channel", "Red|Green|Blue|Alpha", "0"), P("Use 0-255", LX_FSM_BOOL, "false"),
     POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, c_get_channel, c_get_channel, NULL};

static void c_set_channel(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) v /= 255.0f;
    lx_fsm_set_color(c, 0, fsm_c_with(lx_fsm_color(c, 0), lx_fsm_int(c, 1), lx_saturate(v)));
}
static const LxFsmActionDesc A_C_SET_CHANNEL = {"Color Set Channel", "Color", "Changes one channel of a Color variable: fading something out by its alpha, for instance.",
    "{0}.{1} = {2}",
    {POUT("Color Variable", LX_FSM_COLOR), PENUM("Channel", "Red|Green|Blue|Alpha", "3"), P("Value", LX_FSM_FLOAT, "1"),
     P("Use 0-255", LX_FSM_BOOL, "false")},
    0, 0, LX_FSM_ONCE, c_set_channel, c_set_channel, NULL};

/* ==========================================================================================
 *  arithmetic and blending
 * ========================================================================================== */
static void c_operator(LxFsmCtx *c) {
    LxVec4 a = lx_fsm_color(c, 0), b = lx_fsm_color(c, 1), r;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = lx_v4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); break;
    case 1: r = lx_v4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); break;
    case 2: r = lx_v4(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w); break;
    case 3: r = lx_v4(lx_minf(a.x, b.x), lx_minf(a.y, b.y), lx_minf(a.z, b.z), lx_minf(a.w, b.w)); break;
    case 4: r = lx_v4(lx_maxf(a.x, b.x), lx_maxf(a.y, b.y), lx_maxf(a.z, b.z), lx_maxf(a.w, b.w)); break;
    default: /* screen */ r = lx_v4(1 - (1 - a.x) * (1 - b.x), 1 - (1 - a.y) * (1 - b.y), 1 - (1 - a.z) * (1 - b.z), lx_maxf(a.w, b.w)); break;
    }
    lx_fsm_set_color(c, 3, lx_fsm_bool(c, 4) ? fsm_c_sat(r) : r);
}
static const LxFsmActionDesc A_C_OPERATOR = {"Color Operator", "Color", "Adds, subtracts, multiplies, screens or takes the darker/lighter of two colours.",
    "{0} {2} {1} -> {3}",
    {P("Color 1", LX_FSM_COLOR, "1 1 1 1"), P("Color 2", LX_FSM_COLOR, "1 1 1 1"),
     PENUM("Operation", "Add|Subtract|Multiply|Min|Max|Screen", "0"), POUT("Store Result", LX_FSM_COLOR),
     PT("Clamp", LX_FSM_BOOL, "true", "keep every channel within 0..1")},
    0, 0, LX_FSM_ONCE, c_operator, c_operator, NULL};

static void c_scale(LxFsmCtx *c) {
    LxVec4 a = lx_fsm_color(c, 0);
    float k = lx_fsm_float(c, 1);
    LxVec4 r = lx_v4(a.x * k, a.y * k, a.z * k, lx_fsm_bool(c, 2) ? a.w * k : a.w);
    lx_fsm_set_color(c, 3, r);
}
static const LxFsmActionDesc A_C_SCALE = {"Color Scale", "Color", "Brightens or darkens a colour by multiplying it.", "{0} * {1} -> {3}",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), P("Multiply By", LX_FSM_FLOAT, "1"),
     PT("Include Alpha", LX_FSM_BOOL, "false", "scale the transparency too"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_scale, c_scale, NULL};

static void c_lerp(LxFsmCtx *c) {
    LxVec4 a = lx_fsm_color(c, 0), b = lx_fsm_color(c, 1);
    float t = lx_fsm_float(c, 2);
    if (!lx_fsm_bool(c, 3)) t = lx_saturate(t);
    lx_fsm_set_color(c, 4, lx_v4(lx_lerpf(a.x, b.x, t), lx_lerpf(a.y, b.y, t), lx_lerpf(a.z, b.z, t), lx_lerpf(a.w, b.w, t)));
}
static const LxFsmActionDesc A_C_LERP = {"Color Lerp", "Color", "Blends between two colours: health bars going green to red, damage flashes.",
    "lerp {0}..{1} at {2} -> {4}",
    {P("From", LX_FSM_COLOR, "1 1 1 1"), P("To", LX_FSM_COLOR, "0 0 0 1"), P("Amount", LX_FSM_FLOAT, "0.5"),
     P("Unclamped", LX_FSM_BOOL, "false"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_lerp, c_lerp, NULL};

static void c_move_towards(LxFsmCtx *c) {
    LxVec4 a = lx_fsm_color(c, 0), b = lx_fsm_color(c, 1);
    float step = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) step *= c->dt;
    LxVec4 r;
    float *pa = &a.x, *pb = &b.x, *pr = &r.x;
    for (int i = 0; i < 4; i++) {
        float d = pb[i] - pa[i];
        pr[i] = fabsf(d) <= step ? pb[i] : pa[i] + (d > 0 ? step : -step);
    }
    lx_fsm_set_color(c, 4, r);
}
static const LxFsmActionDesc A_C_MOVE_TOWARDS = {"Color Move Towards", "Color", "Steps a colour towards a target at a fixed rate.", "{0} -> {1} by {2}",
    {P("Current", LX_FSM_COLOR, "1 1 1 1"), P("Target", LX_FSM_COLOR, "0 0 0 1"), P("Max Delta", LX_FSM_FLOAT, "1"),
     P("Per Second", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_EVERY_FRAME, c_move_towards, c_move_towards, NULL};

static void c_select(LxFsmCtx *c) { lx_fsm_set_color(c, 3, lx_fsm_bool(c, 0) ? lx_fsm_color(c, 1) : lx_fsm_color(c, 2)); }
static const LxFsmActionDesc A_C_SELECT = {"Color Select Value", "Color", "Picks one of two colours depending on a bool.", "{3} = {0} ? {1} : {2}",
    {P("Condition", LX_FSM_BOOL, "true"), P("If True", LX_FSM_COLOR, "1 1 1 1"), P("If False", LX_FSM_COLOR, "0 0 0 1"),
     POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_select, c_select, NULL};

static void c_equals(LxFsmCtx *c) {
    LxVec4 a = lx_fsm_color(c, 0), b = lx_fsm_color(c, 1);
    float tol = lx_fsm_float(c, 2);
    bool eq = fabsf(a.x - b.x) <= tol && fabsf(a.y - b.y) <= tol && fabsf(a.z - b.z) <= tol && fabsf(a.w - b.w) <= tol;
    lx_fsm_set_bool(c, 3, eq);
    fsm_send(c, eq ? 4 : 5);
}
static const LxFsmActionDesc A_C_EQUALS = {"Color Equals", "Color", "Tests whether two colours are the same, within a tolerance.", "{0} == {1}?",
    {P("Color 1", LX_FSM_COLOR, "1 1 1 1"), P("Color 2", LX_FSM_COLOR, "1 1 1 1"), P("Tolerance", LX_FSM_FLOAT, "0.01"),
     POPT("Store Result", LX_FSM_BOOL), PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, c_equals, c_equals, NULL};

/* ==========================================================================================
 *  HSV, grayscale, gamma, hex
 * ========================================================================================== */
static void c_to_hsv(LxFsmCtx *c) {
    float h, s, v;
    LxVec4 col = lx_fsm_color(c, 0);
    fsm_rgb_to_hsv(col, &h, &s, &v);
    lx_fsm_set_float(c, 1, h);
    lx_fsm_set_float(c, 2, s);
    lx_fsm_set_float(c, 3, v);
    lx_fsm_set_float(c, 4, col.w);
}
static const LxFsmActionDesc A_C_TO_HSV = {"Color RGB To HSV", "Color", "Breaks a colour into hue, saturation and value, each 0..1.", "{0} -> hsv",
    {P("Color", LX_FSM_COLOR, "1 0 0 1"), POPT("Store Hue", LX_FSM_FLOAT), POPT("Store Saturation", LX_FSM_FLOAT),
     POPT("Store Value", LX_FSM_FLOAT), POPT("Store Alpha", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, c_to_hsv, c_to_hsv, NULL};

static void c_from_hsv(LxFsmCtx *c) {
    lx_fsm_set_color(c, 4, fsm_hsv_to_rgb(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3)));
}
static const LxFsmActionDesc A_C_FROM_HSV = {"Color HSV To RGB", "Color", "Builds a colour from hue, saturation and value: rainbow cycling, team colours.",
    "hsv({0}, {1}, {2}) -> {4}",
    {PT("Hue", LX_FSM_FLOAT, "0", "0..1 around the colour wheel; it wraps"), P("Saturation", LX_FSM_FLOAT, "1"),
     P("Value", LX_FSM_FLOAT, "1"), P("Alpha", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_from_hsv, c_from_hsv, NULL};

static void c_shift_hue(LxFsmCtx *c) {
    float h, s, v;
    LxVec4 col = lx_fsm_color(c, 0);
    fsm_rgb_to_hsv(col, &h, &s, &v);
    h += lx_fsm_float(c, 1);
    s = lx_saturate(s * lx_fsm_float(c, 2));
    v = lx_saturate(v * lx_fsm_float(c, 3));
    lx_fsm_set_color(c, 4, fsm_hsv_to_rgb(h, s, v, col.w));
}
static const LxFsmActionDesc A_C_SHIFT_HUE = {"Color Adjust HSV", "Color", "Shifts a colour's hue and scales its saturation and brightness.",
    "adjust {0} -> {4}",
    {P("Color", LX_FSM_COLOR, "1 0 0 1"), PT("Hue Shift", LX_FSM_FLOAT, "0", "added to the hue; 0.5 is the opposite colour"),
     P("Saturation Scale", LX_FSM_FLOAT, "1"), P("Value Scale", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_shift_hue, c_shift_hue, NULL};

static void c_grayscale(LxFsmCtx *c) {
    LxVec4 col = lx_fsm_color(c, 0);
    float g = col.x * 0.299f + col.y * 0.587f + col.z * 0.114f;
    lx_fsm_set_float(c, 1, g);
    lx_fsm_set_color(c, 2, lx_v4(g, g, g, col.w));
    lx_fsm_set_float(c, 3, lx_maxf(col.x, lx_maxf(col.y, col.z)));
}
static const LxFsmActionDesc A_C_GRAYSCALE = {"Color Grayscale", "Color", "How bright a colour is, and the colour with all the hue taken out.",
    "gray {0} -> {1}",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), POPT("Store Brightness", LX_FSM_FLOAT), POPT("Store Gray Color", LX_FSM_COLOR),
     POPT("Store Max Channel", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, c_grayscale, c_grayscale, NULL};

static void c_gamma(LxFsmCtx *c) {
    LxVec4 col = lx_fsm_color(c, 0);
    float p = lx_fsm_int(c, 1) == 0 ? 2.2f : 1.0f / 2.2f;
    lx_fsm_set_color(c, 2, lx_v4(powf(lx_saturate(col.x), p), powf(lx_saturate(col.y), p), powf(lx_saturate(col.z), p), col.w));
}
static const LxFsmActionDesc A_C_GAMMA = {"Color Gamma Convert", "Color", "Converts a colour between gamma (sRGB) and linear space.", "{0} {1} -> {2}",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), PENUM("Direction", "Gamma To Linear|Linear To Gamma", "0"), POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_gamma, c_gamma, NULL};

static void c_to_hex(LxFsmCtx *c) {
    LxVec4 col = fsm_c_sat(lx_fsm_color(c, 0));
    int r = (int)(col.x * 255.0f + 0.5f), g = (int)(col.y * 255.0f + 0.5f);
    int b = (int)(col.z * 255.0f + 0.5f), a = (int)(col.w * 255.0f + 0.5f);
    char buf[16];
    if (lx_fsm_bool(c, 1)) snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", r, g, b, a);
    else snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
    lx_fsm_set_string(c, 2, buf);
}
static const LxFsmActionDesc A_C_TO_HEX = {"Color To Hex String", "Color", "Writes a colour as \"#RRGGBB\" (or \"#RRGGBBAA\").", "{0} -> {2}",
    {P("Color", LX_FSM_COLOR, "1 1 1 1"), PT("Include Alpha", LX_FSM_BOOL, "false", "write the alpha too"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, c_to_hex, c_to_hex, NULL};

static void c_from_hex(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    if (!s) s = "";
    if (*s == '#') s++;
    int n = (int)strlen(s), d[8] = {0};
    bool ok = (n == 3 || n == 6 || n == 8);
    for (int i = 0; ok && i < n; i++) {
        d[i] = fsm_hex_digit(s[i]);
        if (d[i] < 0) ok = false;
    }
    LxVec4 col = lx_v4(1, 1, 1, 1);
    if (ok) {
        if (n == 3) col = lx_v4(d[0] * 17 / 255.0f, d[1] * 17 / 255.0f, d[2] * 17 / 255.0f, 1.0f);
        else {
            col.x = (float)(d[0] * 16 + d[1]) / 255.0f;
            col.y = (float)(d[2] * 16 + d[3]) / 255.0f;
            col.z = (float)(d[4] * 16 + d[5]) / 255.0f;
            col.w = n == 8 ? (float)(d[6] * 16 + d[7]) / 255.0f : 1.0f;
        }
    }
    lx_fsm_set_color(c, 1, col);
    lx_fsm_set_bool(c, 2, ok);
    fsm_send(c, ok ? 3 : 4);
}
static const LxFsmActionDesc A_C_FROM_HEX = {"Color From Hex String", "Color", "Reads a colour out of \"#RRGGBB\", \"#RGB\" or \"#RRGGBBAA\".", "{0} -> {1}",
    {P("Hex String", LX_FSM_STRING, "#FFFFFF"), POUT("Store Result", LX_FSM_COLOR), POPT("Store Success", LX_FSM_BOOL),
     PEV("Success Event"), PEV("Failure Event")},
    0, 0, LX_FSM_ONCE, c_from_hex, c_from_hex, NULL};

static void c_temperature(LxFsmCtx *c) {
    /* Approximation of a blackbody colour in kelvin (1000..40000), as Unity's ColorTemperature does. */
    float k = lx_clampf(lx_fsm_float(c, 0), 1000.0f, 40000.0f) / 100.0f;
    float r, g, b;
    if (k <= 66.0f) {
        r = 1.0f;
        g = lx_saturate((99.4708025861f * logf(k) - 161.1195681661f) / 255.0f);
        b = k <= 19.0f ? 0.0f : lx_saturate((138.5177312231f * logf(k - 10.0f) - 305.0447927307f) / 255.0f);
    } else {
        r = lx_saturate(329.698727446f * powf(k - 60.0f, -0.1332047592f) / 255.0f);
        g = lx_saturate(288.1221695283f * powf(k - 60.0f, -0.0755148492f) / 255.0f);
        b = 1.0f;
    }
    lx_fsm_set_color(c, 1, lx_v4(r, g, b, 1.0f));
}
static const LxFsmActionDesc A_C_TEMPERATURE = {"Color From Temperature", "Color", "The colour of light at a temperature in kelvin: 2700 warm indoor, 6500 daylight.",
    "{0}K -> {1}",
    {P("Kelvin", LX_FSM_FLOAT, "6500"), POUT("Store Result", LX_FSM_COLOR)}, 0, 0, LX_FSM_ONCE, c_temperature, c_temperature, NULL};

static void c_random(LxFsmCtx *c) {
    LxVec4 r;
    if (lx_fsm_bool(c, 0)) {
        r = fsm_hsv_to_rgb(lx_random(), lx_fsm_float(c, 1), lx_fsm_float(c, 2), lx_fsm_float(c, 3));
    } else {
        r = lx_v4(lx_random(), lx_random(), lx_random(), lx_fsm_float(c, 3));
    }
    lx_fsm_set_color(c, 4, r);
}
static const LxFsmActionDesc A_C_RANDOM = {"Color Random", "Color", "A random colour. Bright Colors Only keeps the saturation and brightness you set and varies only the hue.",
    "random color -> {4}",
    {PT("Bright Colors Only", LX_FSM_BOOL, "true", "vary the hue only, so the colour stays vivid"),
     P("Saturation", LX_FSM_FLOAT, "0.8"), P("Value", LX_FSM_FLOAT, "1"), P("Alpha", LX_FSM_FLOAT, "1"),
     POUT("Store Result", LX_FSM_COLOR)},
    0, 0, LX_FSM_ONCE, c_random, c_random, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_color(void) {
    static const LxFsmActionDesc *all[] = {
        &A_C_SET, &A_C_FROM_RGBA, &A_C_GET_RGBA, &A_C_GET_CHANNEL, &A_C_SET_CHANNEL,
        &A_C_OPERATOR, &A_C_SCALE, &A_C_LERP, &A_C_MOVE_TOWARDS, &A_C_SELECT, &A_C_EQUALS,
        &A_C_TO_HSV, &A_C_FROM_HSV, &A_C_SHIFT_HUE, &A_C_GRAYSCALE, &A_C_GAMMA,
        &A_C_TO_HEX, &A_C_FROM_HEX, &A_C_TEMPERATURE, &A_C_RANDOM,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
