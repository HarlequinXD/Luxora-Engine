/* Sound: the Audio Source, Audio Listener and Audio Mixer categories.
 *
 * Luxora mixes through four fixed buses (SFX, Music, Voice, Ambient) rather than mixer assets with
 * named groups, so mixing is done through Set Bus Volume. Spatial sources are placed by
 * the acoustics engine, which handles occlusion, reflections and Doppler on its own. */
#include "fsm_actions.h"

#define FSM_BUSES "SFX|Music|Voice|Ambient"

static LxAudioBus fsm_bus(int i) {
    switch (i) {
    case 1: return LX_BUS_MUSIC;
    case 2: return LX_BUS_VOICE;
    case 3: return LX_BUS_AMBIENT;
    default: return LX_BUS_SFX;
    }
}

/* ==========================================================================================
 *  one-shot sounds
 * ========================================================================================== */
static void au_play_oneshot(LxFsmCtx *c) {
    const char *clip = lx_fsm_asset(c, 0);
    if (!clip || !clip[0]) return;
    LxEntity at = fsm_obj(c, 1);
    LxVec3 pos = at ? lx_entity_world_position(c->scene, at) : lx_fsm_vec3(c, 2);
    float vol = lx_fsm_float(c, 3);
    float spread = lx_fsm_float(c, 4);
    if (spread > 0) vol *= lx_random_range(lx_maxf(0.0f, 1.0f - spread), 1.0f + spread);
    lx_audio_play_oneshot(clip, pos, lx_maxf(0, vol));
}
static const LxFsmActionDesc A_AU_ONESHOT = {"Play Sound", "Audio Source", "Fires off a sound at a point in the world and forgets about it: footsteps, gunshots, impacts.",
    "play {0} at {1}",
    {PASSET("Sound", "sound"), POBJT("At Object", "where the sound comes from"),
     PT("Position", LX_FSM_VEC3, "0 0 0", "used when At Object is None"), P("Volume", LX_FSM_FLOAT, "1"),
     PT("Volume Variation", LX_FSM_FLOAT, "0", "randomises the volume a little so repeats do not sound identical")},
    0, 0, LX_FSM_ONCE, au_play_oneshot, au_play_oneshot, NULL};

static void au_play_random(LxFsmCtx *c) {
    const char *clips[4];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        const char *s = lx_fsm_asset(c, i);
        if (s && s[0]) clips[n++] = s;
    }
    if (n == 0) return;
    LxEntity at = fsm_obj(c, 4);
    LxVec3 pos = at ? lx_entity_world_position(c->scene, at) : lx_v3(0, 0, 0);
    lx_audio_play_oneshot(clips[lx_random_int(0, n - 1)], pos, lx_maxf(0, lx_fsm_float(c, 5)));
}
static const LxFsmActionDesc A_AU_RANDOM = {"Play Random Sound", "Audio Source", "Picks one of up to four sounds at random: footstep sets, hit sounds, voice barks.",
    "play one of {0}..{3}",
    {PASSET("Sound 1", "sound"), PASSET("Sound 2", "sound"), PASSET("Sound 3", "sound"), PASSET("Sound 4", "sound"),
     POBJ("At Object"), P("Volume", LX_FSM_FLOAT, "1")},
    0, 0, LX_FSM_ONCE, au_play_random, au_play_random, NULL};

/* ==========================================================================================
 *  audio sources on objects
 * ========================================================================================== */
