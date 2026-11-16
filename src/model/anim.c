/* Animation: the Animator component plays clips on a model's skeleton (bones are ordinary objects),
 * cross-fading between clips. Generic rigs play clips bone-by-name; humanoid rigs retarget through
 * their avatars. Skinned meshes are then deformed to follow their bones (on the CPU, in parallel). */
#include "model_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* ======================================================================
 *  clips
 * ====================================================================== */
typedef struct Binding {
    LxModel *src;          /* the model the clip belongs to */
    ModelClip *clip;
    bool humanoid;         /* retarget through avatars */
    int *track_of;         /* direct: target node -> clip track (-1 = none) */
} Binding;

/* one layer of an animator graph as it runs */
typedef struct LayerRun {
    int state;             /* index into the layer's states, -1 = nothing playing */
    float norm;            /* how far through the state's motion, in cycles */
    int from;              /* the state being left: -1 not transitioning, -2 fading from a blended pose */
    float from_norm;
    float fade, fade_len;  /* seconds */
    int cur_interrupt;     /* what may interrupt the transition running now */
    float weight;          /* how much of this layer shows */
    LxPose pose, a, b;     /* its result, and the two states blended into it */
    bool *writes;          /* per node: whether this layer writes it (from its mask) */
    uint64_t mask_built;
    bool mask_valid;
} LayerRun;

typedef struct ClipBind { char ref[LX_REF_MAX]; Binding b; bool ok; } ClipBind;
typedef struct ParamVal { float f; int i; bool b; } ParamVal;
typedef struct IkGoal { LxVec3 pos; LxQuat rot; float weight, rot_weight; bool active, rot_active; } IkGoal;

typedef struct LxAnimState {
    LxModel *model;        /* the target skeleton */
    LxEntity root;
    LxEntity *ents;        /* target node -> object */
    int ent_count;
    Binding cur, prev;
    char clip_ref[LX_REF_MAX];
    float time, prev_time, fade, fade_len, speed;
    bool playing, loop, started;
    uint32_t generation;   /* models reloaded since -> rebuild (clip pointers would dangle) */
    /* out is the pose that gets applied to the bones; a and b are what gets blended into it */
    LxPose out, a, b;
    /* an animator graph, when the component names one: states, parameters, transitions, blend trees */
    LxAnimGraph *graph;
    uint32_t graph_version;
    ParamVal *pvals;       /* one per parameter of the graph */
    int pval_count;
    LayerRun *layers;
    int layer_count;
    ClipBind *binds;       /* clips bound once and kept (binding walks every bone by name) */
    int bind_count, bind_cap;
    LxPose scratch[LX_ANIM_MAX_DEPTH + 1];   /* blend trees, one per level of nesting */
    /* inverse kinematics */
    IkGoal ik[LX_IK_GOAL_COUNT];
    LxVec3 look_at;
    float look_weight;
    bool look_active;
    float hip_drop;        /* foot grounding: how far the hips are eased down */
    float ankle_rest[2];   /* how high each ankle sits above the sole, from the model's rest pose */
    bool ankles_measured;
    /* root motion: where the clip's root was last frame, and the step it took */
    LxVec3 rm_prev_pos, rm_last_step;
    float rm_prev_yaw, rm_last_yaw_step;
    bool rm_have_prev;
    bool rm_wrapped;       /* the clip looped or the state changed: this frame's jump is not movement */
    float rm_prev_norm;    /* the single-clip path's place in its cycle last frame */
    bool rm_started_clip;
    LxVec3 rm_delta;       /* what the playing motions moved the root by this frame, mixed by weight */
    float rm_delta_yaw;
    bool rm_delta_ok;      /* nothing could be measured: there is no movement to give away */
    LxClipSettings rm_settings;   /* what was in force last frame, for the component to show */
    char rm_clip[LX_REF_MAX];
    int rm_root_node;
} LxAnimState;

typedef struct LxSkinState {
    LxMesh *src;           /* the imported (bind pose) mesh */
    LxMesh *mesh;          /* deformed copy that gets drawn */
    LxEntity *bones;
    int bone_count;
    LxMat4 *palette;
    LxMat4 last_world;
    uint64_t last_hash;
    uint32_t generation;
} LxSkinState;

static void binding_free(Binding *b) { lx_free(b->track_of); memset(b, 0, sizeof *b); }

/* ======================================================================
 *  poses and weighted blending
 * ====================================================================== */
static LxQuat quat4(float x, float y, float z, float w) { LxQuat q = {x, y, z, w}; return q; }

bool lx_pose_alloc(LxPose *p, int nodes) {
    size_t n = (size_t)(nodes > 0 ? nodes : 1);
    memset(p, 0, sizeof *p);
    p->pos = lx_alloc(sizeof(LxVec3) * n);
    p->rot = lx_alloc(sizeof(LxQuat) * n);
    p->scl = lx_alloc(sizeof(LxVec3) * n);
    if (!p->pos || !p->rot || !p->scl) { lx_pose_free(p); return false; }
    p->count = nodes > 0 ? nodes : 0;
    return true;
}

void lx_pose_free(LxPose *p) {
    if (!p) return;
    lx_free(p->pos);
    lx_free(p->rot);
    lx_free(p->scl);
    memset(p, 0, sizeof *p);
}

void lx_pose_rest(LxPose *p, const LxModel *m) {
    if (!p || !m) return;
    int n = p->count < m->node_count ? p->count : m->node_count;
    for (int i = 0; i < n; i++) {
        p->pos[i] = m->nodes[i].pos;
        p->rot[i] = m->nodes[i].rot;
        p->scl[i] = m->nodes[i].scale;
    }
}

void lx_pose_copy(LxPose *dst, const LxPose *src) {
    if (!dst || !src) return;
    int n = dst->count < src->count ? dst->count : src->count;
    for (int i = 0; i < n; i++) {
        dst->pos[i] = src->pos[i];
        dst->rot[i] = src->rot[i];
        dst->scl[i] = src->scl[i];
    }
}

void lx_pose_blend_begin(LxPose *acc, float *total) {
    LX_UNUSED(acc);
    if (total) *total = 0;
}

void lx_pose_blend_add(LxPose *acc, const LxPose *in, float weight, float *total) {
    if (!acc || !in || !total || !(weight > 0) || weight != weight) return;   /* ignore zero, negative and NaN */
    int n = acc->count < in->count ? acc->count : in->count;
    bool first = !(*total > 0);
    for (int i = 0; i < n; i++) {
        LxQuat q = in->rot[i];
        if (first) {
            acc->pos[i] = lx_scale3(in->pos[i], weight);
            acc->scl[i] = lx_scale3(in->scl[i], weight);
            acc->rot[i] = quat4(q.x * weight, q.y * weight, q.z * weight, q.w * weight);
        } else {
            acc->pos[i] = lx_madd3(acc->pos[i], in->pos[i], weight);
            acc->scl[i] = lx_madd3(acc->scl[i], in->scl[i], weight);
            /* align the incoming rotation with what has accumulated: q and -q are the same rotation,
               and adding the wrong one blends the long way round */
            LxQuat a = acc->rot[i];
            if (a.x * q.x + a.y * q.y + a.z * q.z + a.w * q.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
            acc->rot[i] = quat4(a.x + q.x * weight, a.y + q.y * weight, a.z + q.z * weight, a.w + q.w * weight);
        }
    }
    *total += weight;
}

void lx_pose_blend_end(LxPose *acc, float total, const LxPose *fallback) {
    if (!acc) return;
    if (!(total > 0)) {
        if (fallback) lx_pose_copy(acc, fallback);
        return;
    }
    float inv = 1.0f / total;
    for (int i = 0; i < acc->count; i++) {
        acc->pos[i] = lx_scale3(acc->pos[i], inv);
        acc->scl[i] = lx_scale3(acc->scl[i], inv);
        acc->rot[i] = lx_quat_norm(acc->rot[i]);   /* normalising absorbs the weight sum */
    }
}

static bool same_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if ((*a | 32) != (*b | 32)) return false;
    return !*a && !*b;
}
static ModelClip *find_clip(LxModel *m, const char *ref) {
    const char *h = strchr(ref, '#');
    if (!m || !m->clip_count) return NULL;
    if (!h || !h[1]) return &m->clips[0];
    const char *name = h + 1;
    for (int i = 0; i < m->clip_count; i++) if (!strcmp(m->clips[i].name, name)) return &m->clips[i];
    /* forgiving: "run" for "Run", and "Run" for "Armature|Run" (Blender / Mixamo take names) */
    for (int i = 0; i < m->clip_count; i++) if (same_ci(m->clips[i].name, name)) return &m->clips[i];
    for (int i = 0; i < m->clip_count; i++) {
        const char *bar = strrchr(m->clips[i].name, '|');
        if (bar && same_ci(bar + 1, name)) return &m->clips[i];
    }
    char *end;
    long idx = strtol(name, &end, 10);
    if (!*end && idx >= 0 && idx < m->clip_count) return &m->clips[idx];
    return NULL;
}

static bool bind_clip(Binding *b, LxModel *target, const char *clip_ref) {
    memset(b, 0, sizeof *b);
    /* "Models/Hero.fbx#Run" names a model's clip; a bare "Run" (or "#Run") is a clip of the character's own model */
    const char *h = strchr(clip_ref, '#');
    bool has_file = h ? h != clip_ref : strchr(clip_ref, '.') != NULL || strchr(clip_ref, '/') != NULL;
    LxModel *src = has_file ? lx_asset_model(clip_ref) : NULL;
    ModelClip *clip = src ? find_clip(src, clip_ref) : NULL;
    if (!clip && target) {
        char own[LX_REF_MAX * 2];
        snprintf(own, sizeof own, "%s#%s", target->ref, h ? h + 1 : clip_ref);
        src = target;
        clip = find_clip(target, own);
    }
    if (!src || !clip || !target) return false;
    b->src = src;
    b->clip = clip;
    /* humanoid retargeting when both sides have avatars (and they aren't the same skeleton) */
    if (src != target && src->imp.rig == LX_RIG_HUMANOID && target->imp.rig == LX_RIG_HUMANOID && src->humanoid_ok && target->humanoid_ok) {
        b->humanoid = true;
        return true;
    }
    b->track_of = lx_alloc(sizeof(int) * (size_t)(target->node_count ? target->node_count : 1));
    int hits = 0;
    for (int i = 0; i < target->node_count; i++) {
        b->track_of[i] = -1;
        for (int t = 0; t < clip->tracks; t++)
            if (!strcmp(src->nodes[clip->node[t]].name, target->nodes[i].name)) { b->track_of[i] = t; hits++; break; }
    }
    if (!hits) {
        LX_WARN("Clip %s has no bones in common with %s (generic rigs need matching bone names; set both to Humanoid to retarget)",
                clip_ref, target->ref);
        binding_free(b);
        return false;
    }
    return true;
}

/* one clip sample (track t at time) */
static void sample(const ModelClip *c, int t, float time, bool loop, LxVec3 *p, LxQuat *q, LxVec3 *s) {
    float f = time * c->fps;
    int last = c->frames - 1;
    if (last <= 0 || !(time == time)) { f = 0; loop = false; }   /* one-frame clip, or a bad time: no NaN */
    if (loop && c->length > 0 && last > 0) {
        float len = (float)last;
        f = fmodf(f, len);
        if (f < 0) f += len;
    }
    if (f < 0) f = 0;
    if (f > (float)last) f = (float)last;
    int f0 = (int)f, f1 = f0 + 1 > last ? last : f0 + 1;
    float a = f - (float)f0;
    size_t k0 = (size_t)f0 * (size_t)c->tracks + (size_t)t, k1 = (size_t)f1 * (size_t)c->tracks + (size_t)t;
    *p = lx_lerp3(c->pos[k0], c->pos[k1], a);
    *s = lx_lerp3(c->scl[k0], c->scl[k1], a);
    LxQuat q0 = c->rot[k0], q1 = c->rot[k1];
    if (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0) { q1.x = -q1.x; q1.y = -q1.y; q1.z = -q1.z; q1.w = -q1.w; }
    LxQuat r = {q0.x + (q1.x - q0.x) * a, q0.y + (q1.y - q0.y) * a, q0.z + (q1.z - q0.z) * a, q0.w + (q1.w - q0.w) * a};
    *q = lx_quat_norm(r);
}

/* The target pose (local TRS per target node) for a binding at a time.
 *
 * The clip's root movement is left in the pose here, exactly as it was authored. Taking it out - and
 * deciding whether the object gets it instead - happens once, after everything has been blended, in
 * root_motion() below; doing it here would have to be undone for every clip in a blend tree. */
