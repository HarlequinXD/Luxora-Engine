/* Numbers: Float, Integer, Bool, Enum, Convert, Trigonometry, Exponents, Rounding, Range,
 * Math Constants, Math Utilities, Interpolation, Noise and Random.
 *
 * Everything here is pure arithmetic on variables: no scene, no engine state, no side effects. */
#include "fsm_actions.h"

/* ==========================================================================================
 *  shared bits
 * ========================================================================================== */
static float fsm_repeat(float t, float length) {
    if (length <= 0) return 0;
    return lx_clampf(t - floorf(t / length) * length, 0, length);
}
static float fsm_ping_pong(float t, float length) {
    if (length <= 0) return 0;
    t = fsm_repeat(t, length * 2);
    return length - fabsf(t - length);
}
static float fsm_delta_angle(float a, float b) {
    float d = fsm_repeat(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    return d;
}
static float fsm_lerp_angle(float a, float b, float t) {
    return a + fsm_delta_angle(a, b) * lx_saturate(t);
}
static float fsm_move_towards(float cur, float target, float max_delta) {
    if (fabsf(target - cur) <= max_delta) return target;
    return cur + (target > cur ? max_delta : -max_delta);
}
static float fsm_smooth_step(float from, float to, float t) {
    t = lx_saturate(t);
    t = t * t * (3.0f - 2.0f * t);
    return from + (to - from) * t;
}
/* Critically damped spring, as Unity's Mathf.SmoothDamp. `vel` is carried between frames. */
static float fsm_smooth_damp(float cur, float target, float *vel, float smooth_time, float max_speed, float dt) {
    smooth_time = lx_maxf(0.0001f, smooth_time);
    float omega = 2.0f / smooth_time;
    float x = omega * dt;
    float exp_ = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = cur - target;
    float max_change = max_speed > 0 ? max_speed * smooth_time : FLT_MAX;
    change = lx_clampf(change, -max_change, max_change);
    float t = cur - change;
    float temp = (*vel + omega * change) * dt;
    *vel = (*vel - omega * temp) * exp_;
    float out = t + (change + temp) * exp_;
    if ((target - cur > 0.0f) == (out > target)) { out = target; *vel = 0; }
    return out;
}

/* ==========================================================================================
 *  Float
 * ========================================================================================== */
static void f_set(LxFsmCtx *c) { lx_fsm_set_float(c, 1, lx_fsm_float(c, 0)); }
static const LxFsmActionDesc A_FLOAT_SET = {"Float Set Value", "Float", "Stores a number in a Float variable.", "{1} = {0}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, f_set, f_set, NULL};

static void f_add(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_fsm_float(c, 0) + lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_FLOAT_ADD = {"Float Add", "Float", "Adds a number to a Float variable.", "{0} += {1}",
    {POUTT("Float Variable", LX_FSM_FLOAT, "the variable that is changed"), P("Add", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, f_add, f_add, NULL};

static void f_add_clamped(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0) + lx_fsm_float(c, 1);
    lx_fsm_set_float(c, 0, lx_clampf(v, lx_fsm_float(c, 2), lx_fsm_float(c, 3)));
}
static const LxFsmActionDesc A_FLOAT_ADD_CLAMPED = {"Float Add Clamped", "Float", "Adds a number to a Float variable and keeps it between Min and Max.",
    "{0} += {1} ({2}..{3})",
    {POUT("Float Variable", LX_FSM_FLOAT), P("Add", LX_FSM_FLOAT, "1"), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, f_add_clamped, f_add_clamped, NULL};

static void f_add_wrap(LxFsmCtx *c) {
    float lo = lx_fsm_float(c, 2), hi = lx_fsm_float(c, 3);
    float v = lx_fsm_float(c, 0) + lx_fsm_float(c, 1);
    if (hi > lo) v = lo + fsm_repeat(v - lo, hi - lo);
    lx_fsm_set_float(c, 0, v);
}
static const LxFsmActionDesc A_FLOAT_ADD_WRAP = {"Float Add Wrap", "Float", "Adds a number to a Float variable; past Max it wraps round to Min.",
    "{0} += {1} wrap {2}..{3}",
    {POUT("Float Variable", LX_FSM_FLOAT), P("Add", LX_FSM_FLOAT, "1"), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "360")},
    0, 0, LX_FSM_ONCE, f_add_wrap, f_add_wrap, NULL};

static void f_add_multiple(LxFsmCtx *c) {
    float sum = 0;
    for (int i = 1; i <= 5; i++) if (!lx_fsm_is_none(c, i)) sum += lx_fsm_float(c, i);
    lx_fsm_set_float(c, 0, lx_fsm_float(c, 0) + sum);
}
static const LxFsmActionDesc A_FLOAT_ADD_MULTIPLE = {"Float Add Multiple", "Float", "Adds up to five numbers to a Float variable at once.", "{0} += {1}+{2}+{3}",
    {POUT("Float Variable", LX_FSM_FLOAT), PIN_OPT("Add 1", LX_FSM_FLOAT), PIN_OPT("Add 2", LX_FSM_FLOAT),
     PIN_OPT("Add 3", LX_FSM_FLOAT), PIN_OPT("Add 4", LX_FSM_FLOAT), PIN_OPT("Add 5", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_add_multiple, f_add_multiple, NULL};

static void f_subtract(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_fsm_float(c, 0) - lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_FLOAT_SUBTRACT = {"Float Subtract", "Float", "Subtracts a number from a Float variable.", "{0} -= {1}",
    {POUT("Float Variable", LX_FSM_FLOAT), P("Subtract", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, f_subtract, f_subtract, NULL};

static void f_multiply(LxFsmCtx *c) { lx_fsm_set_float(c, 0, lx_fsm_float(c, 0) * lx_fsm_float(c, 1)); }
static const LxFsmActionDesc A_FLOAT_MULTIPLY = {"Float Multiply", "Float", "Multiplies a Float variable by a number.", "{0} *= {1}",
    {POUT("Float Variable", LX_FSM_FLOAT), P("Multiply By", LX_FSM_FLOAT, "2")}, 0, 0, LX_FSM_ONCE, f_multiply, f_multiply, NULL};

static void f_divide(LxFsmCtx *c) {
    float d = lx_fsm_float(c, 1);
    if (d != 0) lx_fsm_set_float(c, 0, lx_fsm_float(c, 0) / d);
}
static const LxFsmActionDesc A_FLOAT_DIVIDE = {"Float Divide", "Float", "Divides a Float variable by a number (dividing by zero does nothing).", "{0} /= {1}",
    {POUT("Float Variable", LX_FSM_FLOAT), P("Divide By", LX_FSM_FLOAT, "2")}, 0, 0, LX_FSM_ONCE, f_divide, f_divide, NULL};

static void f_operator(LxFsmCtx *c) {
    float a = lx_fsm_float(c, 0), b = lx_fsm_float(c, 1), r = 0;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = a + b; break;
    case 1: r = a - b; break;
    case 2: r = a * b; break;
    case 3: r = b != 0 ? a / b : 0; break;
    case 4: r = lx_minf(a, b); break;
    case 5: r = lx_maxf(a, b); break;
    case 6: r = powf(a, b); break;
    case 7: r = b != 0 ? fmodf(a, b) : 0; break;
    default: r = a; break;
    }
    lx_fsm_set_float(c, 3, r);
}
static const LxFsmActionDesc A_FLOAT_OPERATOR = {"Float Operator", "Float", "Adds, subtracts, multiplies, divides, or takes the smaller, larger, power or remainder of two numbers.",
    "{0} {2} {1} -> {3}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"),
     PENUM("Operation", "Add|Subtract|Multiply|Divide|Min|Max|Pow|Mod", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_operator, f_operator, NULL};

static void f_equals(LxFsmCtx *c) {
    bool eq = fabsf(lx_fsm_float(c, 0) - lx_fsm_float(c, 1)) <= lx_fsm_float(c, 2);
    lx_fsm_set_bool(c, 3, eq);
    fsm_send(c, eq ? 4 : 5);
}
static const LxFsmActionDesc A_FLOAT_EQUALS = {"Float Equals", "Float", "Tests whether two numbers are the same, within a tolerance.", "{0} == {1}?",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"),
     PT("Tolerance", LX_FSM_FLOAT, "0.001", "how close counts as equal"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, f_equals, f_equals, NULL};

static void f_compare_to(LxFsmCtx *c) {
    float a = lx_fsm_float(c, 0), b = lx_fsm_float(c, 1);
    lx_fsm_set_int(c, 2, a < b ? -1 : (a > b ? 1 : 0));
}
static const LxFsmActionDesc A_FLOAT_COMPARE_TO = {"Float Compare To", "Float", "Stores -1, 0 or 1 depending on whether the first number is less than, equal to or greater than the second.",
    "compare {0} to {1} -> {2}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, f_compare_to, f_compare_to, NULL};

static void f_compare(LxFsmCtx *c) {
    float a = lx_fsm_float(c, 0), b = lx_fsm_float(c, 1), tol = lx_fsm_float(c, 2);
    if (fabsf(a - b) <= tol) fsm_send(c, 3);
    else if (a < b) fsm_send(c, 4);
    else fsm_send(c, 5);
}
static const LxFsmActionDesc A_FLOAT_COMPARE = {"Float Compare", "Float", "Compares two numbers and sends an event: equal, less than or greater than.",
    "if {0} vs {1}: = {3}, < {4}, > {5}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), P("Tolerance", LX_FSM_FLOAT, "0"),
     PEV("Equal Event"), PEV("Less Than Event"), PEV("Greater Than Event")},
    0, 0, LX_FSM_ONCE, f_compare, f_compare, NULL};

static void f_select(LxFsmCtx *c) { lx_fsm_set_float(c, 3, lx_fsm_bool(c, 0) ? lx_fsm_float(c, 1) : lx_fsm_float(c, 2)); }
static const LxFsmActionDesc A_FLOAT_SELECT = {"Float Select Value", "Float", "Picks one of two numbers depending on a bool.", "{3} = {0} ? {1} : {2}",
    {P("Condition", LX_FSM_BOOL, "true"), P("If True", LX_FSM_FLOAT, "1"), P("If False", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_select, f_select, NULL};

static void f_remap(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0), a0 = lx_fsm_float(c, 1), a1 = lx_fsm_float(c, 2);
    float b0 = lx_fsm_float(c, 3), b1 = lx_fsm_float(c, 4);
    float t = (a1 - a0) != 0 ? (v - a0) / (a1 - a0) : 0;
    if (lx_fsm_bool(c, 5)) t = lx_saturate(t);
    lx_fsm_set_float(c, 6, b0 + (b1 - b0) * t);
}
static const LxFsmActionDesc A_FLOAT_REMAP = {"Float Remap", "Float", "Moves a number from one range into another (0..1 health into 0..100 percent, for example).",
    "{0} from {1}..{2} to {3}..{4} -> {6}",
    {P("Value", LX_FSM_FLOAT, "0"), P("From Min", LX_FSM_FLOAT, "0"), P("From Max", LX_FSM_FLOAT, "1"),
     P("To Min", LX_FSM_FLOAT, "0"), P("To Max", LX_FSM_FLOAT, "100"), P("Clamp", LX_FSM_BOOL, "true"),
     POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_remap, f_remap, NULL};

static void f_normalize(LxFsmCtx *c) {
    float lo = lx_fsm_float(c, 1), hi = lx_fsm_float(c, 2);
    float t = (hi - lo) != 0 ? (lx_fsm_float(c, 0) - lo) / (hi - lo) : 0;
    lx_fsm_set_float(c, 3, lx_saturate(t));
}
static const LxFsmActionDesc A_FLOAT_NORMALIZE = {"Float Normalize", "Float", "Turns a number in a range into 0..1 (health bars, progress).", "{0} in {1}..{2} -> {3}",
    {P("Value", LX_FSM_FLOAT, "0"), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "100"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_normalize, f_normalize, NULL};

static void f_parse(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    char *end = NULL;
    float v = strtof(s ? s : "", &end);
    bool ok = end && end != s;
    lx_fsm_set_float(c, 1, ok ? v : lx_fsm_float(c, 2));
    lx_fsm_set_bool(c, 3, ok);
    fsm_send(c, ok ? 4 : 5);
}
static const LxFsmActionDesc A_FLOAT_PARSE = {"Float Parse", "Float", "Reads a number out of a piece of text.", "parse {0} -> {1}",
    {P("String Value", LX_FSM_STRING, "0"), POUT("Store Result", LX_FSM_FLOAT), P("Default Value", LX_FSM_FLOAT, "0"),
     POPT("Store Success", LX_FSM_BOOL), PEV("Success Event"), PEV("Failure Event")},
    0, 0, LX_FSM_ONCE, f_parse, f_parse, NULL};

static void f_to_string(LxFsmCtx *c) {
    char buf[64];
    int places = lx_fsm_int(c, 1);
    if (places < 0) places = 0;
    if (places > 9) places = 9;
    snprintf(buf, sizeof buf, "%.*f", places, (double)lx_fsm_float(c, 0));
    lx_fsm_set_string(c, 2, buf);
}
static const LxFsmActionDesc A_FLOAT_TO_STRING = {"Float To String", "Float", "Writes a number as text with a chosen number of decimal places.", "{0} -> {2}",
    {P("Float Variable", LX_FSM_FLOAT, "0"), P("Decimal Places", LX_FSM_INT, "2"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, f_to_string, f_to_string, NULL};

static void f_abs(LxFsmCtx *c) { lx_fsm_set_float(c, 1, fabsf(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_FLOAT_ABS = {"Float Abs", "Float", "The number without its sign.", "|{0}| -> {1}",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, f_abs, f_abs, NULL};

static void f_sign(LxFsmCtx *c) { float v = lx_fsm_float(c, 0); lx_fsm_set_float(c, 1, v < 0 ? -1.0f : 1.0f); }
static const LxFsmActionDesc A_FLOAT_SIGN = {"Float Sign", "Float", "-1 when the number is negative, otherwise 1.", "sign {0} -> {1}",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, f_sign, f_sign, NULL};

static void f_min(LxFsmCtx *c) { lx_fsm_set_float(c, 2, lx_minf(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_FLOAT_MIN = {"Float Min", "Float", "The smaller of two numbers.", "min({0}, {1}) -> {2}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_min, f_min, NULL};

static void f_max(LxFsmCtx *c) { lx_fsm_set_float(c, 2, lx_maxf(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_FLOAT_MAX = {"Float Max", "Float", "The larger of two numbers.", "max({0}, {1}) -> {2}",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_max, f_max, NULL};

static void f_approximately(LxFsmCtx *c) {
    bool eq = fabsf(lx_fsm_float(c, 0) - lx_fsm_float(c, 1)) < 1e-5f;
    lx_fsm_set_bool(c, 2, eq);
    fsm_send(c, eq ? 3 : 4);
}
static const LxFsmActionDesc A_FLOAT_APPROX = {"Float Approximately", "Float", "True when two numbers are the same allowing for rounding error.", "{0} ~= {1}?",
    {P("Float 1", LX_FSM_FLOAT, "0"), P("Float 2", LX_FSM_FLOAT, "0"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, f_approximately, f_approximately, NULL};

static void f_list_sum(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    float sum = 0;
    for (int i = 0; i < n; i++) sum += lx_array_get_float(a, i);
    lx_fsm_set_float(c, 1, sum);
    lx_fsm_set_float(c, 2, n > 0 ? sum / (float)n : 0);
}
static const LxFsmActionDesc A_FLOAT_LIST_SUM = {"Float List Sum", "Float", "Adds up every number in an array, and gives the average too.", "sum {0} -> {1}",
    {PARR("Array"), POUT("Store Sum", LX_FSM_FLOAT), POPT("Store Average", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_list_sum, f_list_sum, NULL};

static void f_list_min_max(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    if (n <= 0) return;
    float lo = lx_array_get_float(a, 0), hi = lo;
    int lo_i = 0, hi_i = 0;
    for (int i = 1; i < n; i++) {
        float v = lx_array_get_float(a, i);
        if (v < lo) { lo = v; lo_i = i; }
        if (v > hi) { hi = v; hi_i = i; }
    }
    lx_fsm_set_float(c, 1, lo);
    lx_fsm_set_float(c, 2, hi);
    lx_fsm_set_int(c, 3, lo_i);
    lx_fsm_set_int(c, 4, hi_i);
}
static const LxFsmActionDesc A_FLOAT_LIST_MIN_MAX = {"Float List Min Max", "Float", "The smallest and largest number in an array, and where they are.",
    "min/max of {0}",
    {PARR("Array"), POPT("Store Min", LX_FSM_FLOAT), POPT("Store Max", LX_FSM_FLOAT),
     POPT("Store Min Index", LX_FSM_INT), POPT("Store Max Index", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, f_list_min_max, f_list_min_max, NULL};

/* --- float tests --- */
static void f_is_negative(LxFsmCtx *c) { bool r = lx_fsm_float(c, 0) < 0; lx_fsm_set_bool(c, 1, r); fsm_send(c, r ? 2 : 3); }
static const LxFsmActionDesc A_FLOAT_IS_NEGATIVE = {"Float Is Negative", "Float", "True when the number is below zero.", "{0} < 0?",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, f_is_negative, f_is_negative, NULL};

static void f_is_positive(LxFsmCtx *c) { bool r = lx_fsm_float(c, 0) > 0; lx_fsm_set_bool(c, 1, r); fsm_send(c, r ? 2 : 3); }
static const LxFsmActionDesc A_FLOAT_IS_POSITIVE = {"Float Is Positive", "Float", "True when the number is above zero.", "{0} > 0?",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, f_is_positive, f_is_positive, NULL};

static void f_is_nan(LxFsmCtx *c) { float v = lx_fsm_float(c, 0); bool r = v != v; lx_fsm_set_bool(c, 1, r); fsm_send(c, r ? 2 : 3); }
static const LxFsmActionDesc A_FLOAT_IS_NAN = {"Float Is NaN", "Float", "True when the number is \"not a number\" (the result of 0/0 and similar).", "{0} is NaN?",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, f_is_nan, f_is_nan, NULL};

static void f_is_infinity(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    bool r = isinf(v) != 0;
    lx_fsm_set_bool(c, 1, r);
    fsm_send(c, r ? 2 : 3);
}
static const LxFsmActionDesc A_FLOAT_IS_INF = {"Float Is Infinity", "Float", "True when the number is infinite.", "{0} is infinite?",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, f_is_infinity, f_is_infinity, NULL};

static void f_is_finite(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    bool r = isfinite(v) != 0;
    lx_fsm_set_bool(c, 1, r);
    fsm_send(c, r ? 2 : 3);
}
static const LxFsmActionDesc A_FLOAT_IS_FINITE = {"Float Is Finite", "Float", "True when the number is an ordinary number (not infinite, not NaN).", "{0} is finite?",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, f_is_finite, f_is_finite, NULL};

static void f_limits(LxFsmCtx *c) {
    switch (lx_fsm_int(c, 0)) {
    case 0: lx_fsm_set_float(c, 1, FLT_MAX); break;
    case 1: lx_fsm_set_float(c, 1, -FLT_MAX); break;
    case 2: lx_fsm_set_float(c, 1, FLT_EPSILON); break;
    case 3: lx_fsm_set_float(c, 1, INFINITY); break;
    case 4: lx_fsm_set_float(c, 1, -INFINITY); break;
    default: lx_fsm_set_float(c, 1, NAN); break;
    }
}
static const LxFsmActionDesc A_FLOAT_LIMITS = {"Float Get Limit", "Float", "The largest or smallest number a Float can hold, the smallest step between numbers, infinity or NaN.",
    "{0} -> {1}",
    {PENUM("Limit", "Max Value|Min Value|Epsilon|Positive Infinity|Negative Infinity|NaN", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, f_limits, f_limits, NULL};

/* ==========================================================================================
 *  Integer
 * ========================================================================================== */
static void i_set(LxFsmCtx *c) { lx_fsm_set_int(c, 1, lx_fsm_int(c, 0)); }
static const LxFsmActionDesc A_INT_SET = {"Int Set Value", "Integer", "Stores a whole number in an Int variable.", "{1} = {0}",
    {P("Value", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, i_set, i_set, NULL};

static void i_add(LxFsmCtx *c) { lx_fsm_set_int(c, 0, lx_fsm_int(c, 0) + lx_fsm_int(c, 1)); }
static const LxFsmActionDesc A_INT_ADD = {"Int Add", "Integer", "Adds a whole number to an Int variable (scores, counters).", "{0} += {1}",
    {POUT("Int Variable", LX_FSM_INT), P("Add", LX_FSM_INT, "1")}, 0, 0, LX_FSM_ONCE, i_add, i_add, NULL};

static void i_add_clamped(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0) + lx_fsm_int(c, 1), lo = lx_fsm_int(c, 2), hi = lx_fsm_int(c, 3);
    lx_fsm_set_int(c, 0, v < lo ? lo : (v > hi ? hi : v));
}
static const LxFsmActionDesc A_INT_ADD_CLAMPED = {"Int Add Clamped", "Integer", "Adds to an Int variable and keeps it between Min and Max (lives, ammo).",
    "{0} += {1} ({2}..{3})",
    {POUT("Int Variable", LX_FSM_INT), P("Add", LX_FSM_INT, "1"), P("Min", LX_FSM_INT, "0"), P("Max", LX_FSM_INT, "99")},
    0, 0, LX_FSM_ONCE, i_add_clamped, i_add_clamped, NULL};

static void i_add_wrap(LxFsmCtx *c) {
    int lo = lx_fsm_int(c, 2), hi = lx_fsm_int(c, 3);
    int v = lx_fsm_int(c, 0) + lx_fsm_int(c, 1);
    int span = hi - lo + 1;
    if (span > 0) { v = (v - lo) % span; if (v < 0) v += span; v += lo; }
    lx_fsm_set_int(c, 0, v);
}
static const LxFsmActionDesc A_INT_ADD_WRAP = {"Int Add Wrap", "Integer", "Adds to an Int variable; past Max it wraps round to Min (menu selection, weapon cycling).",
    "{0} += {1} wrap {2}..{3}",
    {POUT("Int Variable", LX_FSM_INT), P("Add", LX_FSM_INT, "1"), P("Min", LX_FSM_INT, "0"), P("Max", LX_FSM_INT, "3")},
    0, 0, LX_FSM_ONCE, i_add_wrap, i_add_wrap, NULL};

static void i_add_multiple(LxFsmCtx *c) {
    int sum = 0;
    for (int i = 1; i <= 5; i++) if (!lx_fsm_is_none(c, i)) sum += lx_fsm_int(c, i);
    lx_fsm_set_int(c, 0, lx_fsm_int(c, 0) + sum);
}
static const LxFsmActionDesc A_INT_ADD_MULTIPLE = {"Int Add Multiple", "Integer", "Adds up to five whole numbers to an Int variable at once.", "{0} += {1}+{2}+{3}",
    {POUT("Int Variable", LX_FSM_INT), PIN_OPT("Add 1", LX_FSM_INT), PIN_OPT("Add 2", LX_FSM_INT),
     PIN_OPT("Add 3", LX_FSM_INT), PIN_OPT("Add 4", LX_FSM_INT), PIN_OPT("Add 5", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_add_multiple, i_add_multiple, NULL};

static void i_subtract(LxFsmCtx *c) { lx_fsm_set_int(c, 0, lx_fsm_int(c, 0) - lx_fsm_int(c, 1)); }
static const LxFsmActionDesc A_INT_SUBTRACT = {"Int Subtract", "Integer", "Subtracts a whole number from an Int variable.", "{0} -= {1}",
    {POUT("Int Variable", LX_FSM_INT), P("Subtract", LX_FSM_INT, "1")}, 0, 0, LX_FSM_ONCE, i_subtract, i_subtract, NULL};

static void i_multiply(LxFsmCtx *c) { lx_fsm_set_int(c, 0, lx_fsm_int(c, 0) * lx_fsm_int(c, 1)); }
static const LxFsmActionDesc A_INT_MULTIPLY = {"Int Multiply", "Integer", "Multiplies an Int variable by a whole number.", "{0} *= {1}",
    {POUT("Int Variable", LX_FSM_INT), P("Multiply By", LX_FSM_INT, "2")}, 0, 0, LX_FSM_ONCE, i_multiply, i_multiply, NULL};

static void i_divide(LxFsmCtx *c) {
    int d = lx_fsm_int(c, 1);
    if (d != 0) lx_fsm_set_int(c, 0, lx_fsm_int(c, 0) / d);
}
static const LxFsmActionDesc A_INT_DIVIDE = {"Int Divide", "Integer", "Divides an Int variable by a whole number, throwing away the remainder.", "{0} /= {1}",
    {POUT("Int Variable", LX_FSM_INT), P("Divide By", LX_FSM_INT, "2")}, 0, 0, LX_FSM_ONCE, i_divide, i_divide, NULL};

static void i_operator(LxFsmCtx *c) {
    int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1), r = 0;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = a + b; break;
    case 1: r = a - b; break;
    case 2: r = a * b; break;
    case 3: r = b != 0 ? a / b : 0; break;
    case 4: r = a < b ? a : b; break;
    case 5: r = a > b ? a : b; break;
    case 6: r = b != 0 ? a % b : 0; break;
    default: r = a; break;
    }
    lx_fsm_set_int(c, 3, r);
}
static const LxFsmActionDesc A_INT_OPERATOR = {"Int Operator", "Integer", "Adds, subtracts, multiplies, divides, or takes the smaller, larger or remainder of two whole numbers.",
    "{0} {2} {1} -> {3}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"),
     PENUM("Operation", "Add|Subtract|Multiply|Divide|Min|Max|Mod", "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_operator, i_operator, NULL};

static void i_equals(LxFsmCtx *c) {
    bool eq = lx_fsm_int(c, 0) == lx_fsm_int(c, 1);
    lx_fsm_set_bool(c, 2, eq);
    fsm_send(c, eq ? 3 : 4);
}
static const LxFsmActionDesc A_INT_EQUALS = {"Int Equals", "Integer", "Tests whether two whole numbers are the same.", "{0} == {1}?",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, i_equals, i_equals, NULL};

static void i_compare(LxFsmCtx *c) {
    int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1);
    if (a == b) fsm_send(c, 2);
    else if (a < b) fsm_send(c, 3);
    else fsm_send(c, 4);
}
static const LxFsmActionDesc A_INT_COMPARE = {"Int Compare", "Integer", "Compares two whole numbers and sends an event: equal, less than or greater than.",
    "if {0} vs {1}: = {2}, < {3}, > {4}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), PEV("Equal Event"), PEV("Less Than Event"), PEV("Greater Than Event")},
    0, 0, LX_FSM_ONCE, i_compare, i_compare, NULL};

static void i_compare_to(LxFsmCtx *c) {
    int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1);
    lx_fsm_set_int(c, 2, a < b ? -1 : (a > b ? 1 : 0));
}
static const LxFsmActionDesc A_INT_COMPARE_TO = {"Int Compare To", "Integer", "Stores -1, 0 or 1 depending on whether the first number is less than, equal to or greater than the second.",
    "compare {0} to {1} -> {2}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_compare_to, i_compare_to, NULL};

static void i_select(LxFsmCtx *c) { lx_fsm_set_int(c, 3, lx_fsm_bool(c, 0) ? lx_fsm_int(c, 1) : lx_fsm_int(c, 2)); }
static const LxFsmActionDesc A_INT_SELECT = {"Int Select Value", "Integer", "Picks one of two whole numbers depending on a bool.", "{3} = {0} ? {1} : {2}",
    {P("Condition", LX_FSM_BOOL, "true"), P("If True", LX_FSM_INT, "1"), P("If False", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_select, i_select, NULL};

static void i_abs(LxFsmCtx *c) { int v = lx_fsm_int(c, 0); lx_fsm_set_int(c, 1, v < 0 ? -v : v); }
static const LxFsmActionDesc A_INT_ABS = {"Int Abs", "Integer", "The whole number without its sign.", "|{0}| -> {1}",
    {P("Int Variable", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, i_abs, i_abs, NULL};

static void i_min(LxFsmCtx *c) { int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1); lx_fsm_set_int(c, 2, a < b ? a : b); }
static const LxFsmActionDesc A_INT_MIN = {"Int Min", "Integer", "The smaller of two whole numbers.", "min({0}, {1}) -> {2}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_min, i_min, NULL};

static void i_max(LxFsmCtx *c) { int a = lx_fsm_int(c, 0), b = lx_fsm_int(c, 1); lx_fsm_set_int(c, 2, a > b ? a : b); }
static const LxFsmActionDesc A_INT_MAX = {"Int Max", "Integer", "The larger of two whole numbers.", "max({0}, {1}) -> {2}",
    {P("Int 1", LX_FSM_INT, "0"), P("Int 2", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_max, i_max, NULL};

static void i_clamp(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0), lo = lx_fsm_int(c, 1), hi = lx_fsm_int(c, 2);
    lx_fsm_set_int(c, 3, v < lo ? lo : (v > hi ? hi : v));
}
static const LxFsmActionDesc A_INT_CLAMP = {"Int Clamp", "Integer", "Keeps a whole number between Min and Max.", "clamp {0} to {1}..{2} -> {3}",
    {P("Int Variable", LX_FSM_INT, "0"), P("Min", LX_FSM_INT, "0"), P("Max", LX_FSM_INT, "10"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, i_clamp, i_clamp, NULL};

static void i_parse(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    char *end = NULL;
    long v = strtol(s ? s : "", &end, 10);
    bool ok = end && end != s;
    lx_fsm_set_int(c, 1, ok ? (int)v : lx_fsm_int(c, 2));
    lx_fsm_set_bool(c, 3, ok);
    fsm_send(c, ok ? 4 : 5);
}
static const LxFsmActionDesc A_INT_PARSE = {"Int Parse", "Integer", "Reads a whole number out of a piece of text.", "parse {0} -> {1}",
    {P("String Value", LX_FSM_STRING, "0"), POUT("Store Result", LX_FSM_INT), P("Default Value", LX_FSM_INT, "0"),
     POPT("Store Success", LX_FSM_BOOL), PEV("Success Event"), PEV("Failure Event")},
    0, 0, LX_FSM_ONCE, i_parse, i_parse, NULL};

static void i_to_string(LxFsmCtx *c) {
    char buf[64], num[32];
    snprintf(num, sizeof num, "%d", lx_fsm_int(c, 0));
    int pad = lx_fsm_int(c, 1);
    int len = (int)strlen(num);
    bool neg = num[0] == '-';
    const char *digits = neg ? num + 1 : num;
    int dlen = neg ? len - 1 : len;
    if (pad > dlen && pad < 24) {
        char zeros[24];
        int z = pad - dlen;
        for (int i = 0; i < z; i++) zeros[i] = '0';
        zeros[z] = 0;
        snprintf(buf, sizeof buf, "%s%s%s", neg ? "-" : "", zeros, digits);
    } else {
        lx_strcpy(buf, sizeof buf, num);
    }
    lx_fsm_set_string(c, 2, buf);
}
static const LxFsmActionDesc A_INT_TO_STRING = {"Int To String", "Integer", "Writes a whole number as text, optionally padded with leading zeros (\"007\").",
    "{0} -> {2}",
    {P("Int Variable", LX_FSM_INT, "0"), PT("Leading Zeros", LX_FSM_INT, "0", "pad to this many digits; 0 = no padding"),
     POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, i_to_string, i_to_string, NULL};

static void i_list_sum(LxFsmCtx *c) {
    LxArray a = lx_fsm_array(c, 0);
    int n = lx_array_valid(a) ? lx_array_length(a) : 0;
    int sum = 0;
    for (int i = 0; i < n; i++) sum += lx_array_get_int(a, i);
    lx_fsm_set_int(c, 1, sum);
}
static const LxFsmActionDesc A_INT_LIST_SUM = {"Int List Sum", "Integer", "Adds up every whole number in an array.", "sum {0} -> {1}",
    {PARR("Array"), POUT("Store Sum", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, i_list_sum, i_list_sum, NULL};

static void i_limits(LxFsmCtx *c) { lx_fsm_set_int(c, 1, lx_fsm_int(c, 0) == 0 ? 2147483647 : (-2147483647 - 1)); }
static const LxFsmActionDesc A_INT_LIMITS = {"Int Get Limit", "Integer", "The largest or smallest whole number an Int can hold.", "{0} -> {1}",
    {PENUM("Limit", "Max Value|Min Value", "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, i_limits, i_limits, NULL};

/* ==========================================================================================
 *  Bool
 * ========================================================================================== */
static void b_set(LxFsmCtx *c) { lx_fsm_set_bool(c, 1, lx_fsm_bool(c, 0)); }
static const LxFsmActionDesc A_BOOL_SET = {"Bool Set Value", "Bool", "Stores true or false in a Bool variable.", "{1} = {0}",
    {P("Value", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, b_set, b_set, NULL};

static void b_invert(LxFsmCtx *c) { lx_fsm_set_bool(c, 0, !lx_fsm_bool(c, 0)); }
static const LxFsmActionDesc A_BOOL_INVERT = {"Bool Invert", "Bool", "Turns true into false and false into true (toggles).", "flip {0}",
    {POUT("Bool Variable", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, b_invert, b_invert, NULL};

static void b_operator(LxFsmCtx *c) {
    bool a = lx_fsm_bool(c, 0), b = lx_fsm_bool(c, 1), r;
    switch (lx_fsm_int(c, 2)) {
    case 0: r = a && b; break;
    case 1: r = a || b; break;
    case 2: r = a != b; break;
    case 3: r = !(a && b); break;
    case 4: r = !(a || b); break;
    default: r = a == b; break;
    }
    lx_fsm_set_bool(c, 3, r);
    fsm_send(c, r ? 4 : 5);
}
static const LxFsmActionDesc A_BOOL_OPERATOR = {"Bool Operator", "Bool", "Combines two bools with AND, OR, XOR, NAND, NOR or XNOR.", "{0} {2} {1} -> {3}",
    {P("Bool 1", LX_FSM_BOOL, "true"), P("Bool 2", LX_FSM_BOOL, "true"),
     PENUM("Operation", "AND|OR|XOR|NAND|NOR|XNOR", "0"), POPT("Store Result", LX_FSM_BOOL),
     PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, b_operator, b_operator, NULL};

static void b_all_true(LxFsmCtx *c) {
    bool all = true, any = false;
    for (int i = 0; i < 5; i++) {
        if (lx_fsm_is_none(c, i)) continue;
        if (lx_fsm_bool(c, i)) any = true; else all = false;
    }
    lx_fsm_set_bool(c, 5, all);
    lx_fsm_set_bool(c, 6, any);
    fsm_send(c, all ? 7 : 8);
}
static const LxFsmActionDesc A_BOOL_ALL_TRUE = {"Bool All True", "Bool", "True when every bool you give it is true; also reports whether any of them is.",
    "all of {0}..{4} true?",
    {PIN_OPT("Bool 1", LX_FSM_BOOL), PIN_OPT("Bool 2", LX_FSM_BOOL), PIN_OPT("Bool 3", LX_FSM_BOOL),
     PIN_OPT("Bool 4", LX_FSM_BOOL), PIN_OPT("Bool 5", LX_FSM_BOOL),
     POPT("Store All True", LX_FSM_BOOL), POPT("Store Any True", LX_FSM_BOOL), PEV("All True Event"), PEV("Not All Event")},
    0, 0, LX_FSM_ONCE, b_all_true, b_all_true, NULL};

static void b_is_true(LxFsmCtx *c) { fsm_send(c, lx_fsm_bool(c, 0) ? 1 : 2); }
static const LxFsmActionDesc A_BOOL_TEST = {"Bool Test", "Bool", "Sends one event when a bool is true and another when it is false.", "if {0}: {1}, else {2}",
    {P("Bool Variable", LX_FSM_BOOL, "true"), PEV("Is True Event"), PEV("Is False Event")},
    0, 0, LX_FSM_ONCE, b_is_true, b_is_true, NULL};

/* ==========================================================================================
 *  Enum
 * ========================================================================================== */
static void e_set(LxFsmCtx *c) { lx_fsm_set_int(c, 1, lx_fsm_int(c, 0)); }
static const LxFsmActionDesc A_ENUM_SET = {"Enum Set Value", "Enum", "Stores a choice (kept as its number) in an Int variable.", "{1} = {0}",
    {P("Value", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, e_set, e_set, NULL};

static void e_equals(LxFsmCtx *c) {
    bool eq = lx_fsm_int(c, 0) == lx_fsm_int(c, 1);
    lx_fsm_set_bool(c, 2, eq);
    fsm_send(c, eq ? 3 : 4);
}
static const LxFsmActionDesc A_ENUM_EQUALS = {"Enum Equals", "Enum", "Tests whether two choices are the same.", "{0} == {1}?",
    {P("Enum 1", LX_FSM_INT, "0"), P("Enum 2", LX_FSM_INT, "0"), POPT("Store Result", LX_FSM_BOOL),
     PEV("Equal Event"), PEV("Not Equal Event")},
    0, 0, LX_FSM_ONCE, e_equals, e_equals, NULL};

static void e_switch(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0);
    if (v >= 0 && v < 6) fsm_send(c, 1 + v);
    else fsm_send(c, 7);
}
static const LxFsmActionDesc A_ENUM_SWITCH = {"Enum Switch", "Enum", "Sends a different event for each value 0..5 of a number: one state per case.",
    "switch on {0}",
    {P("Enum Variable", LX_FSM_INT, "0"), PEV("Event 0"), PEV("Event 1"), PEV("Event 2"), PEV("Event 3"),
     PEV("Event 4"), PEV("Event 5"), PEV("Default Event")},
    0, 0, LX_FSM_ONCE, e_switch, e_switch, NULL};

/* ==========================================================================================
 *  Convert
 * ========================================================================================== */
static void cv_f_to_i(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    int r;
    switch (lx_fsm_int(c, 1)) {
    case 1: r = (int)floorf(v); break;
    case 2: r = (int)ceilf(v); break;
    case 3: r = (int)v; break;
    default: r = (int)floorf(v + 0.5f); break;
    }
    lx_fsm_set_int(c, 2, r);
}
static const LxFsmActionDesc A_CONV_F_TO_I = {"Convert Float To Int", "Convert", "Turns a number into a whole number, rounding, flooring, ceiling or cutting off the decimals.",
    "{0} -> {2}",
    {P("Float Variable", LX_FSM_FLOAT, "0"), PENUM("Rounding", "Round|Floor|Ceiling|Truncate", "0"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, cv_f_to_i, cv_f_to_i, NULL};

static void cv_i_to_f(LxFsmCtx *c) { lx_fsm_set_float(c, 1, (float)lx_fsm_int(c, 0)); }
static const LxFsmActionDesc A_CONV_I_TO_F = {"Convert Int To Float", "Convert", "Turns a whole number into a number with decimals.", "{0} -> {1}",
    {P("Int Variable", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, cv_i_to_f, cv_i_to_f, NULL};

static void cv_b_to_f(LxFsmCtx *c) { lx_fsm_set_float(c, 1, lx_fsm_bool(c, 0) ? lx_fsm_float(c, 2) : lx_fsm_float(c, 3)); }
static const LxFsmActionDesc A_CONV_B_TO_F = {"Convert Bool To Float", "Convert", "Turns true/false into two numbers of your choosing.", "{0} -> {1}",
    {P("Bool Variable", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_FLOAT),
     P("True Value", LX_FSM_FLOAT, "1"), P("False Value", LX_FSM_FLOAT, "0")},
    0, 0, LX_FSM_ONCE, cv_b_to_f, cv_b_to_f, NULL};

static void cv_b_to_i(LxFsmCtx *c) { lx_fsm_set_int(c, 1, lx_fsm_bool(c, 0) ? lx_fsm_int(c, 2) : lx_fsm_int(c, 3)); }
static const LxFsmActionDesc A_CONV_B_TO_I = {"Convert Bool To Int", "Convert", "Turns true/false into two whole numbers of your choosing.", "{0} -> {1}",
    {P("Bool Variable", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_INT),
     P("True Value", LX_FSM_INT, "1"), P("False Value", LX_FSM_INT, "0")},
    0, 0, LX_FSM_ONCE, cv_b_to_i, cv_b_to_i, NULL};

static void cv_b_to_s(LxFsmCtx *c) { lx_fsm_set_string(c, 1, lx_fsm_bool(c, 0) ? lx_fsm_string(c, 2) : lx_fsm_string(c, 3)); }
static const LxFsmActionDesc A_CONV_B_TO_S = {"Convert Bool To String", "Convert", "Turns true/false into two pieces of text of your choosing.", "{0} -> {1}",
    {P("Bool Variable", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_STRING),
     P("True Text", LX_FSM_STRING, "True"), P("False Text", LX_FSM_STRING, "False")},
    0, 0, LX_FSM_ONCE, cv_b_to_s, cv_b_to_s, NULL};

static void cv_f_to_b(LxFsmCtx *c) { lx_fsm_set_bool(c, 1, lx_fsm_float(c, 0) > lx_fsm_float(c, 2)); }
static const LxFsmActionDesc A_CONV_F_TO_B = {"Convert Float To Bool", "Convert", "True when a number is above a threshold.", "{0} > {2} -> {1}",
    {P("Float Variable", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_BOOL), P("Threshold", LX_FSM_FLOAT, "0")},
    0, 0, LX_FSM_ONCE, cv_f_to_b, cv_f_to_b, NULL};

static void cv_i_to_b(LxFsmCtx *c) { lx_fsm_set_bool(c, 1, lx_fsm_int(c, 0) != 0); }
static const LxFsmActionDesc A_CONV_I_TO_B = {"Convert Int To Bool", "Convert", "True when a whole number is not zero.", "{0} != 0 -> {1}",
    {P("Int Variable", LX_FSM_INT, "0"), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, cv_i_to_b, cv_i_to_b, NULL};

static void cv_s_to_b(LxFsmCtx *c) {
    const char *s = lx_fsm_string(c, 0);
    bool r = s && (!fsm_stricmp(s, "true") || !fsm_stricmp(s, "yes") || !fsm_stricmp(s, "1") || !fsm_stricmp(s, "on"));
    lx_fsm_set_bool(c, 1, r);
}
static const LxFsmActionDesc A_CONV_S_TO_B = {"Convert String To Bool", "Convert", "True when the text reads \"true\", \"yes\", \"on\" or \"1\".", "{0} -> {1}",
    {P("String Variable", LX_FSM_STRING, "true"), POUT("Store Result", LX_FSM_BOOL)}, 0, 0, LX_FSM_ONCE, cv_s_to_b, cv_s_to_b, NULL};

static void cv_seconds_to_s(LxFsmCtx *c) {
    char out[64];
    lx_format_time(out, sizeof out, lx_fsm_float(c, 0), lx_fsm_int(c, 1));
    lx_fsm_set_string(c, 2, out);
}
static const LxFsmActionDesc A_CONV_SECONDS = {"Convert Seconds To String", "Convert", "Writes a number of seconds as a clock (\"1:05\", \"1:02:03\", \"1:05.42\").",
    "{0}s -> {2}",
    {P("Seconds", LX_FSM_FLOAT, "90"), PENUM("Format", "m:ss|h:mm:ss|m:ss.cc", "0"), POUT("Store Result", LX_FSM_STRING)},
    0, 0, LX_FSM_ONCE, cv_seconds_to_s, cv_seconds_to_s, NULL};

static void cv_v3_to_v2(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    int plane = lx_fsm_int(c, 1);
    LxVec3 r = plane == 0 ? lx_v3(v.x, v.y, 0) : (plane == 1 ? lx_v3(v.x, v.z, 0) : lx_v3(v.y, v.z, 0));
    lx_fsm_set_vec3(c, 2, r);
}
static const LxFsmActionDesc A_CONV_V3_TO_V2 = {"Convert Vector3 To Vector2", "Convert", "Drops one axis of a Vector3 to make a Vector2.", "{0} ({1}) -> {2}",
    {P("Vector3", LX_FSM_VEC3, "0 0 0"), PENUM("Use", "XY|XZ|YZ", "0"), POUT("Store Result", LX_FSM_VEC2)},
    0, 0, LX_FSM_ONCE, cv_v3_to_v2, cv_v3_to_v2, NULL};

static void cv_v2_to_v3(LxFsmCtx *c) {
    LxVec3 v = lx_fsm_vec3(c, 0);
    float z = lx_fsm_float(c, 2);
    int plane = lx_fsm_int(c, 1);
    LxVec3 r = plane == 0 ? lx_v3(v.x, v.y, z) : (plane == 1 ? lx_v3(v.x, z, v.y) : lx_v3(z, v.x, v.y));
    lx_fsm_set_vec3(c, 3, r);
}
static const LxFsmActionDesc A_CONV_V2_TO_V3 = {"Convert Vector2 To Vector3", "Convert", "Makes a Vector3 from a Vector2, putting the missing axis where you choose.",
    "{0} ({1}) -> {3}",
    {P("Vector2", LX_FSM_VEC2, "0 0"), PENUM("Into", "XY|XZ|YZ", "1"), P("Other Axis", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, cv_v2_to_v3, cv_v2_to_v3, NULL};

static void cv_any_to_s(LxFsmCtx *c) {
    LxFsmValue *v = lx_fsm_param_value(c, 0);
    char out[LX_REF_MAX];
    if (v) lx_fsm_value_format(v, out, sizeof out, NULL);
    else out[0] = 0;
    lx_fsm_set_string(c, 1, out);
}
static const LxFsmActionDesc A_CONV_ANY_TO_S = {"Convert Variable To String", "Convert", "Writes a variable of any type as text, for messages and debugging.",
    "{0} -> {1}",
    {P("Variable", LX_FSM_ANY, ""), POUT("Store Result", LX_FSM_STRING)}, 0, 0, LX_FSM_ONCE, cv_any_to_s, cv_any_to_s, NULL};

/* ==========================================================================================
 *  Trigonometry
 * ========================================================================================== */
#define TRIG(fn, nm, desc_, call)                                                                        \
    static void fn(LxFsmCtx *c) {                                                                        \
        float a = lx_fsm_float(c, 0);                                                                    \
        if (lx_fsm_bool(c, 1)) a *= LX_DEG2RAD;                                                          \
        lx_fsm_set_float(c, 2, (call));                                                                  \
    }                                                                                                    \
    static const LxFsmActionDesc A_##fn = {nm, "Trigonometry", desc_, nm " {0} -> {2}",                   \
        {P("Angle", LX_FSM_FLOAT, "0"), PT("In Degrees", LX_FSM_BOOL, "true", "the angle is given in degrees"), \
         POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, fn, fn, NULL};

TRIG(t_sin, "Sin", "The sine of an angle.", sinf(a))
TRIG(t_cos, "Cos", "The cosine of an angle.", cosf(a))
TRIG(t_tan, "Tan", "The tangent of an angle.", tanf(a))
#undef TRIG

#define ATRIG(fn, nm, desc_, call)                                                                       \
    static void fn(LxFsmCtx *c) {                                                                        \
        float v = lx_fsm_float(c, 0);                                                                    \
        float r = (call);                                                                                \
        if (lx_fsm_bool(c, 1)) r *= LX_RAD2DEG;                                                          \
        lx_fsm_set_float(c, 2, r);                                                                       \
    }                                                                                                    \
    static const LxFsmActionDesc A_##fn = {nm, "Trigonometry", desc_, nm " {0} -> {2}",                   \
        {P("Value", LX_FSM_FLOAT, "0"), PT("In Degrees", LX_FSM_BOOL, "true", "give the answer in degrees"), \
         POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, fn, fn, NULL};

ATRIG(t_asin, "Asin", "The angle whose sine is this value.", asinf(lx_clampf(v, -1, 1)))
ATRIG(t_acos, "Acos", "The angle whose cosine is this value.", acosf(lx_clampf(v, -1, 1)))
ATRIG(t_atan, "Atan", "The angle whose tangent is this value.", atanf(v))
#undef ATRIG

static void t_atan2(LxFsmCtx *c) {
    float r = atan2f(lx_fsm_float(c, 0), lx_fsm_float(c, 1));
    if (lx_fsm_bool(c, 2)) r *= LX_RAD2DEG;
    lx_fsm_set_float(c, 3, r);
}
static const LxFsmActionDesc A_ATAN2 = {"Atan2", "Trigonometry", "The angle of the direction (x, y): the usual way to turn a direction into a heading.",
    "atan2({0}, {1}) -> {3}",
    {P("Y", LX_FSM_FLOAT, "0"), P("X", LX_FSM_FLOAT, "1"), P("In Degrees", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, t_atan2, t_atan2, NULL};

static void t_delta_angle(LxFsmCtx *c) { lx_fsm_set_float(c, 2, fsm_delta_angle(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_DELTA_ANGLE = {"Delta Angle", "Trigonometry", "The shortest turn in degrees from one angle to another (-180..180).",
    "{0} to {1} -> {2}",
    {P("From Angle", LX_FSM_FLOAT, "0"), P("To Angle", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, t_delta_angle, t_delta_angle, NULL};

static void t_consts(LxFsmCtx *c) {
    switch (lx_fsm_int(c, 0)) {
    case 0: lx_fsm_set_float(c, 1, LX_PI); break;
    case 1: lx_fsm_set_float(c, 1, LX_PI * 2.0f); break;
    case 2: lx_fsm_set_float(c, 1, LX_DEG2RAD); break;
    case 3: lx_fsm_set_float(c, 1, LX_RAD2DEG); break;
    case 4: lx_fsm_set_float(c, 1, 2.71828182845904523536f); break;
    default: lx_fsm_set_float(c, 1, 1.41421356237309504880f); break;
    }
}
static const LxFsmActionDesc A_MATH_CONST = {"Get Math Constant", "Math Constants", "Pi, Tau, the degree/radian conversions, e or the square root of 2.",
    "{0} -> {1}",
    {PENUM("Constant", "PI|TAU|Deg To Rad|Rad To Deg|E|Sqrt 2", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, t_consts, t_consts, NULL};

/* ==========================================================================================
 *  Exponents
 * ========================================================================================== */
static void x_pow(LxFsmCtx *c) { lx_fsm_set_float(c, 2, powf(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_POW = {"Pow", "Exponents", "Raises a number to a power.", "{0} ^ {1} -> {2}",
    {P("Base", LX_FSM_FLOAT, "2"), P("Power", LX_FSM_FLOAT, "2"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, x_pow, x_pow, NULL};

static void x_sqrt(LxFsmCtx *c) { lx_fsm_set_float(c, 1, sqrtf(lx_maxf(0, lx_fsm_float(c, 0)))); }
static const LxFsmActionDesc A_SQRT = {"Sqrt", "Exponents", "The square root of a number.", "sqrt {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "4"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, x_sqrt, x_sqrt, NULL};

static void x_exp(LxFsmCtx *c) { lx_fsm_set_float(c, 1, expf(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_EXP = {"Exp", "Exponents", "e raised to this power.", "e^{0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, x_exp, x_exp, NULL};

static void x_log(LxFsmCtx *c) {
    float v = lx_maxf(1e-20f, lx_fsm_float(c, 0));
    float base = lx_fsm_float(c, 1);
    float r;
    if (base <= 0 || fabsf(base - 2.718281828f) < 1e-4f) r = logf(v);
    else if (fabsf(base - 10.0f) < 1e-6f) r = log10f(v);
    else r = logf(v) / logf(base);
    lx_fsm_set_float(c, 2, r);
}
static const LxFsmActionDesc A_LOG = {"Logarithm", "Exponents", "The logarithm of a number; leave Base at 0 for the natural log.", "log{1}({0}) -> {2}",
    {P("Value", LX_FSM_FLOAT, "1"), PT("Base", LX_FSM_FLOAT, "0", "0 = natural log (e); 10 = log10; 2 = log2"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, x_log, x_log, NULL};

static void x_gamma(LxFsmCtx *c) {
    float v = lx_saturate(lx_fsm_float(c, 0));
    lx_fsm_set_float(c, 2, lx_fsm_int(c, 1) == 0 ? powf(v, 2.2f) : powf(v, 1.0f / 2.2f));
}
static const LxFsmActionDesc A_GAMMA = {"Gamma Space Convert", "Exponents", "Converts a 0..1 value between gamma (sRGB) and linear space.",
    "{0} {1} -> {2}",
    {P("Value", LX_FSM_FLOAT, "0.5"), PENUM("Direction", "Gamma To Linear|Linear To Gamma", "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, x_gamma, x_gamma, NULL};

/* ==========================================================================================
 *  Rounding
 * ========================================================================================== */
static void r_round(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0), step = lx_fsm_float(c, 1);
    if (step > 0) v = floorf(v / step + 0.5f) * step; else v = floorf(v + 0.5f);
    lx_fsm_set_float(c, 2, v);
}
static const LxFsmActionDesc A_ROUND = {"Round", "Rounding", "Rounds a number to the nearest whole number, or to the nearest step (0.25, 10...).",
    "round {0} -> {2}",
    {P("Value", LX_FSM_FLOAT, "0"), PT("Step", LX_FSM_FLOAT, "0", "round to multiples of this; 0 = whole numbers"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, r_round, r_round, NULL};

static void r_floor(LxFsmCtx *c) { lx_fsm_set_float(c, 1, floorf(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_FLOOR = {"Floor", "Rounding", "Rounds a number down.", "floor {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, r_floor, r_floor, NULL};

static void r_ceil(LxFsmCtx *c) { lx_fsm_set_float(c, 1, ceilf(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_CEIL = {"Ceiling", "Rounding", "Rounds a number up.", "ceil {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, r_ceil, r_ceil, NULL};

static void r_round_to_int(LxFsmCtx *c) { lx_fsm_set_int(c, 1, (int)floorf(lx_fsm_float(c, 0) + 0.5f)); }
static const LxFsmActionDesc A_ROUND_TO_INT = {"Round To Int", "Rounding", "Rounds a number to the nearest whole number and stores it in an Int.", "round {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, r_round_to_int, r_round_to_int, NULL};

static void r_floor_to_int(LxFsmCtx *c) { lx_fsm_set_int(c, 1, (int)floorf(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_FLOOR_TO_INT = {"Floor To Int", "Rounding", "Rounds a number down into an Int.", "floor {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, r_floor_to_int, r_floor_to_int, NULL};

static void r_ceil_to_int(LxFsmCtx *c) { lx_fsm_set_int(c, 1, (int)ceilf(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_CEIL_TO_INT = {"Ceiling To Int", "Rounding", "Rounds a number up into an Int.", "ceil {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, r_ceil_to_int, r_ceil_to_int, NULL};

/* ==========================================================================================
 *  Range
 * ========================================================================================== */
static void rg_clamp(LxFsmCtx *c) {
    lx_fsm_set_float(c, 3, lx_clampf(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2)));
}
static const LxFsmActionDesc A_CLAMP = {"Clamp", "Range", "Keeps a number between Min and Max.", "clamp {0} to {1}..{2} -> {3}",
    {P("Value", LX_FSM_FLOAT, "0"), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rg_clamp, rg_clamp, NULL};

static void rg_clamp01(LxFsmCtx *c) { lx_fsm_set_float(c, 1, lx_saturate(lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_CLAMP01 = {"Clamp 0 1", "Range", "Keeps a number between 0 and 1.", "clamp01 {0} -> {1}",
    {P("Value", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)}, 0, 0, LX_FSM_ONCE, rg_clamp01, rg_clamp01, NULL};

static void rg_repeat(LxFsmCtx *c) { lx_fsm_set_float(c, 2, fsm_repeat(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_REPEAT = {"Repeat", "Range", "Wraps a number round so it always stays between 0 and Length (angles, looping timers).",
    "{0} mod {1} -> {2}",
    {P("Value", LX_FSM_FLOAT, "0"), P("Length", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rg_repeat, rg_repeat, NULL};

static void rg_ping_pong(LxFsmCtx *c) { lx_fsm_set_float(c, 2, fsm_ping_pong(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_PING_PONG = {"Ping Pong", "Range", "Bounces a number back and forth between 0 and Length.", "pingpong {0} in {1} -> {2}",
    {P("Value", LX_FSM_FLOAT, "0"), P("Length", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rg_ping_pong, rg_ping_pong, NULL};

static void rg_in_range(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0);
    bool in = v >= lx_fsm_float(c, 1) && v <= lx_fsm_float(c, 2);
    lx_fsm_set_bool(c, 3, in);
    fsm_send(c, in ? 4 : 5);
}
static const LxFsmActionDesc A_IN_RANGE = {"In Range", "Range", "True when a number is between Min and Max.", "{0} in {1}..{2}?",
    {P("Value", LX_FSM_FLOAT, "0"), P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "1"),
     POPT("Store Result", LX_FSM_BOOL), PEV("In Range Event"), PEV("Out Of Range Event")},
    0, 0, LX_FSM_ONCE, rg_in_range, rg_in_range, NULL};

/* ==========================================================================================
 *  Math Utilities
 * ========================================================================================== */
static void u_is_pow2(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0);
    bool r = v > 0 && (v & (v - 1)) == 0;
    lx_fsm_set_bool(c, 1, r);
    fsm_send(c, r ? 2 : 3);
}
static const LxFsmActionDesc A_IS_POW2 = {"Is Power Of Two", "Math Utilities", "True when a whole number is 1, 2, 4, 8, 16...", "{0} power of two?",
    {P("Value", LX_FSM_INT, "8"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, u_is_pow2, u_is_pow2, NULL};

static void u_next_pow2(LxFsmCtx *c) {
    unsigned v = (unsigned)(lx_fsm_int(c, 0) < 1 ? 1 : lx_fsm_int(c, 0));
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v++;
    lx_fsm_set_int(c, 1, (int)v);
}
static const LxFsmActionDesc A_NEXT_POW2 = {"Next Power Of Two", "Math Utilities", "The next power of two at or above a whole number.", "next pow2 {0} -> {1}",
    {P("Value", LX_FSM_INT, "100"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, u_next_pow2, u_next_pow2, NULL};

static void u_closest_pow2(LxFsmCtx *c) {
    int v = lx_fsm_int(c, 0) < 1 ? 1 : lx_fsm_int(c, 0);
    int lo = 1;
    while (lo * 2 <= v && lo < (1 << 30)) lo *= 2;
    int hi = lo * 2;
    lx_fsm_set_int(c, 1, (v - lo) < (hi - v) ? lo : hi);
}
static const LxFsmActionDesc A_CLOSEST_POW2 = {"Closest Power Of Two", "Math Utilities", "The power of two nearest a whole number.", "closest pow2 {0} -> {1}",
    {P("Value", LX_FSM_INT, "100"), POUT("Store Result", LX_FSM_INT)}, 0, 0, LX_FSM_ONCE, u_closest_pow2, u_closest_pow2, NULL};

/* ==========================================================================================
 *  Interpolation
 * ========================================================================================== */
static void ip_lerp(LxFsmCtx *c) {
    float t = lx_fsm_float(c, 2);
    if (!lx_fsm_bool(c, 3)) t = lx_saturate(t);
    lx_fsm_set_float(c, 4, lx_lerpf(lx_fsm_float(c, 0), lx_fsm_float(c, 1), t));
}
static const LxFsmActionDesc A_LERP = {"Lerp", "Interpolation", "Blends between two numbers: 0 gives the first, 1 gives the second.",
    "lerp {0}..{1} at {2} -> {4}",
    {P("From", LX_FSM_FLOAT, "0"), P("To", LX_FSM_FLOAT, "1"), P("Amount", LX_FSM_FLOAT, "0.5"),
     PT("Unclamped", LX_FSM_BOOL, "false", "let Amount go outside 0..1 and carry on past the ends"),
     POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, ip_lerp, ip_lerp, NULL};

static void ip_inverse_lerp(LxFsmCtx *c) {
    float a = lx_fsm_float(c, 0), b = lx_fsm_float(c, 1);
    lx_fsm_set_float(c, 3, (b - a) != 0 ? lx_saturate((lx_fsm_float(c, 2) - a) / (b - a)) : 0);
}
static const LxFsmActionDesc A_INVERSE_LERP = {"Inverse Lerp", "Interpolation", "Where a number sits between two others, as 0..1.",
    "{2} within {0}..{1} -> {3}",
    {P("From", LX_FSM_FLOAT, "0"), P("To", LX_FSM_FLOAT, "1"), P("Value", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, ip_inverse_lerp, ip_inverse_lerp, NULL};

static void ip_lerp_angle(LxFsmCtx *c) {
    lx_fsm_set_float(c, 3, fsm_lerp_angle(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2)));
}
static const LxFsmActionDesc A_LERP_ANGLE = {"Lerp Angle", "Interpolation", "Blends between two angles the short way round.", "lerp angle {0}..{1} -> {3}",
    {P("From", LX_FSM_FLOAT, "0"), P("To", LX_FSM_FLOAT, "90"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, ip_lerp_angle, ip_lerp_angle, NULL};

static void ip_smooth_step(LxFsmCtx *c) {
    lx_fsm_set_float(c, 3, fsm_smooth_step(lx_fsm_float(c, 0), lx_fsm_float(c, 1), lx_fsm_float(c, 2)));
}
static const LxFsmActionDesc A_SMOOTH_STEP = {"Smooth Step", "Interpolation", "Like Lerp, but eases in and out instead of moving evenly.",
    "smoothstep {0}..{1} at {2} -> {3}",
    {P("From", LX_FSM_FLOAT, "0"), P("To", LX_FSM_FLOAT, "1"), P("Amount", LX_FSM_FLOAT, "0.5"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, ip_smooth_step, ip_smooth_step, NULL};

static void ip_move_towards(LxFsmCtx *c) {
    float step = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) step *= c->dt;
    float r = fsm_move_towards(lx_fsm_float(c, 0), lx_fsm_float(c, 1), step);
    lx_fsm_set_float(c, 4, r);
    if (fabsf(r - lx_fsm_float(c, 1)) < 1e-6f) fsm_send(c, 5);
}
static const LxFsmActionDesc A_MOVE_TOWARDS_F = {"Float Move Towards", "Interpolation", "Steps a number towards a target at a fixed rate, never overshooting.",
    "{0} -> {1} by {2}",
    {P("Current", LX_FSM_FLOAT, "0"), P("Target", LX_FSM_FLOAT, "1"), P("Max Delta", LX_FSM_FLOAT, "1"),
     PT("Per Second", LX_FSM_BOOL, "true", "treat Max Delta as a speed rather than a step"),
     POUT("Store Result", LX_FSM_FLOAT), PEV("Finish Event")},
    0, 0, LX_FSM_EVERY_FRAME, ip_move_towards, ip_move_towards, NULL};

static void ip_move_towards_angle(LxFsmCtx *c) {
    float cur = lx_fsm_float(c, 0), target = lx_fsm_float(c, 1);
    float step = lx_fsm_float(c, 2);
    if (lx_fsm_bool(c, 3)) step *= c->dt;
    float d = fsm_delta_angle(cur, target);
    float r = fabsf(d) <= step ? target : cur + (d > 0 ? step : -step);
    lx_fsm_set_float(c, 4, r);
}
static const LxFsmActionDesc A_MOVE_TOWARDS_ANGLE = {"Move Towards Angle", "Interpolation", "Steps an angle towards a target the short way round.",
    "{0} -> {1} by {2} deg",
    {P("Current", LX_FSM_FLOAT, "0"), P("Target", LX_FSM_FLOAT, "90"), P("Max Degrees", LX_FSM_FLOAT, "90"),
     P("Per Second", LX_FSM_BOOL, "true"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, ip_move_towards_angle, ip_move_towards_angle, NULL};

typedef struct { float vel; } FsmSpring;
static void ip_smooth_damp(LxFsmCtx *c) {
    FsmSpring *s = c->state;
    float r = fsm_smooth_damp(lx_fsm_float(c, 0), lx_fsm_float(c, 1), &s->vel, lx_fsm_float(c, 2), lx_fsm_float(c, 3), c->dt);
    lx_fsm_set_float(c, 4, r);
    lx_fsm_set_float(c, 5, s->vel);
}
static const LxFsmActionDesc A_SMOOTH_DAMP = {"Smooth Damp", "Interpolation", "Eases a number towards a target like a spring: smooth, with no overshoot. Ideal for cameras.",
    "{0} -> {1} over {2}s",
    {P("Current", LX_FSM_FLOAT, "0"), P("Target", LX_FSM_FLOAT, "1"),
     PT("Smooth Time", LX_FSM_FLOAT, "0.3", "roughly how long it takes to arrive"),
     PT("Max Speed", LX_FSM_FLOAT, "0", "0 = no limit"), POUT("Store Result", LX_FSM_FLOAT), POPT("Store Speed", LX_FSM_FLOAT)},
    sizeof(FsmSpring), 0, LX_FSM_EVERY_FRAME, ip_smooth_damp, ip_smooth_damp, NULL};

static void ip_smooth_damp_angle(LxFsmCtx *c) {
    FsmSpring *s = c->state;
    float cur = lx_fsm_float(c, 0);
    float target = cur + fsm_delta_angle(cur, lx_fsm_float(c, 1));
    lx_fsm_set_float(c, 4, fsm_smooth_damp(cur, target, &s->vel, lx_fsm_float(c, 2), lx_fsm_float(c, 3), c->dt));
}
static const LxFsmActionDesc A_SMOOTH_DAMP_ANGLE = {"Smooth Damp Angle", "Interpolation", "Eases an angle towards a target the short way round, like a spring.",
    "{0} -> {1} over {2}s",
    {P("Current", LX_FSM_FLOAT, "0"), P("Target", LX_FSM_FLOAT, "90"), P("Smooth Time", LX_FSM_FLOAT, "0.3"),
     P("Max Speed", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    sizeof(FsmSpring), 0, LX_FSM_EVERY_FRAME, ip_smooth_damp_angle, ip_smooth_damp_angle, NULL};

static void ip_map_range(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0), a0 = lx_fsm_float(c, 1), a1 = lx_fsm_float(c, 2);
    float b0 = lx_fsm_float(c, 3), b1 = lx_fsm_float(c, 4);
    float t = (a1 - a0) != 0 ? lx_saturate((v - a0) / (a1 - a0)) : 0;
    int ease = lx_fsm_int(c, 5);
    t = lx_ease(fsm_ease(ease), t);
    lx_fsm_set_float(c, 6, b0 + (b1 - b0) * t);
}
static const LxFsmActionDesc A_MAP_RANGE = {"Map Range", "Interpolation", "Moves a number from one range into another, with an easing curve applied along the way.",
    "{0} {1}..{2} -> {3}..{4}",
    {P("Value", LX_FSM_FLOAT, "0"), P("From Min", LX_FSM_FLOAT, "0"), P("From Max", LX_FSM_FLOAT, "1"),
     P("To Min", LX_FSM_FLOAT, "0"), P("To Max", LX_FSM_FLOAT, "1"), PENUM("Ease", FSM_EASES, "0"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, ip_map_range, ip_map_range, NULL};

static void ip_falloff(LxFsmCtx *c) {
    float d = lx_fsm_float(c, 0), inner = lx_fsm_float(c, 1), outer = lx_fsm_float(c, 2);
    float t;
    if (d <= inner) t = 1;
    else if (d >= outer || outer <= inner) t = 0;
    else t = 1.0f - (d - inner) / (outer - inner);
    lx_fsm_set_float(c, 3, lx_saturate(t));
}
static const LxFsmActionDesc A_FALLOFF = {"Distance Falloff", "Interpolation", "1 close up, fading to 0 at the outer distance: explosion damage, sound volume.",
    "falloff {0} ({1}..{2}) -> {3}",
    {P("Distance", LX_FSM_FLOAT, "0"), P("Inner Radius", LX_FSM_FLOAT, "1"), P("Outer Radius", LX_FSM_FLOAT, "10"),
     POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, ip_falloff, ip_falloff, NULL};

/* ==========================================================================================
 *  Noise — a small value-noise implementation with smooth interpolation (Perlin-like).
 * ========================================================================================== */
static float fsm_hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffff) / (float)0xffffff;
}
static float fsm_noise2(float x, float y) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float xf = x - (float)xi, yf = y - (float)yi;
    float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    float a = fsm_hash2(xi, yi), b = fsm_hash2(xi + 1, yi);
    float cc = fsm_hash2(xi, yi + 1), d = fsm_hash2(xi + 1, yi + 1);
    return lx_lerpf(lx_lerpf(a, b, u), lx_lerpf(cc, d, u), v);
}
static void n_perlin(LxFsmCtx *c) {
    float scale = lx_fsm_float(c, 2);
    if (scale <= 0) scale = 1;
    float n = fsm_noise2(lx_fsm_float(c, 0) / scale, lx_fsm_float(c, 1) / scale);
    lx_fsm_set_float(c, 3, lx_lerpf(lx_fsm_float(c, 4), lx_fsm_float(c, 5), n));
}
static const LxFsmActionDesc A_PERLIN = {"Perlin Noise", "Noise", "Smooth random values that change gradually: camera shake, wind, wandering, terrain.",
    "noise({0}, {1}) -> {3}",
    {P("X", LX_FSM_FLOAT, "0"), P("Y", LX_FSM_FLOAT, "0"), PT("Scale", LX_FSM_FLOAT, "1", "larger = slower, smoother change"),
     POUT("Store Result", LX_FSM_FLOAT), P("Output Min", LX_FSM_FLOAT, "0"), P("Output Max", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_EVERY_FRAME, n_perlin, n_perlin, NULL};

static void n_perlin_time(LxFsmCtx *c) {
    float t = lx_game_time(c->scene) * lx_fsm_float(c, 0);
    float n = fsm_noise2(t, lx_fsm_float(c, 1) * 37.7f);
    lx_fsm_set_float(c, 2, lx_lerpf(lx_fsm_float(c, 3), lx_fsm_float(c, 4), n));
}
static const LxFsmActionDesc A_PERLIN_TIME = {"Perlin Noise Over Time", "Noise", "A smooth random value that drifts as time passes; Seed picks a different wiggle.",
    "noise over time -> {2}",
    {P("Speed", LX_FSM_FLOAT, "1"), P("Seed", LX_FSM_FLOAT, "0"), POUT("Store Result", LX_FSM_FLOAT),
     P("Output Min", LX_FSM_FLOAT, "-1"), P("Output Max", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_EVERY_FRAME, n_perlin_time, n_perlin_time, NULL};

/* ==========================================================================================
 *  Random
 * ========================================================================================== */
static void rnd_float(LxFsmCtx *c) { lx_fsm_set_float(c, 2, lx_random_range(lx_fsm_float(c, 0), lx_fsm_float(c, 1))); }
static const LxFsmActionDesc A_RANDOM_FLOAT = {"Random Float", "Random", "A random number between Min and Max.", "random {0}..{1} -> {2}",
    {P("Min", LX_FSM_FLOAT, "0"), P("Max", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rnd_float, rnd_float, NULL};

static void rnd_int(LxFsmCtx *c) { lx_fsm_set_int(c, 2, lx_random_int(lx_fsm_int(c, 0), lx_fsm_int(c, 1))); }
static const LxFsmActionDesc A_RANDOM_INT = {"Random Int", "Random", "A random whole number from Min to Max, both included.", "random {0}..{1} -> {2}",
    {P("Min", LX_FSM_INT, "0"), P("Max", LX_FSM_INT, "10"), POUT("Store Result", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, rnd_int, rnd_int, NULL};

static void rnd_bool(LxFsmCtx *c) {
    bool r = lx_random_chance(lx_saturate(lx_fsm_float(c, 0)));
    lx_fsm_set_bool(c, 1, r);
    fsm_send(c, r ? 2 : 3);
}
static const LxFsmActionDesc A_RANDOM_BOOL = {"Random Bool", "Random", "True with the given chance (0.5 is a coin flip).", "{0} chance",
    {P("Chance", LX_FSM_FLOAT, "0.5"), POPT("Store Result", LX_FSM_BOOL), PEV("True Event"), PEV("False Event")},
    0, 0, LX_FSM_ONCE, rnd_bool, rnd_bool, NULL};

static void rnd_around(LxFsmCtx *c) {
    float v = lx_fsm_float(c, 0), spread = fabsf(lx_fsm_float(c, 1));
    lx_fsm_set_float(c, 2, lx_random_range(v - spread, v + spread));
}
static const LxFsmActionDesc A_RANDOM_AROUND = {"Random Float Around Center", "Random", "A random number within Spread of a centre value (pitch variation, scatter).",
    "{0} +/- {1} -> {2}",
    {P("Center", LX_FSM_FLOAT, "1"), P("Spread", LX_FSM_FLOAT, "0.1"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rnd_around, rnd_around, NULL};

static void rnd_gaussian(LxFsmCtx *c) {
    /* Box-Muller: most results land near the mean, a few far out. */
    float u1 = lx_maxf(1e-6f, lx_random()), u2 = lx_random();
    float z = sqrtf(-2.0f * logf(u1)) * cosf(2.0f * LX_PI * u2);
    lx_fsm_set_float(c, 2, lx_fsm_float(c, 0) + z * lx_fsm_float(c, 1));
}
static const LxFsmActionDesc A_RANDOM_GAUSSIAN = {"Random Gaussian", "Random", "A random number clustered around the mean (a bell curve), not spread evenly.",
    "gaussian {0} +/- {1} -> {2}",
    {P("Mean", LX_FSM_FLOAT, "0"), P("Standard Deviation", LX_FSM_FLOAT, "1"), POUT("Store Result", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, rnd_gaussian, rnd_gaussian, NULL};

static void rnd_in_sphere(LxFsmCtx *c) {
    LxVec3 p = lx_random_in_sphere(lx_fsm_float(c, 0));
    if (lx_fsm_bool(c, 1)) p = lx_scale3(lx_norm3(p), lx_fsm_float(c, 0));
    lx_fsm_set_vec3(c, 2, lx_add3(p, lx_fsm_vec3(c, 3)));
}
static const LxFsmActionDesc A_RANDOM_IN_SPHERE = {"Random Position In Sphere", "Random", "A random point inside (or on) a sphere around a centre.",
    "random in sphere r={0} -> {2}",
    {P("Radius", LX_FSM_FLOAT, "1"), PT("On Surface", LX_FSM_BOOL, "false", "only points on the shell"),
     POUT("Store Result", LX_FSM_VEC3), P("Center", LX_FSM_VEC3, "0 0 0")},
    0, 0, LX_FSM_ONCE, rnd_in_sphere, rnd_in_sphere, NULL};

static void rnd_in_box(LxFsmCtx *c) {
    LxVec3 cen = lx_fsm_vec3(c, 0), size = lx_fsm_vec3(c, 1);
    LxVec3 r = lx_v3(cen.x + lx_random_range(-size.x, size.x) * 0.5f,
                     cen.y + lx_random_range(-size.y, size.y) * 0.5f,
                     cen.z + lx_random_range(-size.z, size.z) * 0.5f);
    lx_fsm_set_vec3(c, 2, r);
}
static const LxFsmActionDesc A_RANDOM_IN_BOX = {"Random Position In Box", "Random", "A random point inside a box (spawn areas, scatter).",
    "random in box -> {2}",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Size", LX_FSM_VEC3, "1 1 1"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, rnd_in_box, rnd_in_box, NULL};

static void rnd_in_ring(LxFsmCtx *c) {
    float inner = lx_fsm_float(c, 1), outer = lx_fsm_float(c, 2);
    if (outer < inner) { float t = inner; inner = outer; outer = t; }
    float a = lx_random_range(0, 2.0f * LX_PI);
    /* sqrt keeps points evenly spread over the area rather than bunched at the middle */
    float r = sqrtf(lx_lerpf(inner * inner, outer * outer, lx_random()));
    LxVec3 cen = lx_fsm_vec3(c, 0);
    lx_fsm_set_vec3(c, 3, lx_v3(cen.x + cosf(a) * r, cen.y, cen.z + sinf(a) * r));
}
static const LxFsmActionDesc A_RANDOM_IN_RING = {"Random Position In Ring", "Random", "A random point on the ground in a ring around a centre: spawning around the player.",
    "random ring {1}..{2} -> {3}",
    {P("Center", LX_FSM_VEC3, "0 0 0"), P("Inner Radius", LX_FSM_FLOAT, "5"), P("Outer Radius", LX_FSM_FLOAT, "10"),
     POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, rnd_in_ring, rnd_in_ring, NULL};

static void rnd_direction(LxFsmCtx *c) {
    LxVec3 d = lx_norm3(lx_random_in_sphere(1.0f));
    if (lx_fsm_bool(c, 0)) { d.y = 0; d = lx_norm3(d); }
    if (lx_len3sq(d) < 1e-8f) d = lx_v3(0, 0, 1);
    lx_fsm_set_vec3(c, 1, d);
}
static const LxFsmActionDesc A_RANDOM_DIRECTION = {"Random Direction", "Random", "A random direction, optionally flat on the ground.", "random direction -> {1}",
    {PT("Flat", LX_FSM_BOOL, "false", "keep it level with the ground"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, rnd_direction, rnd_direction, NULL};

static void rnd_in_cone(LxFsmCtx *c) {
    LxVec3 axis = lx_norm3(lx_fsm_vec3(c, 0));
    if (lx_len3sq(axis) < 1e-8f) axis = lx_v3(0, 0, 1);
    float half = lx_clampf(lx_fsm_float(c, 1), 0, 180.0f) * LX_DEG2RAD;
    float cos_min = cosf(half);
    float z = lx_lerpf(cos_min, 1.0f, lx_random());
    float s = sqrtf(lx_maxf(0, 1 - z * z));
    float phi = lx_random_range(0, 2.0f * LX_PI);
    /* build a frame around the axis */
    LxVec3 t = fabsf(axis.x) > 0.9f ? lx_v3(0, 1, 0) : lx_v3(1, 0, 0);
    LxVec3 b1 = lx_norm3(lx_cross3(t, axis)), b2 = lx_cross3(axis, b1);
    LxVec3 d = lx_add3(lx_add3(lx_scale3(b1, s * cosf(phi)), lx_scale3(b2, s * sinf(phi))), lx_scale3(axis, z));
    lx_fsm_set_vec3(c, 2, lx_norm3(d));
}
static const LxFsmActionDesc A_RANDOM_IN_CONE = {"Random Direction In Cone", "Random", "A random direction within an angle of an axis: shotgun spread, scatter fire.",
    "cone {1} deg about {0} -> {2}",
    {P("Axis", LX_FSM_VEC3, "0 0 1"), P("Half Angle", LX_FSM_FLOAT, "15"), POUT("Store Result", LX_FSM_VEC3)},
    0, 0, LX_FSM_ONCE, rnd_in_cone, rnd_in_cone, NULL};

static void rnd_seed(LxFsmCtx *c) {
    int seed = lx_fsm_int(c, 0);
    lx_random_seed(seed != 0 ? (uint64_t)seed : (uint64_t)(lx_time() * 1000.0));
}
static const LxFsmActionDesc A_RANDOM_SEED = {"Random Seed", "Random", "Sets the random number generator's seed, so a run can be repeated exactly. 0 seeds it from the clock.",
    "seed {0}",
    {P("Seed", LX_FSM_INT, "0")}, 0, 0, LX_FSM_ONCE, rnd_seed, NULL, NULL};

static void rnd_weighted(LxFsmCtx *c) {
    float w[4], total = 0;
    for (int i = 0; i < 4; i++) { w[i] = lx_maxf(0, lx_fsm_float(c, i)); total += w[i]; }
    if (total <= 0) { lx_fsm_set_int(c, 4, 0); fsm_send(c, 5); return; }
    float r = lx_random_range(0, total), acc = 0;
    int pick = 0;
    for (int i = 0; i < 4; i++) { acc += w[i]; if (r <= acc) { pick = i; break; } }
    lx_fsm_set_int(c, 4, pick);
    fsm_send(c, 5 + pick);
}
static const LxFsmActionDesc A_RANDOM_WEIGHTED = {"Random Weighted", "Random", "Picks one of four outcomes, each with its own chance: loot tables, enemy choice.",
    "weighted pick -> {4}",
    {P("Weight 0", LX_FSM_FLOAT, "1"), P("Weight 1", LX_FSM_FLOAT, "1"), P("Weight 2", LX_FSM_FLOAT, "0"), P("Weight 3", LX_FSM_FLOAT, "0"),
     POPT("Store Index", LX_FSM_INT), PEV("Event 0"), PEV("Event 1"), PEV("Event 2"), PEV("Event 3")},
    0, 0, LX_FSM_ONCE, rnd_weighted, rnd_weighted, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_math(void) {
    static const LxFsmActionDesc *all[] = {
        /* Float */
        &A_FLOAT_SET, &A_FLOAT_ADD, &A_FLOAT_ADD_CLAMPED, &A_FLOAT_ADD_WRAP, &A_FLOAT_ADD_MULTIPLE,
        &A_FLOAT_SUBTRACT, &A_FLOAT_MULTIPLY, &A_FLOAT_DIVIDE, &A_FLOAT_OPERATOR,
        &A_FLOAT_EQUALS, &A_FLOAT_COMPARE_TO, &A_FLOAT_COMPARE, &A_FLOAT_SELECT,
        &A_FLOAT_REMAP, &A_FLOAT_NORMALIZE, &A_FLOAT_PARSE, &A_FLOAT_TO_STRING,
        &A_FLOAT_ABS, &A_FLOAT_SIGN, &A_FLOAT_MIN, &A_FLOAT_MAX, &A_FLOAT_APPROX,
        &A_FLOAT_LIST_SUM, &A_FLOAT_LIST_MIN_MAX,
        &A_FLOAT_IS_NEGATIVE, &A_FLOAT_IS_POSITIVE, &A_FLOAT_IS_NAN, &A_FLOAT_IS_INF, &A_FLOAT_IS_FINITE, &A_FLOAT_LIMITS,
        /* Integer */
        &A_INT_SET, &A_INT_ADD, &A_INT_ADD_CLAMPED, &A_INT_ADD_WRAP, &A_INT_ADD_MULTIPLE,
        &A_INT_SUBTRACT, &A_INT_MULTIPLY, &A_INT_DIVIDE, &A_INT_OPERATOR,
        &A_INT_EQUALS, &A_INT_COMPARE, &A_INT_COMPARE_TO, &A_INT_SELECT,
        &A_INT_ABS, &A_INT_MIN, &A_INT_MAX, &A_INT_CLAMP, &A_INT_PARSE, &A_INT_TO_STRING,
        &A_INT_LIST_SUM, &A_INT_LIMITS,
        /* Bool */
        &A_BOOL_SET, &A_BOOL_INVERT, &A_BOOL_OPERATOR, &A_BOOL_ALL_TRUE, &A_BOOL_TEST,
        /* Enum */
        &A_ENUM_SET, &A_ENUM_EQUALS, &A_ENUM_SWITCH,
        /* Convert */
        &A_CONV_F_TO_I, &A_CONV_I_TO_F, &A_CONV_B_TO_F, &A_CONV_B_TO_I, &A_CONV_B_TO_S,
        &A_CONV_F_TO_B, &A_CONV_I_TO_B, &A_CONV_S_TO_B, &A_CONV_SECONDS,
        &A_CONV_V3_TO_V2, &A_CONV_V2_TO_V3, &A_CONV_ANY_TO_S,
        /* Trigonometry + constants */
        &A_t_sin, &A_t_cos, &A_t_tan, &A_t_asin, &A_t_acos, &A_t_atan, &A_ATAN2, &A_DELTA_ANGLE, &A_MATH_CONST,
        /* Exponents */
        &A_POW, &A_SQRT, &A_EXP, &A_LOG, &A_GAMMA,
        /* Rounding */
        &A_ROUND, &A_FLOOR, &A_CEIL, &A_ROUND_TO_INT, &A_FLOOR_TO_INT, &A_CEIL_TO_INT,
        /* Range */
        &A_CLAMP, &A_CLAMP01, &A_REPEAT, &A_PING_PONG, &A_IN_RANGE,
        /* Math Utilities */
        &A_IS_POW2, &A_NEXT_POW2, &A_CLOSEST_POW2,
        /* Interpolation */
        &A_LERP, &A_INVERSE_LERP, &A_LERP_ANGLE, &A_SMOOTH_STEP, &A_MOVE_TOWARDS_F, &A_MOVE_TOWARDS_ANGLE,
        &A_SMOOTH_DAMP, &A_SMOOTH_DAMP_ANGLE, &A_MAP_RANGE, &A_FALLOFF,
        /* Noise */
        &A_PERLIN, &A_PERLIN_TIME,
        /* Random */
        &A_RANDOM_FLOAT, &A_RANDOM_INT, &A_RANDOM_BOOL, &A_RANDOM_AROUND, &A_RANDOM_GAUSSIAN,
        &A_RANDOM_IN_SPHERE, &A_RANDOM_IN_BOX, &A_RANDOM_IN_RING, &A_RANDOM_DIRECTION, &A_RANDOM_IN_CONE,
        &A_RANDOM_SEED, &A_RANDOM_WEIGHTED,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
