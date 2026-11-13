/* Humanoid avatars: which bone of a rig is which part of a human body, worked out automatically from
 * the usual naming conventions (Unity, Mixamo, Unreal, Blender/Rigify, 3ds Max Biped, Maya/HumanIK),
 * and the normalised T-pose that retargeting compares rigs in. */
#include "model_internal.h"
#include <stdio.h>
#include <ctype.h>

static const char *HB_NAMES[LX_HB_COUNT] = {
    "Hips", "LeftUpperLeg", "RightUpperLeg", "LeftLowerLeg", "RightLowerLeg", "LeftFoot", "RightFoot", "Spine", "Chest", "Neck", "Head",
    "LeftShoulder", "RightShoulder", "LeftUpperArm", "RightUpperArm", "LeftLowerArm", "RightLowerArm", "LeftHand", "RightHand",
    "LeftToes", "RightToes", "LeftEye", "RightEye", "Jaw",
    "LeftThumbProximal", "LeftThumbIntermediate", "LeftThumbDistal", "LeftIndexProximal", "LeftIndexIntermediate", "LeftIndexDistal",
    "LeftMiddleProximal", "LeftMiddleIntermediate", "LeftMiddleDistal", "LeftRingProximal", "LeftRingIntermediate", "LeftRingDistal",
    "LeftLittleProximal", "LeftLittleIntermediate", "LeftLittleDistal",
    "RightThumbProximal", "RightThumbIntermediate", "RightThumbDistal", "RightIndexProximal", "RightIndexIntermediate", "RightIndexDistal",
    "RightMiddleProximal", "RightMiddleIntermediate", "RightMiddleDistal", "RightRingProximal", "RightRingIntermediate", "RightRingDistal",
    "RightLittleProximal", "RightLittleIntermediate", "RightLittleDistal",
    "UpperChest"};

const char *lx_human_bone_name(LxHumanBone b) { return b >= 0 && b < LX_HB_COUNT ? HB_NAMES[b] : ""; }

bool lx_human_bone_required(LxHumanBone b) {
    switch (b) {
    case LX_HB_HIPS: case LX_HB_SPINE: case LX_HB_HEAD:
    case LX_HB_LEFT_UPPER_LEG: case LX_HB_RIGHT_UPPER_LEG: case LX_HB_LEFT_LOWER_LEG: case LX_HB_RIGHT_LOWER_LEG:
    case LX_HB_LEFT_FOOT: case LX_HB_RIGHT_FOOT:
    case LX_HB_LEFT_UPPER_ARM: case LX_HB_RIGHT_UPPER_ARM: case LX_HB_LEFT_LOWER_ARM: case LX_HB_RIGHT_LOWER_ARM:
    case LX_HB_LEFT_HAND: case LX_HB_RIGHT_HAND: return true;
    default: return false;
    }
}

bool lx_avatar_is_valid(char avatar[LX_HB_COUNT][64], char *missing, size_t cap) {
    if (missing && cap) missing[0] = 0;
    bool ok = true;
    for (int b = 0; b < LX_HB_COUNT; b++)
        if (lx_human_bone_required((LxHumanBone)b) && !avatar[b][0]) {
            if (missing && cap) {
                if (missing[0]) lx_strcpy(missing + strlen(missing), cap - strlen(missing), ", ");
                lx_strcpy(missing + strlen(missing), cap - strlen(missing), HB_NAMES[b]);
            }
            ok = false;
        }
    return ok;
}

/* ======================================================================
 *  automatic mapping from names
 * ====================================================================== */
/* A bone name reduced to its essentials: lower case, no namespace ("mixamorig:"), no Biped prefix,
 * no separators; the side (L/R) is pulled out separately. */
typedef struct Norm { char base[64]; int side; /* 0 none, 1 left, 2 right */ } Norm;

static bool is_sep(char c) { return c == '_' || c == '.' || c == ' ' || c == '-' || c == ':' || c == '|'; }

