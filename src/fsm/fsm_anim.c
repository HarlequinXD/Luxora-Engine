/* Animation: the Animator and Animation categories.
 *
 * Luxora plays clips on a model's skeleton through an Animator component. Clips are named
 * "Models/Hero.fbx#Run" (or "#0" for the first), and the Animator category plays them directly: a state
 * of a State Graph says what to play, and that is the whole controller.
 *
 * An object may instead be given an animator graph (.lxanim), which is a state machine over animation
 * in its own right - states that play clips or blend trees, and transitions driven by typed parameters.
 * The Animator Graph category is for those: it sets the parameters and reads back what is playing, and
 * leaves the choosing and the blending to the animator. */
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
     PASSETT("Clip", "animation", "\"Models/Hero.fbx#Run\", or \"#0\" for the first clip"),
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
 *  animator graphs: the Animator Graph category
 *
 *  An animator graph is the other kind of state machine - one over animation rather than over game
 *  logic. A State Graph drives it by setting parameters and leaves the blending to it: set Speed as the
 *  character accelerates and the animator walks, then runs, cross-fading as it goes. That division is
 *  why these actions only ever set a value: which clip that plays is the animator graph's business.
 * ========================================================================================== */
static LxEntity ag_target(LxFsmCtx *c, int p) { return fsm_animator_of(c, p); }

static void ag_set_float(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    const char *name = lx_fsm_string(c, 1);
    if (!a || !name || !name[0]) { fsm_send(c, 4); return; }
    float want = lx_fsm_float(c, 2);
    float smooth = lx_fsm_is_none(c, 3) ? 0 : lx_fsm_float(c, 3);
    if (smooth > 0 && c->dt > 0) {
        /* ease towards it, so a parameter a blend tree reads cannot jump and snap the pose */
        float cur = lx_animator_get_float(c->scene, a, name);
        float k = 1.0f - expf(-c->dt / smooth);
        want = cur + (want - cur) * k;
    }
    lx_animator_set_float(c->scene, a, name, want);
}
static const LxFsmActionDesc A_AG_SET_FLOAT = {"Set Animator Float", "Animator Graph",
    "Sets a float parameter of an object's animator graph - the usual way to drive a blend tree such as a walk-to-run.",
    "{1} = {2} on {0}",
    {POBJT("Object", "the Animator on it, or on the first model below it"),
     PT("Parameter", LX_FSM_STRING, "Speed", "as the animator graph names it"),
     P("Value", LX_FSM_FLOAT, "0"),
     PT("Smooth Time", LX_FSM_FLOAT, "", "seconds to ease over; None sets it at once"),
     PEV("No Animator Event")},
    0, 0, LX_FSM_EVERY_FRAME, ag_set_float, ag_set_float, NULL};

