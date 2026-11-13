#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC { vec4 screen; } pc;
void main() {
    vec4 t = texture(u_tex, v_uv);
    // UI colours are authored in sRGB and the swapchain is UNORM, so blend directly.
    if (pc.screen.z < 0.5) o_color = vec4(v_color.rgb, v_color.a * t.r);   // font atlas (alpha)
    else o_color = v_color * t;                                              // rgba image
}
