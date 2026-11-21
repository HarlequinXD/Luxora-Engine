/* Day-night sky and weather.
 *
 * The clock places the sun and moon (latitude, season, heading). Their light reaches the ground through a
 * physically based atmosphere: Rayleigh scattering (blue sky, red sunsets), Mie scattering from haze (the
 * bright glow around the sun) and ozone absorption (the blue of twilight). The same model runs in the sky
 * shader per pixel (scene_common.glsl, lx_atmosphere); here it gives:
 *   - the colour and brightness of the scene's sun light (the first directional light, which this drives:
 *     the sun by day, the moon by night),
 *   - the sky's colour overhead, at the horizon and from the ground, which everything that used the
 *     Gradient sky reads (ambient light, reflections, photon GI, fog) through lx_env_effective.
 * Weather: the scene's settings are targets; while playing the live weather moves towards them over the
 * blend time, the ground gets wet in rain and white in snow, and lightning strikes with thunder that
 * arrives as late as sound would.
 */
#include "../lx_internal.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ the atmosphere (keep in step with lx_atmosphere in scene_common.glsl) */
#define RG 6360e3f                 /* planet radius (m) */
#define RT 6460e3f                 /* top of the atmosphere */
#define HR 8000.0f                 /* Rayleigh scale height */
#define HM 1200.0f                 /* Mie (haze) scale height */
#define MIE_S 3.996e-6f            /* haze scattering at haze = 1 */
#define MIE_G 0.8f
#define MS_BOOST 0.35f             /* light scattered more than once, folded into the phase functions */
#define SKY_GAIN 1.8f              /* single scattering misses light; brings the sky up to what a camera sees */
static const float BR[3] = {5.802e-6f, 13.558e-6f, 33.1e-6f};
static const float BO[3] = {0.650e-6f * 1.875f, 1.881e-6f * 1.875f, 0.085e-6f * 1.875f};   /* ozone, following the air */

/* Schueler's approximation of the Chapman function: the air along a ray from height h (in scale heights,
 * X = planet radius in scale heights) with cosine mu to the vertical, out to space, in scale heights of air
 * at sea-level density. A ray into the planet carries no light. */
static float chapman(float X, float h, float mu) {
    float c = sqrtf(X + h);
    if (mu >= 0.0f) return c / (c * mu + 1.0f) * expf(-h);
    float x0 = sqrtf(lx_maxf(0.0f, 1.0f - mu * mu)) * (X + h);
    if (x0 < X) return 1e9f;
    float c0 = sqrtf(x0);
    return 2.0f * c0 * expf(X - x0) - c / (1.0f - c * mu) * expf(-h);
}
static LxVec3 sun_transmittance(float h, float mu, float haze) {
    float r = HR * chapman(RG / HR, h / HR, mu), m = HM * chapman(RG / HM, h / HM, mu);
    float me = MIE_S * haze * 1.11f;
    return lx_v3(expf(-(BR[0] + BO[0]) * r - me * m), expf(-(BR[1] + BO[1]) * r - me * m), expf(-(BR[2] + BO[2]) * r - me * m));
}

