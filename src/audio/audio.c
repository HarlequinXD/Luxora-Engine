/* Luxora audio: mixer, WAV loading and physically based sound propagation.
 *
 * Every playing voice writes its samples into a history ring buffer. The listener hears
 *  - the direct path, read back with a delay of distance / speed-of-sound (smoothly varying,
 *    which produces Doppler shift naturally), attenuated by distance, air absorption and,
 *    when geometry blocks the line of sight, by the blocking material's transmission plus a
 *    low-pass filter (sound barriers);
 *  - early reflections: rays are traced from the source through the scene, losing energy to
 *    each surface's absorption; wherever the listener is visible from a bounce point a tap is
 *    added with the full path's delay and direction (stereo panned);
 *  - late reverberation: a feedback delay network whose decay time is estimated from how
 *    quickly the traced ray energy dies away in the current space. */
#include "../lx_internal.h"

#define MAX_VOICES 64
#define HISTORY 65536          /* ~1.36 s at 48 kHz => ~470 m of sound travel */
#define MAX_TAPS 24
#define FDN_LINES 4

typedef struct Tap { float delay; float gl, gr; } Tap;

typedef struct VoiceParams {
    float direct_delay;       /* samples */
    float direct_gl, direct_gr;
    float lowpass;            /* 0..1 coefficient, 1 = open */
    Tap taps[MAX_TAPS];
    int tap_count;
    float reverb_send;
    float pitch;
} VoiceParams;

typedef struct Voice {
    bool active, loop, spatial, stopping;
    LxSound *sound;
    double cursor;            /* position in source samples */
    float volume;
    LxAudioBus bus;
    LxScene *scene;
    LxEntity entity;          /* 0 for one-shots */
    LxVec3 oneshot_pos;
    float *history;
    uint32_t write_pos;
    float cur_delay;          /* smoothed */
    float lp_state;
    VoiceParams target, cur;
    bool params_valid;
    float fade;               /* fade-out for stop */
    uint32_t serial;
} Voice;

typedef struct Fdn {
    float *lines[FDN_LINES];
    int len[FDN_LINES];
    int pos[FDN_LINES];
    float gain[FDN_LINES];
    float lp[FDN_LINES];
    float rt60;
} Fdn;

static struct {
    bool initialised;
    LxAudioSettings settings;
    Voice voices[MAX_VOICES];
    LxMutex *lock;
    LxThread *thread;
    volatile long quit;
    const char *backend;
    int rate;
    Fdn fdn;
    float target_rt60;
    float room_send;
    /* acoustic geometry */
    LxBvh bvh;
    LxScene *bvh_scene;
    uint32_t bvh_version;
    double last_prop_time;
    uint32_t serial;
} AU;

/* ===================================================================
 *  WAV loading
 * =================================================================== */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

LxSound *lx_sound_load_wav(const char *path) {
    size_t n = 0;
    uint8_t *d = (uint8_t *)lx_file_read(path, &n);
    if (!d) return NULL;
    if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { lx_free(d); return NULL; }
    int fmt = 0, channels = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t data_len = 0;
    size_t pos = 12;
    while (pos + 8 <= n) {
        uint32_t len = rd32(d + pos + 4);
        const uint8_t *body = d + pos + 8;
        if (pos + 8 + len > n) len = (uint32_t)(n - pos - 8);
        if (!memcmp(d + pos, "fmt ", 4) && len >= 16) {
            fmt = rd16(body);
            channels = rd16(body + 2);
            rate = (int)rd32(body + 4);
            bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24);
        } else if (!memcmp(d + pos, "data", 4)) {
            data = body;
            data_len = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!data || channels < 1 || rate <= 0 || (fmt != 1 && fmt != 3)) { lx_free(d); return NULL; }
    int bps = bits / 8;
    if (bps < 1) { lx_free(d); return NULL; }
    int frames = (int)(data_len / (uint32_t)(bps * channels));
    float *mono = lx_alloc(sizeof(float) * (size_t)(frames ? frames : 1));
    for (int f = 0; f < frames; f++) {
        float sum = 0;
        for (int c = 0; c < channels; c++) {
            const uint8_t *s = data + ((size_t)f * (size_t)channels + (size_t)c) * (size_t)bps;
            float v = 0;
            if (fmt == 3 && bits == 32) { memcpy(&v, s, 4); }
            else if (bits == 8) v = ((float)s[0] - 128.0f) / 128.0f;
            else if (bits == 16) v = (float)(int16_t)rd16(s) / 32768.0f;
            else if (bits == 24) v = (float)((int32_t)((uint32_t)s[0] << 8 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 24) >> 8) / 8388608.0f;
            else if (bits == 32) v = (float)(int32_t)rd32(s) / 2147483648.0f;
            sum += v;
        }
        mono[f] = sum / (float)channels;
    }
    lx_free(d);
    LxSound *snd = lx_calloc(1, sizeof *snd);
    snd->samples = mono;
    snd->frame_count = frames;
    snd->sample_rate = rate;
    lx_strcpy(snd->ref, sizeof snd->ref, path);
    return snd;
}

