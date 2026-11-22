// Shared scene uniform block (set 0) and helpers. Inserted where "#pragma lx_common" appears.
#define LX_MAX_LIGHTS 16
#define LX_MAX_CASCADES 4
#define LX_PROBE_SLOTS 5   // slot 0: automatic environment probe, 1..4: Reflection Probe objects

struct LxLightData {
    vec4 pos_range;   // xyz position, w range
    vec4 color_type;  // rgb colour * intensity, w type (1 point, 2 spot)
    vec4 dir_cos;     // xyz direction the light points, w cos(outer half-angle)
};

layout(set = 0, binding = 0, std140) uniform SceneUBO {
    mat4 view;
    mat4 proj;
    mat4 view_proj;
    mat4 inv_view_proj;
    vec4 camera_pos;      // w = time
    vec4 sun_dir;         // xyz direction light travels, w = shadows enabled
    vec4 sun_color;       // rgb * intensity, w = has sun
    vec4 sky_zenith;      // w = ambient intensity
    vec4 sky_horizon;     // w = exposure
    vec4 ground_color;    // w = fog density
    vec4 fog_color;       // w = GI intensity
    mat4 cascade_vp[LX_MAX_CASCADES];
    vec4 cascade_splits;  // view-space far distance of each cascade
    vec4 gi_origin;       // w = GI enabled
    vec4 gi_cell;         // xyz cell size
    vec4 gi_res;          // xyz resolution
    ivec4 counts;         // x lights, y cascades, z shadow map size, w the precipitation map's layer in u_shadow
    vec4 probe_pos[LX_PROBE_SLOTS];  // xyz capture point, w intensity (0 = slot not in use)
    vec4 probe_min[LX_PROBE_SLOTS];  // xyz influence box min, w blend distance
    vec4 probe_max[LX_PROBE_SLOTS];  // xyz influence box max, w 1 = box projection
    vec4 refl_params;     // x probe mip count, y 1 = probes on
    LxLightData lights[LX_MAX_LIGHTS];
    vec4 light_shadow[LX_MAX_LIGHTS * 2];  // per light: (first atlas tile or -1, near, far, tan half-angle), (up xyz, tile resolution)
    vec4 gi_casc[2];      // camera GI grids, finest first: xyz first cell's corner, w cell size (0 = not in use)
    ivec4 gi_casc_res;    // x cells per side (each cascade is stacked along z in the u_gc_* textures)
    vec4 bvol_rows[24];   // Baked Lighting Volumes: per volume 3 rows mapping world -> 0..1 inside its box
    vec4 bvol_info[8];    // per volume: z offset, nx, ny, nz (voxels)
    vec4 bvol_params;     // x volumes in use, y one layer's depth, z the sun's shadowmask channel (-1 none)
    // Day-night sky and weather (src/scene/sky.c)
    vec4 sky_params;      // x 1 = Day-night sky, y sunlight above the air, z haze, w stars
    vec4 sky_sun;         // xyz towards the sun, w the eye's height in the air (m)
    vec4 sky_moon;        // xyz towards the moon, w moonlight above the air (its phase included)
    vec4 sky_misc;        // x moon phase 0..1, y night 0..1, z lightning flash, w sidereal angle (radians, the stars turn)
    vec4 sky_pole;        // xyz the celestial pole (the stars turn around it)
    vec4 cloud_a;         // x coverage, y density, z type (0 cumulus, 1 stratus, 2 storm), w march steps (0 = no clouds)
    vec4 cloud_b;         // x base height, y thickness, zw wind drift (m)
    vec4 cloud_c;         // x light steps, y average shade (0..1), zw -
    vec4 weather_a;       // x rain, y snow, z wetness, w snow cover
    vec4 weather_b;       // xy wind (m/s, world x z), z mist, w 1 = the precipitation map is drawn
    vec4 bolt;            // xyz towards the last lightning bolt, w seconds since it struck
    vec4 bolt_b;          // x its shape seed, y distance (m)
    mat4 precip_vp;       // looking straight down on the area around the eye: what keeps rain and snow off
} u;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow u_shadow;
layout(set = 0, binding = 2) uniform sampler3D u_gi_r;
layout(set = 0, binding = 3) uniform sampler3D u_gi_g;
layout(set = 0, binding = 4) uniform sampler3D u_gi_b;
layout(set = 0, binding = 5) uniform sampler2DShadow u_local_shadow;   // point / spot shadow atlas, 8 x 8 tiles
layout(set = 0, binding = 6) uniform sampler3D u_gi_vis_a;   // GI cell walls: distance / 1.5 cells along +x -x +y -y
layout(set = 0, binding = 7) uniform sampler3D u_gi_vis_b;   //   ... and +z -z
layout(set = 0, binding = 8) uniform sampler3D u_gc_r;       // camera GI grids (stacked along z): SH red, green, blue
layout(set = 0, binding = 9) uniform sampler3D u_gc_g;
layout(set = 0, binding = 10) uniform sampler3D u_gc_b;
layout(set = 0, binding = 11) uniform sampler3D u_gc_vis_a;  // their cell walls
layout(set = 0, binding = 12) uniform sampler3D u_gc_vis_b;
layout(set = 0, binding = 13) uniform sampler2DArray u_lightmap;   // baked lighting: per atlas irradiance, then light direction
layout(set = 0, binding = 15) uniform sampler3D u_gi_sdf;   // Live GI: the camera levels' distance fields (in half cells, stacked along z)
layout(set = 0, binding = 14) uniform sampler3D u_bvol;   // Baked Lighting Volumes: SH red, green, blue, shadowmask (each a block along z)

