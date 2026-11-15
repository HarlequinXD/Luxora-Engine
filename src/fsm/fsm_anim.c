/* Animation: the Animator and Animation categories.
 *
 * Luxora plays clips on a model's skeleton through an Animator component. Clips are named
 * "Models/Hero.fbx#Run" (or "#0" for the first). There is no state-machine controller asset with
 * parameters: the State Graph itself is the controller, so you drive the graph and play clips from its
 * states rather than setting parameters on a separate animation asset. */
#include "fsm_actions.h"

/* An Animator may sit on the object itself or on a model root below it. */
static LxEntity fsm_animator_of(LxFsmCtx *c, int p) {
    LxEntity e = fsm_obj(c, p);
    return e ? lx_find_animator(c->scene, e) : 0;
}

/* ==========================================================================================
 *  playing clips
 * ========================================================================================== */
static void an_play(LxFsmCtx *c) {
    LxEntity a = fsm_animator_of(c, 0);
    if (!a) { fsm_send(c, 6); return; }
    const char *clip = lx_fsm_asset(c, 1);
    if (!clip || !clip[0]) return;
    /* already playing it and not asked to restart: leave it alone, or the pose resets every frame */
    const char *cur = lx_animator_current_clip(c->scene, a);
    if (!lx_fsm_bool(c, 4) && cur && !strcmp(cur, clip) && lx_animator_playing(c->scene, a)) return;
    lx_animator_play(c->scene, a, clip, lx_maxf(0, lx_fsm_float(c, 2)));
    LxAnimator *an = lx_get_animator(c->scene, a);
    if (an) {
        if (!lx_fsm_is_none(c, 3)) an->speed = lx_fsm_float(c, 3);
        if (!lx_fsm_is_none(c, 5)) an->loop = lx_fsm_bool(c, 5);
    }
}
static const LxFsmActionDesc A_AN_PLAY = {"Play Animation", "Animator", "Plays a clip on an object's Animator, blending out of whatever was playing.",
    "play {1} on {0}",
    {POBJT("Object", "the Animator on it, or on the first model below it"),
     PASSETT("Clip", "clip", "\"Models/Hero.fbx#Run\", or \"#0\" for the first clip"),
     PT("Fade Seconds", LX_FSM_FLOAT, "0.2", "how long to blend out of the old clip"),
     PIN_OPT("Speed", LX_FSM_FLOAT), PT("Restart If Same", LX_FSM_BOOL, "false", "play from the start even if this clip is already running"),
     PIN_OPT("Loop", LX_FSM_BOOL), PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, an_play, an_play, NULL};

static void an_stop(LxFsmCtx *c) {
    LxEntity a = fsm_animator_of(c, 0);
    if (!a) return;
    lx_animator_stop(c->scene, a);
    if (lx_fsm_bool(c, 1)) lx_animator_reset_pose(c->scene, a);
}
static const LxFsmActionDesc A_AN_STOP = {"Stop Animation", "Animator", "Stops the Animator, optionally dropping the model back into its rest pose.",
    "stop {0}",
    {POBJ("Object"), PT("Reset Pose", LX_FSM_BOOL, "false", "return the bones to how the model was made")},
    0, 0, LX_FSM_ONCE, an_stop, an_stop, NULL};

static void an_get(LxFsmCtx *c) {
    LxEntity a = fsm_animator_of(c, 0);
    lx_fsm_set_bool(c, 5, a != 0);
    if (!a) return;
    const char *clip = lx_animator_current_clip(c->scene, a);
    lx_fsm_set_string(c, 1, clip ? clip : "");
    lx_fsm_set_bool(c, 2, lx_animator_playing(c->scene, a));
    lx_fsm_set_float(c, 3, lx_animator_time(c->scene, a));
    LxAnimator *an = lx_get_animator(c->scene, a);
    lx_fsm_set_float(c, 4, an ? an->speed : 0.0f);
    lx_fsm_set_object(c, 6, a);
}
static const LxFsmActionDesc A_AN_GET = {"Get Animation Info", "Animator", "Which clip is playing, whether it is running, and how far through it is.",
    "animation on {0}",
    {POBJ("Object"), POPT("Store Clip", LX_FSM_STRING), POPT("Store Is Playing", LX_FSM_BOOL),
     POPTT("Store Time", LX_FSM_FLOAT, "seconds into the clip"), POPT("Store Speed", LX_FSM_FLOAT),
     POPT("Store Has Animator", LX_FSM_BOOL), POPT("Store Animator Object", LX_FSM_OBJECT)},
    0, 0, LX_FSM_EVERY_FRAME, an_get, an_get, NULL};

static void an_set(LxFsmCtx *c) {
    LxEntity a = fsm_animator_of(c, 0);
    LxAnimator *an = a ? lx_get_animator(c->scene, a) : NULL;
    if (!an) return;
    if (!lx_fsm_is_none(c, 1)) an->speed = lx_fsm_float(c, 1);
    if (!lx_fsm_is_none(c, 2)) an->loop = lx_fsm_bool(c, 2);
    if (!lx_fsm_is_none(c, 3)) an->root_motion = lx_fsm_bool(c, 3);
    if (!lx_fsm_is_none(c, 4)) lx_animator_set_time(c->scene, a, lx_maxf(0, lx_fsm_float(c, 4)));
}
static const LxFsmActionDesc A_AN_SET = {"Set Animator", "Animator", "Changes playback speed, looping, root motion and the playhead. None leaves a setting alone.",
    "animator on {0}",
    {POBJ("Object"), PT("Speed", LX_FSM_FLOAT, "", "1 is normal; negative plays backwards"),
     PIN_OPT("Loop", LX_FSM_BOOL), PT("Root Motion", LX_FSM_BOOL, "", "let the animation move the object itself"),
     PT("Time", LX_FSM_FLOAT, "", "jump to this many seconds into the clip")},
    0, 0, LX_FSM_ONCE, an_set, an_set, NULL};

static void an_speed_from(LxFsmCtx *c) {
    LxEntity a = fsm_animator_of(c, 0);
    LxAnimator *an = a ? lx_get_animator(c->scene, a) : NULL;
    if (!an) return;
    float v = lx_fsm_float(c, 1);
    float ref = lx_fsm_float(c, 2);
    float speed = ref > 0 ? v / ref : v;
    an->speed = lx_clampf(speed, lx_fsm_float(c, 3), lx_fsm_float(c, 4));
    lx_fsm_set_float(c, 5, an->speed);
}
static const LxFsmActionDesc A_AN_SPEED_FROM = {"Match Animation Speed", "Animator", "Ties playback speed to a value such as the walking speed, so feet keep up with the ground.",
    "{0} speed from {1}",
    {POBJ("Object"), PT("Value", LX_FSM_FLOAT, "0", "usually the object's current speed"),
     PT("Value At Normal Speed", LX_FSM_FLOAT, "4", "the value at which the clip should play at speed 1"),
     P("Min Speed", LX_FSM_FLOAT, "0"), P("Max Speed", LX_FSM_FLOAT, "2"), POPT("Store Speed", LX_FSM_FLOAT)},
    0, 0, LX_FSM_EVERY_FRAME, an_speed_from, an_speed_from, NULL};

typedef struct { bool fired; } FsmAnimWatch;
static void an_wait_finish(LxFsmCtx *c) {
    FsmAnimWatch *w = c->state;
    LxEntity a = fsm_animator_of(c, 0);
    if (!a) { lx_fsm_finish(c); fsm_send(c, 2); return; }
    if (w->fired) return;
    if (!lx_animator_playing(c->scene, a)) {
        w->fired = true;
        lx_fsm_finish(c);
        fsm_send(c, 1);
        return;
    }
    float timeout = lx_fsm_float(c, 3);
    if (timeout > 0 && lx_fsm_state_time(c) >= timeout) {
        w->fired = true;
        lx_fsm_finish(c);
        fsm_send(c, 2);
    }
}
static const LxFsmActionDesc A_AN_WAIT = {"Wait For Animation", "Animator", "Holds the state until the clip that is playing finishes: attack swings, door opens, cutscene beats.",
    "wait for {0}",
    {POBJ("Object"), PEV("Finish Event"), PEV("Timeout Event"), PT("Timeout", LX_FSM_FLOAT, "0", "seconds; 0 = wait as long as it takes")},
    sizeof(FsmAnimWatch), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, NULL, an_wait_finish, NULL};

static void an_clip_info(LxFsmCtx *c) {
    const char *ref = lx_fsm_asset(c, 0);
    LxModel *m = (ref && ref[0]) ? lx_asset_model(ref) : NULL;
    int n = m ? lx_model_clip_count(m) : 0;
    lx_fsm_set_int(c, 1, n);
    int i = lx_fsm_int(c, 2);
    if (m && i >= 0 && i < n) {
        lx_fsm_set_string(c, 3, lx_model_clip_name(m, i));
        lx_fsm_set_float(c, 4, lx_model_clip_length(m, i));
    }
    fsm_send(c, m ? 5 : 6);
}
static const LxFsmActionDesc A_AN_CLIP_INFO = {"Get Model Clips", "Animator", "How many animation clips a model file has, and the name and length of one of them.",
    "clips of {0}",
    {PASSETT("Model", "model", "the .fbx file"), POPT("Store Clip Count", LX_FSM_INT),
     PT("Clip Index", LX_FSM_INT, "0", "which clip to report on"), POPT("Store Clip Name", LX_FSM_STRING),
     POPTT("Store Clip Length", LX_FSM_FLOAT, "seconds"), PEV("Loaded Event"), PEV("Failed Event")},
    0, 0, LX_FSM_ONCE, an_clip_info, an_clip_info, NULL};

static void an_find_bone(LxFsmCtx *c) {
    LxEntity root = fsm_obj(c, 0);
    const char *name = lx_fsm_string(c, 1);
    LxEntity found = (root && name && name[0]) ? lx_entity_find_child(c->scene, root, name) : 0;
    lx_fsm_set_object(c, 2, found);
    fsm_send(c, found ? 3 : 4);
}
static const LxFsmActionDesc A_AN_FIND_BONE = {"Find Bone", "Animator", "Finds a bone under a model by name, so things can be attached to a hand or a head.",
    "bone \"{1}\" of {0} -> {2}",
    {POBJ("Model Object"), PT("Bone Name", LX_FSM_STRING, "RightHand", "as the model names it"),
     POUT("Store Bone", LX_FSM_OBJECT), PEV("Found Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, an_find_bone, an_find_bone, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_anim(void) {
    static const LxFsmActionDesc *all[] = {
        &A_AN_PLAY, &A_AN_STOP, &A_AN_GET, &A_AN_SET, &A_AN_SPEED_FROM, &A_AN_WAIT,
        &A_AN_CLIP_INFO, &A_AN_FIND_BONE,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