float lx_sound_duration(const LxSound *s) { return s && s->sample_rate ? (float)s->frame_count / (float)s->sample_rate : 0.0f; }

/* ===================================================================
 *  Reverb (feedback delay network with Householder mixing)
 * =================================================================== */
static void fdn_init(Fdn *f, int rate) {
    static const float ms[FDN_LINES] = {29.7f, 37.1f, 41.1f, 43.7f};
    for (int i = 0; i < FDN_LINES; i++) {
        f->len[i] = (int)(ms[i] * 0.001f * (float)rate);
        f->lines[i] = lx_calloc((size_t)f->len[i], sizeof(float));
        f->pos[i] = 0;
        f->lp[i] = 0;
    }
    f->rt60 = 0;
}

static void fdn_set_rt60(Fdn *f, float rt60, int rate) {
    f->rt60 = rt60;
    for (int i = 0; i < FDN_LINES; i++) {
        float t = (float)f->len[i] / (float)rate;
        f->gain[i] = rt60 > 0.01f ? powf(10.0f, -3.0f * t / rt60) : 0.0f;
    }
}

static void fdn_process(Fdn *f, float in, float *l, float *r) {
    float o[FDN_LINES];
    for (int i = 0; i < FDN_LINES; i++) o[i] = f->lines[i][f->pos[i]];
    float sum = (o[0] + o[1] + o[2] + o[3]) * 0.5f;
    for (int i = 0; i < FDN_LINES; i++) {
        float v = (o[i] - sum) * f->gain[i];
        f->lp[i] += (v - f->lp[i]) * 0.6f; /* air/surface damping of high frequencies */
        f->lines[i][f->pos[i]] = f->lp[i] + in;
        if (++f->pos[i] >= f->len[i]) f->pos[i] = 0;
    }
    *l = (o[0] - o[2]) * 0.5f + (o[1] * 0.3f);
    *r = (o[1] - o[3]) * 0.5f + (o[2] * 0.3f);
}

/* ===================================================================
 *  Mixer (runs on the audio device thread)
 * =================================================================== */
static inline float history_read(const Voice *v, float delay) {
    float p = (float)v->write_pos - delay;
    while (p < 0) p += HISTORY;
    uint32_t i0 = (uint32_t)p;
    float fr = p - (float)i0;
    uint32_t a = i0 & (HISTORY - 1), b = (i0 + 1) & (HISTORY - 1);
    return v->history[a] + (v->history[b] - v->history[a]) * fr;
}

