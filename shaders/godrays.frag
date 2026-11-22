#version 450
// Sun shafts (half resolution): from each pixel, steps towards the sun on screen gathering the open sky around it.
// Leaves, clouds and window frames in between leave gaps, and the light through the gaps streaks out as shafts.
#pragma lx_common
#pragma lx_include post_common.glsl
layout(location = 0) out vec4 o_color;
// u_in0 the picture, u_in1 depth; pc.a: xy the sun on screen, z strength (0 = off), w width / height

vec3 sky_light(vec2 uv) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(0.0);
    if (textureLod(u_in1, uv, 0.0).r < 1.0) return vec3(0.0);   // something solid: no light from there
    vec2 off = (uv - pc.a.xy) * vec2(pc.a.w, 1.0);
    float fall = exp(-dot(off, off) * 18.0);   // only the sky close to the sun
    return min(textureLod(u_in0, uv, 0.0).rgb, vec3(40.0)) * fall;
}
void main() {
    if (pc.a.z <= 0.0) { o_color = vec4(0.0); return; }
    const int N = 40;
    vec2 to_sun = pc.a.xy - v_uv;
    float jitter = post_noise(gl_FragCoord.xy);
    vec3 acc = vec3(0.0);
    float w = 1.0, wsum = 0.0;
    for (int i = 0; i < N; i++) {
        float f = (float(i) + jitter) / float(N);
        acc += sky_light(v_uv + to_sun * f) * w;
        wsum += w;
        w *= 0.965;   // nearer the pixel counts more: shafts fade out away from the sun
    }
    o_color = vec4(acc / wsum * pc.a.z, 1.0);
}