static void evaluate(const Binding *b, LxModel *dst, float time, bool loop, LxVec3 *pos, LxQuat *rot, LxVec3 *scl) {
    for (int i = 0; i < dst->node_count; i++) { pos[i] = dst->nodes[i].pos; rot[i] = dst->nodes[i].rot; scl[i] = dst->nodes[i].scale; }
    if (!b->clip) return;
    if (!b->humanoid) {
        for (int i = 0; i < dst->node_count; i++)
            if (b->track_of[i] >= 0) sample(b->clip, b->track_of[i], time, loop, &pos[i], &rot[i], &scl[i]);
        return;
    }
    /* humanoid: source pose -> canonical deltas from its T-pose -> applied to the target's T-pose */
    LxModel *src = b->src;
    int n = src->node_count;
    LxVec3 *sp = lx_alloc(sizeof(LxVec3) * (size_t)n * 2);
    LxQuat *sr = lx_alloc(sizeof(LxQuat) * (size_t)n * 2);
    LxVec3 *swp = sp + n;
    LxQuat *swr = sr + n;
    for (int i = 0; i < n; i++) { sp[i] = src->nodes[i].pos; sr[i] = src->nodes[i].rot; }
    for (int t = 0; t < b->clip->tracks; t++) {
        LxVec3 s;
        int node = b->clip->node[t];
        sample(b->clip, t, time, loop, &sp[node], &sr[node], &s);
    }
    for (int i = 0; i < n; i++) {
        int p = src->nodes[i].parent;
        if (p < 0) { swp[i] = sp[i]; swr[i] = sr[i]; }
        else {
            swp[i] = lx_add3(swp[p], lx_quat_rotate(swr[p], lx_mul3(sp[i], src->nodes[p].scale)));
            swr[i] = lx_quat_norm(lx_quat_mul(swr[p], sr[i]));
        }
    }
    /* target world rotations of mapped bones */
    LxQuat target_world[LX_HB_COUNT];
    bool have[LX_HB_COUNT];
    for (int h = 0; h < LX_HB_COUNT; h++) {
        have[h] = src->avatar[h] >= 0 && dst->avatar[h] >= 0;
        if (!have[h]) continue;
        LxQuat sc = lx_quat_mul(src->to_canon, swr[src->avatar[h]]);
        LxQuat delta = lx_quat_mul(sc, lx_quat_conj(src->tpose[h]));
        LxQuat tc = lx_quat_mul(delta, dst->tpose[h]);
        target_world[h] = lx_quat_norm(lx_quat_mul(lx_quat_conj(dst->to_canon), tc));
    }
    /* hips position: the source's movement from its rest position, scaled by leg length */
    LxVec3 hips_world = dst->hips_rest;
    int sh = src->avatar[LX_HB_HIPS];
    if (sh >= 0) {
        LxVec3 off = lx_quat_rotate(src->to_canon, lx_sub3(swp[sh], src->hips_rest));
        off = lx_scale3(off, dst->hips_height / src->hips_height);
        hips_world = lx_add3(dst->hips_rest, lx_quat_rotate(lx_quat_conj(dst->to_canon), off));
    }
    /* target FK in node order, replacing mapped rotations */
    int dn = dst->node_count;
    LxQuat *dwr = lx_alloc(sizeof(LxQuat) * (size_t)dn);
    LxVec3 *dwp = lx_alloc(sizeof(LxVec3) * (size_t)dn);
    int hb_of[512];
    int *hb_of_node = dn <= 512 ? hb_of : lx_alloc(sizeof(int) * (size_t)dn);
    for (int i = 0; i < dn; i++) hb_of_node[i] = -1;
    for (int h = 0; h < LX_HB_COUNT; h++) if (dst->avatar[h] >= 0 && have[h]) hb_of_node[dst->avatar[h]] = h;
    for (int i = 0; i < dn; i++) {
        int p = dst->nodes[i].parent;
        LxQuat pw = p >= 0 ? dwr[p] : lx_quat_identity();
        LxVec3 pp = p >= 0 ? dwp[p] : lx_v3(0, 0, 0);
        int h = hb_of_node[i];
        if (h >= 0) {
            dwr[i] = target_world[h];
            rot[i] = lx_quat_norm(lx_quat_mul(lx_quat_conj(pw), dwr[i]));
        } else dwr[i] = lx_quat_norm(lx_quat_mul(pw, rot[i]));
        if (h == LX_HB_HIPS) {
            /* hips local position from its world position */
            LxVec3 local = lx_quat_rotate(lx_quat_conj(pw), lx_sub3(hips_world, pp));
            if (p >= 0) {
                LxVec3 ps = dst->nodes[p].scale;
                local = lx_v3(local.x / (ps.x ? ps.x : 1), local.y / (ps.y ? ps.y : 1), local.z / (ps.z ? ps.z : 1));
            }
            pos[i] = local;
        }
        dwp[i] = p >= 0 ? lx_add3(pp, lx_quat_rotate(pw, lx_mul3(pos[i], dst->nodes[p].scale))) : pos[i];
    }
    if (hb_of_node != hb_of) lx_free(hb_of_node);
    lx_free(dwr);
    lx_free(dwp);
    lx_free(sp);
    lx_free(sr);
}

/* ======================================================================
 *  animator runtime
 * ====================================================================== */
static LxEntity find_descendant(LxScene *s, LxEntity root, const char *name) {
    for (LxEntity c = lx_entity_first_child(s, root); c; c = lx_entity_next_sibling(s, c)) {
        if (!strcmp(lx_entity_name(s, c), name)) return c;
        LxEntity d = find_descendant(s, c, name);
        if (d) return d;
    }
    return 0;
}

static void layers_free(LxAnimState *st);
static void binds_free(LxAnimState *st);

static void state_free(LxAnimState *st) {
    if (!st) return;
    binding_free(&st->cur);
    binding_free(&st->prev);
    lx_free(st->ents);
    lx_pose_free(&st->out);
    lx_pose_free(&st->a);
    lx_pose_free(&st->b);
    for (int i = 0; i <= LX_ANIM_MAX_DEPTH; i++) lx_pose_free(&st->scratch[i]);
    layers_free(st);
    binds_free(st);
    lx_free(st->pvals);
    lx_free(st);
}

static void skin_free(LxSkinState *sk) {
    if (!sk) return;
    if (sk->mesh) {
        sk->mesh->skin = NULL;
        lx_mesh_destroy(sk->mesh);
    }
    lx_free(sk->bones);
    lx_free(sk->palette);
    lx_free(sk);
}

/* after an Animator or mesh is removed: drop what no longer applies */
void lx_anim_free_entity(LxEntityData *d) {
    if (d->anim && !(d->comps & LX_COMP_ANIMATOR)) { state_free(d->anim); d->anim = NULL; }
    if (d->skin && !(d->comps & LX_COMP_MESH)) { skin_free(d->skin); d->skin = NULL; }
}
/* the object is going away */
void lx_anim_release(LxEntityData *d) {
    state_free(d->anim);
    d->anim = NULL;
    skin_free(d->skin);
    d->skin = NULL;
}

static void sync_graph(LxScene *s, LxEntity e, LxAnimState *st);

/* the state for an animator, (re)built when its model or bones change */
static LxAnimState *state_for(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !(d->comps & LX_COMP_ANIMATOR)) return NULL;
    LxModel *m = lx_asset_model(d->animator.model);
    LxAnimState *st = d->anim;
    bool stale = !st || st->model != m || st->root != e || st->generation != lx_model_generation();
    if (st && !stale)
        for (int i = 0; i < st->ent_count; i++)
            if (st->ents[i] && !lx_entity_valid(s, st->ents[i])) { stale = true; break; }
    if (!stale) { sync_graph(s, e, st); return st; }
    char clip[LX_REF_MAX] = "";
    float time = 0;
    bool playing = false, started = false;
    if (st) { lx_strcpy(clip, sizeof clip, st->clip_ref); time = st->time; playing = st->playing; started = st->started; }
    state_free(st);
    d = lx_entity_data(s, e);
    d->anim = NULL;
    if (!m) return NULL;
    st = lx_calloc(1, sizeof *st);
    st->model = m;
    st->root = e;
    st->generation = lx_model_generation();
    st->ent_count = m->node_count;
    size_t n = (size_t)(m->node_count ? m->node_count : 1);
    st->ents = lx_calloc(n, sizeof(LxEntity));
    lx_pose_alloc(&st->out, (int)n);
    lx_pose_alloc(&st->a, (int)n);
    lx_pose_alloc(&st->b, (int)n);
    for (int i = 0; i < m->node_count; i++) st->ents[i] = find_descendant(s, e, m->nodes[i].name);
    st->speed = 1;
    st->started = started;
    d = lx_entity_data(s, e);
    d->anim = st;
    if (clip[0]) {
        lx_strcpy(st->clip_ref, sizeof st->clip_ref, clip);
        if (bind_clip(&st->cur, m, clip)) { st->time = time; st->playing = playing; }
    }
    sync_graph(s, e, st);
    return st;
}

void lx_animator_play(LxScene *s, LxEntity e, const char *clip_ref, float fade) {
    LxAnimState *st = state_for(s, e);
    if (!st || !clip_ref || !clip_ref[0]) return;
    LxEntityData *d = lx_entity_data(s, e);
    Binding nb;
    if (!bind_clip(&nb, st->model, clip_ref)) { LX_WARN("Can't play %s on %s", clip_ref, lx_entity_name(s, e)); return; }
    binding_free(&st->prev);
    if (fade > 0 && st->cur.clip) { st->prev = st->cur; st->prev_time = st->time; st->fade = 0; st->fade_len = fade; }
    else binding_free(&st->cur);
    st->cur = nb;
    st->time = 0;
    st->playing = true;
    st->started = true;
    st->loop = d->animator.loop;
    st->rm_wrapped = true;   /* a new clip: wherever its root starts is not a step from the old one */
    st->rm_prev_norm = 0;
    st->rm_started_clip = false;
    /* the clip's own loop setting from its model's import settings */
    for (int i = 0; i < nb.src->clip_count && i < LX_MAX_CLIPS; i++)
        if (&nb.src->clips[i] == nb.clip && i < nb.src->imp.clip_count) st->loop = d->animator.loop && nb.src->imp.clips[i].loop;
    lx_strcpy(st->clip_ref, sizeof st->clip_ref, clip_ref);
}

/* the object with the Animator: e itself, else the first one below it (a player object usually holds the
 * character model, and the model has the Animator) */
LxEntity lx_find_animator(LxScene *s, LxEntity e) {
    if (!e || !lx_entity_valid(s, e)) return 0;
    if (lx_get_animator(s, e)) return e;
    for (LxEntity c = lx_entity_first_child(s, e); c; c = lx_entity_next_sibling(s, c)) {
        LxEntity f = lx_find_animator(s, c);
        if (f) return f;
    }
    return 0;
}

void lx_animator_stop(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    if (d && d->anim) d->anim->playing = false;
}
bool lx_animator_playing(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && d->anim && d->anim->playing;
}
float lx_animator_time(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && d->anim ? d->anim->time : 0;
}
void lx_animator_set_time(LxScene *s, LxEntity e, float t) {
    LxAnimState *st = state_for(s, e);
    if (st) st->time = t;
}
const char *lx_animator_current_clip(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && d->anim ? d->anim->clip_ref : "";
}

static void apply_pose(LxScene *s, LxAnimState *st) {
    for (int i = 0; i < st->ent_count; i++) {
        LxEntity b = st->ents[i];
        LxEntityData *bd = b ? lx_entity_data(s, b) : NULL;
        if (!bd) continue;
        bd->position = st->out.pos[i];
        bd->rotation = st->out.rot[i];
        bd->scale = st->out.scl[i];
        bd->euler = lx_quat_to_euler_deg(st->out.rot[i]);
    }
}

/* stop and put every bone back where the model has it at rest (the editor's preview leaves no changes) */
void lx_animator_reset_pose(LxScene *s, LxEntity e) {
    LxAnimState *st = state_for(s, e);
    if (!st) return;
    st->playing = false;
    st->time = 0;
    binding_free(&st->prev);
    lx_pose_rest(&st->out, st->model);
    apply_pose(s, st);
}

/* entering play mode: every Animator starts from scratch (an editor preview may have run before) */
void lx_scene_animation_begin(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->anim) continue;
        state_free(d->anim);
        d->anim = NULL;
    }
}

/* ======================================================================
 *  animator graphs: states, parameters, transitions, blend trees, layers
 *
 *  A graph's clock is the cycle, not the second: a state's progress runs 0..1 over its motion, whatever
 *  that motion's length is. That is what lets a blend tree hold a walk and a run at different lengths
 *  and still keep their feet together, and what makes an exit time of 0.75 mean the same thing wherever
 *  it is used.
 * ====================================================================== */

/* clips are bound once and kept: binding walks every bone by name, which is not a per-frame cost */
static Binding *bind_cached(LxAnimState *st, const char *ref) {
    if (!ref || !ref[0]) return NULL;
    for (int i = 0; i < st->bind_count; i++)
        if (!strcmp(st->binds[i].ref, ref)) return st->binds[i].ok ? &st->binds[i].b : NULL;
    ClipBind cb;
    memset(&cb, 0, sizeof cb);
    lx_strcpy(cb.ref, sizeof cb.ref, ref);
    cb.ok = bind_clip(&cb.b, st->model, ref);
    LX_VEC_PUSH(st->binds, st->bind_count, st->bind_cap, cb);
    return cb.ok ? &st->binds[st->bind_count - 1].b : NULL;
}

static void binds_free(LxAnimState *st) {
    for (int i = 0; i < st->bind_count; i++) if (st->binds[i].ok) binding_free(&st->binds[i].b);
    lx_free(st->binds);
    st->binds = NULL;
    st->bind_count = st->bind_cap = 0;
}

/* ---------------- parameters ---------------- */
static int param_index(const LxAnimGraph *g, const char *name) { return lx_anim_param_index(g, name); }