static void mix(float *out, int frames) {
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    lx_mutex_lock(AU.lock);
    float master = AU.settings.master_volume;
    float *reverb_in = lx_calloc((size_t)frames, sizeof(float));
    for (int vi = 0; vi < MAX_VOICES; vi++) {
        Voice *v = &AU.voices[vi];
        if (!v->active || !v->sound) continue;
        if (!v->params_valid) {
            /* non-spatial or not yet propagated: centred, no delay */
            memset(&v->target, 0, sizeof v->target);
            v->target.direct_gl = v->target.direct_gr = 0.7071f;
            v->target.lowpass = 1.0f;
            v->target.pitch = 1.0f;
            v->cur = v->target;
            v->params_valid = true;
        }
        float bus = AU.settings.bus_volume[v->bus] * master * v->volume;
        const LxSound *s = v->sound;
        float step = v->target.pitch * (float)s->sample_rate / (float)AU.rate;
        for (int f = 0; f < frames; f++) {
            /* produce the source sample into history */
            float smp = 0;
            if (v->cursor < s->frame_count) {
                int i0 = (int)v->cursor;
                float fr = (float)(v->cursor - i0);
                float a = s->samples[i0], b = (i0 + 1 < s->frame_count) ? s->samples[i0 + 1] : (v->loop ? s->samples[0] : 0.0f);
                smp = a + (b - a) * fr;
                v->cursor += step;
                if (v->cursor >= s->frame_count && v->loop) v->cursor -= s->frame_count;
            }
            if (v->stopping) { v->fade -= 1.0f / 512.0f; if (v->fade <= 0) { v->fade = 0; } smp *= v->fade; }
            v->history[v->write_pos & (HISTORY - 1)] = smp;
            v->write_pos++;
            /* parameter smoothing (~5 ms); delay glides -> doppler */
            float k = 0.004f;
            float delay_k = AU.settings.doppler ? 0.0008f : 0.02f;
            v->cur_delay += (v->target.direct_delay - v->cur_delay) * delay_k;
            v->cur.direct_gl += (v->target.direct_gl - v->cur.direct_gl) * k;
            v->cur.direct_gr += (v->target.direct_gr - v->cur.direct_gr) * k;
            v->cur.lowpass += (v->target.lowpass - v->cur.lowpass) * k;
            v->cur.reverb_send += (v->target.reverb_send - v->cur.reverb_send) * k;
            float direct = history_read(v, v->cur_delay);
            v->lp_state += (direct - v->lp_state) * v->cur.lowpass;
            float l = v->lp_state * v->cur.direct_gl, r = v->lp_state * v->cur.direct_gr;
            for (int t = 0; t < v->target.tap_count; t++) {
                const Tap *tp = &v->target.taps[t];
                float x = history_read(v, tp->delay);
                l += x * tp->gl;
                r += x * tp->gr;
            }
            out[f * 2] += l * bus;
            out[f * 2 + 1] += r * bus;
            reverb_in[f] += history_read(v, v->cur_delay) * v->cur.reverb_send * bus;
        }
        /* finished? allow the tail (max delay) to drain before releasing */
        bool source_done = v->cursor >= s->frame_count && !v->loop;
        if ((source_done || (v->stopping && v->fade <= 0)) && v->write_pos > 0) {
            float max_delay = v->cur_delay;
            for (int t = 0; t < v->target.tap_count; t++) max_delay = lx_maxf(max_delay, v->target.taps[t].delay);
            static uint32_t drained[MAX_VOICES];
            if (drained[vi] == 0) drained[vi] = v->write_pos;
            if ((float)(v->write_pos - drained[vi]) > max_delay + 256.0f) { v->active = false; drained[vi] = 0; }
        }
    }
    if (AU.fdn.rt60 != AU.target_rt60) fdn_set_rt60(&AU.fdn, AU.target_rt60, AU.rate);
    for (int f = 0; f < frames; f++) {
        float l, r;
        fdn_process(&AU.fdn, reverb_in[f] * 0.25f, &l, &r);
        out[f * 2] += l;
        out[f * 2 + 1] += r;
    }
    lx_free(reverb_in);
    lx_mutex_unlock(AU.lock);
    /* soft clip */
    for (int i = 0; i < frames * 2; i++) {
        float x = out[i];
        out[i] = x / (1.0f + fabsf(x) * 0.25f) * 1.0f;
    }
}

/* ===================================================================
 *  Device backends
 * =================================================================== */
