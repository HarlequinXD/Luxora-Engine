#version 450
#pragma lx_common
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in vec4 in_m0;
layout(location = 5) in vec4 in_m1;
layout(location = 6) in vec4 in_m2;
layout(location = 7) in vec4 in_m3;
layout(location = 8) in vec4 in_tint;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_tangent;
layout(location = 4) out vec4 v_tint;
layout(location = 5) out float v_view_depth;
layout(location = 6) flat out float v_moving;   // a moving object (it has a GI box stand-in)

void main() {
    mat4 model = mat4(in_m0, in_m1, in_m2, vec4(in_m3.xyz, 1.0));   // w carries the moving-object mark
    v_moving = in_m3.w > 1.5 ? 1.0 : 0.0;
    vec4 world = model * vec4(in_pos, 1.0);
    // exact normal transform for translate/rotate/scale matrices without a matrix inverse
    vec3 s2 = vec3(dot(in_m0.xyz, in_m0.xyz), dot(in_m1.xyz, in_m1.xyz), dot(in_m2.xyz, in_m2.xyz));
    v_normal = normalize(mat3(model) * (in_normal / s2));
    v_tangent = vec4(normalize(mat3(model) * in_tangent.xyz), in_tangent.w);
    v_world = world.xyz;
    v_uv = in_uv;
    v_tint = in_tint;
    v_view_depth = -(u.view * world).z;
    gl_Position = lx_to_vulkan(u.view_proj * world);
}