static float param_float(LxAnimState *st, const char *name) {
    if (!st->graph) return 0;
    int i = param_index(st->graph, name);
    if (i < 0 || i >= st->pval_count) return 0;
    switch (st->graph->params[i].type) {
    case LX_AP_FLOAT: return st->pvals[i].f;
    case LX_AP_INT: return (float)st->pvals[i].i;
    default: return st->pvals[i].b ? 1.0f : 0.0f;
    }
}

typedef struct ParamLookup { LxAnimState *st; } ParamLookup;
static float param_lookup(void *user, const char *name) {
    ParamLookup *p = user;
    return param_float(p->st, name);
}

/* ---------------- motion: length and pose ---------------- */
static float clip_len_of(const Binding *b) {
    if (!b || !b->clip) return 0;
    return b->clip->length > 0 ? b->clip->length : 1.0f / 30.0f;
}

static float tree_length(LxAnimState *st, const LxAnimTree *t, int depth);

static float child_length(LxAnimState *st, const LxAnimChild *c, int depth) {
    float len;
    if (c->tree >= 0 && st->graph && c->tree < st->graph->tree_count && depth < LX_ANIM_MAX_DEPTH)
        len = tree_length(st, &st->graph->trees[c->tree], depth + 1);
    else len = clip_len_of(bind_cached(st, c->clip));
    float sp = c->speed > 0 ? c->speed : 1;
    return len / sp;
}

/* a blend tree is as long as its children are, weighted by how much of each shows: speeding up means
 * the whole tree's cycle shortens, which is what keeps the children in step */
static float tree_length(LxAnimState *st, const LxAnimTree *t, int depth) {
    float w[LX_ANIM_MAX_CHILDREN];
    ParamLookup pl = {st};
    lx_anim_tree_weights_ex(t, param_lookup, &pl, w);
    float len = 0, total = 0;
    for (int i = 0; i < t->child_count; i++) {
        if (!(w[i] > 0)) continue;
        len += child_length(st, &t->child[i], depth) * w[i];
        total += w[i];
    }
    if (total > 0) return len / total;
    /* nothing has any weight: fall back to the first child, so the state still has a clock */
    return t->child_count ? child_length(st, &t->child[0], depth) : 0;
}

static float state_length(LxAnimState *st, const LxAnimNode *n) {
    if (n->motion == LX_AM_TREE) {
        if (!st->graph || n->tree < 0 || n->tree >= st->graph->tree_count) return 0;
        return tree_length(st, &st->graph->trees[n->tree], 0);
    }
    return clip_len_of(bind_cached(st, n->clip));
}

/* Mirroring swaps the left and right of a pose. It needs a humanoid avatar to know which bone is which,
 * so a generic rig is left as it is (there is nothing to pair its bones by). */
static void mirror_pose(LxPose *p, const LxModel *m) {
    static const int PAIRS[][2] = {
        {LX_HB_LEFT_UPPER_LEG, LX_HB_RIGHT_UPPER_LEG}, {LX_HB_LEFT_LOWER_LEG, LX_HB_RIGHT_LOWER_LEG},
        {LX_HB_LEFT_FOOT, LX_HB_RIGHT_FOOT}, {LX_HB_LEFT_TOES, LX_HB_RIGHT_TOES},
        {LX_HB_LEFT_SHOULDER, LX_HB_RIGHT_SHOULDER}, {LX_HB_LEFT_UPPER_ARM, LX_HB_RIGHT_UPPER_ARM},
        {LX_HB_LEFT_LOWER_ARM, LX_HB_RIGHT_LOWER_ARM}, {LX_HB_LEFT_HAND, LX_HB_RIGHT_HAND},
        {LX_HB_LEFT_EYE, LX_HB_RIGHT_EYE},
    };
    if (!m || m->imp.rig != LX_RIG_HUMANOID) return;
    for (int i = 0; i < LX_ARRAY_COUNT(PAIRS); i++) {
        int a = m->avatar[PAIRS[i][0]], b = m->avatar[PAIRS[i][1]];
        if (a < 0 || b < 0 || a >= p->count || b >= p->count) continue;
        LxVec3 tp = p->pos[a];
        LxQuat tr = p->rot[a];
        LxVec3 ts = p->scl[a];
        p->pos[a] = p->pos[b];
        p->rot[a] = p->rot[b];
        p->scl[a] = p->scl[b];
        p->pos[b] = tp;
        p->rot[b] = tr;
        p->scl[b] = ts;
    }
    /* then reflect every bone across the body's own mid-plane (X, with the rig's +Z forward) */
    for (int i = 0; i < p->count; i++) {
        p->pos[i].x = -p->pos[i].x;
        p->rot[i].y = -p->rot[i].y;
        p->rot[i].z = -p->rot[i].z;
    }
}

static void motion_eval(LxAnimState *st, const LxAnimNode *n, float norm, LxPose *out, int depth);

static void tree_eval(LxAnimState *st, const LxAnimTree *t, float norm, LxPose *out, int depth) {
    float w[LX_ANIM_MAX_CHILDREN];
    ParamLookup pl = {st};
    lx_anim_tree_weights_ex(t, param_lookup, &pl, w);
    LxPose *scratch = &st->scratch[depth < LX_ANIM_MAX_DEPTH ? depth : LX_ANIM_MAX_DEPTH];
    float total;
    lx_pose_blend_begin(out, &total);
    for (int i = 0; i < t->child_count; i++) {
        if (!(w[i] > 0)) continue;
        const LxAnimChild *c = &t->child[i];
        if (c->tree >= 0 && st->graph && c->tree < st->graph->tree_count && depth + 1 < LX_ANIM_MAX_DEPTH) {
            tree_eval(st, &st->graph->trees[c->tree], norm, scratch, depth + 1);
        } else {
            Binding *b = bind_cached(st, c->clip);
            if (!b) continue;
            float len = clip_len_of(b);
            evaluate(b, st->model, norm * len, true, scratch->pos, scratch->rot, scratch->scl);
            if (c->mirror) mirror_pose(scratch, st->model);
        }
        lx_pose_blend_add(out, scratch, w[i], &total);
    }
    lx_pose_blend_end(out, total, NULL);
    if (!(total > 0)) lx_pose_rest(out, st->model);   /* nothing had any weight: the rest pose, not rubbish */
}

static void motion_eval(LxAnimState *st, const LxAnimNode *n, float norm, LxPose *out, int depth) {
    if (n->motion == LX_AM_TREE) {
        if (!st->graph || n->tree < 0 || n->tree >= st->graph->tree_count) { lx_pose_rest(out, st->model); return; }
        tree_eval(st, &st->graph->trees[n->tree], norm, out, depth);
    } else {
        Binding *b = bind_cached(st, n->clip);
        if (!b) { lx_pose_rest(out, st->model); return; }
        float len = clip_len_of(b);
        evaluate(b, st->model, norm * len, n->loop, out->pos, out->rot, out->scl);
    }
    if (n->mirror) mirror_pose(out, st->model);
}

/* ---------------- transitions ---------------- */
static bool cond_passes(LxAnimState *st, const LxAnimCond *c) {
    const LxAnimGraph *g = st->graph;
    int i = param_index(g, c->param);
    if (i < 0 || i >= st->pval_count) return false;   /* a condition on a parameter that is gone never passes */
    const LxAnimParam *p = &g->params[i];
    const ParamVal *v = &st->pvals[i];
    switch (c->mode) {
    case LX_AC_GREATER: return (p->type == LX_AP_INT ? (float)v->i : v->f) > c->value;
    case LX_AC_LESS: return (p->type == LX_AP_INT ? (float)v->i : v->f) < c->value;
    case LX_AC_EQUALS: return p->type == LX_AP_INT ? v->i == (int)c->value : v->f == c->value;
    case LX_AC_NOT_EQUAL: return p->type == LX_AP_INT ? v->i != (int)c->value : v->f != c->value;
    case LX_AC_IF: return v->b;
    case LX_AC_IF_NOT: return !v->b;
    default: return false;
    }
}

/* did progress reach the transition's exit time during this frame? */
static bool exit_time_reached(const LxAnimTransition *t, const LxAnimNode *n, float prev, float now) {
    if (!t->has_exit_time) return true;
    float et = t->exit_time;
    if (!n->loop) return now >= et;
    if (et <= 0) return true;
    if (now < et) return false;
    if (prev < et) return true;
    /* looping: the exit time comes round again every cycle, so look for the crossing, not the value */
    return floorf(now - et) > floorf(prev - et);
}

static bool trans_ready(LxAnimState *st, const LxAnimTransition *t, const LxAnimNode *from, float prev, float now) {
    if (t->mute || !t->to[0]) return false;
    for (int c = 0; c < t->cond_count; c++) if (!cond_passes(st, &t->cond[c])) return false;
    if (!t->cond_count && !t->has_exit_time) return true;      /* nothing to wait for: it fires at once */
    return exit_time_reached(t, from, prev, now);
}

/* the triggers a taken transition tested are spent */
static void consume_triggers(LxAnimState *st, const LxAnimTransition *t) {
    for (int c = 0; c < t->cond_count; c++) {
        int i = param_index(st->graph, t->cond[c].param);
        if (i >= 0 && i < st->pval_count && st->graph->params[i].type == LX_AP_TRIGGER) st->pvals[i].b = false;
    }
}

static void start_transition(LxAnimState *st, const LxAnimLayer *l, LayerRun *lr, int to, const LxAnimTransition *t) {
    const LxAnimNode *leaving = lr->state >= 0 && lr->state < l->state_count ? &l->states[lr->state] : NULL;
    float from_len = leaving ? state_length(st, leaving) : 0;
    float fade = t->fixed_duration ? t->duration : t->duration * (from_len > 0 ? from_len : 1);
    if (lr->from >= 0 && lr->fade_len > 0) {
        /* interrupting a cross-fade: what is on screen now becomes what the new one fades from */
        lx_pose_copy(&lr->b, &lr->pose);
        lr->from = -2;   /* -2: fade from the pose already blended, not from a state */
    } else lr->from = lr->state;
    lr->from_norm = lr->norm;
    lr->state = to;
    lr->norm = t->offset + (to >= 0 && to < l->state_count ? l->states[to].cycle_offset : 0);
    lr->fade = 0;
    lr->fade_len = fade > 0 ? fade : 0;
    lr->cur_interrupt = t->interrupt;
    st->rm_wrapped = true;   /* a different motion: the root is somewhere else, which is not a step */
    if (lr->fade_len <= 0) lr->from = -1;
    consume_triggers(st, t);
}

/* while any transition out of a state is solo, only the solo ones are considered */
static bool solo_blocks(const LxAnimTransition *list, int count, const LxAnimTransition *t) {
    bool any_solo = false;
    for (int i = 0; i < count; i++) if (list[i].solo && !list[i].mute) any_solo = true;
    return any_solo && !t->solo;
}

static void layer_transitions(LxAnimState *st, const LxAnimLayer *l, LayerRun *lr, float prev) {
    bool in_trans = lr->from != -1;
    bool check_src = !in_trans || lr->cur_interrupt == LX_AI_CURRENT || lr->cur_interrupt == LX_AI_BOTH;
    bool check_dst = !in_trans || lr->cur_interrupt == LX_AI_NEXT || lr->cur_interrupt == LX_AI_BOTH;
    /* Any State first: it is what a graph uses for "whatever is playing, go and die now" */
    if (!in_trans || lr->cur_interrupt != LX_AI_NONE) {
        for (int a = 0; a < l->any_count; a++) {
            const LxAnimTransition *t = &l->any[a];
            if (solo_blocks(l->any, l->any_count, t)) continue;
            int to = lx_anim_state_index(l, t->to);
            if (to < 0) continue;
            if (to == lr->state && !t->to_self) continue;
            const LxAnimNode *cur = lr->state >= 0 && lr->state < l->state_count ? &l->states[lr->state] : NULL;
            if (!cur || !trans_ready(st, t, cur, prev, lr->norm)) continue;
            start_transition(st, l, lr, to, t);
            return;
        }
    }
    if (in_trans && !check_src && !check_dst) return;
    /* the state being left may still want out of here, and the one being entered may already want out */
    int sources[2];
    int nsrc = 0;
    if (!in_trans) sources[nsrc++] = lr->state;
    else {
        if (check_src && lr->from >= 0) sources[nsrc++] = lr->from;
        if (check_dst) sources[nsrc++] = lr->state;
    }
    for (int k = 0; k < nsrc; k++) {
        int si = sources[k];
        if (si < 0 || si >= l->state_count) continue;
        const LxAnimNode *n = &l->states[si];
        float p = si == lr->state ? prev : lr->from_norm;
        float now = si == lr->state ? lr->norm : lr->from_norm;
        for (int t = 0; t < n->trans_count; t++) {
            const LxAnimTransition *tr = &n->trans[t];
            if (solo_blocks(n->trans, n->trans_count, tr)) continue;
            int to = lx_anim_state_index(l, tr->to);
            if (to < 0) continue;
            if (to == si && !tr->to_self) continue;
            if (!trans_ready(st, tr, n, p, now)) continue;
            start_transition(st, l, lr, to, tr);
            return;
        }
    }
}

/* ---------------- layers ---------------- */
/* Which nodes a layer writes. A mask names humanoid bones; each one brings its descendants with it, so
 * masking the chest gives the whole upper body without listing every finger. */