static void ag_set_int(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    const char *name = lx_fsm_string(c, 1);
    if (!a || !name || !name[0]) { fsm_send(c, 3); return; }
    lx_animator_set_int(c->scene, a, name, lx_fsm_int(c, 2));
}
static const LxFsmActionDesc A_AG_SET_INT = {"Set Animator Int", "Animator Graph",
    "Sets an int parameter of an object's animator graph.", "{1} = {2} on {0}",
    {POBJ("Object"), PT("Parameter", LX_FSM_STRING, "State", "as the animator graph names it"),
     P("Value", LX_FSM_INT, "0"), PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, ag_set_int, ag_set_int, NULL};

static void ag_set_bool(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    const char *name = lx_fsm_string(c, 1);
    if (!a || !name || !name[0]) { fsm_send(c, 3); return; }
    lx_animator_set_bool(c->scene, a, name, lx_fsm_bool(c, 2));
}
static const LxFsmActionDesc A_AG_SET_BOOL = {"Set Animator Bool", "Animator Graph",
    "Sets a bool parameter of an object's animator graph - grounded, crouching, holding a weapon.",
    "{1} = {2} on {0}",
    {POBJ("Object"), PT("Parameter", LX_FSM_STRING, "Grounded", "as the animator graph names it"),
     P("Value", LX_FSM_BOOL, "true"), PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, ag_set_bool, ag_set_bool, NULL};

static void ag_trigger(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    const char *name = lx_fsm_string(c, 1);
    if (!a || !name || !name[0]) { fsm_send(c, 3); return; }
    if (lx_fsm_bool(c, 2)) lx_animator_reset_trigger(c->scene, a, name);
    else lx_animator_set_trigger(c->scene, a, name);
}
static const LxFsmActionDesc A_AG_TRIGGER = {"Set Animator Trigger", "Animator Graph",
    "Fires a trigger parameter: one transition takes it and it clears itself, so an attack plays once however long the state lasts.",
    "trigger {1} on {0}",
    {POBJ("Object"), PT("Parameter", LX_FSM_STRING, "Attack", "as the animator graph names it"),
     PT("Reset Instead", LX_FSM_BOOL, "false", "clear it rather than fire it"), PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, ag_trigger, ag_trigger, NULL};

static void ag_get(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    const char *name = lx_fsm_string(c, 1);
    bool have = a && name && name[0] && lx_animator_has_param(c->scene, a, name);
    lx_fsm_set_bool(c, 5, have);
    if (!have) { fsm_send(c, 6); return; }
    lx_fsm_set_float(c, 2, lx_animator_get_float(c->scene, a, name));
    lx_fsm_set_int(c, 3, lx_animator_get_int(c->scene, a, name));
    lx_fsm_set_bool(c, 4, lx_animator_get_bool(c->scene, a, name));
}
static const LxFsmActionDesc A_AG_GET = {"Get Animator Parameter", "Animator Graph",
    "Reads a parameter of an animator graph, as a float, an int and a bool - whichever of them the graph is actually holding.",
    "{1} of {0}",
    {POBJ("Object"), PT("Parameter", LX_FSM_STRING, "Speed", "as the animator graph names it"),
     POPT("Store Float", LX_FSM_FLOAT), POPT("Store Int", LX_FSM_INT), POPT("Store Bool", LX_FSM_BOOL),
     POPTT("Store Found", LX_FSM_BOOL, "false when the animator has no such parameter"), PEV("Not Found Event")},
    0, 0, LX_FSM_EVERY_FRAME, ag_get, ag_get, NULL};

static void ag_play_state(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    const char *state = lx_fsm_string(c, 2);
    bool ok = a && state && state[0] && lx_animator_play_state(c->scene, a, lx_fsm_int(c, 1), state, lx_maxf(0, lx_fsm_float(c, 3)));
    fsm_send(c, ok ? 4 : 5);
}
static const LxFsmActionDesc A_AG_PLAY_STATE = {"Play Animator State", "Animator Graph",
    "Goes straight to a state of an animator graph, whatever its transitions say - for cutscenes and scripted moments.",
    "play {2} on {0}",
    {POBJ("Object"), PT("Layer", LX_FSM_INT, "0", "0 is the base layer"),
     PT("State", LX_FSM_STRING, "", "as the animator graph names it"),
     PT("Fade Seconds", LX_FSM_FLOAT, "0.2", "how long to blend into it"),
     PEV("Played Event"), PEV("Not Found Event")},
    0, 0, LX_FSM_ONCE, ag_play_state, ag_play_state, NULL};

static void ag_get_state(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    int layer = lx_fsm_int(c, 1);
    if (!a || !lx_animator_layer_count(c->scene, a)) { lx_fsm_set_bool(c, 6, false); fsm_send(c, 7); return; }
    lx_fsm_set_string(c, 2, lx_animator_state(c->scene, a, layer));
    lx_fsm_set_string(c, 3, lx_animator_state_tag(c->scene, a, layer));
    lx_fsm_set_float(c, 4, lx_animator_normalized_time(c->scene, a, layer));
    lx_fsm_set_bool(c, 5, lx_animator_in_transition(c->scene, a, layer));
    lx_fsm_set_bool(c, 6, true);
}
static const LxFsmActionDesc A_AG_GET_STATE = {"Get Animator State", "Animator Graph",
    "Which state a layer of an animator graph is playing, its tag, how far through it is, and whether it is mid-transition.",
    "state of {0}",
    {POBJ("Object"), PT("Layer", LX_FSM_INT, "0", "0 is the base layer"),
     POPT("Store State", LX_FSM_STRING), POPTT("Store Tag", LX_FSM_STRING, "the state's label, when it has one"),
     POPTT("Store Normalized Time", LX_FSM_FLOAT, "in cycles: 0.5 is half way through"),
     POPT("Store In Transition", LX_FSM_BOOL), POPTT("Store Has Graph", LX_FSM_BOOL, "false when the animator has no graph"),
     PEV("No Graph Event")},
    0, 0, LX_FSM_EVERY_FRAME, ag_get_state, ag_get_state, NULL};

typedef struct { char last[LX_FSM_NAME]; bool started; } FsmStateWatch;
static void ag_state_changed(LxFsmCtx *c) {
    FsmStateWatch *w = c->state;
    LxEntity a = ag_target(c, 0);
    if (!a) return;
    const char *now = lx_animator_state(c->scene, a, lx_fsm_int(c, 1));
    if (!w->started) { w->started = true; lx_strcpy(w->last, sizeof w->last, now); return; }
    if (!strcmp(w->last, now)) return;
    lx_strcpy(w->last, sizeof w->last, now);
    lx_fsm_set_string(c, 2, now);
    fsm_send(c, 3);
}
static const LxFsmActionDesc A_AG_STATE_CHANGED = {"Animator State Changed", "Animator Graph",
    "Watches a layer and sends an event whenever it enters a different state - footstep sounds, hit frames, finishing a swing.",
    "watch {0} layer {1}",
    {POBJ("Object"), PT("Layer", LX_FSM_INT, "0", "0 is the base layer"),
     POPT("Store State", LX_FSM_STRING), PEV("Changed Event")},
    sizeof(FsmStateWatch), 0, LX_FSM_EVERY_FRAME, NULL, ag_state_changed, NULL};

static void ag_layer_weight(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    if (!a) { fsm_send(c, 4); return; }
    int layer = lx_fsm_int(c, 1);
    if (!lx_fsm_is_none(c, 2)) lx_animator_set_layer_weight(c->scene, a, layer, lx_fsm_float(c, 2));
    lx_fsm_set_float(c, 3, lx_animator_layer_weight(c->scene, a, layer));
}
static const LxFsmActionDesc A_AG_LAYER_WEIGHT = {"Set Animator Layer Weight", "Animator Graph",
    "How much of a layer shows: fade an upper-body aim in over a walk, or take an injury layer away as the character heals.",
    "layer {1} of {0} = {2}",
    {POBJ("Object"), PT("Layer", LX_FSM_INT, "1", "layer 0 is the base and always shows in full"),
     PT("Weight", LX_FSM_FLOAT, "1", "0 to 1; None only reads it"), POPT("Store Weight", LX_FSM_FLOAT),
     PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, ag_layer_weight, ag_layer_weight, NULL};

static void ag_set_graph(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    LxAnimator *an = a ? lx_get_animator(c->scene, a) : NULL;
    if (!an) { fsm_send(c, 2); return; }
    const char *ref = lx_fsm_asset(c, 1);
    lx_strcpy(an->graph, sizeof an->graph, ref ? ref : "");
}
static const LxFsmActionDesc A_AG_SET_GRAPH = {"Set Animator Graph", "Animator Graph",
    "Swaps which animator graph drives an object - a different move set for a different weapon, or a possessed version of the same character.",
    "{0} uses {1}",
    {POBJ("Object"), PASSETT("Animator Graph", "animator", "a .lxanim asset; empty goes back to playing single clips"),
     PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, ag_set_graph, ag_set_graph, NULL};

/* ==========================================================================================
 *  inverse kinematics
 * ========================================================================================== */
#define FSM_IK_GOALS "Left Foot|Right Foot|Left Hand|Right Hand"

static void ag_ik_goal(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    if (!a) { fsm_send(c, 5); return; }
    LxIkGoal goal = (LxIkGoal)lx_fsm_int(c, 1);
    if (goal < 0 || goal >= LX_IK_GOAL_COUNT) goal = LX_IK_LEFT_HAND;
    float weight = lx_fsm_float(c, 3);
    LxEntity at = fsm_obj(c, 2);
    LxVec3 pos = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 4);
    if (weight > 0) lx_animator_set_ik_goal(c->scene, a, goal, pos, weight);
    else lx_animator_clear_ik_goal(c->scene, a, goal);
}
static const LxFsmActionDesc A_AG_IK_GOAL = {"Set IK Goal", "Animator Graph",
    "Puts a hand or a foot on a point in the world and bends the limb to reach it - a hand on a ledge, a foot on a step.",
    "{1} of {0} to {2}",
    {POBJ("Object"), PENUMT("Goal", FSM_IK_GOALS, "2", "which limb to move"),
     POBJNT("Target Object", "its position is used; None uses Position below"),
     PT("Weight", LX_FSM_FLOAT, "1", "0 to 1; 0 turns the goal off"),
     PT("Position", LX_FSM_VEC3, "0 0 0", "used when there is no Target Object"),
     PEV("No Animator Event")},
    0, 0, LX_FSM_EVERY_FRAME, ag_ik_goal, ag_ik_goal, NULL};

static void ag_look_at(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    if (!a) { fsm_send(c, 4); return; }
    float weight = lx_fsm_float(c, 2);
    LxEntity at = fsm_obj(c, 1);
    LxVec3 pos = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 3);
    if (weight > 0) lx_animator_set_look_at(c->scene, a, pos, weight);
    else lx_animator_clear_look_at(c->scene, a);
}
static const LxFsmActionDesc A_AG_LOOK_AT = {"Set Look At", "Animator Graph",
    "Turns a character's head towards something, over whatever the animation is already doing.",
    "{0} looks at {1}",
    {POBJ("Object"), POBJNT("Target Object", "None uses Position below"),
     PT("Weight", LX_FSM_FLOAT, "1", "0 to 1; 0 lets the head go"),
     PT("Position", LX_FSM_VEC3, "0 0 0", "used when there is no Target Object"), PEV("No Animator Event")},
    0, 0, LX_FSM_EVERY_FRAME, ag_look_at, ag_look_at, NULL};

static void ag_foot_ik(LxFsmCtx *c) {
    LxEntity a = ag_target(c, 0);
    LxAnimator *an = a ? lx_get_animator(c->scene, a) : NULL;
    if (!an) { fsm_send(c, 3); return; }
    an->foot_ik = lx_fsm_bool(c, 1);
    if (!lx_fsm_is_none(c, 2)) an->foot_ik_weight = lx_clampf(lx_fsm_float(c, 2), 0, 1);
}
static const LxFsmActionDesc A_AG_FOOT_IK = {"Set Foot IK", "Animator Graph",
    "Plants the feet on the ground under them, so a walk authored on the flat holds up on a slope or a stair.",
    "foot IK on {0} = {1}",
    {POBJ("Object"), P("Enabled", LX_FSM_BOOL, "true"),
     PT("Weight", LX_FSM_FLOAT, "", "0 to 1; None leaves it alone"), PEV("No Animator Event")},
    0, 0, LX_FSM_ONCE, ag_foot_ik, ag_foot_ik, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_anim(void) {
    static const LxFsmActionDesc *all[] = {
        &A_AN_PLAY, &A_AN_STOP, &A_AN_GET, &A_AN_SET, &A_AN_SPEED_FROM, &A_AN_WAIT,
        &A_AN_CLIP_INFO, &A_AN_FIND_BONE,
        &A_AG_SET_FLOAT, &A_AG_SET_INT, &A_AG_SET_BOOL, &A_AG_TRIGGER, &A_AG_GET, &A_AG_PLAY_STATE,
        &A_AG_GET_STATE, &A_AG_STATE_CHANGED, &A_AG_LAYER_WEIGHT, &A_AG_SET_GRAPH,
        &A_AG_IK_GOAL, &A_AG_LOOK_AT, &A_AG_FOOT_IK,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