static void au_source_play(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (!e) return;
    LxAudioSource *src = lx_get_audio_source(c->scene, e);
    if (!src) return;
    const char *clip = lx_fsm_asset(c, 1);
    if (clip && clip[0]) lx_strcpy(src->clip, sizeof src->clip, clip);
    if (!lx_fsm_is_none(c, 2)) src->volume = lx_maxf(0, lx_fsm_float(c, 2));
    if (!lx_fsm_is_none(c, 3)) src->pitch = lx_clampf(lx_fsm_float(c, 3), 0.05f, 4.0f);
    if (!lx_fsm_is_none(c, 4)) src->loop = lx_fsm_bool(c, 4);
    lx_audio_play(c->scene, e);
}
static const LxFsmActionDesc A_AU_SOURCE_PLAY = {"Play Audio Source", "Audio Source", "Starts the Audio Source on an object, optionally swapping the clip and settings first.",
    "play source on {0}",
    {POBJ("Object"), PASSETT("Sound", "sound", "leave empty to keep the source's own clip"),
     PIN_OPT("Volume", LX_FSM_FLOAT), PT("Pitch", LX_FSM_FLOAT, "", "1 is normal; higher is faster and higher pitched"),
     PIN_OPT("Loop", LX_FSM_BOOL)},
    0, 0, LX_FSM_ONCE, au_source_play, au_source_play, NULL};

static void au_source_stop(LxFsmCtx *c) {
    if (lx_fsm_bool(c, 1)) { lx_audio_stop_all(); return; }
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_audio_stop(c->scene, e);
}
static const LxFsmActionDesc A_AU_SOURCE_STOP = {"Stop Audio Source", "Audio Source", "Stops the sound on an object, or every sound at once.", "stop {0}",
    {POBJ("Object"), PT("Stop Everything", LX_FSM_BOOL, "false", "silence every sound in the game")},
    0, 0, LX_FSM_ONCE, au_source_stop, au_source_stop, NULL};

static void au_source_set(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxAudioSource *src = e ? lx_get_audio_source(c->scene, e) : NULL;
    if (!src) return;
    if (!lx_fsm_is_none(c, 1)) src->volume = lx_maxf(0, lx_fsm_float(c, 1));
    if (!lx_fsm_is_none(c, 2)) src->pitch = lx_clampf(lx_fsm_float(c, 2), 0.05f, 4.0f);
    if (!lx_fsm_is_none(c, 3)) src->loop = lx_fsm_bool(c, 3);
    if (!lx_fsm_is_none(c, 4)) src->spatial = lx_fsm_bool(c, 4);
    if (!lx_fsm_is_none(c, 5)) src->min_distance = lx_maxf(0, lx_fsm_float(c, 5));
    if (!lx_fsm_is_none(c, 6)) src->max_distance = lx_maxf(src->min_distance, lx_fsm_float(c, 6));
    if (!lx_fsm_is_none(c, 7)) src->bus = fsm_bus(lx_fsm_int(c, 7));
}
static const LxFsmActionDesc A_AU_SOURCE_SET = {"Set Audio Source", "Audio Source", "Changes a source's volume, pitch, looping and how far it carries. None leaves a setting alone.",
    "source on {0}",
    {POBJ("Object"), PIN_OPT("Volume", LX_FSM_FLOAT), PIN_OPT("Pitch", LX_FSM_FLOAT), PIN_OPT("Loop", LX_FSM_BOOL),
     PT("Spatial", LX_FSM_BOOL, "", "positioned in the world rather than played flat"),
     PT("Min Distance", LX_FSM_FLOAT, "", "full volume within this"), PT("Max Distance", LX_FSM_FLOAT, "", "silent beyond this"),
     {"Bus", LX_FSM_ENUM, "", LX_FSM_P_OPTIONAL, FSM_BUSES, "which mix bus it plays on"}},
    0, 0, LX_FSM_ONCE, au_source_set, au_source_set, NULL};

static void au_source_get(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    LxAudioSource *src = e ? lx_get_audio_source(c->scene, e) : NULL;
    lx_fsm_set_bool(c, 4, src != NULL);
    if (!src) return;
    lx_fsm_set_string(c, 1, src->clip);
    lx_fsm_set_float(c, 2, src->volume);
    lx_fsm_set_float(c, 3, src->pitch);
    const LxSound *snd = src->clip[0] ? lx_asset_sound(src->clip) : NULL;
    lx_fsm_set_float(c, 5, snd ? lx_sound_duration(snd) : 0.0f);
}
static const LxFsmActionDesc A_AU_SOURCE_GET = {"Get Audio Source", "Audio Source", "Reads a source's clip, volume, pitch and how long the clip is.", "source on {0}",
    {POBJ("Object"), POPT("Store Clip", LX_FSM_STRING), POPT("Store Volume", LX_FSM_FLOAT), POPT("Store Pitch", LX_FSM_FLOAT),
     POPT("Store Has Source", LX_FSM_BOOL), POPTT("Store Clip Length", LX_FSM_FLOAT, "seconds")},
    0, 0, LX_FSM_ONCE, au_source_get, au_source_get, NULL};

