#version 450
// Blends the traced reflections into the lit image (additive): where a ray found the surface on
// screen it replaces the probe/sky reflection the main pass already added, by exactly the amount
// the PBR shader weighted it with.
#pragma lx_common
#pragma lx_include reflections.glsl
#pragma lx_include ssr_common.glsl
layout(set = 1, binding = 4) uniform sampler2D u_ssr;   // half-res trace result, premultiplied by confidence
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec2 uv = v_uv;
    float d = textureLod(u_depth, uv, 0.0).r;
    if (d >= 1.0) discard;
    vec4 sp = textureLod(u_gspec, uv, 0.0);
    if (max(sp.r, max(sp.g, sp.b)) < 0.02) discard;
    float rough = sp.a;
    vec4 s = texture(u_ssr, uv);
    if (rough > 0.12) {
        // glossy: widen the gather with roughness (the trace is sharp)
        vec2 o = rough * 6.0 / pc.b.zw;
        s = s * 0.4 + 0.15 * (texture(u_ssr, uv + vec2(o.x, o.y)) + texture(u_ssr, uv + vec2(-o.x, o.y)) +
                              texture(u_ssr, uv + vec2(o.x, -o.y)) + texture(u_ssr, uv + vec2(-o.x, -o.y)));
    }
    if (s.a < 0.004) discard;
    vec3 n = normalize(textureLod(u_gnormal, uv, 0.0).xyz * 2.0 - 1.0);
    vec3 p = ssr_world_pos(uv, d);
    vec3 r = reflect(normalize(p - u.camera_pos.xyz), n);
    float local_w;
    vec3 env = lx_environment_reflection(p, r, rough, local_w);
    o_color = vec4(sp.rgb * (s.rgb - env * s.a), 0.0);
}
