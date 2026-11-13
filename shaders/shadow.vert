#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 4) in vec4 in_m0;
layout(location = 5) in vec4 in_m1;
layout(location = 6) in vec4 in_m2;
layout(location = 7) in vec4 in_m3;
layout(push_constant) uniform PC { mat4 light_vp; } pc;
void main() {
    vec4 p = pc.light_vp * (mat4(in_m0, in_m1, in_m2, in_m3) * vec4(in_pos, 1.0));
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;
    gl_Position = p;
}