typedef struct { float t, from; bool started; } FsmAudioFade;
static void au_fade(LxFsmCtx *c) {
    FsmAudioFade *f = c->state;
    LxEntity e = fsm_obj(c, 0);
    LxAudioSource *src = e ? lx_get_audio_source(c->scene, e) : NULL;
    if (!src) { lx_fsm_finish(c); return; }
    if (!f->started) { f->started = true; f->from = src->volume; }
    float len = lx_fsm_float(c, 2);
    f->t += c->dt;
    float k = len > 0 ? lx_saturate(f->t / len) : 1.0f;
    src->volume = lx_lerpf(f->from, lx_maxf(0, lx_fsm_float(c, 1)), k);
    if (k >= 1.0f) {
        if (lx_fsm_bool(c, 3) && src->volume <= 0.0001f) lx_audio_stop(c->scene, e);
        lx_fsm_finish(c);
        fsm_send(c, 4);
    }
}
static const LxFsmActionDesc A_AU_FADE = {"Fade Audio Source", "Audio Source", "Brings a source's volume up or down over time, and can stop it once it is silent.",
    "fade {0} to {1} over {2}s",
    {POBJ("Object"), P("Target Volume", LX_FSM_FLOAT, "0"), P("Duration", LX_FSM_FLOAT, "1"),
     PT("Stop When Silent", LX_FSM_BOOL, "true", "stop the source once it reaches zero"), PEV("Finish Event")},
    sizeof(FsmAudioFade), LX_FSM_A_CONTINUOUS, LX_FSM_EVERY_FRAME, au_fade, au_fade, NULL};

/* ==========================================================================================
 *  music
 * ========================================================================================== */
static void au_music_play(LxFsmCtx *c) {
    const char *clip = lx_fsm_asset(c, 0);
    if (!clip || !clip[0]) return;
    lx_music_play(c->scene, clip, lx_maxf(0, lx_fsm_float(c, 1)), lx_maxf(0, lx_fsm_float(c, 2)));
}
static const LxFsmActionDesc A_AU_MUSIC_PLAY = {"Play Music", "Audio Mixer", "Starts a music track, crossfading out whatever was playing. Only one track plays at a time.",
    "music {0}",
    {PASSET("Music", "sound"), P("Volume", LX_FSM_FLOAT, "1"), PT("Fade Seconds", LX_FSM_FLOAT, "1", "how long the crossfade takes")},
    0, 0, LX_FSM_ONCE, au_music_play, au_music_play, NULL};

static void au_music_stop(LxFsmCtx *c) { lx_music_stop(lx_maxf(0, lx_fsm_float(c, 0))); }
static const LxFsmActionDesc A_AU_MUSIC_STOP = {"Stop Music", "Audio Mixer", "Fades the music out.", "stop music over {0}s",
    {P("Fade Seconds", LX_FSM_FLOAT, "1")}, 0, 0, LX_FSM_ONCE, au_music_stop, au_music_stop, NULL};

static void au_music_get(LxFsmCtx *c) {
    const char *cur = lx_music_current();
    lx_fsm_set_string(c, 0, cur ? cur : "");
    bool playing = cur && cur[0];
    lx_fsm_set_bool(c, 1, playing);
    fsm_send(c, playing ? 2 : 3);
}
static const LxFsmActionDesc A_AU_MUSIC_GET = {"Get Music", "Audio Mixer", "Which music track is playing, if any.", "music -> {0}",
    {POPT("Store Track", LX_FSM_STRING), POPT("Store Is Playing", LX_FSM_BOOL), PEV("Playing Event"), PEV("Silent Event")},
    0, 0, LX_FSM_ONCE, au_music_get, au_music_get, NULL};