static void build_mask(LxAnimState *st, const LxAnimLayer *l, LayerRun *lr) {
    int n = st->model->node_count;
    if (!lr->writes) lr->writes = lx_calloc((size_t)(n > 0 ? n : 1), sizeof(bool));
    if (!l->mask) {
        for (int i = 0; i < n; i++) lr->writes[i] = true;
    } else {
        for (int i = 0; i < n; i++) lr->writes[i] = false;
        for (int h = 0; h < LX_HB_COUNT && h < 64; h++) {
            if (!(l->mask & (1ull << h))) continue;
            int node = st->model->avatar[h];
            if (node >= 0 && node < n) lr->writes[node] = true;
        }
        /* nodes come parent-first, so one pass hands the flag down the hierarchy */
        for (int i = 0; i < n; i++) {
            int p = st->model->nodes[i].parent;
            if (p >= 0 && p < i && lr->writes[p]) lr->writes[i] = true;
        }
    }
    lr->mask_built = l->mask;
    lr->mask_valid = true;
}

static void layer_pose(LxAnimState *st, const LxAnimLayer *l, LayerRun *lr) {
    if (lr->state < 0 || lr->state >= l->state_count) { lx_pose_rest(&lr->pose, st->model); return; }
    motion_eval(st, &l->states[lr->state], lr->norm, &lr->a, 0);
    if (lr->from != -1 && lr->fade_len > 0) {
        float w = lx_clampf(lr->fade / lr->fade_len, 0, 1);
        if (lr->from >= 0 && lr->from < l->state_count)
            motion_eval(st, &l->states[lr->from], lr->from_norm, &lr->b, 0);
        /* from == -2: lr->b already holds the pose this fade started from */
        float total;
        lx_pose_blend_begin(&lr->pose, &total);
        lx_pose_blend_add(&lr->pose, &lr->b, 1.0f - w, &total);
        lx_pose_blend_add(&lr->pose, &lr->a, w, &total);
        lx_pose_blend_end(&lr->pose, total, &lr->a);
    } else lx_pose_copy(&lr->pose, &lr->a);
}

/* layer 0 is the whole body; the rest show through by weight, over the bones their mask allows */
static void combine_layers(LxAnimState *st, const LxAnimGraph *g) {
    lx_pose_copy(&st->out, &st->layers[0].pose);
    for (int li = 1; li < st->layer_count && li < g->layer_count; li++) {
        const LxAnimLayer *l = &g->layers[li];
        LayerRun *lr = &st->layers[li];
        float w = lx_clampf(lr->weight, 0, 1);
        if (!(w > 0)) continue;
        if (!lr->mask_valid || lr->mask_built != l->mask) build_mask(st, l, lr);
        for (int i = 0; i < st->out.count && i < lr->pose.count; i++) {
            if (lr->writes && !lr->writes[i]) continue;
            if (l->blend == LX_ABM_ADDITIVE) {
                /* additive: the layer's departure from the rest pose, added on top of what is there */
                LxVec3 rest_p = st->model->nodes[i].pos;
                LxQuat rest_r = st->model->nodes[i].rot;
                st->out.pos[i] = lx_madd3(st->out.pos[i], lx_sub3(lr->pose.pos[i], rest_p), w);
                LxQuat delta = lx_quat_mul(lr->pose.rot[i], lx_quat_conj(rest_r));
                st->out.rot[i] = lx_quat_norm(lx_quat_mul(lx_quat_slerp(lx_quat_identity(), delta, w), st->out.rot[i]));
            } else {
                st->out.pos[i] = lx_lerp3(st->out.pos[i], lr->pose.pos[i], w);
                st->out.scl[i] = lx_lerp3(st->out.scl[i], lr->pose.scl[i], w);
                st->out.rot[i] = lx_quat_slerp(st->out.rot[i], lr->pose.rot[i], w);
            }
        }
    }
}

/* ---------------- binding a graph ---------------- */
static void layers_free(LxAnimState *st) {
    for (int i = 0; i < st->layer_count; i++) {
        lx_pose_free(&st->layers[i].pose);
        lx_pose_free(&st->layers[i].a);
        lx_pose_free(&st->layers[i].b);
        lx_free(st->layers[i].writes);
    }
    lx_free(st->layers);
    st->layers = NULL;
    st->layer_count = 0;
}

/* (Re)binds the animator to its graph. Parameter values and which state each layer is in survive an
 * edit where they can: changing a transition's duration while the game runs should not restart the walk. */
static void bind_graph(LxAnimState *st, LxAnimGraph *g) {
    /* remember what is playing, by name */
    char keep_state[LX_ANIM_MAX_LAYERS][LX_ANIM_NAME];
    float keep_norm[LX_ANIM_MAX_LAYERS];
    int keep = 0;
    if (st->graph && st->layers) {
        for (int i = 0; i < st->layer_count && i < LX_ANIM_MAX_LAYERS && i < st->graph->layer_count; i++) {
            const LxAnimLayer *ol = &st->graph->layers[i];
            keep_state[keep][0] = 0;
            if (st->layers[i].state >= 0 && st->layers[i].state < ol->state_count)
                lx_strcpy(keep_state[keep], LX_ANIM_NAME, ol->states[st->layers[i].state].name);
            keep_norm[keep] = st->layers[i].norm;
            keep++;
        }
    }
    ParamVal *old = st->pvals;
    int old_count = st->pval_count;
    LxAnimGraph *old_graph = st->graph;

    layers_free(st);
    binds_free(st);
    st->graph = g;
    st->graph_version = g ? g->version : 0;
    st->pvals = NULL;
    st->pval_count = 0;
    if (!g) { lx_free(old); return; }

    st->pval_count = g->param_count;
    st->pvals = lx_calloc((size_t)(g->param_count ? g->param_count : 1), sizeof(ParamVal));
    for (int i = 0; i < g->param_count; i++) {
        const LxAnimParam *p = &g->params[i];
        st->pvals[i].f = p->f;
        st->pvals[i].i = p->i;
        st->pvals[i].b = p->type == LX_AP_TRIGGER ? false : p->b;   /* a trigger always starts unset */
        /* a parameter that was already there keeps the value it had */
        if (old && old_graph)
            for (int o = 0; o < old_count && o < old_graph->param_count; o++)
                if (!strcmp(old_graph->params[o].name, p->name) && old_graph->params[o].type == p->type) {
                    st->pvals[i] = old[o];
                    break;
                }
    }
    lx_free(old);

    int n = st->model->node_count ? st->model->node_count : 1;
    st->layer_count = g->layer_count;
    st->layers = lx_calloc((size_t)(g->layer_count ? g->layer_count : 1), sizeof(LayerRun));
    for (int i = 0; i < g->layer_count; i++) {
        LayerRun *lr = &st->layers[i];
        lx_pose_alloc(&lr->pose, n);
        lx_pose_alloc(&lr->a, n);
        lx_pose_alloc(&lr->b, n);
        lx_pose_rest(&lr->pose, st->model);
        lr->weight = i == 0 ? 1.0f : g->layers[i].weight;
        lr->from = -1;
        lr->state = lx_anim_state_index(&g->layers[i], g->layers[i].start);
        lr->norm = lr->state >= 0 ? g->layers[i].states[lr->state].cycle_offset : 0;
        if (i < keep && keep_state[i][0]) {
            int s = lx_anim_state_index(&g->layers[i], keep_state[i]);
            if (s >= 0) { lr->state = s; lr->norm = keep_norm[i]; }
        }
    }
    for (int i = 0; i <= LX_ANIM_MAX_DEPTH; i++)
        if (st->scratch[i].count != n) { lx_pose_free(&st->scratch[i]); lx_pose_alloc(&st->scratch[i], n); }
}

/* The component names a graph (or stops naming one), or the graph was edited: re-bind. */
static void sync_graph(LxScene *s, LxEntity e, LxAnimState *st) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d) return;
    LxAnimGraph *g = d->animator.graph[0] ? lx_asset_anim_graph(d->animator.graph) : NULL;
    if (g == st->graph && (!g || g->version == st->graph_version)) return;
    bind_graph(st, g);
}

static int root_node_of(const LxModel *m);

/* ----------------------------------------------------------------------
 *  the root's movement, per clip
 *
 *  Root motion cannot be read off the blended pose. A blend tree moving from idle towards run slides
 *  the blended root from where one clip holds it to where the other does, and a cross-fade does the
 *  same: that slide is the blend, not the character travelling, and giving it to the object throws the
 *  character forward the moment the joystick moves.
 *
 *  So each clip's own movement is measured inside its own cycle, where there is nothing to slide, and
 *  those are mixed by the same weights as the pose. Mixing rates rather than positions is also what
 *  makes a blend tree travel at a sensible speed between its clips.
 * ---------------------------------------------------------------------- */

/* The track a clip holds for one of the target's nodes, or -1. */
static int clip_track_for(const Binding *b, int node) {
    if (!b->track_of || node < 0) return -1;
    return b->track_of[node];
}

/* Where a clip puts the root at a point in its cycle, in the model's space. */
static bool clip_root_at(LxAnimState *st, const Binding *b, float norm, LxVec3 *pos, float *yaw) {
    const LxModel *m = st->model;
    if (!b || !b->clip) return false;
    float len = clip_len_of(b);
    float t = norm * len;
    if (!b->humanoid) {
        int r = root_node_of(m);
        int track = clip_track_for(b, r);
        if (track < 0) return false;
        LxVec3 p, s;
        LxQuat q;
        sample(b->clip, track, t, false, &p, &q, &s);   /* not looped: norm 1 is the end, where a
                                                                       looped sample would wrap it back to the start */
        /* the root's own movement is local to its parent, which nothing animates: the model's space is
           that rotated by the parent's rest rotation */
        int par = m->nodes[r].parent;
        LxQuat pr = par >= 0 && m->rest_wrot ? m->rest_wrot[par] : lx_quat_identity();
        *pos = lx_quat_rotate(pr, p);
        *yaw = lx_quat_twist_y_angle(lx_quat_mul(pr, q));
        return true;
    }
    /* Humanoid: the hips of the clip's own skeleton, put through the same retarget the pose uses, so
       the travel is in the target's scale rather than the clip's. */
    LxModel *src = b->src;
    const LxModel *dst = m;
    int sh = src ? src->avatar[LX_HB_HIPS] : -1;
    if (sh < 0 || !src->humanoid_ok || !dst->humanoid_ok || src->hips_height <= 1e-6f) return false;
    int chain[16];
    int n = 0;
    for (int i = sh; i >= 0 && n < 16; i = src->nodes[i].parent) chain[n++] = i;
    LxVec3 wp = lx_v3(0, 0, 0);
    LxQuat wr = lx_quat_identity();
    for (int k = n - 1; k >= 0; k--) {
        int i = chain[k];
        LxVec3 lp = src->nodes[i].pos;
        LxQuat lq = src->nodes[i].rot;
        for (int tr = 0; tr < b->clip->tracks; tr++)
            if (b->clip->node[tr] == i) {
                LxVec3 sc;
                sample(b->clip, tr, t, false, &lp, &lq, &sc);
                break;
            }
        int par = src->nodes[i].parent;
        wp = lx_add3(wp, lx_quat_rotate(wr, par >= 0 ? lx_mul3(lp, src->nodes[par].scale) : lp));
        wr = lx_quat_norm(lx_quat_mul(wr, lq));
    }
    LxVec3 off = lx_quat_rotate(src->to_canon, lx_sub3(wp, src->hips_rest));
    off = lx_scale3(off, dst->hips_height / src->hips_height);
    *pos = lx_add3(dst->hips_rest, lx_quat_rotate(lx_quat_conj(dst->to_canon), off));
    *yaw = lx_quat_twist_y_angle(lx_quat_mul(lx_quat_conj(dst->to_canon), lx_quat_mul(src->to_canon, wr)));
    return true;
}

/* How far a clip's root moved between two points of its cycle, going forwards. A cycle that came round
 * is two pieces - to the end, then on from the start - which is the movement, where the difference of
 * the two samples would be the whole cycle backwards. */
/* the root's movement between two places inside one cycle (0 <= a <= b <= 1) */
static bool clip_root_span(LxAnimState *st, const Binding *b, float a, float c, LxVec3 *dpos, float *dyaw) {
    LxVec3 p0, p1;
    float y0, y1;
    if (!clip_root_at(st, b, a, &p0, &y0) || !clip_root_at(st, b, c, &p1, &y1)) return false;
    *dpos = lx_add3(*dpos, lx_sub3(p1, p0));
    *dyaw += lx_root_motion_turn(y0, y1, false, 0);
    return true;
}

/* `prev` and `now` count cycles: 2.25 is a quarter of the way through the third time round. Each place is
 * taken inside its own cycle and every cycle passed in between adds one whole cycle's travel. (Reading
 * 1.02 as just "0.02" made every loop of a walk look like a step of almost a whole cycle backwards: the
 * character jolted back a metre each time the clip came round, then carried on.) */