#if LX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
static int backend_thread(void *u) {
    LX_UNUSED(u);
    HWAVEOUT wo;
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)AU.rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * 4;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        AU.backend = "null";
        return 0;
    }
    enum { NB = 4, FR = 512 };
    WAVEHDR hdr[NB];
    int16_t *bufs = lx_calloc(NB * FR * 2, sizeof(int16_t));
    float *mixbuf = lx_alloc(sizeof(float) * FR * 2);
    for (int i = 0; i < NB; i++) {
        memset(&hdr[i], 0, sizeof hdr[i]);
        hdr[i].lpData = (LPSTR)(bufs + i * FR * 2);
        hdr[i].dwBufferLength = FR * 4;
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        hdr[i].dwFlags |= WHDR_DONE;
    }
    while (!lx_atomic_load(&AU.quit)) {
        bool wrote = false;
        for (int i = 0; i < NB; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            mix(mixbuf, FR);
            int16_t *dst = bufs + i * FR * 2;
            for (int k = 0; k < FR * 2; k++) dst[k] = (int16_t)(lx_clampf(mixbuf[k], -1, 1) * 32767.0f);
            hdr[i].dwFlags &= ~(DWORD)WHDR_DONE;
            waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
            wrote = true;
        }
        if (!wrote) WaitForSingleObject(ev, 20);
    }
    waveOutReset(wo);
    for (int i = 0; i < NB; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
    waveOutClose(wo);
    CloseHandle(ev);
    lx_free(bufs);
    lx_free(mixbuf);
    return 0;
}
static bool backend_start(void) {
    AU.backend = "WinMM";
    AU.thread = lx_thread_create(backend_thread, NULL);
    return true;
}
#elif LX_OS_ANDROID
#include <aaudio/AAudio.h>
static AAudioStream *g_stream;
static aaudio_data_callback_result_t aa_cb(AAudioStream *s, void *u, void *data, int32_t frames) {
    LX_UNUSED(s); LX_UNUSED(u);
    mix((float *)data, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static bool backend_start(void) {
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, AU.rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, aa_cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) return false;
    AU.rate = AAudioStream_getSampleRate(g_stream);
    AAudioStream_requestStart(g_stream);
    AU.backend = "AAudio";
    return true;
}
#else
#include <dlfcn.h>
/* ALSA through dlopen: no headers or link-time dependency needed. */
typedef struct snd_pcm snd_pcm_t;
typedef int (*Fn_open)(snd_pcm_t **, const char *, int, int);
typedef int (*Fn_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
typedef long (*Fn_writei)(snd_pcm_t *, const void *, unsigned long);
typedef int (*Fn_recover)(snd_pcm_t *, int, int);
typedef int (*Fn_close)(snd_pcm_t *);
static struct { void *lib; snd_pcm_t *pcm; Fn_writei writei; Fn_recover recover; Fn_close close; } AL;
static int backend_thread(void *u) {
    LX_UNUSED(u);
    enum { FR = 512 };
    float *mixbuf = lx_alloc(sizeof(float) * FR * 2);
    int16_t *pcm = lx_alloc(sizeof(int16_t) * FR * 2);
    while (!lx_atomic_load(&AU.quit)) {
        mix(mixbuf, FR);
        for (int k = 0; k < FR * 2; k++) pcm[k] = (int16_t)(lx_clampf(mixbuf[k], -1, 1) * 32767.0f);
        long r = AL.writei(AL.pcm, pcm, FR);
        if (r < 0) AL.recover(AL.pcm, (int)r, 1);
    }
    lx_free(mixbuf);
    lx_free(pcm);
    return 0;
}
static bool backend_start(void) {
    AL.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!AL.lib) return false;
    Fn_open open_fn = (Fn_open)dlsym(AL.lib, "snd_pcm_open");
    Fn_set_params set_params = (Fn_set_params)dlsym(AL.lib, "snd_pcm_set_params");
    AL.writei = (Fn_writei)dlsym(AL.lib, "snd_pcm_writei");
    AL.recover = (Fn_recover)dlsym(AL.lib, "snd_pcm_recover");
    AL.close = (Fn_close)dlsym(AL.lib, "snd_pcm_close");
    if (!open_fn || !set_params || !AL.writei || !AL.recover) return false;
    if (open_fn(&AL.pcm, "default", 0 /* playback */, 0) < 0) return false;
    /* SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3 */
    if (set_params(AL.pcm, 2, 3, 2, (unsigned)AU.rate, 1, 40000) < 0) { AL.close(AL.pcm); return false; }
    AU.backend = "ALSA";
    AU.thread = lx_thread_create(backend_thread, NULL);
    return true;
}
#endif

/* ===================================================================
 *  Public API
 * =================================================================== */
bool lx_audio_init(void) {
    if (AU.initialised) return true;
    AU.lock = lx_mutex_create();
    if (!AU.settings.sample_rate) AU.settings = lx_audio_defaults();
    AU.rate = AU.settings.sample_rate;
    for (int i = 0; i < MAX_VOICES; i++) AU.voices[i].history = lx_calloc(HISTORY, sizeof(float));
    fdn_init(&AU.fdn, AU.rate);
    AU.backend = "null";
    AU.initialised = true;
    if (!getenv("LUXORA_NO_AUDIO") && !backend_start()) {
        AU.backend = "null";
        LX_WARN("No audio device available; running silently");
    }
    LX_INFO("Audio: %s @ %d Hz, propagation %s", AU.backend, AU.rate, AU.settings.propagation ? "on" : "off");
    return true;
}

void lx_audio_shutdown(void) {
    if (!AU.initialised) return;
    lx_atomic_store(&AU.quit, 1);
    if (AU.thread) lx_thread_join(AU.thread);
#if LX_OS_ANDROID
    if (g_stream) { AAudioStream_requestStop(g_stream); AAudioStream_close(g_stream); g_stream = NULL; }
#elif LX_OS_LINUX
    if (AL.pcm) AL.close(AL.pcm);
#endif
    for (int i = 0; i < MAX_VOICES; i++) lx_free(AU.voices[i].history);
    for (int i = 0; i < FDN_LINES; i++) lx_free(AU.fdn.lines[i]);
    lx_bvh_free(&AU.bvh);
    lx_mutex_destroy(AU.lock);
    memset(&AU, 0, sizeof AU);
}

LxAudioSettings *lx_audio_settings(void) {
    if (!AU.settings.sample_rate) AU.settings = lx_audio_defaults();
    return &AU.settings;
}
const char *lx_audio_backend_name(void) { return AU.backend ? AU.backend : "uninitialised"; }

static Voice *alloc_voice(void) {
    for (int i = 0; i < MAX_VOICES; i++)
        if (!AU.voices[i].active) return &AU.voices[i];
    return NULL;
}

static void start_voice(Voice *v, LxSound *snd, float volume, bool loop, bool spatial, LxAudioBus bus) {
    v->sound = snd;
    v->cursor = 0;
    v->volume = volume;
    v->loop = loop;
    v->spatial = spatial;
    v->bus = bus;
    v->stopping = false;
    v->fade = 1.0f;
    v->params_valid = false;
    v->cur_delay = 0;
    v->lp_state = 0;
    v->write_pos = 0;
    memset(v->history, 0, sizeof(float) * HISTORY);
    v->serial = ++AU.serial;
    v->active = true;
}

void lx_audio_play(LxScene *s, LxEntity e) {
    if (!AU.initialised) return;
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || !(d->comps & LX_COMP_AUDIO_SOURCE)) return;
    LxSound *snd = lx_asset_sound(d->audio.clip);
    if (!snd) return;
    lx_mutex_lock(AU.lock);
    if (d->audio_voice >= 0 && AU.voices[d->audio_voice].entity == e) AU.voices[d->audio_voice].active = false;
    Voice *v = alloc_voice();
    if (v) {
        start_voice(v, snd, d->audio.volume, d->audio.loop, d->audio.spatial, d->audio.bus);
        v->scene = s;
        v->entity = e;
        d->audio_voice = (int)(v - AU.voices);
    }
    lx_mutex_unlock(AU.lock);
}

