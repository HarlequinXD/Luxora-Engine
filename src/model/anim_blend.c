/* Blend tree weights: how much of each child motion shows, for a given set of parameter values.
 *
 * This is pure maths on the children's positions - no skeleton, no clips, no time - which is what makes
 * it the easy half of a blend tree to be sure of. The runtime asks for the weights, evaluates each child
 * that has one into a pose, and mixes them with lx_pose_blend_add. */
#include "../lx_internal.h"
#include "anim_graph.h"
#include <math.h>

static void normalise(float *w, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) {
        if (!(w[i] > 0) || w[i] != w[i]) w[i] = 0;   /* no negative or NaN weights get out of here */
        sum += w[i];
    }
    if (!(sum > 0)) return;
    for (int i = 0; i < n; i++) w[i] /= sum;
}

/* ---------------- 1D ----------------
 * The children sit at thresholds along one parameter. Between two of them the blend is linear; outside
 * the ends the nearest child takes everything. The children need not be in order. */
static void weights_1d(const LxAnimTree *t, float x, float *w) {
    int n = t->child_count;
    int lo = -1, hi = -1;
    for (int i = 0; i < n; i++) {
        float th = t->child[i].threshold;
        if (th <= x && (lo < 0 || th > t->child[lo].threshold)) lo = i;
        if (th >= x && (hi < 0 || th < t->child[hi].threshold)) hi = i;
    }
    if (lo < 0 && hi < 0) return;
    if (lo < 0) { w[hi] = 1; return; }
    if (hi < 0) { w[lo] = 1; return; }
    if (lo == hi) { w[lo] = 1; return; }
    float a = t->child[lo].threshold, b = t->child[hi].threshold;
    float f = b > a ? (x - a) / (b - a) : 0;
    w[lo] = 1 - f;
    w[hi] = f;
}

/* ---------------- 2D: gradient bands ----------------
 * For each child i, every other child j carves away part of its influence: how far the input has moved
 * from i towards j. The child's weight is whatever influence survives all of them. Children that are
 * not near the input end up at zero, and the result is continuous as the input moves, which is what
 * keeps the animation from jumping. Positions in the (already mapped) space come in through p. */
static void gradient_band(const LxVec2 *p, int n, LxVec2 at, float *w) {
    for (int i = 0; i < n; i++) {
        float wi = 1;
        for (int j = 0; j < n; j++) {
            if (j == i) continue;
            LxVec2 ij = {p[j].x - p[i].x, p[j].y - p[i].y};
            float len2 = ij.x * ij.x + ij.y * ij.y;
            if (len2 <= 1e-12f) continue;              /* two children in the same place: neither cuts the other */
            LxVec2 ia = {at.x - p[i].x, at.y - p[i].y};
            float h = 1.0f - (ia.x * ij.x + ia.y * ij.y) / len2;
            if (h < wi) wi = h;
            if (wi <= 0) break;
        }
        w[i] = wi > 0 ? wi : 0;
    }
    normalise(w, n);
}

/* Directional trees read the children as directions with a magnitude - a locomotion rose, where "run
 * forward" and "walk forward" are the same direction at different speeds. Mapping into (angle,
 * log magnitude) before the gradient band is what makes turning and speeding up independent. */
static LxVec2 polar(LxVec2 v, float ref_mag) {
    float mag = sqrtf(v.x * v.x + v.y * v.y);
    LxVec2 o;
    o.x = mag > 1e-6f ? atan2f(v.x, v.y) : 0;                         /* angle, radians */
    o.y = ref_mag > 1e-6f ? logf((mag > 1e-6f ? mag : 1e-6f) / ref_mag) : 0;   /* magnitude, relative */
    return o;
}