/* ==========================================================================================
 *  mixing
 * ========================================================================================== */
static void au_bus_volume(LxFsmCtx *c) {
    LxAudioSettings *s = lx_audio_settings();
    if (!s) return;
    int bus = lx_fsm_int(c, 0);
    if (lx_fsm_is_none(c, 1)) {
        lx_fsm_set_float(c, 2, bus == 4 ? s->master_volume : s->bus_volume[fsm_bus(bus)]);
        return;
    }
    float v = lx_saturate(lx_fsm_float(c, 1));
    if (bus == 4) s->master_volume = v;
    else s->bus_volume[fsm_bus(bus)] = v;
    lx_fsm_set_float(c, 2, v);
}
static const LxFsmActionDesc A_AU_BUS = {"Set Bus Volume", "Audio Mixer", "Sets one of the mix buses, or the master volume: exactly what an audio options menu needs. Leave Volume as None to read it instead.",
    "{0} volume {1}",
    {PENUM("Bus", FSM_BUSES "|Master", "4"), PT("Volume", LX_FSM_FLOAT, "", "0..1; None reads the current value"),
     POPT("Store Volume", LX_FSM_FLOAT)},
    0, 0, LX_FSM_ONCE, au_bus_volume, au_bus_volume, NULL};

static void au_settings(LxFsmCtx *c) {
    LxAudioSettings *s = lx_audio_settings();
    if (!s) return;
    if (!lx_fsm_is_none(c, 0)) s->propagation = lx_fsm_bool(c, 0);
    if (!lx_fsm_is_none(c, 1)) s->doppler = lx_fsm_bool(c, 1);
    if (!lx_fsm_is_none(c, 2)) s->speed_of_sound = lx_maxf(1.0f, lx_fsm_float(c, 2));
    lx_fsm_set_int(c, 3, lx_audio_playing_count());
}
static const LxFsmActionDesc A_AU_SETTINGS = {"Set Audio Settings", "Audio Mixer", "Turns the ray-traced acoustics and Doppler on or off, and reports how many sounds are audible.",
    "audio settings",
    {PT("Propagation", LX_FSM_BOOL, "", "ray-traced reflections and occlusion"),
     PT("Doppler", LX_FSM_BOOL, "", "pitch shifts as things rush past"),
     PT("Speed Of Sound", LX_FSM_FLOAT, "", "343 m/s in air"), POPT("Store Playing Count", LX_FSM_INT)},
    0, 0, LX_FSM_ONCE, au_settings, au_settings, NULL};

static void au_listener(LxFsmCtx *c) {
    LxEntity e = fsm_obj(c, 0);
    if (e) lx_add_audio_listener(c->scene, e);
    lx_fsm_set_object(c, 1, lx_scene_listener(c->scene));
}
static const LxFsmActionDesc A_AU_LISTENER = {"Set Audio Listener", "Audio Listener", "Moves the ears of the game onto an object: first person on the camera, third person on the player.",
    "listener = {0}",
    {POBJ("Object"), POPT("Store Listener", LX_FSM_OBJECT)}, 0, 0, LX_FSM_ONCE, au_listener, au_listener, NULL};

/* ==========================================================================================
 *  registration
 * ========================================================================================== */
void fsm_register_audio(void) {
    static const LxFsmActionDesc *all[] = {
        &A_AU_ONESHOT, &A_AU_RANDOM,
        &A_AU_SOURCE_PLAY, &A_AU_SOURCE_STOP, &A_AU_SOURCE_SET, &A_AU_SOURCE_GET, &A_AU_FADE,
        &A_AU_MUSIC_PLAY, &A_AU_MUSIC_STOP, &A_AU_MUSIC_GET,
        &A_AU_BUS, &A_AU_SETTINGS, &A_AU_LISTENER,
    };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) lx_fsm_register_action(all[i]);
}