static void clip_root_delta(LxAnimState *st, const Binding *b, float prev, float now, LxVec3 *dpos, float *dyaw) {
    *dpos = lx_v3(0, 0, 0);
    *dyaw = 0;
    if (!(prev == prev) || !(now == now) || now == prev) return;
    bool back = now < prev;   /* played backwards: the same span, the other way */
    if (back) { float t = prev; prev = now; now = t; }
    float fp = floorf(prev), fn = floorf(now);
    float a = prev - fp, c = now - fn;
    if (fn - fp > 64) return;   /* a jump, not playing: the object is not flung across the level */
    if (fp == fn) clip_root_span(st, b, a, c, dpos, dyaw);
    else {
        clip_root_span(st, b, a, 1.0f, dpos, dyaw);
        LxVec3 whole = lx_v3(0, 0, 0);
        float whole_yaw = 0;
        int cycles = (int)(fn - fp) - 1;
        if (cycles > 0 && clip_root_span(st, b, 0.0f, 1.0f, &whole, &whole_yaw)) {
            *dpos = lx_madd3(*dpos, whole, (float)cycles);
            *dyaw += whole_yaw * (float)cycles;
        }
        if (c > 0) clip_root_span(st, b, 0.0f, c, dpos, dyaw);
    }
    if (back) { *dpos = lx_scale3(*dpos, -1.0f); *dyaw = -*dyaw; }
}

static void motion_root_delta(LxAnimState *st, const LxAnimNode *n, float prev, float now, LxVec3 *dpos,
                              float *dyaw, int depth);

static void tree_root_delta(LxAnimState *st, const LxAnimTree *t, float prev, float now, LxVec3 *dpos,
                            float *dyaw, int depth) {
    float w[LX_ANIM_MAX_CHILDREN];
    ParamLookup pl = {st};
    lx_anim_tree_weights_ex(t, param_lookup, &pl, w);
    *dpos = lx_v3(0, 0, 0);
    *dyaw = 0;
    for (int i = 0; i < t->child_count; i++) {
        if (!(w[i] > 0)) continue;
        const LxAnimChild *c = &t->child[i];
        LxVec3 d = lx_v3(0, 0, 0);
        float dy = 0;
        if (c->tree >= 0 && st->graph && c->tree < st->graph->tree_count && depth + 1 < LX_ANIM_MAX_DEPTH) {
            tree_root_delta(st, &st->graph->trees[c->tree], prev, now, &d, &dy, depth + 1);
        } else {
            Binding *b = bind_cached(st, c->clip);
            if (!b) continue;
            clip_root_delta(st, b, prev, now, &d, &dy);
        }
        *dpos = lx_madd3(*dpos, d, w[i]);
        *dyaw += dy * w[i];
    }
}

static void motion_root_delta(LxAnimState *st, const LxAnimNode *n, float prev, float now, LxVec3 *dpos,
                              float *dyaw, int depth) {
    *dpos = lx_v3(0, 0, 0);
    *dyaw = 0;
    if (!n) return;
    if (n->motion == LX_AM_TREE) {
        if (!st->graph || n->tree < 0 || n->tree >= st->graph->tree_count) return;
        tree_root_delta(st, &st->graph->trees[n->tree], prev, now, dpos, dyaw, depth);
        return;
    }
    Binding *b = bind_cached(st, n->clip);
    if (b) clip_root_delta(st, b, prev, now, dpos, dyaw);
}

/* ======================================================================
 *  root motion
 *
 *  A clip that walks forward moves its root bone forward. Two things can be done with that movement:
 *  leave it in the pose, where the body drifts away from the object it belongs to and its collider
 *  stays behind, or take it out of the pose and give it to the object, so the character actually
 *  travels and takes its collider with it. The second is root motion, and it is what this does.
 *
 *  Which of the root's channels travel is per clip (its import settings): a locomotion clip gives its
 *  travel to the object and keeps its hops and its turning in the pose. The Animator's Root Motion
 *  switch is above all of that - with it off, nothing moves and the character animates in place.
 * ====================================================================== */

/* The root of the skeleton: the hips of a humanoid, else the topmost bone. */
static int root_node_of(const LxModel *m) {
    if (m->avatar[LX_HB_HIPS] >= 0) return m->avatar[LX_HB_HIPS];
    for (int i = 0; i < m->node_count; i++)
        if (m->nodes[i].bone && m->nodes[i].parent < 0) return i;
    for (int i = 0; i < m->node_count; i++)
        if (m->nodes[i].parent < 0) return i;
    return -1;
}

/* The settings of the clip that is driving the root. With a graph that is the base layer's current
 * state; otherwise the one clip the animator is playing. A clip with no settings of its own travels. */
static LxClipSettings root_clip_settings(LxAnimState *st) {
    st->rm_clip[0] = 0;
    LxClipSettings out;
    memset(&out, 0, sizeof out);
    lx_clip_settings_defaults(&out);
    const char *ref = NULL;
    if (st->graph && st->layers && st->layer_count > 0 && st->graph->layer_count > 0) {
        const LxAnimLayer *l = &st->graph->layers[0];
        int si = st->layers[0].state;
        if (si >= 0 && si < l->state_count && l->states[si].motion == LX_AM_CLIP) ref = l->states[si].clip;
    } else if (st->clip_ref[0]) ref = st->clip_ref;
    if (!ref || !ref[0]) return out;
    lx_strcpy(st->rm_clip, sizeof st->rm_clip, ref);
    Binding *b = st->graph ? bind_cached(st, ref) : &st->cur;
    if (!b || !b->clip || !b->src) return out;
    for (int i = 0; i < b->src->clip_count && i < LX_MAX_CLIPS; i++)
        if (&b->src->clips[i] == b->clip && i < b->src->imp.clip_count) return b->src->imp.clips[i];
    return out;
}

/* The step the root took this frame. On the frame a clip loops, the sample jumps back to where the clip
 * starts - a long way backwards, which is not movement - so the last frame's step is used instead and
 * the travel stays continuous. */
LxVec3 lx_root_motion_step(LxVec3 prev, LxVec3 now, bool wrapped, LxVec3 last_step) {
    if (wrapped) return last_step;
    return lx_sub3(now, prev);
}

/* the same for turning, in radians, taking the short way round */
float lx_root_motion_turn(float prev, float now, bool wrapped, float last_step) {
    if (wrapped) return last_step;
    float d = now - prev;
    const float pi = 3.14159265358979f;
    while (d > pi) d -= 2 * pi;
    while (d < -pi) d += 2 * pi;
    return d;
}

/* the object that carries the character: the nearest one with a body, else the top of the hierarchy */
static LxEntity mover_of(LxScene *s, LxEntity e) {
    LxEntity top = e;
    for (LxEntity c = e; c; c = lx_entity_parent(s, c)) {
        uint32_t comps = lx_entity_components(s, c);
        if (comps & (LX_COMP_CHARACTER | LX_COMP_RIGIDBODY)) return c;
        top = c;
    }
    return top;
}

/* Where a node sits in the model's own space, by walking up to the root. The root is near the top of
 * the hierarchy, so this is a couple of steps - and it is the space root motion belongs in. A bone's
 * local axes are not the model's: an FBX rig often hangs its skeleton under a node turned -90 degrees
 * about X, and pinning "local X and Z" there would pin the model's X and *Y* and let it drift forward. */
static void node_in_model(const LxModel *m, const LxPose *p, int node, LxVec3 *out_pos, LxQuat *out_rot) {
    int chain[32];
    int n = 0;
    for (int i = node; i >= 0 && n < 32; i = m->nodes[i].parent) chain[n++] = i;
    LxVec3 pos = lx_v3(0, 0, 0);
    LxQuat rot = lx_quat_identity();
    for (int k = n - 1; k >= 0; k--) {
        int i = chain[k];
        int par = m->nodes[i].parent;
        LxVec3 lp = p->pos[i];
        pos = lx_add3(pos, lx_quat_rotate(rot, par >= 0 ? lx_mul3(lp, m->nodes[par].scale) : lp));
        rot = lx_quat_norm(lx_quat_mul(rot, p->rot[i]));
    }
    if (out_pos) *out_pos = pos;
    if (out_rot) *out_rot = rot;
}

/* the local position that puts a node at a given place in the model's space */
static LxVec3 local_for_model_pos(const LxModel *m, const LxPose *p, int node, LxVec3 want) {
    int par = m->nodes[node].parent;
    if (par < 0) return want;
    LxVec3 pp;
    LxQuat pr;
    node_in_model(m, p, par, &pp, &pr);
    LxVec3 local = lx_quat_rotate(lx_quat_conj(pr), lx_sub3(want, pp));
    LxVec3 ps = m->nodes[par].scale;
    return lx_v3(local.x / (ps.x ? ps.x : 1), local.y / (ps.y ? ps.y : 1), local.z / (ps.z ? ps.z : 1));
}

static void root_motion(LxScene *s, LxEntity e, LxAnimState *st, LxEntityData *d, float dt) {
    LX_UNUSED(dt);
    const LxModel *m = st->model;
    int r = root_node_of(m);
    if (r < 0 || r >= st->out.count) return;
    LxClipSettings cs = root_clip_settings(st);
    st->rm_settings = cs;
    st->rm_root_node = r;
    /* everything here is in the model's space, where "up" is up and "forward" is forward */
    LxVec3 raw_p;
    LxQuat raw_r;
    node_in_model(m, &st->out, r, &raw_p, &raw_r);
    LxVec3 rest_p = m->rest_wpos ? m->rest_wpos[r] : m->nodes[r].pos;
    LxQuat rest_r = m->rest_wrot ? m->rest_wrot[r] : m->nodes[r].rot;
    float yaw = lx_quat_twist_y_angle(raw_r);

    LxVec3 step = lx_v3(0, 0, 0);
    float turn = 0;
    if (st->rm_delta_ok) {
        /* measured from the clips themselves, which is the only reading a blend can be trusted with */
        step = st->rm_delta;
        turn = st->rm_delta_yaw;
    } else if (st->rm_have_prev) {
        step = lx_root_motion_step(st->rm_prev_pos, raw_p, st->rm_wrapped, st->rm_last_step);
        turn = lx_root_motion_turn(st->rm_prev_yaw, yaw, st->rm_wrapped, st->rm_last_yaw_step);
        /* A root that moves further than the character is tall in one frame has not walked there: the
           clip was changed, scrubbed, or came round again without anyone noticing. Whatever it was, it
           is not travel, and handing it to the object would fling the character across the level. */
    }
    {
        /* A root that moves further than the character is tall in one frame has not walked there. */
        float limit = m->bounds.max.y - m->bounds.min.y;
        if (!(limit > 0.1f)) limit = 2.0f;
        if (lx_len3(step) > limit) { step = lx_v3(0, 0, 0); turn = 0; }
    }
    st->rm_prev_pos = raw_p;
    st->rm_prev_yaw = yaw;
    st->rm_have_prev = true;
    st->rm_wrapped = false;
    st->rm_last_step = step;
    st->rm_last_yaw_step = turn;

    /* Take out of the pose everything that is not baked into it. A channel the object is not given
       either - because Root Motion is off - is simply pinned, which is what animating in place is. */
    LxVec3 want = raw_p;
    if (cs.travel != LX_ROOT_IN_POSE) { want.x = rest_p.x; want.z = rest_p.z; }
    if (cs.up_down != LX_ROOT_IN_POSE) want.y = rest_p.y;
    if (want.x != raw_p.x || want.y != raw_p.y || want.z != raw_p.z)
        st->out.pos[r] = local_for_model_pos(m, &st->out, r, want);
    if (cs.turn != LX_ROOT_IN_POSE) {
        /* the turn comes out in model space, and the local rotation is what is left of it there */
        LxQuat tw = lx_quat_twist_y(raw_r);
        LxQuat want_r = lx_quat_mul(lx_quat_twist_y(rest_r), lx_quat_mul(lx_quat_conj(tw), raw_r));
        int par = m->nodes[r].parent;
        if (par < 0) st->out.rot[r] = lx_quat_norm(want_r);
        else {
            LxQuat pr;
            node_in_model(m, &st->out, par, NULL, &pr);
            st->out.rot[r] = lx_quat_norm(lx_quat_mul(lx_quat_conj(pr), want_r));
        }
    }
    if (!d->animator.root_motion) return;

    LxEntity mover = mover_of(s, e);
    if (!mover || !lx_entity_valid(s, mover)) return;
    /* only a channel asked to move the object does; one that is pinned went nowhere */
    LxVec3 local = lx_v3(cs.travel == LX_ROOT_TO_OBJECT ? step.x : 0,
                         cs.up_down == LX_ROOT_TO_OBJECT ? step.y : 0,
                         cs.travel == LX_ROOT_TO_OBJECT ? step.z : 0);
    float speed = d->animator.speed;
    LX_UNUSED(speed);   /* the step is already in the pose, which the speed has moved */
    if (lx_len3(local) > 1e-9f) {
        /* the step is in the model's own space: the object's rotation and scale put it in the world */
        LxMat4 w = lx_entity_world_matrix(s, e);
        float scale = lx_len3(lx_v3(w.m[4], w.m[5], w.m[6]));
        if (!(scale > 1e-4f)) scale = 1;
        LxVec3 world = lx_scale3(lx_quat_rotate(lx_entity_world_rotation(s, e), local), scale);
        if (lx_get_character(s, mover)) lx_character_move(s, mover, world);
        else lx_entity_set_position(s, mover, lx_add3(lx_entity_position(s, mover), world));
    }
    if (cs.turn == LX_ROOT_TO_OBJECT && fabsf(turn) > 1e-7f) {
        LxQuat spin = lx_quat_axis_angle(lx_v3(0, 1, 0), turn);
        lx_entity_set_rotation(s, mover, lx_quat_norm(lx_quat_mul(spin, lx_entity_rotation(s, mover))));
    }
}