// The engine builds OpenGL-style matrices; convert clip space for Vulkan (y down, z 0..1).
vec4 lx_to_vulkan(vec4 p) {
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;
    return p;
}

vec3 lx_sky(vec3 dir) {
    float t = clamp(dir.y, -1.0, 1.0);
    vec3 sky = mix(u.sky_horizon.rgb, u.sky_zenith.rgb, pow(max(t, 0.0), 0.45));
    vec3 ground = mix(u.sky_horizon.rgb, u.ground_color.rgb, clamp(-t * 4.0, 0.0, 1.0));
    return t >= 0.0 ? sky : ground;
}

/* The sky exactly as the sky pass draws it (reflections must match what you see). */
const float LX_SKY_SCALE = 1.6;
vec3 lx_sky_radiance(vec3 dir) { return lx_sky(dir) * u.sky_zenith.w * LX_SKY_SCALE; }

/* Glossy sky reflection: the sky averaged over a cone around the mirror direction whose
   width follows the GGX lobe (alpha = roughness^2). Mirrors stay sharp, rough metal blurs
   the horizon and gradient instead of fading to a flat colour. */
vec3 lx_sky_glossy(vec3 r, float rough) {
    float a = rough * rough;
    if (a < 0.003) return lx_sky_radiance(r);
    vec3 up = abs(r.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, r));
    vec3 b = cross(r, t);
    float spread = min(a * 1.6, 2.0);
    vec3 sum = lx_sky_radiance(r);
    const int N = 12;
    for (int i = 0; i < N; i++) {
        float k = (float(i) + 0.5) / float(N);
        float rad = sqrt(k) * spread, ang = float(i) * 2.39996323; /* golden-angle spiral */
        sum += lx_sky_radiance(normalize(r + (t * cos(ang) + b * sin(ang)) * rad));
    }
    return sum / float(N + 1);
}


/* ======================================================================================
 *  Day-night sky: the same atmosphere as src/scene/sky.c (keep them in step)
 * ====================================================================================== */
const float LX_RG = 6360e3, LX_RT = 6460e3, LX_HR = 8000.0, LX_HM = 1200.0, LX_MIE_S = 3.996e-6, LX_MIE_G = 0.8;
const float LX_MS_BOOST = 0.35, LX_SKY_GAIN = 1.8, LX_SPI = 3.14159265;
const vec3 LX_BR = vec3(5.802e-6, 13.558e-6, 33.1e-6);
const vec3 LX_BO = vec3(0.650e-6, 1.881e-6, 0.085e-6) * 1.875;
bool lx_day_night() { return u.sky_params.x > 0.5; }

float lx_chapman(float X, float h, float mu) {
    float c = sqrt(X + h);
    if (mu >= 0.0) return c / (c * mu + 1.0) * exp(-h);
    float x0 = sqrt(max(0.0, 1.0 - mu * mu)) * (X + h);
    if (x0 < X) return 1e9;
    return 2.0 * sqrt(x0) * exp(X - x0) - c / (1.0 - c * mu) * exp(-h);
}
vec3 lx_sun_trans(float h, float mu, float haze) {
    float r = LX_HR * lx_chapman(LX_RG / LX_HR, h / LX_HR, mu), m = LX_HM * lx_chapman(LX_RG / LX_HM, h / LX_HM, mu);
    return exp(-(LX_BR + LX_BO) * r - LX_MIE_S * haze * 1.11 * m);
}
/* light scattered towards the eye along d by the air, from one light (s: towards it, E: its strength);
   trans: how much of what lies behind (the sun's disc, space) gets through */