LxVec3 lx_sky_atmosphere(LxVec3 d, LxVec3 s, float E, float haze, float height, LxVec3 albedo, LxVec3 *trans) {
    const int N = 24;
    float r0 = RG + lx_maxf(height, 1.0f);
    float b = r0 * d.y;
    float t_top = -b + sqrtf(lx_maxf(0.0f, b * b - (r0 * r0 - RT * RT)));
    float dg = b * b - (r0 * r0 - RG * RG);
    bool ground = d.y < 0.0f && dg > 0.0f;
    float L = ground ? -b - sqrtf(dg) : t_top;
    float mu = lx_dot3(d, s);
    float pr = 3.0f / (16.0f * LX_PI) * (1.0f + mu * mu) + MS_BOOST / (4.0f * LX_PI);
    float g2 = MIE_G * MIE_G;
    float pm = 3.0f / (8.0f * LX_PI) * (1.0f - g2) * (1.0f + mu * mu) / ((2.0f + g2) * powf(1.0f + g2 - 2.0f * MIE_G * mu, 1.5f)) +
               MS_BOOST / (4.0f * LX_PI);
    float bms = MIE_S * haze, bme = bms * 1.11f;
    float odr = 0, odm = 0, S[3] = {0, 0, 0};
    for (int i = 0; i < N; i++) {
        float a0 = (float)i / N, a1 = (float)(i + 1) / N;
        float t0 = L * a0 * a0, t1 = L * a1 * a1, dt = t1 - t0, t = 0.5f * (t0 + t1);
        LxVec3 p = lx_v3(d.x * t, r0 + d.y * t, d.z * t);
        float r = lx_len3(p), h = lx_maxf(r - RG, 0.0f);
        float mus = lx_dot3(lx_scale3(p, 1.0f / r), s);
        float rr = expf(-h / HR), rm = expf(-h / HM);
        float cr = odr + rr * dt * 0.5f, cm = odm + rm * dt * 0.5f;   /* air between the eye and this sample */
        odr += rr * dt;
        odm += rm * dt;
        float sr = HR * chapman(RG / HR, h / HR, mus), sm = HM * chapman(RG / HM, h / HM, mus);
        for (int c = 0; c < 3; c++) {
            float T = expf(-(BR[c] + BO[c]) * (cr + sr) - bme * (cm + sm));
            S[c] += T * (BR[c] * rr * pr + bms * rm * pm) * dt;
        }
    }
    LxVec3 tv = lx_v3(expf(-(BR[0] + BO[0]) * odr - bme * odm), expf(-(BR[1] + BO[1]) * odr - bme * odm),
                      expf(-(BR[2] + BO[2]) * odr - bme * odm));
    if (trans) *trans = tv;
    LxVec3 out = lx_v3(S[0] * E * SKY_GAIN, S[1] * E * SKY_GAIN, S[2] * E * SKY_GAIN);
    if (ground) {   /* the planet's surface, lit by the sun through the air above it */
        LxVec3 p = lx_v3(d.x * L, r0 + d.y * L, d.z * L);
        LxVec3 up = lx_norm3(p);
        float mus = lx_dot3(up, s);
        if (mus > 0) out = lx_add3(out, lx_mul3(lx_mul3(lx_scale3(albedo, E * mus / LX_PI), sun_transmittance(0, mus, haze)), tv));
    }
    return out;
}

/* ------------------------------------------------------------------ the clock: where the sun and moon are */
static LxVec3 to_world(LxVec3 enu, float heading_deg) {   /* (east, north, up) -> world, east = +X turned by the heading */
    float a = heading_deg * LX_DEG2RAD, c = cosf(a), s = sinf(a);
    float x = enu.x, z = -enu.y;   /* north is -Z, so +X east, +Y up */
    return lx_v3(x * c - z * s, enu.z, x * s + z * c);
}
static LxVec3 body_dir(float hour_angle_deg, float decl_deg, float lat_deg, float heading) {
    float H = hour_angle_deg * LX_DEG2RAD, d = decl_deg * LX_DEG2RAD, p = lat_deg * LX_DEG2RAD;
    LxVec3 enu = lx_v3(-cosf(d) * sinf(H), cosf(p) * sinf(d) - sinf(p) * cosf(d) * cosf(H), sinf(p) * sinf(d) + cosf(p) * cosf(d) * cosf(H));
    return lx_norm3(to_world(enu, heading));
}
static float wrap24(float h) { h = fmodf(h, 24.0f); return h < 0 ? h + 24.0f : h; }
static float declination(const LxEnvironment *e) { return 23.44f * sinf(2.0f * LX_PI * (284.0f + e->day_of_year) / 365.0f); }
static LxVec3 sun_dir_at(const LxEnvironment *e, float hour) {
    return body_dir((wrap24(hour) - 12.0f) * 15.0f, declination(e), lx_clampf(e->latitude, -89.9f, 89.9f), e->sun_heading);
}
static LxVec3 moon_dir_at(const LxEnvironment *e, float hour) {
    /* the moon trails the sun by its phase (full: opposite it), on roughly the opposite declination when full */
    float ph = e->moon_phase - floorf(e->moon_phase);
    return body_dir((wrap24(hour) - 12.0f) * 15.0f - ph * 360.0f, declination(e) * cosf(2.0f * LX_PI * ph),
                    lx_clampf(e->latitude, -89.9f, 89.9f), e->sun_heading);
}

