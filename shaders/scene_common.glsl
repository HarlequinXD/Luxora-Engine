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
    ivec4 counts;         // x lights, y cascades, z shadow map size
    vec4 probe_pos[LX_PROBE_SLOTS];  // xyz capture point, w intensity (0 = slot not in use)
    vec4 probe_min[LX_PROBE_SLOTS];  // xyz influence box min, w blend distance
    vec4 probe_max[LX_PROBE_SLOTS];  // xyz influence box max, w 1 = box projection
    vec4 refl_params;     // x probe mip count, y 1 = probes on
    LxLightData lights[LX_MAX_LIGHTS];
    vec4 light_shadow[LX_MAX_LIGHTS * 2];  // per light: (first atlas tile or -1, near, far, tan half-angle), (up xyz, tile resolution)
} u;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow u_shadow;
layout(set = 0, binding = 2) uniform sampler3D u_gi_r;
layout(set = 0, binding = 3) uniform sampler3D u_gi_g;
layout(set = 0, binding = 4) uniform sampler3D u_gi_b;
layout(set = 0, binding = 5) uniform sampler2DShadow u_local_shadow;   // point / spot shadow atlas, 8 x 8 tiles
layout(set = 0, binding = 6) uniform sampler3D u_gi_vis_a;   // GI cell walls: distance / 1.5 cells along +x -x +y -y
layout(set = 0, binding = 7) uniform sampler3D u_gi_vis_b;   //   ... and +z -z

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