/* ---------------- inverse kinematics on the posed skeleton ---------------- */
/* Model-space transforms of every node, from a pose. Nodes come parent-first, so one pass does it. */
static void pose_fk(const LxAnimState *st, const LxPose *p, LxVec3 *wp, LxQuat *wr) {
    const LxModel *m = st->model;
    for (int i = 0; i < m->node_count; i++) {
        int par = m->nodes[i].parent;
        if (par < 0) { wp[i] = p->pos[i]; wr[i] = p->rot[i]; }
        else {
            wp[i] = lx_add3(wp[par], lx_quat_rotate(wr[par], lx_mul3(p->pos[i], m->nodes[par].scale)));
            wr[i] = lx_quat_norm(lx_quat_mul(wr[par], p->rot[i]));
        }
    }
}

/* a bone's local rotation that puts it at a world rotation, given its parent's */
static LxQuat local_from_world(LxQuat parent_world, LxQuat want_world) {
    return lx_quat_norm(lx_quat_mul(lx_quat_conj(parent_world), want_world));
}

typedef struct Limb { int root, mid, end; } Limb;

static Limb limb_of(const LxModel *m, LxIkGoal goal) {
    Limb l = {-1, -1, -1};
    switch (goal) {
    case LX_IK_LEFT_FOOT: l.root = m->avatar[LX_HB_LEFT_UPPER_LEG]; l.mid = m->avatar[LX_HB_LEFT_LOWER_LEG]; l.end = m->avatar[LX_HB_LEFT_FOOT]; break;
    case LX_IK_RIGHT_FOOT: l.root = m->avatar[LX_HB_RIGHT_UPPER_LEG]; l.mid = m->avatar[LX_HB_RIGHT_LOWER_LEG]; l.end = m->avatar[LX_HB_RIGHT_FOOT]; break;
    case LX_IK_LEFT_HAND: l.root = m->avatar[LX_HB_LEFT_UPPER_ARM]; l.mid = m->avatar[LX_HB_LEFT_LOWER_ARM]; l.end = m->avatar[LX_HB_LEFT_HAND]; break;
    default: l.root = m->avatar[LX_HB_RIGHT_UPPER_ARM]; l.mid = m->avatar[LX_HB_RIGHT_LOWER_ARM]; l.end = m->avatar[LX_HB_RIGHT_HAND]; break;
    }
    return l;
}

typedef struct IkFrame {
    LxVec3 *wp;
    LxQuat *wr;
    LxMat4 to_world;
    LxMat4 to_model;
    LxQuat root_rot, root_rot_inv;
} IkFrame;

static LxVec3 to_world_p(const IkFrame *f, LxVec3 model) { return lx_mat4_point(f->to_world, model); }
static LxVec3 to_model_p(const IkFrame *f, LxVec3 world) { return lx_mat4_point(f->to_model, world); }

/* Solves one limb towards a world-space target and writes the result back into the pose. */
/* `bend` is the way the joint should point - the character's forward, for a knee. Without one the limb
 * keeps the bend it already has, which is fine until it is nearly straight: then the midpoint sits on
 * the line between the ends, there is no bend direction left to read, and the joint can snap to either
 * side. A hint is what stops a knee turning inside out as the leg straightens. */
static void solve_limb(LxAnimState *st, IkFrame *f, Limb lb, LxVec3 target_world, float weight,
                       const LxQuat *end_rot_world, float end_rot_weight, const LxVec3 *bend) {
    if (lb.root < 0 || lb.mid < 0 || lb.end < 0 || !(weight > 0)) return;
    const LxModel *m = st->model;
    LxVec3 root_w = to_world_p(f, f->wp[lb.root]), mid_w = to_world_p(f, f->wp[lb.mid]), end_w = to_world_p(f, f->wp[lb.end]);
    LxVec3 limb = lx_sub3(end_w, root_w);
    float limb_len = lx_len3(limb);
    LxVec3 off = lx_sub3(mid_w, lx_scale3(lx_add3(root_w, end_w), 0.5f));
    /* how much of a bend there is to read: little, on a straight limb */
    float bent = limb_len > 1e-5f ? lx_len3(off) / limb_len : 0;
    LxVec3 pole;
    if (bend && bent < 0.25f) {
        float t = bent / 0.25f;   /* ease between the two, so the knee never jumps as the leg straightens */
        LxVec3 hint = lx_scale3(lx_norm3(*bend), limb_len > 1e-5f ? limb_len : 1.0f);
        pole = lx_add3(mid_w, lx_add3(lx_scale3(hint, 1.0f - t), lx_scale3(off, 2.0f * t)));
    } else {
        pole = lx_add3(mid_w, lx_scale3(off, 2.0f));
    }
    LxQuat root_rot_w = lx_quat_mul(f->root_rot, f->wr[lb.root]);
    LxQuat mid_rot_w = lx_quat_mul(f->root_rot, f->wr[lb.mid]);
    LxIkResult r;
    if (!lx_ik_two_bone(root_w, mid_w, end_w, target_world, pole, root_rot_w, mid_rot_w, weight, &r)) return;
    /* back to local: the upper bone's parent did not move, the lower bone's did */
    int rp = m->nodes[lb.root].parent;
    LxQuat root_parent_w = rp >= 0 ? lx_quat_mul(f->root_rot, f->wr[rp]) : f->root_rot;
    LxQuat new_root_local = local_from_world(root_parent_w, r.root);
    LxQuat new_mid_local = local_from_world(r.root, r.mid);
    st->out.rot[lb.root] = new_root_local;
    st->out.rot[lb.mid] = new_mid_local;
    if (end_rot_world && end_rot_weight > 0) {
        LxQuat want = lx_quat_slerp(lx_quat_mul(r.mid, st->out.rot[lb.end]), *end_rot_world, lx_clampf(end_rot_weight, 0, 1));
        st->out.rot[lb.end] = local_from_world(r.mid, want);
    }
    /* the rest of the limb moved: refresh the model-space transforms so later goals see the truth */
    pose_fk(st, &st->out, f->wp, f->wr);
}

/* How high each ankle sits above the sole. Measured once from the model's rest pose - where the
 * character stands on the ground, so the ankle's height above the lowest bone is exactly it - rather
 * than from the pose being played, where the foot rolls and the number changes every frame. */
static void measure_ankles(LxAnimState *st) {
    const LxModel *m = st->model;
    int n = m->node_count;
    st->ankles_measured = true;
    st->ankle_rest[0] = st->ankle_rest[1] = 0.08f;   /* a plausible ankle, if the rest pose says nothing */
    if (n <= 0) return;
    LxPose rest;
    if (!lx_pose_alloc(&rest, n)) return;
    lx_pose_rest(&rest, m);
    LxVec3 *wp = lx_alloc(sizeof(LxVec3) * (size_t)n);
    LxQuat *wr = lx_alloc(sizeof(LxQuat) * (size_t)n);
    if (wp && wr) {
        pose_fk(st, &rest, wp, wr);
        float sole = wp[0].y, top = wp[0].y;
        for (int i = 1; i < n; i++) {
            if (wp[i].y < sole) sole = wp[i].y;
            if (wp[i].y > top) top = wp[i].y;
        }
        /* An ankle sits a little above the sole. If the lowest bone is not a foot at all - a prop
           hanging below the rig, say - the measurement is nonsense, and a foot held a nonsense
           distance above the ground is a character floating. Keep it to what an ankle can be. */
        float limit = (top - sole) * 0.2f;
        if (!(limit > 0.02f)) limit = 0.3f;
        for (int side = 0; side < 2; side++) {
            int a = m->avatar[side == 0 ? LX_HB_LEFT_FOOT : LX_HB_RIGHT_FOOT];
            if (a < 0 || a >= n) continue;
            float h = wp[a].y - sole;
            if (h > limit) h = limit;
            if (h > 0.01f) st->ankle_rest[side] = h;
        }
    }
    lx_free(wp);
    lx_free(wr);
    lx_pose_free(&rest);
}

/* Foot grounding: a foot that is on the ground is put on the ground it is actually over, and lies along
 * it. A foot in the air is left alone - a stride has one of those half the time, and dragging it down is
 * what turns a walk into a shuffle - so the grounding fades out as the foot lifts.
 *
 * The hips come down only when a leg cannot reach what it is standing on. Dropping them by how far a
 * foot is *above* the ground instead would sink the whole body every time a foot came up. */
static void foot_ik(LxScene *s, LxEntity e, LxAnimState *st, IkFrame *f, float weight, float dt) {
    const LxModel *m = st->model;
    if (m->imp.rig != LX_RIG_HUMANOID || !m->humanoid_ok) return;
    if (!st->ankles_measured) measure_ankles(st);
    /* The collider belongs to the character, which is usually an ancestor of the object holding the
       Animator: ignoring only that object would let a foot take its own body for the ground. */
    LxEntity ignore = e;
    for (LxEntity p = lx_entity_parent(s, e); p; p = lx_entity_parent(s, p)) ignore = p;
    /* the ankle heights are in the model's own units; the object may be scaled */
    float scale = lx_len3(lx_v3(f->to_world.m[4], f->to_world.m[5], f->to_world.m[6]));
    if (!(scale > 1e-4f)) scale = 1;
    const float probe_up = 0.45f, probe_down = 1.0f;
    LxVec3 up = lx_v3(0, 1, 0);
    LxVec3 targets[2];
    LxQuat rots[2];
    float side_weight[2] = {0, 0};
    float drop = 0;
    for (int side = 0; side < 2; side++) {
        Limb lb = limb_of(m, side == 0 ? LX_IK_LEFT_FOOT : LX_IK_RIGHT_FOOT);
        if (lb.root < 0 || lb.mid < 0 || lb.end < 0) continue;
        LxVec3 foot = to_world_p(f, f->wp[lb.end]);
        LxRaycastHit hit;
        LxVec3 origin = lx_add3(foot, lx_scale3(up, probe_up));
        if (!lx_physics_raycast_ignore(s, origin, lx_v3(0, -1, 0), probe_up + probe_down, LX_ALL_LAYERS, ignore, &hit)) continue;
        float ankle_h = st->ankle_rest[side] * scale;
        LxVec3 target = lx_add3(hit.point, lx_scale3(up, ankle_h));
        float w = lx_ik_ground_weight(foot.y - target.y, LX_FOOT_IK_BAND * scale);
        if (!(w > 0)) continue;
        side_weight[side] = w * weight;
        targets[side] = target;
        /* the foot lies along the ground it stands on */
        LxVec3 n = lx_len3(hit.normal) > 0.1f ? lx_norm3(hit.normal) : up;
        LxQuat cur = lx_quat_mul(f->root_rot, f->wr[lb.end]);
        LxQuat align = lx_quat_identity();
        float d = lx_dot3(up, n);
        if (d < 0.9999f) {
            LxVec3 axis = lx_cross3(up, n);
            if (lx_len3(axis) > 1e-6f) align = lx_quat_axis_angle(lx_norm3(axis), acosf(lx_clampf(d, -1, 1)));
        }
        rots[side] = lx_quat_norm(lx_quat_mul(align, cur));
        /* can this leg still reach? only a leg that is short of its target asks the hips to come down */
        LxVec3 hip = to_world_p(f, f->wp[lb.root]), knee = to_world_p(f, f->wp[lb.mid]);
        float leg = lx_len3(lx_sub3(knee, hip)) + lx_len3(lx_sub3(foot, knee));
        /* weighted like the foot: a foot lifting off (a stride, a hop) is barely grounded, and its leg's full
           shortfall would yank the hips down while it is near the floor, then let them spring back up as it
           leaves the band - the body bobbing and snapping down that foot IK must never add */
        float deficit = lx_ik_hip_drop(lx_len3(lx_sub3(target, hip)), leg) * w;
        /* and never more than this foot's ground is below the ground the character stands on: a foot up on
           its toes on flat ground is the animation, not a dip in the floor to lower the body into */
        float lower_ground = f->to_world.m[13] - hit.point.y;
        if (deficit > lower_ground) deficit = lower_ground > 0 ? lower_ground : 0;
        if (deficit > drop) drop = deficit;
    }
    /* ease the hips, so stepping onto a kerb is a movement rather than a jump */
    float k = dt > 0 ? 1.0f - expf(-dt * 12.0f) : 1.0f;
    st->hip_drop += (drop - st->hip_drop) * k;
    if (st->hip_drop > 0.001f) {
        int hips = m->avatar[LX_HB_HIPS];
        if (hips >= 0 && hips < st->out.count) {
            LxQuat hp = m->nodes[hips].parent >= 0 ? f->wr[m->nodes[hips].parent] : lx_quat_identity();
            LxVec3 down_model = lx_quat_rotate(lx_quat_conj(lx_quat_mul(f->root_rot, hp)), lx_v3(0, -st->hip_drop * weight, 0));
            st->out.pos[hips] = lx_add3(st->out.pos[hips], down_model);
            pose_fk(st, &st->out, f->wp, f->wr);
        }
    }
    /* knees bend the way the toes point. The object's forward is only a fallback: a model imported facing
       the other way inside its object would have every straight-legged knee pushed backwards by it. */
    LxVec3 forward = lx_quat_rotate(f->root_rot, lx_v3(0, 0, 1));
    for (int side = 0; side < 2; side++) {
        if (!(side_weight[side] > 0)) continue;
        Limb lb = limb_of(m, side == 0 ? LX_IK_LEFT_FOOT : LX_IK_RIGHT_FOOT);
        LxVec3 knee_dir = forward;
        int toes = m->avatar[side == 0 ? LX_HB_LEFT_TOES : LX_HB_RIGHT_TOES];
        if (toes >= 0 && toes < st->out.count) {
            LxVec3 tf = lx_sub3(to_world_p(f, f->wp[toes]), to_world_p(f, f->wp[lb.end]));
            tf.y = 0;
            if (lx_len3(tf) > 1e-4f) knee_dir = lx_norm3(tf);
        }
        solve_limb(st, f, lb, targets[side], side_weight[side], &rots[side], side_weight[side], &knee_dir);
    }
}