/* ------------------------------------------------------------------ weather presets */
static const char *WEATHER_NAMES[LX_WEATHER_COUNT] = {"Clear", "Cloudy", "Rain", "Storm", "Snow", "Fog", "Custom"};
const char *lx_weather_name(LxWeather w) { return (unsigned)w < LX_WEATHER_COUNT ? WEATHER_NAMES[w] : "Custom"; }
void lx_sky_weather_preset(LxWeather w, LxEnvironment *e) {
    if (!e || w < 0 || w >= LX_WEATHER_CUSTOM) return;
    /*                     coverage density type               rain  snow  lightning mist  wind haze */
    static const float P[LX_WEATHER_CUSTOM][9] = {
        /* Clear  */ {0.25f, 0.45f, LX_CLOUDS_CUMULUS, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f, 1.5f},
        /* Cloudy */ {0.70f, 0.65f, LX_CLOUDS_STRATUS, 0.0f, 0.0f, 0.0f, 0.05f, 6.0f, 2.5f},
        /* Rain   */ {0.88f, 0.85f, LX_CLOUDS_STRATUS, 0.7f, 0.0f, 0.0f, 0.2f, 8.0f, 3.5f},
        /* Storm  */ {0.97f, 1.00f, LX_CLOUDS_STORM, 1.0f, 0.0f, 0.6f, 0.3f, 14.0f, 4.0f},
        /* Snow   */ {0.85f, 0.70f, LX_CLOUDS_STRATUS, 0.0f, 0.7f, 0.0f, 0.25f, 3.0f, 3.0f},
        /* Fog    */ {0.55f, 0.55f, LX_CLOUDS_STRATUS, 0.0f, 0.0f, 0.0f, 0.9f, 1.0f, 6.0f},
    };
    const float *p = P[w];
    e->cloud_coverage = p[0]; e->cloud_density = p[1]; e->cloud_type = (int)p[2];
    e->rain = p[3]; e->snow = p[4]; e->lightning = p[5]; e->mist = p[6]; e->wind_speed = p[7]; e->haze = p[8];
    e->weather = w;
}

