#version 450
// The final picture: sun shafts and bloom over the scene, exposure, lens (colour fringes, vignette), the ACES
// tone curve, then colour grading (white balance, contrast, saturation) and film grain.
#pragma lx_common
#pragma lx_include post_common.glsl
layout(location = 0) out vec4 o_color;
// u_in0 the scene (HDR), u_in1 bloom, u_in2 sun shafts
// pc.a: x exposure, y bloom mix (0 = off), z 1 = sun shafts, w time
// pc.b: x vignette, y film grain, z colour fringes, w width / height
// pc.c: x contrast, y saturation, z temperature

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 to_srgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
void main() {
    vec2 uv = v_uv;
    vec2 centre = uv - 0.5;
    vec3 hdr;
    if (pc.b.z > 0.0) {   // the lens bends colours apart a little towards the edges
        vec2 o = centre * dot(centre, centre) * pc.b.z * 0.05;
        hdr = vec3(texture(u_in0, uv + o).r, texture(u_in0, uv).g, texture(u_in0, uv - o).b);
    } else hdr = texture(u_in0, uv).rgb;
    if (pc.a.z > 0.5) hdr += texture(u_in2, uv).rgb;
    if (pc.a.y > 0.0) hdr = mix(hdr, texture(u_in1, uv).rgb, pc.a.y);
    hdr *= pc.a.x;
    float t = pc.c.z;   // white balance: warmer or cooler light
    hdr *= vec3(1.0 + 0.12 * t, 1.0 + 0.02 * t, 1.0 - 0.14 * t);
    if (pc.b.x > 0.0) {   // natural lens falloff towards the corners
        vec2 v = centre * vec2(pc.b.w, 1.0) / max(pc.b.w, 1.0);
        float r2 = dot(v, v) * 2.0;
        hdr *= mix(1.0, 1.0 / ((1.0 + r2) * (1.0 + r2)), pc.b.x);
    }
    vec3 c = aces(max(hdr, vec3(0.0)));
    if (pc.c.x != 1.0) c = clamp(0.18 * pow(c / 0.18, vec3(pc.c.x)), 0.0, 1.0);   // contrast around mid grey
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(mix(vec3(lum), c, pc.c.y), 0.0, 1.0);
    vec3 s = to_srgb(c);
    if (pc.b.y > 0.0) {   // film grain, stronger in the shadows
        float g = hash(gl_FragCoord.xy + fract(pc.a.w * 7.13) * 917.0) - 0.5;
        s += g * pc.b.y * 0.12 * (1.0 - 0.6 * lum);
    }
    o_color = vec4(s, 1.0);
}