vec3 lx_atmosphere(vec3 d, vec3 s, float E, float haze, float height, int N, out vec3 trans) {
    float r0 = LX_RG + max(height, 1.0);
    float b = r0 * d.y;
    float t_top = -b + sqrt(max(0.0, b * b - (r0 * r0 - LX_RT * LX_RT)));
    float dg = b * b - (r0 * r0 - LX_RG * LX_RG);
    bool ground = d.y < 0.0 && dg > 0.0;
    float L = ground ? -b - sqrt(dg) : t_top;
    float mu = dot(d, s);
    float pr = 3.0 / (16.0 * LX_SPI) * (1.0 + mu * mu) + LX_MS_BOOST / (4.0 * LX_SPI);
    float g2 = LX_MIE_G * LX_MIE_G;
    float pm = 3.0 / (8.0 * LX_SPI) * (1.0 - g2) * (1.0 + mu * mu) / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * LX_MIE_G * mu, 1.5)) + LX_MS_BOOST / (4.0 * LX_SPI);
    float bms = LX_MIE_S * haze, bme = bms * 1.11;
    float odr = 0.0, odm = 0.0;
    vec3 S = vec3(0.0);
    for (int i = 0; i < N; i++) {
        float a0 = float(i) / float(N), a1 = float(i + 1) / float(N);
        float t0 = L * a0 * a0, t1 = L * a1 * a1, dt = t1 - t0, t = 0.5 * (t0 + t1);
        vec3 p = vec3(d.x * t, r0 + d.y * t, d.z * t);
        float r = length(p), h = max(r - LX_RG, 0.0);
        float mus = dot(p / r, s);
        float rr = exp(-h / LX_HR), rm = exp(-h / LX_HM);
        float cr = odr + rr * dt * 0.5, cm = odm + rm * dt * 0.5;
        odr += rr * dt;
        odm += rm * dt;
        float sr = LX_HR * lx_chapman(LX_RG / LX_HR, h / LX_HR, mus), sm = LX_HM * lx_chapman(LX_RG / LX_HM, h / LX_HM, mus);
        vec3 T = exp(-(LX_BR + LX_BO) * (cr + sr) - bme * (cm + sm));
        S += T * (LX_BR * rr * pr + bms * rm * pm) * dt;
    }
    trans = exp(-(LX_BR + LX_BO) * odr - bme * odm);
    return S * E * LX_SKY_GAIN;   /* trans: through the air to space, or to the ground when d points below the horizon */
}

/* ---- noise for clouds, stars and rain ---- */
float lx_hash12(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float lx_hash13(vec3 p3) { p3 = fract(p3 * 0.1031); p3 += dot(p3, p3.zyx + 31.32); return fract((p3.x + p3.y) * p3.z); }
float lx_vnoise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(lx_hash12(i), lx_hash12(i + vec2(1, 0)), f.x), mix(lx_hash12(i + vec2(0, 1)), lx_hash12(i + vec2(1, 1)), f.x), f.y);
}
float lx_vnoise3(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(mix(lx_hash13(i), lx_hash13(i + vec3(1, 0, 0)), f.x), mix(lx_hash13(i + vec3(0, 1, 0)), lx_hash13(i + vec3(1, 1, 0)), f.x), f.y);
    float b = mix(mix(lx_hash13(i + vec3(0, 0, 1)), lx_hash13(i + vec3(1, 0, 1)), f.x), mix(lx_hash13(i + vec3(0, 1, 1)), lx_hash13(i + vec3(1, 1, 1)), f.x), f.y);
    return mix(a, b, f.z);
}
/* gradient noise (smooth, round features; value noise on a grid looks blocky when seen at a slant), 0..1 */
vec2 lx_grad2(vec2 i) { float a = lx_hash12(i) * 6.2831853; return vec2(cos(a), sin(a)); }
float lx_gnoise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(lx_grad2(i), f), b = dot(lx_grad2(i + vec2(1, 0)), f - vec2(1, 0));
    float c = dot(lx_grad2(i + vec2(0, 1)), f - vec2(0, 1)), d = dot(lx_grad2(i + vec2(1, 1)), f - vec2(1, 1));
    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y) * 1.2 + 0.5;
}
/* each octave turned ~37 degrees from the last, so no grid lines up between them */
float lx_fbm2(vec2 p) {
    const mat2 ROT = mat2(0.8, 0.6, -0.6, 0.8) * 2.03;
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 4; i++) { v += a * lx_gnoise2(p); p = ROT * p + vec2(17.1, 9.7); a *= 0.5; }
    return v / 0.9375;
}

