#version 450
// Cutout materials cast the shadow of what's left after the cut; Fade and Glass a dithered one as dark as they
// are solid (the shadow filter softens the dither into a lighter shadow).
layout(set = 0, binding = 0) uniform sampler2D u_albedo_map;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_alpha;
layout(push_constant) uniform PC { mat4 light_vp; vec4 mat; } pc;
void main() {
    float a = v_alpha * texture(u_albedo_map, v_uv).a;
    if (pc.mat.w < 0.5) {
        if (a < pc.mat.y) discard;
    } else {
        float h = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (a <= h) discard;
    }
}