static void weights_2d_directional(const LxAnimTree *t, LxVec2 at, float *w) {
    int n = t->child_count;
    LxVec2 p[LX_ANIM_MAX_CHILDREN];
    float ref = 0;
    int mags = 0;
    for (int i = 0; i < n; i++) {
        LxVec2 c = {t->child[i].px, t->child[i].py};
        float m = sqrtf(c.x * c.x + c.y * c.y);
        if (m > 1e-6f) { ref += m; mags++; }
    }
    ref = mags ? ref / (float)mags : 1;   /* the typical magnitude: what "as fast as the clips" means */
    for (int i = 0; i < n; i++) {
        LxVec2 c = {t->child[i].px, t->child[i].py};
        p[i] = polar(c, ref);
        p[i].x *= 2.0f;   /* angle counts for twice as much as magnitude, so direction wins ties */
    }
    LxVec2 a = polar(at, ref);
    a.x *= 2.0f;
    /* angles wrap: shift each child onto the turn nearest the input so the band maths stays local */
    for (int i = 0; i < n; i++) {
        float two_pi = 2.0f * 3.14159265358979f * 2.0f;   /* doubled, like the angles above */
        while (p[i].x - a.x > two_pi * 0.5f) p[i].x -= two_pi;
        while (a.x - p[i].x > two_pi * 0.5f) p[i].x += two_pi;
    }
    gradient_band(p, n, a, w);
}

static void weights_2d_cartesian(const LxAnimTree *t, LxVec2 at, float *w) {
    int n = t->child_count;
    LxVec2 p[LX_ANIM_MAX_CHILDREN];
    for (int i = 0; i < n; i++) { p[i].x = t->child[i].px; p[i].y = t->child[i].py; }
    gradient_band(p, n, at, w);
}

/* ---------------- entry points ---------------- */
void lx_anim_tree_weights_ex(const LxAnimTree *t, LxAnimParamGet get, void *user, float *w) {
    if (!t || !w) return;
    int n = t->child_count;
    for (int i = 0; i < n; i++) w[i] = 0;
    if (n <= 0) return;
    if (n == 1 && t->type != LX_AT_DIRECT) { w[0] = 1; return; }
    if (t->type == LX_AT_DIRECT) {
        /* every child's weight is its own parameter; they are used as given (then normalised) */
        for (int i = 0; i < n; i++) {
            float v = get && t->child[i].weight_param[0] ? get(user, t->child[i].weight_param) : 0;
            w[i] = v > 0 ? v : 0;
        }
        normalise(w, n);
        return;
    }
    float x = get && t->param_x[0] ? get(user, t->param_x) : 0;
    float y = get && t->param_y[0] ? get(user, t->param_y) : 0;
    if (x != x) x = 0;   /* a NaN parameter would poison every weight */
    if (y != y) y = 0;
    if (t->type == LX_AT_1D) {
        weights_1d(t, x, w);
        normalise(w, n);
        return;
    }
    LxVec2 at = {x, y};
    if (t->type == LX_AT_2D_DIRECTIONAL) weights_2d_directional(t, at, w);
    else weights_2d_cartesian(t, at, w);
}

/* The simple form takes the two axis values directly. A Direct tree has a parameter per child rather
 * than two axes, so it has nothing to read here and shares its weight out evenly; ask through
 * lx_anim_tree_weights_ex, which can look its parameters up, to blend one for real. */
void lx_anim_tree_weights(const LxAnimTree *t, const float *param_x_y, float *w) {
    if (!t || !w) return;
    int n = t->child_count;
    for (int i = 0; i < n; i++) w[i] = 0;
    if (n <= 0) return;
    if (n == 1) { w[0] = 1; return; }
    if (t->type == LX_AT_DIRECT) {
        for (int i = 0; i < n; i++) w[i] = 1.0f / (float)n;
        return;
    }
    float x = param_x_y ? param_x_y[0] : 0, y = param_x_y ? param_x_y[1] : 0;
    if (x != x) x = 0;
    if (y != y) y = 0;
    if (t->type == LX_AT_1D) { weights_1d(t, x, w); normalise(w, n); return; }
    LxVec2 at = {x, y};
    if (t->type == LX_AT_2D_DIRECTIONAL) weights_2d_directional(t, at, w);
    else weights_2d_cartesian(t, at, w);
}
