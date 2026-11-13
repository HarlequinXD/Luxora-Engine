#version 450
#pragma lx_common
layout(location = 0) out vec3 v_world;
void main() {
    vec2 c[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    float size = 400.0;
    vec3 p = vec3(c[gl_VertexIndex].x * size + u.camera_pos.x, 0.0, c[gl_VertexIndex].y * size + u.camera_pos.z);
    v_world = p;
    gl_Position = lx_to_vulkan(u.view_proj * vec4(p, 1.0));
}
