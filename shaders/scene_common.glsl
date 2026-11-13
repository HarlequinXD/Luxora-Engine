// Shared scene uniform block (set 0) and helpers. Inserted where "#pragma lx_common" appears.
#define LX_MAX_LIGHTS 16
#define LX_MAX_CASCADES 4

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
    LxLightData lights[LX_MAX_LIGHTS];
} u;

layout(set = 0, binding = 1) uniform sampler2DArrayShadow u_shadow;
layout(set = 0, binding = 2) uniform sampler3D u_gi_r;
layout(set = 0, binding = 3) uniform sampler3D u_gi_g;
layout(set = 0, binding = 4) uniform sampler3D u_gi_b;

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