void lx_audio_stop(LxScene *s, LxEntity e) {
    if (!AU.initialised) return;
    LxEntityData *d = lx_entity_data(s, e);
    if (!d || d->audio_voice < 0) return;
    lx_mutex_lock(AU.lock);
    Voice *v = &AU.voices[d->audio_voice];
    if (v->active && v->entity == e) { v->stopping = true; v->entity = 0; }
    lx_mutex_unlock(AU.lock);
    d->audio_voice = -1;
}

void lx_audio_stop_all(void) {
    if (!AU.initialised) return;
    lx_mutex_lock(AU.lock);
    for (int i = 0; i < MAX_VOICES; i++) { AU.voices[i].active = false; AU.voices[i].entity = 0; }
    lx_mutex_unlock(AU.lock);
}

void lx_audio_play_oneshot(const char *clip, LxVec3 pos, float volume) {
    if (!AU.initialised) return;
    LxSound *snd = lx_asset_sound(clip);
    if (!snd) return;
    lx_mutex_lock(AU.lock);
    Voice *v = alloc_voice();
    if (v) {
        start_voice(v, snd, volume, false, true, LX_BUS_SFX);
        v->entity = 0;
        v->scene = NULL;
        v->oneshot_pos = pos;
    }
    lx_mutex_unlock(AU.lock);
}

