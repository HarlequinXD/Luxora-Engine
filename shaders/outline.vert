#version 450
#pragma lx_common
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 4) in vec4 in_m0;
layout(location = 5) in vec4 in_m1;
layout(location = 6) in vec4 in_m2;
layout(location = 7) in vec4 in_m3;
void main() {
    mat4 model = mat4(in_m0, in_m1, in_m2, in_m3);
    vec4 world = model * vec4(in_pos, 1.0);
    vec3 n = normalize(mat3(model) * in_normal);
    float d = length(u.camera_pos.xyz - world.xyz);
    world.xyz += n * d * 0.004;
    gl_Position = lx_to_vulkan(u.view_proj * world);
}