static Norm normalise(const char *name) {
    Norm n;
    memset(&n, 0, sizeof n);
    char low[128];
    size_t k = 0;
    /* drop namespaces: "mixamorig:LeftArm" -> "LeftArm", "Armature|Hips" -> "Hips" */
    const char *s = name;
    for (const char *p = name; *p; p++) if (*p == ':' || *p == '|') s = p + 1;
    for (const char *p = s; *p && k + 1 < sizeof low; p++) low[k++] = (char)tolower((unsigned char)*p);
    low[k] = 0;
    /* Biped / HumanIK / Rigify prefixes */
    static const char *prefixes[] = {"bip001 ", "bip01 ", "bip001_", "bip01_", "bip_", "def-", "def_", "org-", "mch-", "b_", "bn_", "jnt_", "j_",
                                     "character1_", "cc_base_", NULL};
    for (int i = 0; prefixes[i]; i++)
        if (!strncmp(low, prefixes[i], strlen(prefixes[i]))) { memmove(low, low + strlen(prefixes[i]), strlen(low + strlen(prefixes[i])) + 1); break; }
    /* side markers: "left"/"right" anywhere, or an "l"/"r" token at either end ("thigh_l", "l thigh", "shoulder.L") */
    char *lp = strstr(low, "left"), *rp = strstr(low, "right");
    if (lp) { n.side = 1; memmove(lp, lp + 4, strlen(lp + 4) + 1); }
    else if (rp) { n.side = 2; memmove(rp, rp + 5, strlen(rp + 5) + 1); }
    else {
        size_t len = strlen(low);
        if (len > 2 && (low[len - 1] == 'l' || low[len - 1] == 'r') && is_sep(low[len - 2])) { n.side = low[len - 1] == 'l' ? 1 : 2; low[len - 2] = 0; }
        else if (len > 2 && (low[0] == 'l' || low[0] == 'r') && is_sep(low[1])) { n.side = low[0] == 'l' ? 1 : 2; memmove(low, low + 2, len - 1); }
        else {
            /* "_l_" in the middle (e.g. "thumb_l_01") */
            for (size_t i = 1; i + 2 < len; i++)
                if (is_sep(low[i - 1]) && (low[i] == 'l' || low[i] == 'r') && is_sep(low[i + 1])) {
                    n.side = low[i] == 'l' ? 1 : 2;
                    memmove(low + i, low + i + 2, len - i - 1);
                    break;
                }
        }
    }
    /* strip separators and leading zeros of numbers ("spine_01" -> "spine1") */
    k = 0;
    for (const char *p = low; *p && k + 1 < sizeof n.base; p++) {
        if (is_sep(*p)) continue;
        /* (but Biped's "Finger01" means finger 0, joint 1) */
        bool finger = k >= 6 && !strncmp(n.base + k - 6, "finger", 6);
        if (*p == '0' && isdigit((unsigned char)p[1]) && (k == 0 || !isdigit((unsigned char)n.base[k - 1])) && !finger) continue;
        n.base[k++] = *p;
    }
    n.base[k] = 0;
    return n;
}

typedef struct Rule { const char *names; int bone; } Rule;   /* names: '/'-separated candidates for a side-less base */

/* body parts (side-less bases; sided bones get the side from the name) */
static const Rule BODY[] = {
    {"hips/pelvis/hip/root.x/cog", LX_HB_HIPS},
    {"upleg/upperleg/thigh/thighs/legupper/uplegtwist", LX_HB_LEFT_UPPER_LEG},
    {"leg/lowerleg/calf/shin/knee/leglower/lowleg", LX_HB_LEFT_LOWER_LEG},
    {"foot/ankle", LX_HB_LEFT_FOOT},
    {"toebase/toes/toe/ball/toe0/foottoe", LX_HB_LEFT_TOES},
    {"shoulder/clavicle/collar/collarbone", LX_HB_LEFT_SHOULDER},
    {"arm/upperarm/uparm/armupper/humerus", LX_HB_LEFT_UPPER_ARM},
    {"forearm/lowerarm/elbow/armlower/lowarm", LX_HB_LEFT_LOWER_ARM},
    {"hand/wrist", LX_HB_LEFT_HAND},
    {"eye", LX_HB_LEFT_EYE},
    {"neck/neck1", LX_HB_NECK},
    {"head", LX_HB_HEAD},
    {"jaw", LX_HB_JAW},
    {"upperchest", LX_HB_UPPER_CHEST},
    {"chest", LX_HB_CHEST},
    {NULL, 0}};

