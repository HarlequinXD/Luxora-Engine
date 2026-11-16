/* Inverse kinematics: posing a limb from where its end should be, rather than from joint angles.
 *
 * A leg or an arm is two bones and three joints - hip, knee, ankle - and for a target the knee can
 * reach, there is a closed-form answer: the triangle made by the two bone lengths and the distance to
 * the target fixes both angles. No iteration, no solver to tune. The only choice left is which way the
 * knee points, and that is what the pole vector decides.
 *
 * Foot grounding (lx_anim_foot_ik) is this applied to both legs against the ground under them, so a
 * walk cycle authored on the flat stops skating over a slope or sinking into a step. */
#include "model_internal.h"
#include <math.h>

/* ---------------- the grounding rules ----------------
 * These two decide what foot grounding does, and they are the whole of it: everything else is the
 * solver above. They are plain functions of numbers so they can be checked directly - the first
 * version of this got the second one backwards and sank every character into a crouch. */

/* How much of the grounding shows. A foot at or through the ground gets all of it; one in the air gets
 * less the higher it is, and none at all past the band - a stride's swinging foot must stay where the
 * animation put it. */
float lx_ik_ground_weight(float foot_above_target, float band) {
    if (!(band > 0)) return foot_above_target <= 0 ? 1.0f : 0.0f;
    if (!(foot_above_target > 0)) return 1.0f;            /* at or below the ground, NaN included */
    if (foot_above_target >= band) return 0.0f;
    return 1.0f - foot_above_target / band;
}

/* How far the hips must come down for a leg to reach. Only a leg that is short of its target asks for
 * anything: a foot that is merely in the air is not the body's problem. */
float lx_ik_hip_drop(float reach, float leg_length) {
    float usable = leg_length * 0.99f;                    /* never ask for a perfectly straight leg */
    if (!(reach > usable)) return 0;
    return reach - usable;
}

/* ---------------- two-bone IK ---------------- */
/* Angle in the triangle with sides a, b and c, opposite c. Clamped, so a target the limb cannot reach
 * straightens it rather than producing a NaN. */
static float law_of_cosines(float a, float b, float c) {
    if (a <= 1e-6f || b <= 1e-6f) return 0;
    float cosv = (a * a + b * b - c * c) / (2 * a * b);
    if (cosv > 1) cosv = 1;
    if (cosv < -1) cosv = -1;
    return acosf(cosv);
}

static LxQuat rot_between(LxVec3 from, LxVec3 to) {
    LxVec3 a = lx_norm3(from), b = lx_norm3(to);
    float d = lx_dot3(a, b);
    if (d > 0.99999f) return lx_quat_identity();
    if (d < -0.99999f) {
        /* opposite: any perpendicular axis will do */
        LxVec3 axis = fabsf(a.x) < 0.9f ? lx_v3(1, 0, 0) : lx_v3(0, 1, 0);
        axis = lx_norm3(lx_cross3(a, axis));
        return lx_quat_axis_angle(axis, 3.14159265358979f);
    }
    LxVec3 axis = lx_cross3(a, b);
    float len = lx_len3(axis);
    if (len <= 1e-9f) return lx_quat_identity();
    return lx_quat_axis_angle(lx_scale3(axis, 1.0f / len), acosf(d));
}

bool lx_ik_two_bone(LxVec3 root, LxVec3 mid, LxVec3 end, LxVec3 target, LxVec3 pole,
                    LxQuat root_world, LxQuat mid_world, float weight, LxIkResult *out) {
    if (!out) return false;
    out->root = root_world;
    out->mid = mid_world;
    if (!(weight > 0)) return false;
    if (weight > 1) weight = 1;
    float upper = lx_len3(lx_sub3(mid, root));
    float lower = lx_len3(lx_sub3(end, mid));
    if (upper <= 1e-5f || lower <= 1e-5f) return false;

    LxVec3 to_target = lx_sub3(target, root);
    float dist = lx_len3(to_target);
    if (dist <= 1e-5f) return false;
    /* never ask for a perfectly straight or fully folded limb: both lose the knee's direction */
    float max_reach = (upper + lower) * 0.999f;
    float min_reach = fabsf(upper - lower) * 1.001f + 1e-4f;
    if (dist > max_reach) dist = max_reach;
    if (dist < min_reach) dist = min_reach;
    LxVec3 dir = lx_scale3(to_target, 1.0f / lx_len3(to_target));

    /* the bend plane: the one holding the limb and the pole */
    LxVec3 pole_dir = lx_sub3(pole, root);
    LxVec3 axis = lx_cross3(dir, pole_dir);
    if (lx_len3(axis) <= 1e-6f) {
        /* the pole is in line with the target: keep the plane the limb is already in */
        axis = lx_cross3(dir, lx_sub3(mid, root));
        if (lx_len3(axis) <= 1e-6f) axis = lx_v3(0, 0, 1);
    }
    axis = lx_norm3(axis);

    /* where the joints go: the upper bone turned off the straight line by the triangle's angle */
    float at_root = law_of_cosines(upper, dist, lower);
    LxVec3 upper_dir = lx_quat_rotate(lx_quat_axis_angle(axis, -at_root), dir);
    LxVec3 want_mid = lx_add3(root, lx_scale3(upper_dir, upper));
    LxVec3 want_end = lx_add3(root, lx_scale3(dir, dist));

    /* turn each bone from where it points now to where it should point */
    LxQuat r1 = rot_between(lx_sub3(mid, root), lx_sub3(want_mid, root));
    LxQuat new_root = lx_quat_norm(lx_quat_mul(r1, root_world));
    /* the lower bone moved with its parent, so ask again from there */
    LxVec3 moved_end = lx_add3(want_mid, lx_quat_rotate(r1, lx_sub3(end, mid)));
    LxQuat r2 = rot_between(lx_sub3(moved_end, want_mid), lx_sub3(want_end, want_mid));
    LxQuat new_mid = lx_quat_norm(lx_quat_mul(r2, lx_quat_mul(r1, mid_world)));

    out->root = weight >= 1 ? new_root : lx_quat_slerp(root_world, new_root, weight);
    out->mid = weight >= 1 ? new_mid : lx_quat_slerp(mid_world, new_mid, weight);
    out->reached = lx_len3(lx_sub3(target, root)) <= (upper + lower);
    return true;
}