static void look_at_ik(LxAnimState *st, IkFrame *f) {
    const LxModel *m = st->model;
    if (!st->look_active || !(st->look_weight > 0)) return;
    int head = m->avatar[LX_HB_HEAD];
    if (head < 0 || head >= st->out.count) return;
    LxVec3 head_w = to_world_p(f, f->wp[head]);
    LxVec3 to = lx_sub3(st->look_at, head_w);
    if (lx_len3(to) < 1e-4f) return;
    LxQuat cur = lx_quat_mul(f->root_rot, f->wr[head]);
    /* turn the head's own forward onto the target, no further than the weight allows */
    LxVec3 fwd = lx_quat_rotate(cur, lx_v3(0, 0, 1));
    LxVec3 want = lx_norm3(to);
    float d = lx_clampf(lx_dot3(lx_norm3(fwd), want), -1, 1);
    LxQuat turn = lx_quat_identity();
    if (d < 0.9999f) {
        LxVec3 axis = lx_cross3(fwd, want);
        if (lx_len3(axis) > 1e-6f) turn = lx_quat_axis_angle(lx_norm3(axis), acosf(d));
    }
    LxQuat full = lx_quat_norm(lx_quat_mul(turn, cur));
    LxQuat mixed = lx_quat_slerp(cur, full, lx_clampf(st->look_weight, 0, 1));
    int hp = m->nodes[head].parent;
    LxQuat parent_w = hp >= 0 ? lx_quat_mul(f->root_rot, f->wr[hp]) : f->root_rot;
    st->out.rot[head] = local_from_world(parent_w, mixed);
    pose_fk(st, &st->out, f->wp, f->wr);
}

/* every IK pass, over the pose the graph (or the clip) produced */
static void run_ik(LxScene *s, LxEntity e, LxAnimState *st, LxEntityData *d, float dt, bool state_foot_ik) {
    const LxModel *m = st->model;
    bool want_feet = (d->animator.foot_ik || state_foot_ik) && d->animator.foot_ik_weight > 0;
    bool want_goals = false;
    for (int i = 0; i < LX_IK_GOAL_COUNT; i++) if (st->ik[i].active && st->ik[i].weight > 0) want_goals = true;
    if (!want_feet && !want_goals && !st->look_active) return;
    if (m->node_count <= 0) return;
    IkFrame f;
    memset(&f, 0, sizeof f);
    f.wp = lx_alloc(sizeof(LxVec3) * (size_t)m->node_count);
    f.wr = lx_alloc(sizeof(LxQuat) * (size_t)m->node_count);
    if (!f.wp || !f.wr) { lx_free(f.wp); lx_free(f.wr); return; }
    f.to_world = lx_entity_world_matrix(s, e);
    f.to_model = lx_mat4_inverse(f.to_world);
    f.root_rot = lx_entity_world_rotation(s, e);
    f.root_rot_inv = lx_quat_conj(f.root_rot);
    pose_fk(st, &st->out, f.wp, f.wr);
    if (want_feet) foot_ik(s, e, st, &f, lx_clampf(d->animator.foot_ik_weight, 0, 1), dt);
    for (int i = 0; i < LX_IK_GOAL_COUNT; i++) {
        if (!st->ik[i].active || !(st->ik[i].weight > 0)) continue;
        Limb lb = limb_of(m, (LxIkGoal)i);
        solve_limb(st, &f, lb, st->ik[i].pos, lx_clampf(st->ik[i].weight, 0, 1),
                   st->ik[i].rot_active ? &st->ik[i].rot : NULL, st->ik[i].rot_weight, NULL);
    }
    look_at_ik(st, &f);
    lx_free(f.wp);
    lx_free(f.wr);
}

/* ---------------- the graph's frame ---------------- */
static void graph_update(LxScene *s, LxEntity e, LxAnimState *st, LxEntityData *d, float dt) {
    LxAnimGraph *g = st->graph;
    float global_speed = d->animator.speed;
    bool state_foot_ik = false;
    for (int li = 0; li < st->layer_count && li < g->layer_count; li++) {
        const LxAnimLayer *l = &g->layers[li];
        LayerRun *lr = &st->layers[li];
        if (lr->state < 0 || lr->state >= l->state_count) {
            lr->state = lx_anim_state_index(l, l->start);
            lr->from = -1;
            lr->norm = lr->state >= 0 ? l->states[lr->state].cycle_offset : 0;
        }
        if (lr->state < 0) { lx_pose_rest(&lr->pose, st->model); continue; }
        const LxAnimNode *n = &l->states[lr->state];
        if (n->foot_ik && li == 0) state_foot_ik = true;
        float len = state_length(st, n);
        float speed = n->speed * global_speed;
        if (n->speed_param[0]) speed *= param_float(st, n->speed_param);
        float prev = lr->norm;
        if (len > 0) lr->norm += dt * speed / len;
        if (!n->loop && lr->norm > 1) lr->norm = 1;
        if (lr->norm != lr->norm) lr->norm = prev;   /* a NaN here would stop the layer dead */
        float from_prev = lr->from_norm;
        if (lr->from >= 0 && lr->from < l->state_count) {
            const LxAnimNode *fn = &l->states[lr->from];
            float flen = state_length(st, fn);
            float fspeed = fn->speed * global_speed;
            if (fn->speed_param[0]) fspeed *= param_float(st, fn->speed_param);
            if (flen > 0) lr->from_norm += dt * fspeed / flen;
        }
        if (li == 0) {
            /* What the motions themselves moved the root by, each measured inside its own cycle and
               mixed by the weights the pose uses. The blended pose cannot say this: a blend tree
               moving from idle towards run, or a cross-fade, slides the root from where one clip
               holds it to where the other does, and that slide is not the character travelling. */
            LxVec3 d;
            float dy;
            motion_root_delta(st, n, prev, lr->norm, &d, &dy, 0);
            if (lr->from >= 0 && lr->from < l->state_count && lr->fade_len > 0) {
                LxVec3 fd;
                float fdy;
                float w = lx_clampf(lr->fade / lr->fade_len, 0, 1);
                motion_root_delta(st, &l->states[lr->from], from_prev, lr->from_norm, &fd, &fdy, 0);
                d = lx_add3(lx_scale3(fd, 1.0f - w), lx_scale3(d, w));
                dy = fdy * (1.0f - w) + dy * w;
            }
            st->rm_delta = d;
            st->rm_delta_yaw = dy;
            st->rm_delta_ok = true;
        }
        if (lr->from != -1) {
            lr->fade += dt;
            if (lr->fade >= lr->fade_len) lr->from = -1;
        }
        layer_transitions(st, l, lr, prev);
        layer_pose(st, l, lr);
    }
    if (st->layer_count > 0) combine_layers(st, g);
    else lx_pose_rest(&st->out, st->model);
    root_motion(s, e, st, d, dt);
    apply_pose(s, st);
    run_ik(s, e, st, d, dt, state_foot_ik);
    apply_pose(s, st);
}

/* ---------------- the public animator graph API ---------------- */
/* Every one of these finds the animator's runtime state the same way, and does nothing at all when the
 * object has no animator or its animator has no graph - a game may set a parameter before a character
 * is loaded, and that should be quiet, not a crash. */
static LxAnimState *graph_state(LxScene *s, LxEntity e) {
    LxAnimState *st = state_for(s, e);
    return st && st->graph ? st : NULL;
}

static ParamVal *param_of(LxScene *s, LxEntity e, const char *name, LxAnimParamType *type) {
    LxAnimState *st = graph_state(s, e);
    if (!st) return NULL;
    int i = param_index(st->graph, name);
    if (i < 0 || i >= st->pval_count) return NULL;
    if (type) *type = st->graph->params[i].type;
    return &st->pvals[i];
}

float lx_animator_get_float(LxScene *s, LxEntity e, const char *param) {
    LxAnimParamType t;
    ParamVal *v = param_of(s, e, param, &t);
    if (!v) return 0;
    return t == LX_AP_INT ? (float)v->i : (t == LX_AP_FLOAT ? v->f : (v->b ? 1.0f : 0.0f));
}
void lx_animator_set_float(LxScene *s, LxEntity e, const char *param, float value) {
    LxAnimParamType t;
    ParamVal *v = param_of(s, e, param, &t);
    if (!v) return;
    if (value != value) return;   /* a NaN parameter would spread through every blend that reads it */
    if (t == LX_AP_INT) v->i = (int)value;
    else if (t == LX_AP_FLOAT) v->f = value;
    else v->b = value != 0;
}
int lx_animator_get_int(LxScene *s, LxEntity e, const char *param) {
    LxAnimParamType t;
    ParamVal *v = param_of(s, e, param, &t);
    if (!v) return 0;
    return t == LX_AP_INT ? v->i : (t == LX_AP_FLOAT ? (int)v->f : (v->b ? 1 : 0));
}
void lx_animator_set_int(LxScene *s, LxEntity e, const char *param, int value) {
    LxAnimParamType t;
    ParamVal *v = param_of(s, e, param, &t);
    if (!v) return;
    if (t == LX_AP_FLOAT) v->f = (float)value;
    else if (t == LX_AP_INT) v->i = value;
    else v->b = value != 0;
}
bool lx_animator_get_bool(LxScene *s, LxEntity e, const char *param) {
    LxAnimParamType t;
    ParamVal *v = param_of(s, e, param, &t);
    if (!v) return false;
    return t == LX_AP_FLOAT ? v->f != 0 : (t == LX_AP_INT ? v->i != 0 : v->b);
}
void lx_animator_set_bool(LxScene *s, LxEntity e, const char *param, bool value) {
    LxAnimParamType t;
    ParamVal *v = param_of(s, e, param, &t);
    if (!v) return;
    if (t == LX_AP_FLOAT) v->f = value ? 1.0f : 0.0f;
    else if (t == LX_AP_INT) v->i = value ? 1 : 0;
    else v->b = value;
}
void lx_animator_set_trigger(LxScene *s, LxEntity e, const char *param) {
    ParamVal *v = param_of(s, e, param, NULL);
    if (v) v->b = true;
}
void lx_animator_reset_trigger(LxScene *s, LxEntity e, const char *param) {
    ParamVal *v = param_of(s, e, param, NULL);
    if (v) v->b = false;
}
bool lx_animator_has_param(LxScene *s, LxEntity e, const char *param) { return param_of(s, e, param, NULL) != NULL; }

int lx_animator_layer_count(LxScene *s, LxEntity e) {
    LxAnimState *st = graph_state(s, e);
    return st ? st->layer_count : 0;
}
float lx_animator_layer_weight(LxScene *s, LxEntity e, int layer) {
    LxAnimState *st = graph_state(s, e);
    return st && layer >= 0 && layer < st->layer_count ? st->layers[layer].weight : 0;
}
void lx_animator_set_layer_weight(LxScene *s, LxEntity e, int layer, float weight) {
    LxAnimState *st = graph_state(s, e);
    if (st && layer > 0 && layer < st->layer_count) st->layers[layer].weight = lx_clampf(weight, 0, 1);
}
const char *lx_animator_state(LxScene *s, LxEntity e, int layer) {
    LxAnimState *st = graph_state(s, e);
    if (!st || layer < 0 || layer >= st->layer_count || layer >= st->graph->layer_count) return "";
    const LxAnimLayer *l = &st->graph->layers[layer];
    int i = st->layers[layer].state;
    return i >= 0 && i < l->state_count ? l->states[i].name : "";
}
const char *lx_animator_state_tag(LxScene *s, LxEntity e, int layer) {
    LxAnimState *st = graph_state(s, e);
    if (!st || layer < 0 || layer >= st->layer_count || layer >= st->graph->layer_count) return "";
    const LxAnimLayer *l = &st->graph->layers[layer];
    int i = st->layers[layer].state;
    return i >= 0 && i < l->state_count ? l->states[i].tag : "";
}
float lx_animator_normalized_time(LxScene *s, LxEntity e, int layer) {
    LxAnimState *st = graph_state(s, e);
    return st && layer >= 0 && layer < st->layer_count ? st->layers[layer].norm : 0;
}
bool lx_animator_in_transition(LxScene *s, LxEntity e, int layer) {
    LxAnimState *st = graph_state(s, e);
    return st && layer >= 0 && layer < st->layer_count && st->layers[layer].from != -1;
}