int lx_audio_playing_count(void) {
    if (!AU.initialised) return 0;
    int n = 0;
    lx_mutex_lock(AU.lock);
    for (int i = 0; i < MAX_VOICES; i++) n += AU.voices[i].active && !AU.voices[i].stopping;
    lx_mutex_unlock(AU.lock);
    return n;
}

/* ---------------- preview channel ---------------- */
static int g_preview_voice = -1;
static uint32_t g_preview_serial;

void lx_audio_preview_stop(void) {
    if (!AU.initialised || g_preview_voice < 0) return;
    lx_mutex_lock(AU.lock);
    Voice *v = &AU.voices[g_preview_voice];
    if (v->active && v->serial == g_preview_serial) v->stopping = true;   /* short fade, no click */
    lx_mutex_unlock(AU.lock);
    g_preview_voice = -1;
}

bool lx_audio_preview_playing(void) {
    if (!AU.initialised || g_preview_voice < 0) return false;
    lx_mutex_lock(AU.lock);
    const Voice *v = &AU.voices[g_preview_voice];
    bool playing = v->active && !v->stopping && v->serial == g_preview_serial;
    lx_mutex_unlock(AU.lock);
    if (!playing) g_preview_voice = -1;
    return playing;
}

void lx_audio_preview(const char *clip, float volume, float pitch) {
    lx_audio_preview_stop();
    if (!AU.initialised) return;
    LxSound *snd = lx_asset_sound(clip);
    if (!snd) return;
    lx_mutex_lock(AU.lock);
    Voice *v = alloc_voice();
    if (v) {
        start_voice(v, snd, volume, false, false, LX_BUS_SFX);
        v->entity = 0;
        v->scene = NULL;
        v->cur.pitch = v->target.pitch = pitch > 0.05f ? pitch : 1.0f;
        g_preview_voice = (int)(v - AU.voices);
        g_preview_serial = v->serial;
    }
    lx_mutex_unlock(AU.lock);
}

/* ---------------- propagation ---------------- */
typedef struct PropResult {
    VoiceParams p;
    float energy_decay_rt60;
} PropResult;

static float pan_left(float pan) { return cosf((pan + 1.0f) * LX_PI * 0.25f); }
static float pan_right(float pan) { return sinf((pan + 1.0f) * LX_PI * 0.25f); }