static int finger_index(const char *base, int *joint) {
    /* thumb/index/middle/ring/little(pinky) + 1/2/3, or Unity's Proximal/Intermediate/Distal, or Biped's finger0..4 */
    static const char *fingers[5][4] = {{"thumb", NULL}, {"index", "pointer", NULL}, {"middle", "mid", NULL}, {"ring", NULL}, {"little", "pinky", "small", NULL}};
    const char *b = base;
    if (!strncmp(b, "hand", 4) && b[4] && !isdigit((unsigned char)b[4])) b += 4;   /* Mixamo "HandThumb1" */
    if (!strncmp(b, "f", 1) && !strncmp(b + 1, "index", 5)) b += 1;                 /* Rigify "f_index.01" */
    if (!strncmp(b, "finger", 6) && isdigit((unsigned char)b[6])) {                 /* Biped "Finger0" "Finger01" */
        int f = b[6] - '0';
        int j = isdigit((unsigned char)b[7]) ? b[7] - '0' : 0;
        if (f < 0 || f > 4 || j < 0 || j > 2) return -1;
        *joint = j;
        return f;
    }
    for (int f = 0; f < 5; f++)
        for (int k = 0; fingers[f][k]; k++) {
            size_t n = strlen(fingers[f][k]);
            if (strncmp(b, fingers[f][k], n)) continue;
            const char *rest = b + n;
            if (!strncmp(rest, "finger", 6)) rest += 6;
            if (!strcmp(rest, "proximal") || !strcmp(rest, "1") || !strcmp(rest, "01")) { *joint = 0; return f; }
            if (!strcmp(rest, "intermediate") || !strcmp(rest, "2") || !strcmp(rest, "02")) { *joint = 1; return f; }
            if (!strcmp(rest, "distal") || !strcmp(rest, "3") || !strcmp(rest, "03")) { *joint = 2; return f; }
            if (!strcmp(rest, "metacarpal") || !strcmp(rest, "0") || !strcmp(rest, "4") || !strcmp(rest, "end")) return -2;
        }
    return -1;
}

static bool matches(const char *list, const char *base) {
    char buf[160];
    lx_strcpy(buf, sizeof buf, list);
    for (char *t = strtok(buf, "/"); t; t = strtok(NULL, "/"))
        if (!strcmp(t, base)) return true;
    return false;
}

static int depth_of(const LxModel *m, int node) {
    int d = 0;
    for (int p = m->nodes[node].parent; p >= 0; p = m->nodes[p].parent) d++;
    return d;
}

static bool is_ancestor(const LxModel *m, int anc, int node) {
    for (int p = m->nodes[node].parent; p >= 0; p = m->nodes[p].parent) if (p == anc) return true;
    return false;
}

int lx_avatar_automap(const LxModel *m, char out[LX_HB_COUNT][64]) {
    for (int b = 0; b < LX_HB_COUNT; b++) out[b][0] = 0;
    if (!m) return 0;
    int found[LX_HB_COUNT];
    for (int b = 0; b < LX_HB_COUNT; b++) found[b] = -1;
    int spine[16], nspine = 0;
    for (int i = 0; i < m->bone_count; i++) {
        int node = m->bones[i];
        const char *name = m->nodes[node].name;
        /* Unity's own names first */
        for (int b = 0; b < LX_HB_COUNT; b++)
            if (!strcmp(name, HB_NAMES[b]) && found[b] < 0) found[b] = node;
        Norm n = normalise(name);
        if (!n.base[0]) continue;
        /* spine chain: spine, spine1, spine2 ... */
        if (!strncmp(n.base, "spine", 5) && n.side == 0) { if (nspine < 16) spine[nspine++] = node; continue; }
        int joint;
        int f = finger_index(n.base, &joint);
        if (f >= 0 && n.side) {
            int b = (n.side == 1 ? LX_HB_LEFT_THUMB_PROXIMAL : LX_HB_RIGHT_THUMB_PROXIMAL) + f * 3 + joint;
            if (found[b] < 0) found[b] = node;
            continue;
        }
        if (f == -2) continue;   /* metacarpals and finger tips aren't mapped */
        for (const Rule *r = BODY; r->names; r++) {
            if (!matches(r->names, n.base)) continue;
            int b = r->bone;
            bool sided = b == LX_HB_LEFT_UPPER_LEG || b == LX_HB_LEFT_LOWER_LEG || b == LX_HB_LEFT_FOOT || b == LX_HB_LEFT_TOES ||
                         b == LX_HB_LEFT_SHOULDER || b == LX_HB_LEFT_UPPER_ARM || b == LX_HB_LEFT_LOWER_ARM || b == LX_HB_LEFT_HAND ||
                         b == LX_HB_LEFT_EYE;
            if (sided) {
                if (!n.side) break;
                if (n.side == 2) b += 1;   /* every Left* is followed by its Right* */
            } else if (n.side) break;
            if (found[b] < 0) found[b] = node;
            break;
        }
    }
    /* Biped/HumanIK spine names and Mixamo "Spine/Spine1/Spine2": order by depth, then
       Spine = first, Chest = second, UpperChest = last of 3+ */
    for (int i = 0; i < nspine; i++)
        for (int j = i + 1; j < nspine; j++)
            if (depth_of(m, spine[j]) < depth_of(m, spine[i])) { int t = spine[i]; spine[i] = spine[j]; spine[j] = t; }
    if (nspine >= 1 && found[LX_HB_SPINE] < 0) found[LX_HB_SPINE] = spine[0];
    if (nspine == 2 && found[LX_HB_CHEST] < 0) found[LX_HB_CHEST] = spine[1];
    if (nspine >= 3) {
        if (found[LX_HB_CHEST] < 0) found[LX_HB_CHEST] = spine[nspine >= 5 ? 2 : 1];
        if (found[LX_HB_UPPER_CHEST] < 0) found[LX_HB_UPPER_CHEST] = spine[nspine - 1];
    }
    /* sanity: limbs must hang below the hips/each other the right way round */
    static const int chains[][2] = {{LX_HB_LEFT_UPPER_LEG, LX_HB_LEFT_LOWER_LEG}, {LX_HB_LEFT_LOWER_LEG, LX_HB_LEFT_FOOT},
                                    {LX_HB_RIGHT_UPPER_LEG, LX_HB_RIGHT_LOWER_LEG}, {LX_HB_RIGHT_LOWER_LEG, LX_HB_RIGHT_FOOT},
                                    {LX_HB_LEFT_UPPER_ARM, LX_HB_LEFT_LOWER_ARM}, {LX_HB_LEFT_LOWER_ARM, LX_HB_LEFT_HAND},
                                    {LX_HB_RIGHT_UPPER_ARM, LX_HB_RIGHT_LOWER_ARM}, {LX_HB_RIGHT_LOWER_ARM, LX_HB_RIGHT_HAND}};
    for (size_t i = 0; i < sizeof chains / sizeof chains[0]; i++) {
        int a = found[chains[i][0]], b = found[chains[i][1]];
        if (a >= 0 && b >= 0 && !is_ancestor(m, a, b)) found[chains[i][1]] = -1;
    }
    int n = 0;
    for (int b = 0; b < LX_HB_COUNT; b++)
        if (found[b] >= 0) { lx_strcpy(out[b], 64, m->nodes[found[b]].name); n++; }
    return n;
}