bool lx_animator_play_state(LxScene *s, LxEntity e, int layer, const char *state, float fade) {
    LxAnimState *st = graph_state(s, e);
    if (!st || layer < 0 || layer >= st->layer_count || layer >= st->graph->layer_count) return false;
    const LxAnimLayer *l = &st->graph->layers[layer];
    int to = lx_anim_state_index(l, state);
    if (to < 0) return false;
    LxAnimTransition t;
    memset(&t, 0, sizeof t);
    lx_strcpy(t.to, sizeof t.to, state);
    t.duration = fade > 0 ? fade : 0;
    t.fixed_duration = true;
    t.to_self = true;
    start_transition(st, l, &st->layers[layer], to, &t);
    return true;
}

bool lx_animator_root_motion_info(LxScene *s, LxEntity e, LxRootMotionInfo *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    LxAnimState *st = state_for(s, e);
    LxEntityData *d = lx_entity_data(s, e);
    if (!st || !d || !st->model) return false;
    out->enabled = d->animator.root_motion;
    int r = st->rm_root_node;
    out->has_root = r >= 0 && r < st->model->node_count;
    if (out->has_root) lx_strcpy(out->root_bone, sizeof out->root_bone, st->model->nodes[r].name);
    lx_strcpy(out->clip, sizeof out->clip, st->rm_clip);
    /* a channel "moves the object" when it is not kept in the pose - and only then does the switch matter */
    out->travel_moves_object = st->rm_settings.travel == LX_ROOT_TO_OBJECT;
    out->up_down_moves_object = st->rm_settings.up_down == LX_ROOT_TO_OBJECT;
    out->turn_moves_object = st->rm_settings.turn == LX_ROOT_TO_OBJECT;
    out->last_step = st->rm_last_step;
    out->last_turn = st->rm_last_yaw_step;
    return true;
}

void lx_animator_set_ik_goal(LxScene *s, LxEntity e, LxIkGoal goal, LxVec3 position, float weight) {
    LxAnimState *st = state_for(s, e);
    if (!st || goal < 0 || goal >= LX_IK_GOAL_COUNT) return;
    st->ik[goal].pos = position;
    st->ik[goal].weight = lx_clampf(weight, 0, 1);
    st->ik[goal].active = weight > 0;
}
void lx_animator_set_ik_rotation(LxScene *s, LxEntity e, LxIkGoal goal, LxQuat rotation, float weight) {
    LxAnimState *st = state_for(s, e);
    if (!st || goal < 0 || goal >= LX_IK_GOAL_COUNT) return;
    st->ik[goal].rot = rotation;
    st->ik[goal].rot_weight = lx_clampf(weight, 0, 1);
    st->ik[goal].rot_active = weight > 0;
}
void lx_animator_clear_ik_goal(LxScene *s, LxEntity e, LxIkGoal goal) {
    LxAnimState *st = state_for(s, e);
    if (!st || goal < 0 || goal >= LX_IK_GOAL_COUNT) return;
    memset(&st->ik[goal], 0, sizeof st->ik[goal]);
}
void lx_animator_set_look_at(LxScene *s, LxEntity e, LxVec3 position, float weight) {
    LxAnimState *st = state_for(s, e);
    if (!st) return;
    st->look_at = position;
    st->look_weight = lx_clampf(weight, 0, 1);
    st->look_active = weight > 0;
}
void lx_animator_clear_look_at(LxScene *s, LxEntity e) {
    LxAnimState *st = state_for(s, e);
    if (st) { st->look_active = false; st->look_weight = 0; }
}

static void graph_update(LxScene *s, LxEntity e, LxAnimState *st, LxEntityData *d, float dt);

void lx_animator_update(LxScene *s, LxEntity e, float dt) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !(d->comps & LX_COMP_ANIMATOR) || !d->active) return;
    LxAnimState *st = state_for(s, e);
    if (!st) return;
    d = lx_entity_data(s, e);
    /* each frame measures the root afresh: a reading left over from a clip that has stopped would
       keep moving the object for ever */
    st->rm_delta_ok = false;
    if (st->graph) { st->started = true; graph_update(s, e, st, d, dt); return; }
    if (!st->started && d->animator.play_on_start && d->animator.clip[0]) lx_animator_play(s, e, d->animator.clip, 0);
    st->started = true;
    if (!st->cur.clip) return;
    float speed = d->animator.speed;
    if (st->playing) {
        st->time += dt * speed;
        if (!st->loop && st->time >= st->cur.clip->length) { st->time = st->cur.clip->length; st->playing = false; }
        if (st->prev.clip) {
            st->prev_time += dt * speed;
            st->fade += dt;
            if (st->fade >= st->fade_len) binding_free(&st->prev);
        }
    }
    LxModel *m = st->model;
    evaluate(&st->cur, m, st->time, st->loop, st->a.pos, st->a.rot, st->a.scl);
    if (st->prev.clip && st->fade_len > 0) {
        /* cross-fade: the clip being left and the one being entered, blended by how far through it is */
        float w = lx_clampf(st->fade / st->fade_len, 0, 1);
        evaluate(&st->prev, m, st->prev_time, true, st->b.pos, st->b.rot, st->b.scl);
        float total;
        lx_pose_blend_begin(&st->out, &total);
        lx_pose_blend_add(&st->out, &st->b, 1.0f - w, &total);
        lx_pose_blend_add(&st->out, &st->a, w, &total);
        lx_pose_blend_end(&st->out, total, &st->a);
    } else {
        lx_pose_copy(&st->out, &st->a);
    }
    /* the one clip it is playing, measured inside its own cycle like the graph's are */
    if (st->cur.clip) {
        float clen = clip_len_of(&st->cur);
        if (clen > 0) {
            /* in cycles, not wrapped: the delta counts the cycles passed itself */
            float now_n = st->time / clen, prev_n = st->rm_prev_norm;
            LxVec3 d;
            float dy;
            clip_root_delta(st, &st->cur, prev_n, now_n, &d, &dy);
            st->rm_delta = d;
            st->rm_delta_yaw = dy;
            st->rm_delta_ok = st->rm_started_clip;
            st->rm_prev_norm = now_n;
            st->rm_started_clip = true;
        }
    }
    root_motion(s, e, st, d, dt);
    apply_pose(s, st);
    run_ik(s, e, st, d, dt, false);
    apply_pose(s, st);
}

void lx_scene_update_animation(LxScene *s, float dt) {
    int n = s->count;
    for (int i = 1; i < n; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !d->active || !(d->comps & LX_COMP_ANIMATOR)) continue;
        lx_animator_update(s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), dt);
    }
}

/* ======================================================================
 *  skinning
 * ====================================================================== */
typedef struct SkinJob { const LxMesh *src; LxMesh *dst; const LxMat4 *pal; const LxMeshSkin *sk; } SkinJob;

static void skin_range(void *u, int begin, int end, int thread) {
    LX_UNUSED(thread);
    SkinJob *j = u;
    const LxVertex *in = j->src->lods[0].vertices;
    LxVertex *out = j->dst->lods[0].vertices;
    for (int v = begin; v < end; v++) {
        const uint16_t *ji = j->sk->joints[v];
        const float *w = j->sk->weights[v];
        float m[12] = {0};
        for (int k = 0; k < 4; k++) {
            if (w[k] <= 0) continue;
            const float *p = j->pal[ji[k]].m;
            for (int r = 0; r < 3; r++) {
                m[r * 4 + 0] += p[r] * w[k];        /* column 0 */
                m[r * 4 + 1] += p[4 + r] * w[k];    /* column 1 */
                m[r * 4 + 2] += p[8 + r] * w[k];    /* column 2 */
                m[r * 4 + 3] += p[12 + r] * w[k];   /* translation */
            }
        }
        const LxVertex *a = &in[v];
        LxVertex *o = &out[v];
        *o = *a;
        o->px = m[0] * a->px + m[1] * a->py + m[2] * a->pz + m[3];
        o->py = m[4] * a->px + m[5] * a->py + m[6] * a->pz + m[7];
        o->pz = m[8] * a->px + m[9] * a->py + m[10] * a->pz + m[11];
        float nx = m[0] * a->nx + m[1] * a->ny + m[2] * a->nz, ny = m[4] * a->nx + m[5] * a->ny + m[6] * a->nz, nz = m[8] * a->nx + m[9] * a->ny + m[10] * a->nz;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl > 1e-12f) { o->nx = nx / nl; o->ny = ny / nl; o->nz = nz / nl; }
        float tx = m[0] * a->tx + m[1] * a->ty + m[2] * a->tz, ty = m[4] * a->tx + m[5] * a->ty + m[6] * a->tz, tz = m[8] * a->tx + m[9] * a->ty + m[10] * a->tz;
        float tl = sqrtf(tx * tx + ty * ty + tz * tz);
        if (tl > 1e-12f) { o->tx = tx / tl; o->ty = ty / tl; o->tz = tz / tl; }
    }
}

/* the object that owns the skeleton: the nearest ancestor with an Animator, else the top ancestor */
static LxEntity skeleton_root(LxScene *s, LxEntity e) {
    LxEntity top = e;
    for (LxEntity p = lx_entity_parent(s, e); p; p = lx_entity_parent(s, p)) {
        if (lx_entity_components(s, p) & LX_COMP_ANIMATOR) return p;
        top = p;
    }
    return top;
}

static void update_skin(LxScene *s, LxEntity e, LxEntityData *d, LxMesh *src) {
    LxSkinState *sk = d->skin;
    if (sk && (sk->src != src || sk->generation != lx_model_generation())) { skin_free(sk); sk = d->skin = NULL; }
    if (!sk) {
        sk = lx_calloc(1, sizeof *sk);
        sk->src = src;
        sk->generation = lx_model_generation();
        sk->mesh = lx_mesh_create(src->lods[0].vertices, src->lods[0].vertex_count, src->lods[0].indices, src->lods[0].index_count, src->ref);
        memcpy(sk->mesh->lods[0].vertices, src->lods[0].vertices, sizeof(LxVertex) * (size_t)src->lods[0].vertex_count);  /* keep imported tangents */
        sk->mesh->dynamic = true;
        sk->bone_count = src->skin->bone_count;
        sk->bones = lx_calloc((size_t)(sk->bone_count ? sk->bone_count : 1), sizeof(LxEntity));
        sk->palette = lx_alloc(sizeof(LxMat4) * (size_t)(sk->bone_count ? sk->bone_count : 1));
        d = lx_entity_data(s, e);
        d->skin = sk;
    }
    LxEntity root = 0;
    for (int b = 0; b < sk->bone_count; b++) {
        if (sk->bones[b] && lx_entity_valid(s, sk->bones[b])) continue;
        if (!root) root = skeleton_root(s, e);
        sk->bones[b] = find_descendant(s, root, src->skin->bones[b]);
    }
    LxMat4 inv_mesh = lx_mat4_inverse(lx_entity_world_matrix(s, e));
    uint64_t h = 1469598103934665603ull;
    for (int b = 0; b < sk->bone_count; b++) {
        LxMat4 bw = sk->bones[b] ? lx_entity_world_matrix(s, sk->bones[b]) : lx_mat4_identity();
        sk->palette[b] = lx_mat4_mul(inv_mesh, lx_mat4_mul(bw, src->skin->inverse_bind[b]));
        const uint32_t *w = (const uint32_t *)sk->palette[b].m;
        for (int k = 0; k < 16; k++) h = (h ^ w[k]) * 1099511628211ull;
    }
    if (h == sk->last_hash) return;   /* nothing moved */
    sk->last_hash = h;
    SkinJob job = {src, sk->mesh, sk->palette, src->skin};
    int vc = src->lods[0].vertex_count;
    if (vc > 4096) lx_parallel_for(vc, 2048, skin_range, &job);
    else skin_range(&job, 0, vc, 0);
    LxAabb bb = lx_aabb_empty();
    const LxVertex *v = sk->mesh->lods[0].vertices;
    for (int i = 0; i < vc; i++) lx_aabb_grow(&bb, lx_v3(v[i].px, v[i].py, v[i].pz));
    sk->mesh->bounds = bb;
    sk->mesh->version++;
}

void lx_scene_update_skins(LxScene *s) {
    lx_scene_update_transforms(s);
    int n = s->count;
    for (int i = 1; i < n; i++) {
        LxEntityData *d = &s->entities[i];
        if (!d->alive || !(d->comps & LX_COMP_MESH)) continue;
        if (!strchr(d->mesh.mesh, '#')) { if (d->skin) { skin_free(d->skin); d->skin = NULL; } continue; }
        LxMesh *src = lx_asset_mesh(d->mesh.mesh);
        if (!src || !src->skin) { if (d->skin) { skin_free(d->skin); d->skin = NULL; } continue; }
        update_skin(s, lx_entity_make((uint32_t)i, d->generation & 0xfffu), d, src);
    }
}

LxMesh *lx_skin_mesh_of(LxEntityData *d) { return d && d->skin && d->skin->mesh ? d->skin->mesh : (d ? lx_asset_mesh(d->mesh.mesh) : NULL); }

LxMesh *lx_skin_mesh_for(LxScene *s, LxEntity e) {
    LxEntityData *d = lx_entity_data(s, e);
    return d && d->skin ? d->skin->mesh : NULL;
}