/* ---- clouds: a slab of air from base to base + thickness ---- */
/* where clouds are, seen from above (0..1): the coverage map every cloud sample and the cloud shadows share */
float lx_cloud_map(vec2 xz) {
    float type = u.cloud_a.z;
    float scale = mix(5200.0, 16000.0, clamp(type, 0.0, 1.0)) * mix(1.0, 1.6, clamp(type - 1.0, 0.0, 1.0));
    vec2 p = (xz + u.cloud_b.zw) / scale;
    p += (vec2(lx_gnoise2(p * 0.9 + 3.1), lx_gnoise2(p * 0.9 + 11.7)) - 0.5) * 0.25;   // a gentle warp: billows, not blobs
    float n = lx_fbm2(p);   // mean 0.5, spread about 0.16
    float cov = u.cloud_a.x;
    float edge = mix(0.12, 0.3, clamp(type, 0.0, 1.0));
    float thr = 0.5 + (0.5 - cov) * 0.52;   // so about `coverage` of the sky is cloud
    return smoothstep(thr - edge * 0.5, thr + edge, n) * smoothstep(0.0, 0.05, cov);
}
/* How high a column of cloud rises (a fraction of the slab), from how much cloud the map puts there. Clouds are
   wider than they are tall: thin edges stay low, thick middles heap up, so every cloud is a dome on a flat base. */
float lx_cloud_top(float m, float type) {
    float cumulus = 0.10 + 0.50 * m * m;   // fair weather: low, wide domes; the thickest heap a little higher
    float stratus = 0.08 + 0.14 * m;       // a flat layer
    float storm = 0.30 + 0.70 * m;         // towers
    return type < 1.0 ? mix(cumulus, stratus, type) : mix(stratus, storm, type - 1.0);
}
float lx_cloud_density(vec3 p, float h, bool detail) {
    float m = lx_cloud_map(p.xz);
    if (m <= 0.001) return 0.0;
    float type = u.cloud_a.z;
    float top = lx_cloud_top(m, type);
    if (h >= top) return 0.0;
    float y = h / top;                                    // 0 at the base .. 1 at this column's top
    float d = m * smoothstep(0.0, 0.06, h)                // a flat, slightly soft base
                * sqrt(max(1.0 - y * y, 0.0));            // a rounded top
    if (detail) {
        /* lumpy edges: rounded puffs eaten out of the shape, more towards the top and sides (cauliflower tops),
           less in a flat stratus layer; each octave turned off the grid */
        vec3 q = (p + vec3(u.cloud_b.z, 0.0, u.cloud_b.w) * 0.3) / 520.0;
        const mat3 R3 = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
        vec3 q2 = R3 * q * 2.3, q3 = R3 * q2 * 2.4;
        float n = lx_vnoise3(q) * 0.6 + lx_vnoise3(q2) * 0.3 + lx_vnoise3(q3) * 0.1;
        float er = mix(mix(0.45, 0.75, y), 0.3, clamp(type, 0.0, 1.0) * (1.0 - clamp(type - 1.0, 0.0, 1.0)));
        float cut = (1.0 - n) * er;
        d = clamp((d - cut) / max(1.0 - cut, 0.05), 0.0, 1.0);
    }
    return d * mix(0.4, 1.6, u.cloud_a.y);
}
/* how thick the cloud is per metre: set so a whole column of the type is optically thick (a flat stratus layer
   only a few hundred metres deep is as opaque as a tall cumulus; real overcast hides the sun) */
float lx_cloud_sigma() { return 7.0 / max(u.cloud_b.y * lx_cloud_top(1.0, u.cloud_a.z), 50.0); }
/* sunlight that gets through the clouds to the ground at p (1 clear sky), against the average the sun light already carries */
float lx_cloud_shadow(vec3 p, vec3 to_light) {
    if (!lx_day_night() || u.cloud_a.x <= 0.001 || to_light.y < 0.02) return 1.0;
    float mid = u.cloud_b.x + u.cloud_b.y * 0.15;
    vec3 c = p + to_light * ((mid - p.y) / to_light.y);
    float m = lx_cloud_map(c.xz);
    /* light crosses the column: its density times its height (0.6 of it, through a dome), as src/scene/sky.c reckons */
    float tau = 4.2 * mix(0.4, 1.6, u.cloud_a.y) * m * lx_cloud_top(m, u.cloud_a.z) / lx_cloud_top(1.0, u.cloud_a.z);
    float t = max(exp(-tau), 0.15 * exp(-0.12 * tau));   // what scatters through
    float avg = max(1.0 - u.cloud_c.y, 0.03);   // the sun light already carries the average
    return clamp(t / avg, 0.0, 1.6);
}
