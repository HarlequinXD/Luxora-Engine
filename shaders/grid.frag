#version 450
#pragma lx_common
layout(location = 0) in vec3 v_world;
layout(location = 0) out vec4 o_color;
float grid_line(vec2 p, float scale) {
    vec2 g = p / scale;
    vec2 d = abs(fract(g - 0.5) - 0.5) / fwidth(g);
    return 1.0 - min(min(d.x, d.y), 1.0);
}
void main() {
    float dist = length(u.camera_pos.xyz - v_world);
    float fade = 1.0 - smoothstep(30.0, 160.0, dist);
    float minor = grid_line(v_world.xz, 1.0) * 0.18;
    float major = grid_line(v_world.xz, 10.0) * 0.35;
    float a = max(minor, major) * fade;
    vec3 col = vec3(0.6);
    vec2 axis = abs(v_world.xz) / fwidth(v_world.xz);
    if (axis.y < 1.5) { col = vec3(0.9, 0.25, 0.25); a = max(a, 0.8 * fade); }   // X axis (z = 0)
    if (axis.x < 1.5) { col = vec3(0.25, 0.45, 0.95); a = max(a, 0.8 * fade); }  // Z axis (x = 0)
    if (a < 0.003) discard;
    o_color = vec4(col, a);
}