/* ------------------------------------------------------------------ live state */
static float approach(float v, float target, float step) {
    return v < target ? lx_minf(target, v + step) : lx_maxf(target, v - step);
}
static void live_snap(LxSkyLive *L, const LxEnvironment *e) {
    L->coverage = e->cloud_coverage; L->density = e->cloud_density; L->ctype = (float)e->cloud_type;
    L->rain = e->rain; L->snow = e->snow; L->lightning = e->lightning; L->mist = e->mist;
    L->haze = e->haze; L->wind_speed = e->wind_speed;
    L->wetness = lx_minf(1.0f, e->rain * 1.4f);
    L->snow_cover = lx_minf(1.0f, e->snow * 1.2f);
}
static void live_init(LxScene *s) {
    LxSkyLive *L = &s->sky;
    if (L->started) return;
    float off[2] = {L->wind_offset[0], L->wind_offset[1]};
    memset(L, 0, sizeof *L);
    live_snap(L, &s->env);
    L->wind_offset[0] = off[0]; L->wind_offset[1] = off[1];
    L->bolt_age = 1e3f;
    L->flash_t = 1e3f;
    L->thunder_in = -1;
    L->next_strike = 2.0f;
    L->rng = 0x9e3779b9u ^ (uint32_t)(s->env.time_of_day * 1000.0f);
    L->last_hour = s->env.time_of_day;
    L->last_sun_y = sun_dir_at(&s->env, s->env.time_of_day).y;
    L->started = true;
}
static float rnd(LxSkyLive *L) {
    uint32_t x = L->rng ? L->rng : 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    L->rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

/* the scene's sun light: its first active directional light */
static LxEntity sun_entity(LxScene *s) {
    for (int i = 1; i < s->count; i++) {
        LxEntityData *d = &s->entities[i];
        if (d->alive && d->active && (d->comps & LX_COMP_LIGHT) && d->light.type == LX_LIGHT_DIRECTIONAL)
            return lx_entity_make((uint32_t)i, d->generation & ((1u << (32 - LX_INDEX_BITS)) - 1u));
    }
    return 0;
}

static float smooth01(float a, float b, float x) { float t = lx_clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static float lum(LxVec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

/* the flicker of one lightning strike: a bright stroke, a dim gap, a return stroke, then a fade */
static float flash_curve(float t) {
    if (t < 0) return 0;
    if (t < 0.06f) return 1.0f;
    if (t < 0.12f) return 0.25f;
    if (t < 0.20f) return 0.85f;
    if (t < 0.26f) return 0.3f;
    if (t < 0.32f) return 0.6f;
    return 0.6f * expf(-(t - 0.32f) * 9.0f);
}

/* how much sunlight the clouds take, on average (the sky shader and the cloud shadows use the same terms) */
static float cloud_shade(float coverage, float density, float ctype) {
    float c = lx_clampf(coverage, 0, 1);
    float thick = lx_clampf(density, 0, 1) * (0.55f + 0.25f * lx_clampf(ctype, 0, 2));
    return lx_clampf(c * c * (0.35f + 0.6f * thick) + c * 0.1f, 0.0f, 0.97f);
}

void lx_sky_prepare(LxScene *s) {
    if (!s) return;
    LxEnvironment *e = &s->env;
    LxSkyLive *L = &s->sky;
    LxSkyEval *v = &L->eval;
    if (e->sky_mode != LX_SKY_DAY_NIGHT) { v->day_night = false; L->started = false; return; }
    live_init(s);
    if (!s->simulating) live_snap(L, e);   /* editing: what the panel says, at once */
    /* cached: the clock, the settings and the live weather decide everything below */
    float key[32] = {e->time_of_day, e->latitude, e->day_of_year, e->sun_heading, e->sun_intensity, e->moon_intensity, e->moon_phase,
                     e->haze, e->ambient_intensity, e->fog_density, e->ground_color.x, e->ground_color.y, e->ground_color.z,
                     L->coverage, L->density, L->ctype, L->rain, L->snow, L->mist, L->haze, L->wind_speed, e->wind_direction,
                     L->wetness, L->snow_cover, L->wind_offset[0], L->wind_offset[1], L->flash_t, L->bolt_age, e->star_intensity,
                     e->exposure, (float)s->simulating, 0};
    uint64_t h = 1469598103934665603ull;
    const uint8_t *kb = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof key; i++) h = (h ^ kb[i]) * 1099511628211ull;
    LxEntity sun = sun_entity(s);
    if (h != L->eval_hash || !v->day_night) {
        L->eval_hash = h;
        memset(v, 0, sizeof *v);
        v->day_night = true;
        float haze = lx_clampf(L->haze, 0.5f, 12.0f);
        v->sun_dir = sun_dir_at(e, e->time_of_day);
        v->moon_dir = moon_dir_at(e, e->time_of_day);
        const float H0 = 60.0f;   /* the eye a little above the ground */
        LxVec3 ts = sun_transmittance(H0, v->sun_dir.y, haze);
        LxVec3 tm = sun_transmittance(H0, v->moon_dir.y, haze);
        float ph = e->moon_phase - floorf(e->moon_phase);
        float lit = 0.5f - 0.5f * cosf(2.0f * LX_PI * ph);   /* how much of the moon is lit */
        float Es = lx_maxf(0.0f, e->sun_intensity), Em = lx_maxf(0.0f, e->moon_intensity) * lit;
        LxVec3 albedo = e->ground_color;
        /* sky colours: straight up, and around the horizon */
        LxVec3 up = lx_v3(0, 1, 0);
        LxVec3 zen = lx_add3(lx_sky_atmosphere(up, v->sun_dir, Es, haze, H0, albedo, NULL),
                             lx_sky_atmosphere(up, v->moon_dir, Em, haze, H0, albedo, NULL));
        LxVec3 hor = lx_v3(0, 0, 0);
        for (int k = 0; k < 4; k++) {
            float a = (float)k * LX_PI * 0.5f + 0.4f;
            LxVec3 d = lx_norm3(lx_v3(cosf(a), 0.08f, sinf(a)));
            hor = lx_add3(hor, lx_sky_atmosphere(d, v->sun_dir, Es, haze, H0, albedo, NULL));
            hor = lx_add3(hor, lx_sky_atmosphere(d, v->moon_dir, Em, haze, H0, albedo, NULL));
        }
        hor = lx_scale3(hor, 0.25f);
        /* a faint glow of the night sky itself (airglow, starlight), so nights are never pure black */
        LxVec3 night_glow = lx_scale3(lx_v3(0.012f, 0.016f, 0.03f), 0.15f + e->star_intensity * 0.1f);
        zen = lx_add3(zen, night_glow);
        hor = lx_add3(hor, night_glow);
        LxVec3 sun_ground = lx_scale3(ts, Es * smooth01(-0.02f, 0.03f, v->sun_dir.y));
        LxVec3 moon_ground = lx_scale3(lx_mul3(tm, lx_v3(0.78f, 0.86f, 1.0f)), Em * smooth01(-0.02f, 0.05f, v->moon_dir.y));
        /* clouds: they take direct light, and grey the sky towards their own colour (lit from above) */
        v->coverage = L->coverage; v->density = L->density; v->ctype = L->ctype;
        v->cloud_shade = cloud_shade(L->coverage, L->density, L->ctype);
        LxVec3 above = lx_add3(lx_scale3(sun_ground, lx_maxf(v->sun_dir.y, 0.0f)), lx_scale3(moon_ground, lx_maxf(v->moon_dir.y, 0.0f)));
        float cloud_l = (lum(above) + LX_PI * lum(zen)) * (0.30f - 0.22f * lx_clampf(L->density, 0, 1)) / LX_PI;
        LxVec3 cloud_col = lx_scale3(lx_v3(0.92f, 0.95f, 1.0f), cloud_l);
        float over = lx_clampf(L->coverage, 0, 1);
        over = over * over * (0.4f + 0.6f * lx_clampf(L->density, 0, 1));
        zen = lx_lerp3(zen, cloud_col, over);
        hor = lx_lerp3(hor, lx_scale3(cloud_col, 1.1f), over * 0.85f);
        v->sun_light = lx_scale3(sun_ground, 1.0f - v->cloud_shade);
        v->moon_light = lx_scale3(moon_ground, 1.0f - v->cloud_shade);
        v->zenith = zen;
        v->horizon = hor;
        /* the ground below the horizon: the albedo lit by sun, moon and sky */
        LxVec3 ground_irr = lx_add3(lx_add3(lx_scale3(v->sun_light, lx_maxf(v->sun_dir.y, 0.0f)), lx_scale3(v->moon_light, lx_maxf(v->moon_dir.y, 0.0f))),
                                    lx_scale3(lx_add3(zen, hor), LX_PI * 0.5f));
        v->ground = lx_scale3(lx_mul3(albedo, ground_irr), 1.0f / LX_PI);
        v->ambient = lx_maxf(0.0f, e->ambient_intensity) / 0.6f;   /* 0.6 is the scene default: physical sky light */
        /* weather */
        v->rain = L->rain; v->snow = L->snow; v->lightning = L->lightning; v->mist = L->mist; v->haze = haze;
        v->wind_speed = L->wind_speed; v->wind_dir = e->wind_direction;
        v->wetness = L->wetness; v->snow_cover = L->snow_cover;
        v->wind_offset[0] = L->wind_offset[0]; v->wind_offset[1] = L->wind_offset[1];
        v->fog = lx_lerp3(hor, lx_scale3(lx_add3(hor, zen), 0.5f), 0.3f);
        v->fog_density = lx_maxf(0.0f, e->fog_density) + L->mist * 0.03f + L->rain * 0.004f + L->snow * 0.008f;
        v->night = 1.0f - smooth01(-0.2f, 0.02f, v->sun_dir.y);
        v->flash = L->flash_t < 5.0f ? flash_curve(L->flash_t) : 0.0f;
        v->bolt_dir = L->bolt_dir;
        v->bolt_seed = L->bolt_seed;
        v->bolt_age = L->bolt_age;
        v->exposure_scale = 1.0f;
    }
    /* drive the sun light: the sun by day, the moon by night (dark in between, so the switch is never seen) */
    if (sun) {
        LxEntityData *d = lx_entity_data(s, sun);
        float ws = smooth01(-0.05f, 0.0f, v->sun_dir.y), wm = (1.0f - smooth01(-0.12f, -0.07f, v->sun_dir.y));
        bool moon = ws <= 0.0f && wm > 0.0f;
        LxVec3 to = moon ? v->moon_dir : v->sun_dir;
        LxVec3 col = moon ? lx_scale3(v->moon_light, wm) : lx_scale3(v->sun_light, ws);
        if (to.y < 0.02f) to = lx_norm3(lx_v3(to.x, 0.02f, to.z));   /* never lights from below the ground */
        LxVec3 fwd = lx_neg3(to);
        LxVec3 upv = fabsf(fwd.y) > 0.99f ? lx_v3(0, 0, 1) : lx_v3(0, 1, 0);
        LxQuat q = lx_quat_look(fwd, upv);
        LxVec3 cur = lx_norm3(lx_mat4_dir(d->world, lx_v3(0, 0, 1)));
        if (lx_dot3(cur, fwd) < 0.999999f) lx_entity_set_world_pose(s, sun, lx_entity_world_position(s, sun), q);
        float in = lx_maxf(col.x, lx_maxf(col.y, col.z));
        d = lx_entity_data(s, sun);
        d->light.intensity = in;
        if (in > 1e-6f) d->light.color = lx_scale3(col, 1.0f / in);
        if (d->light.mode != LX_LIGHT_REALTIME) d->light.mode = LX_LIGHT_REALTIME;   /* a moving sun can't be baked */
    }
}

const LxSkyEval *lx_sky_eval(LxScene *s) {
    static LxSkyEval none;
    if (!s || s->env.sky_mode != LX_SKY_DAY_NIGHT) return &none;
    if (!s->sky.eval.day_night) lx_sky_prepare(s);
    return &s->sky.eval;
}

void lx_env_effective(LxScene *s, LxEnvironment *out) {
    *out = s->env;
    if (s->env.sky_mode != LX_SKY_DAY_NIGHT) return;
    const LxSkyEval *v = lx_sky_eval(s);
    out->sky_zenith = v->zenith;
    out->sky_horizon = v->horizon;
    out->ground_color = v->ground;
    out->fog_color = v->fog;
    out->ambient_intensity = v->ambient;
    out->fog_density = v->fog_density;
}

/* ------------------------------------------------------------------ playing */
static void send(LxScene *s, const char *ev, float hour) { lx_fsm_broadcast_event(s, ev, hour, 0); }

static void strike(LxScene *s) {
    LxSkyLive *L = &s->sky;
    float a = rnd(L) * 2.0f * LX_PI;
    L->bolt_dir = lx_norm3(lx_v3(cosf(a), 0.0f, sinf(a)));
    L->bolt_dist = 800.0f + rnd(L) * rnd(L) * 7000.0f;
    L->bolt_seed = rnd(L) * 1000.0f;
    L->bolt_age = 0;
    L->flash_t = 0;
    L->thunder_in = L->bolt_dist / 343.0f;
    L->thunder_vol = lx_clampf(1.5f - L->bolt_dist / 5000.0f, 0.3f, 1.3f);
    send(s, "LIGHTNING", s->env.time_of_day);
}
void lx_sky_strike_lightning(LxScene *s) {
    if (!s || s->env.sky_mode != LX_SKY_DAY_NIGHT) return;
    live_init(s);
    strike(s);
}

void lx_sky_simulate(LxScene *s, float dt) {
    if (!s || s->env.sky_mode != LX_SKY_DAY_NIGHT || dt <= 0) return;
    LxEnvironment *e = &s->env;
    live_init(s);
    LxSkyLive *L = &s->sky;
    /* the clock */
    if (e->day_length > 0) e->time_of_day = wrap24(e->time_of_day + dt * 24.0f / (e->day_length * 60.0f));
    float hour = e->time_of_day, sy = sun_dir_at(e, hour).y;
    if (L->last_sun_y <= 0 && sy > 0) send(s, "SUNRISE", hour);
    if (L->last_sun_y > 0 && sy <= 0) send(s, "SUNSET", hour);
    float lh = L->last_hour;
    if (e->day_length > 0) {
        if (lh < 12.0f && hour >= 12.0f && hour - lh < 6.0f) send(s, "NOON", hour);
        if (hour < lh && lh - hour > 12.0f) send(s, "MIDNIGHT", hour);
    }
    L->last_hour = hour;
    L->last_sun_y = sy;
    /* the weather moves towards the settings, all of it arriving together after the blend time */
    float k = dt / lx_maxf(e->weather_blend, 0.01f);
    L->coverage = approach(L->coverage, e->cloud_coverage, k);
    L->density = approach(L->density, e->cloud_density, k);
    L->ctype = approach(L->ctype, (float)e->cloud_type, k * 2.0f);
    L->rain = approach(L->rain, e->rain, k);
    L->snow = approach(L->snow, e->snow, k);
    L->lightning = approach(L->lightning, e->lightning, k);
    L->mist = approach(L->mist, e->mist, k);
    L->haze = approach(L->haze, e->haze, k * 9.0f);
    L->wind_speed = approach(L->wind_speed, e->wind_speed, k * 20.0f);
    /* the ground gets wet in rain (half a minute) and dries slowly; snow settles and melts more slowly still */
    float wet_to = lx_minf(1.0f, L->rain * 1.4f);
    L->wetness = approach(L->wetness, wet_to, dt * (wet_to > L->wetness ? 1.0f / 30.0f : 1.0f / 120.0f));
    float snow_to = lx_minf(1.0f, L->snow * 1.2f);
    L->snow_cover = approach(L->snow_cover, snow_to, dt * (snow_to > L->snow_cover ? 1.0f / 60.0f : 1.0f / 180.0f));
    /* clouds drift with the wind */
    float wa = e->wind_direction * LX_DEG2RAD;
    L->wind_offset[0] = fmodf(L->wind_offset[0] + cosf(wa) * L->wind_speed * dt * 3.0f, 100000.0f);
    L->wind_offset[1] = fmodf(L->wind_offset[1] + sinf(wa) * L->wind_speed * dt * 3.0f, 100000.0f);
    /* lightning: random strikes, as often as the setting says, under thick enough cloud */
    L->bolt_age += dt;
    L->flash_t += dt;
    if (L->lightning > 0.01f && L->coverage > 0.4f) {
        L->next_strike -= dt;
        if (L->next_strike <= 0) {
            strike(s);
            float mean = 3.0f + 40.0f * (1.0f - L->lightning) * (1.0f - L->lightning);
            L->next_strike = -logf(lx_maxf(rnd(L), 1e-4f)) * mean + 0.5f;
        }
    }
    if (L->thunder_in >= 0) {
        L->thunder_in -= dt;
        if (L->thunder_in < 0) {   /* thunder: from the bolt's direction, close enough to the listener to be heard */
            LxEntity le = lx_scene_listener(s);
            LxVec3 at = le ? lx_entity_world_position(s, le) : lx_v3(0, 0, 0);
            lx_audio_play_oneshot("builtin:thunder", lx_madd3(at, lx_norm3(lx_v3(L->bolt_dir.x, 0.25f, L->bolt_dir.z)), 30.0f),
                                  L->thunder_vol);
        }
    }
}

/* ------------------------------------------------------------------ the API */
void lx_sky_set_time(LxScene *s, float hour) {
    if (!s) return;
    s->env.time_of_day = wrap24(hour);
    s->sky.last_hour = s->env.time_of_day;
    s->sky.last_sun_y = sun_dir_at(&s->env, s->env.time_of_day).y;
}
float lx_sky_time(const LxScene *s) { return s ? s->env.time_of_day : 0; }
LxVec3 lx_sky_sun_direction(const LxScene *s) { return s ? sun_dir_at(&s->env, s->env.time_of_day) : lx_v3(0, 1, 0); }
LxVec3 lx_sky_moon_direction(const LxScene *s) { return s ? moon_dir_at(&s->env, s->env.time_of_day) : lx_v3(0, 1, 0); }
bool lx_sky_is_day(const LxScene *s) { return lx_sky_sun_direction(s).y > 0; }
void lx_sky_set_weather(LxScene *s, LxWeather w, float blend_seconds) {
    if (!s || w < 0 || w >= LX_WEATHER_CUSTOM) return;
    lx_sky_weather_preset(w, &s->env);
    if (blend_seconds >= 0) s->env.weather_blend = blend_seconds;
    if (blend_seconds == 0) live_snap(&s->sky, &s->env);
}
LxWeather lx_sky_weather(const LxScene *s) { return s ? (LxWeather)s->env.weather : LX_WEATHER_CLEAR; }
float lx_sky_wetness(const LxScene *s) {
    if (!s || s->env.sky_mode != LX_SKY_DAY_NIGHT) return 0;
    return s->sky.started ? s->sky.wetness : lx_minf(1.0f, s->env.rain * 1.4f);
}

/* ------------------------------------------------------------------ thunder, made here */
static LxSound *g_thunder;
LxSound *lx_sky_builtin_sound(const char *name) {
    if (!name || strcmp(name, "thunder") != 0) return NULL;
    if (g_thunder) return g_thunder;
    /* brown noise, low-passed; a crack at the start, then a rumble that swells and rolls away */
    const int rate = 22050, n = rate * 6;
    LxSound *snd = lx_calloc(1, sizeof *snd);
    snd->samples = lx_calloc((size_t)n, sizeof(float));
    snd->frame_count = n;
    snd->sample_rate = rate;
    lx_strcpy(snd->ref, sizeof snd->ref, "builtin:thunder");
    uint32_t r = 12345u;
    float brown = 0, lp = 0, lp2 = 0, peak = 0;
    for (int i = 0; i < n; i++) {
        r = r * 1664525u + 1013904223u;
        float white = (float)(r >> 8) * (2.0f / 16777216.0f) - 1.0f;
        brown = lx_clampf(brown * 0.995f + white * 0.06f, -1.0f, 1.0f);
        lp += (brown - lp) * 0.08f;
        lp2 += (lp - lp2) * 0.15f;
        float t = (float)i / (float)rate;
        float crack = expf(-t * 18.0f) * white * 0.5f;
        float swell = (0.55f + 0.45f * sinf(t * 2.3f + 1.0f) * sinf(t * 0.9f)) * expf(-t * 0.55f);
        float attack = lx_minf(1.0f, t * 6.0f);
        float v = crack + lp2 * 3.5f * swell * attack;
        if (t > 5.0f) v *= (6.0f - t);   /* fade out cleanly */
        snd->samples[i] = v;
        peak = lx_maxf(peak, fabsf(v));
    }
    if (peak > 0) for (int i = 0; i < n; i++) snd->samples[i] *= 0.9f / peak;
    g_thunder = snd;
    return snd;
}
