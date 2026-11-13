#version 450
layout(location = 0) out vec4 o_color;
layout(push_constant) uniform PC { vec4 color; } pc;
void main() { o_color = pc.color; }