static void propagate(const LxBvh *bvh, LxVec3 src, LxVec3 lst, LxVec3 lst_right, float min_d, float max_d, int rate,
                      PropResult *out) {
    const LxAudioSettings *as = &AU.settings;
    float c = as->speed_of_sound > 1.0f ? as->speed_of_sound : 343.0f;
    memset(out, 0, sizeof *out);
    VoiceParams *p = &out->p;
    p->pitch = 1.0f;
    LxVec3 to_src = lx_sub3(src, lst);
    float d = lx_len3(to_src);
    float att = min_d / lx_maxf(d, min_d);
    if (d > max_d) att *= lx_maxf(0.0f, 1.0f - (d - max_d) / (max_d * 0.25f + 1.0f));
    att *= expf(-as->air_absorption * d);
    float pan = d > 1e-3f ? lx_dot3(lx_scale3(to_src, 1.0f / d), lst_right) : 0.0f;
    p->direct_delay = d / c * (float)rate;
    p->lowpass = 1.0f;
    if (bvh && bvh->tri_count && d > 1e-3f) {
        LxRay r = {lst, lx_scale3(to_src, 1.0f / d)};
        /* accumulate transmission through every blocking surface on the straight path */
        float travelled = 0.0f;
        int walls = 0;
        LxHit hit;
        while (walls < 4 && lx_bvh_intersect(bvh, r, d - travelled - 1e-3f, &hit)) {
            const LxTriMaterial *m = &bvh->materials[bvh->mat_index[hit.tri]];
            att *= lx_clampf(m->transmission, 0.0f, 1.0f) * 0.5f + 0.02f;
            p->lowpass *= 0.35f;
            travelled += hit.t + 1e-3f;
            r.origin = lx_madd3(r.origin, r.dir, hit.t + 1e-3f);
            walls++;
        }
    }
    p->direct_gl = att * pan_left(pan);
    p->direct_gr = att * pan_right(pan);
    if (!as->propagation || !bvh || !bvh->tri_count) { p->reverb_send = 0.0f; return; }

    /* ray-traced early reflections + energy decay for the late reverb */
    int rays = as->rays_per_source < 16 ? 16 : as->rays_per_source;
    int bounces = as->max_bounces < 1 ? 1 : as->max_bounces;
    enum { BINS = 64 };
    float bin_l[BINS] = {0}, bin_r[BINS] = {0}, bin_delay[BINS];
    float bin_ms = 4.0f;
    for (int b = 0; b < BINS; b++) bin_delay[b] = (float)b * bin_ms;
    double energy_hist[32] = {0}; /* energy per 25 ms */
    float golden = LX_PI * (3.0f - sqrtf(5.0f));
    for (int i = 0; i < rays; i++) {
        /* fibonacci sphere: deterministic, even coverage */
        float y = 1.0f - (2.0f * ((float)i + 0.5f)) / (float)rays;
        float rr = sqrtf(lx_maxf(0, 1 - y * y));
        LxVec3 dir = lx_v3(cosf(golden * (float)i) * rr, y, sinf(golden * (float)i) * rr);
        LxVec3 o = src;
        float energy = 1.0f / (float)rays, path = 0.0f;
        for (int b = 0; b < bounces; b++) {
            LxHit hit;
            if (!lx_bvh_intersect(bvh, (LxRay){o, dir}, 500.0f, &hit)) break;
            LxVec3 pos = lx_madd3(o, dir, hit.t);
            LxVec3 n = bvh->normals[hit.tri];
            if (lx_dot3(n, dir) > 0) n = lx_neg3(n);
            const LxTriMaterial *m = &bvh->materials[bvh->mat_index[hit.tri]];
            path += hit.t;
            energy *= (1.0f - lx_clampf(m->absorption, 0.0f, 1.0f)) * expf(-as->air_absorption * hit.t);
            int eb = (int)(path / c / 0.025f);
            if (eb < 32) energy_hist[eb] += energy;
            /* is the listener visible from this reflection point? */
            LxVec3 to_l = lx_sub3(lst, pos);
            float dl = lx_len3(to_l);
            if (dl > 1e-3f && lx_dot3(to_l, n) > 0) {
                LxVec3 dirl = lx_scale3(to_l, 1.0f / dl);
                if (!lx_bvh_occluded(bvh, (LxRay){lx_madd3(pos, n, 1e-3f), dirl}, dl - 2e-3f)) {
                    float total = path + dl;
                    float ms = total / c * 1000.0f;
                    int bin = (int)(ms / bin_ms);
                    if (bin < BINS) {
                        /* Lambert-diffuse scattering towards the listener, inverse-distance pressure */
                        float cosv = lx_dot3(dirl, n);
                        float amp = sqrtf(energy * cosv * 2.0f) * (min_d / lx_maxf(total, min_d)) * 6.0f;
                        float tp = -lx_dot3(dirl, lst_right);
                        bin_l[bin] += amp * pan_left(tp);
                        bin_r[bin] += amp * pan_right(tp);
                    }
                }
            }
            o = lx_madd3(pos, n, 1e-3f);
            dir = lx_reflect3(dir, n);
            if (energy < 1e-6f) break;
        }
    }
    /* keep the strongest bins as delay taps */
    for (int t = 0; t < MAX_TAPS; t++) {
        int best = -1;
        float best_e = 1e-5f;
        for (int b = 0; b < BINS; b++) {
            float e = bin_l[b] + bin_r[b];
            if (e > best_e) { best_e = e; best = b; }
        }
        if (best < 0) break;
        Tap *tp = &p->taps[p->tap_count++];
        tp->delay = (bin_delay[best] + bin_ms * 0.5f) * 0.001f * (float)rate;
        tp->gl = bin_l[best];
        tp->gr = bin_r[best];
        bin_l[best] = bin_r[best] = 0;
    }
    /* RT60 from the slope of the energy decay (Schroeder-style log-linear fit) */
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    for (int b = 0; b < 32; b++) {
        if (energy_hist[b] <= 1e-9) continue;
        double x = b * 0.025, yv = 10.0 * log10(energy_hist[b]);
        sx += x; sy += yv; sxx += x * x; sxy += x * yv;
        n++;
    }
    float rt60 = 0.0f;
    if (n >= 3) {
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        if (slope < -1.0) rt60 = (float)(-60.0 / slope);
    }
    out->energy_decay_rt60 = lx_clampf(rt60, 0.0f, 8.0f);
    double total_e = 0;
    for (int b = 0; b < 32; b++) total_e += energy_hist[b];
    p->reverb_send = lx_clampf((float)sqrt(total_e) * 0.6f, 0.0f, 0.8f) * att / lx_maxf(att, 0.05f) * 0.5f;
}

