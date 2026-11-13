// Screen-space reflection inputs (set 1), written by the main pass.
// binding 0 (lit scene) is read only by the trace, binding 4 (trace result) only by the composite.
layout(set = 1, binding = 1) uniform sampler2D u_depth;
layout(set = 1, binding = 2) uniform sampler2D u_gnormal;  // world normal * 0.5 + 0.5
layout(set = 1, binding = 3) uniform sampler2D u_gspec;    // rgb reflection weight, a roughness
layout(push_constant) uniform SsrParams {
    vec4 a;  // x max distance, y thickness, z max steps, w max roughness
    vec4 b;  // xy full-res size, zw half-res size
} pc;

vec3 ssr_world_pos(vec2 uv, float d) {
    vec4 p = u.inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}
vec2 ssr_uv(vec4 clip) { vec2 ndc = clip.xy / clip.w; return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5); }
// distance along the view axis for a depth-buffer value (perspective projection)
float ssr_view_depth(float d) { return u.proj[3][2] / ((d * 2.0 - 1.0) + u.proj[2][2]); }
