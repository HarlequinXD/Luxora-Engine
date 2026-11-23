#version 450
// Shadows of see-through materials: like shadow.vert, and hands the texture coordinates and alpha on.
layout(location = 0) in vec3 in_pos;
layout(location = 2) in vec2 in_uv;
layout(location = 4) in vec4 in_m0;
layout(location = 5) in vec4 in_m1;
layout(location = 6) in vec4 in_m2;
layout(location = 7) in vec4 in_m3;
layout(location = 8) in vec4 in_tint;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_alpha;
layout(push_constant) uniform PC { mat4 light_vp; vec4 mat; } pc;   // mat: alpha, cutoff, uv scale, 0 cutout / 1 dithered
void main() {
    vec4 p = pc.light_vp * (mat4(in_m0, in_m1, in_m2, vec4(in_m3.xyz, 1.0)) * vec4(in_pos, 1.0));
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;
    gl_Position = p;
    v_uv = in_uv * pc.mat.z;
    v_alpha = in_tint.a * pc.mat.x;
}