int lx_acoustic_impulse(LxScene *s, LxVec3 src, LxVec3 lst, float *ir, int len, int rate) {
    LxBvh bvh;
    memset(&bvh, 0, sizeof bvh);
    lx_bvh_build_from_scene(&bvh, s);
    if (!AU.settings.sample_rate) AU.settings = lx_audio_defaults();
    PropResult pr;
    propagate(&bvh, src, lst, lx_v3(1, 0, 0), 1.0f, 100.0f, rate, &pr);
    memset(ir, 0, sizeof(float) * (size_t)len);
    int count = 0;
    int di = (int)pr.p.direct_delay;
    if (di < len) { ir[di] += pr.p.direct_gl + pr.p.direct_gr; count++; }
    for (int t = 0; t < pr.p.tap_count; t++) {
        int i = (int)pr.p.taps[t].delay;
        if (i < len) { ir[i] += pr.p.taps[t].gl + pr.p.taps[t].gr; count++; }
    }
    lx_bvh_free(&bvh);
    return count;
}

void lx_audio_update(LxScene *s, float dt) {
    LX_UNUSED(dt);
    if (!AU.initialised || !s) return;
    lx_scene_update_transforms(s);
    /* start play-on-start sources while simulating */
    if (s->simulating) {
        for (int i = 1; i < s->count; i++) {
            LxEntityData *d = &s->entities[i];
            if (d->alive && d->active && (d->comps & LX_COMP_AUDIO_SOURCE) && d->audio.play_on_start && d->audio_voice == -1 && d->audio.clip[0]) {
                d->audio_voice = -2; /* mark as started even if the clip is missing */
                lx_audio_play(s, lx_entity_make((uint32_t)i, d->generation & 0xfffu));
            }
        }
    }
    if (AU.settings.propagation && (AU.bvh_scene != s || AU.bvh_version != s->static_version)) {
        lx_bvh_build_from_scene(&AU.bvh, s);
        AU.bvh_scene = s;
        AU.bvh_version = s->static_version;
    }
    LxEntity le = lx_scene_listener(s);
    LxVec3 lpos = le ? lx_entity_world_position(s, le) : lx_v3(0, 0, 0);
    LxVec3 lright = le ? lx_entity_right(s, le) : lx_v3(1, 0, 0);
    /* propagation at ~20 Hz; the mixer interpolates in between */
    double now = lx_time();
    if (now - AU.last_prop_time < 0.05) return;
    AU.last_prop_time = now;
    float rt_sum = 0;
    int rt_n = 0;
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &AU.voices[i];
        if (!v->active || !v->spatial) continue;
        LxVec3 spos;
        float min_d = 1.0f, max_d = 60.0f;
        if (v->entity) {
            LxEntityData *d = lx_entity_data(s, v->entity);
            if (!d || v->scene != s) continue;
            spos = lx_v3(d->world.m[12], d->world.m[13], d->world.m[14]);
            min_d = d->audio.min_distance;
            max_d = d->audio.max_distance;
            v->volume = d->audio.volume;
        } else spos = v->oneshot_pos;
        PropResult pr;
        propagate(AU.settings.propagation ? &AU.bvh : NULL, spos, lpos, lright, min_d, max_d, AU.rate, &pr);
        if (pr.energy_decay_rt60 > 0) { rt_sum += pr.energy_decay_rt60; rt_n++; }
        lx_mutex_lock(AU.lock);
        bool first = !v->params_valid;
        v->target = pr.p;
        if (first) { v->cur = pr.p; v->cur_delay = pr.p.direct_delay; v->params_valid = true; }
        lx_mutex_unlock(AU.lock);
    }
    AU.target_rt60 = rt_n ? rt_sum / (float)rt_n : AU.target_rt60 * 0.9f;
}
