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

/* the target pose (local TRS per target node) for a binding at a time */
static void evaluate(const Binding *b, LxModel *dst, float time, bool loop, bool root_motion, LxVec3 *pos, LxQuat *rot, LxVec3 *scl) {
    for (int i = 0; i < dst->node_count; i++) { pos[i] = dst->nodes[i].pos; rot[i] = dst->nodes[i].rot; scl[i] = dst->nodes[i].scale; }
    if (!b->clip) return;
    if (!b->humanoid) {
        for (int i = 0; i < dst->node_count; i++)
            if (b->track_of[i] >= 0) sample(b->clip, b->track_of[i], time, loop, &pos[i], &rot[i], &scl[i]);
        if (!root_motion && dst->avatar[LX_HB_HIPS] >= 0 && b->track_of[dst->avatar[LX_HB_HIPS]] >= 0) {
            int h = dst->avatar[LX_HB_HIPS];
            pos[h].x = dst->nodes[h].pos.x;
            pos[h].z = dst->nodes[h].pos.z;
        }
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
        if (!root_motion) { off.x = 0; off.z = 0; }
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

static void state_free(LxAnimState *st) {
    if (!st) return;
    binding_free(&st->cur);
    binding_free(&st->prev);
    lx_free(st->ents);
    lx_pose_free(&st->out);
    lx_pose_free(&st->a);
    lx_pose_free(&st->b);
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
    if (!stale) return st;
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

void lx_animator_update(LxScene *s, LxEntity e, float dt) {
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !(d->comps & LX_COMP_ANIMATOR) || !d->active) return;
    LxAnimState *st = state_for(s, e);
    if (!st) return;
    d = lx_entity_data(s, e);
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
    bool rm = d->animator.root_motion;
    evaluate(&st->cur, m, st->time, st->loop, rm, st->a.pos, st->a.rot, st->a.scl);
    if (st->prev.clip && st->fade_len > 0) {
        /* cross-fade: the clip being left and the one being entered, blended by how far through it is */
        float w = lx_clampf(st->fade / st->fade_len, 0, 1);
        evaluate(&st->prev, m, st->prev_time, true, rm, st->b.pos, st->b.rot, st->b.scl);
        float total;
        lx_pose_blend_begin(&st->out, &total);
        lx_pose_blend_add(&st->out, &st->b, 1.0f - w, &total);
        lx_pose_blend_add(&st->out, &st->a, w, &total);
        lx_pose_blend_end(&st->out, total, &st->a);
    } else {
        lx_pose_copy(&st->out, &st->a);
    }
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