/* ======================================================================
 *  normalised T-pose (what retargeting compares rigs in)
 * ====================================================================== */
/* rotation turning unit vector a onto unit vector b */
static LxQuat from_to(LxVec3 a, LxVec3 b) {
    float d = lx_dot3(a, b);
    if (d > 0.99999f) return lx_quat_identity();
    if (d < -0.99999f) {
        LxVec3 axis = lx_cross3(lx_v3(1, 0, 0), a);
        if (lx_len3sq(axis) < 1e-6f) axis = lx_cross3(lx_v3(0, 1, 0), a);
        return lx_quat_axis_angle(lx_norm3(axis), LX_PI);
    }
    LxVec3 c = lx_cross3(a, b);
    LxQuat q = {c.x, c.y, c.z, 1.0f + d};
    return lx_quat_norm(q);
}

void lx_model_prepare_avatar(LxModel *m) {
    m->humanoid_ok = false;
    for (int b = 0; b < LX_HB_COUNT; b++) m->avatar[b] = lx_model_find_node(m, m->imp.avatar[b]);
    for (int b = 0; b < LX_HB_COUNT; b++)
        if (lx_human_bone_required((LxHumanBone)b) && m->avatar[b] < 0) return;
    int n = m->node_count;
    LxVec3 *wp = lx_alloc(sizeof(LxVec3) * (size_t)n);
    LxQuat *wr = lx_alloc(sizeof(LxQuat) * (size_t)n);
    memcpy(wp, m->rest_wpos, sizeof(LxVec3) * (size_t)n);
    memcpy(wr, m->rest_wrot, sizeof(LxQuat) * (size_t)n);
    int *hb = m->avatar;
    /* canonical frame: up = hips -> head, left = right leg -> left leg, forward = left x up */
    LxVec3 up = lx_norm3(lx_sub3(wp[hb[LX_HB_HEAD]], wp[hb[LX_HB_HIPS]]));
    LxVec3 left = lx_sub3(wp[hb[LX_HB_LEFT_UPPER_LEG]], wp[hb[LX_HB_RIGHT_UPPER_LEG]]);
    left = lx_norm3(lx_sub3(left, lx_scale3(up, lx_dot3(left, up))));
    LxVec3 fwd = lx_cross3(left, up);
    /* model -> canonical: left -> +X, up -> +Y, forward -> +Z */
    LxQuat model_from_canon = lx_quat_from_basis(left, up, fwd);
    m->to_canon = lx_quat_conj(model_from_canon);
    for (int i = 0; i < n; i++) {
        wp[i] = lx_quat_rotate(m->to_canon, wp[i]);
        wr[i] = lx_quat_norm(lx_quat_mul(m->to_canon, wr[i]));
    }
    /* straighten limbs into a T-pose: each bone is turned (with everything below it) so it points
       where a T-pose points it: arms out sideways, legs down, spine and neck up */
    struct { int bone, child; LxVec3 dir; } aim[] = {
        {LX_HB_SPINE, LX_HB_NECK, {0, 1, 0}},
        {LX_HB_NECK, LX_HB_HEAD, {0, 1, 0}},
        {LX_HB_LEFT_UPPER_LEG, LX_HB_LEFT_LOWER_LEG, {0, -1, 0}}, {LX_HB_LEFT_LOWER_LEG, LX_HB_LEFT_FOOT, {0, -1, 0}},
        {LX_HB_RIGHT_UPPER_LEG, LX_HB_RIGHT_LOWER_LEG, {0, -1, 0}}, {LX_HB_RIGHT_LOWER_LEG, LX_HB_RIGHT_FOOT, {0, -1, 0}},
        {LX_HB_LEFT_SHOULDER, LX_HB_LEFT_UPPER_ARM, {1, 0, 0}}, {LX_HB_RIGHT_SHOULDER, LX_HB_RIGHT_UPPER_ARM, {-1, 0, 0}},
        {LX_HB_LEFT_UPPER_ARM, LX_HB_LEFT_LOWER_ARM, {1, 0, 0}}, {LX_HB_LEFT_LOWER_ARM, LX_HB_LEFT_HAND, {1, 0, 0}},
        {LX_HB_RIGHT_UPPER_ARM, LX_HB_RIGHT_LOWER_ARM, {-1, 0, 0}}, {LX_HB_RIGHT_LOWER_ARM, LX_HB_RIGHT_HAND, {-1, 0, 0}},
        {LX_HB_LEFT_HAND, LX_HB_LEFT_MIDDLE_PROXIMAL, {1, 0, 0}}, {LX_HB_RIGHT_HAND, LX_HB_RIGHT_MIDDLE_PROXIMAL, {-1, 0, 0}},
    };
    for (size_t a = 0; a < sizeof aim / sizeof aim[0]; a++) {
        int b = hb[aim[a].bone], c = hb[aim[a].child];
        if (b < 0 || c < 0) continue;
        LxVec3 d = lx_sub3(wp[c], wp[b]);
        if (lx_len3sq(d) < 1e-10f) continue;
        if (aim[a].bone == LX_HB_SPINE || aim[a].bone == LX_HB_NECK) continue;   /* spines keep their natural curve */
        LxQuat q = from_to(lx_norm3(d), aim[a].dir);
        /* rotate b's subtree about b */
        for (int i = 0; i < n; i++) {
            if (i != b && !is_ancestor(m, b, i)) continue;
            wp[i] = lx_add3(wp[b], lx_quat_rotate(q, lx_sub3(wp[i], wp[b])));
            wr[i] = lx_quat_norm(lx_quat_mul(q, wr[i]));
        }
    }
    for (int b = 0; b < LX_HB_COUNT; b++) m->tpose[b] = hb[b] >= 0 ? wr[hb[b]] : lx_quat_identity();
    float foot = 0.5f * (m->rest_wpos[hb[LX_HB_LEFT_FOOT]].y + m->rest_wpos[hb[LX_HB_RIGHT_FOOT]].y);
    m->hips_rest = m->rest_wpos[hb[LX_HB_HIPS]];
    m->hips_height = lx_maxf(1e-3f, lx_dot3(lx_sub3(m->hips_rest, lx_v3(m->hips_rest.x, foot, m->hips_rest.z)), lx_v3(0, 1, 0)));
    /* the hips height in canonical space (feet to hips along up) */
    LxVec3 hc = lx_quat_rotate(m->to_canon, m->hips_rest);
    LxVec3 fc = lx_quat_rotate(m->to_canon, lx_scale3(lx_add3(m->rest_wpos[hb[LX_HB_LEFT_FOOT]], m->rest_wpos[hb[LX_HB_RIGHT_FOOT]]), 0.5f));
    m->hips_height = lx_maxf(1e-3f, hc.y - fc.y);
    lx_free(wp);
    lx_free(wr);
    m->humanoid_ok = true;
}
