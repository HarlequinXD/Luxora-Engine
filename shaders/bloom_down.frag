#version 450
// Bloom, going down: each level is the one above shrunk to half with a wide 13-tap filter.
// The first step averages its groups by brightness so single blazing pixels don't flicker.
#pragma lx_common
#pragma lx_include post_common.glsl
layout(location = 0) out vec4 o_color;
// u_in0 the level above; pc.a: xy its texel size, z 1 = the first step (from the picture itself)

vec3 tap(vec2 o) { return min(textureLod(u_in0, v_uv + o * pc.a.xy, 0.0).rgb, vec3(1000.0)); }
float karis(vec3 c) { return 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722))); }
void main() {
    vec3 a = tap(vec2(-2, -2)), b = tap(vec2(0, -2)), c = tap(vec2(2, -2));
    vec3 d = tap(vec2(-2, 0)), e = tap(vec2(0, 0)), f = tap(vec2(2, 0));
    vec3 g = tap(vec2(-2, 2)), h = tap(vec2(0, 2)), i = tap(vec2(2, 2));
    vec3 j = tap(vec2(-1, -1)), k = tap(vec2(1, -1)), l = tap(vec2(-1, 1)), m = tap(vec2(1, 1));
    vec3 g0 = (j + k + l + m) * 0.25, g1 = (a + b + d + e) * 0.25, g2 = (b + c + e + f) * 0.25;
    vec3 g3 = (d + e + g + h) * 0.25, g4 = (e + f + h + i) * 0.25;
    vec3 col;
    if (pc.a.z > 0.5) {
        float w0 = karis(g0) * 0.5, w1 = karis(g1) * 0.125, w2 = karis(g2) * 0.125, w3 = karis(g3) * 0.125, w4 = karis(g4) * 0.125;
        col = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else col = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    o_color = vec4(max(col, vec3(0.0)), 1.0);
}
