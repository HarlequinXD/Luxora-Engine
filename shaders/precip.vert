#version 450
// Rain and snow: streaks and flakes in a box that travels with the camera. Each one is placed by a hash and
// falls through the box anchored in the world (moving doesn't drag them along). No vertex buffers: instance i is
// a drop (the first cloud_c.z of them) or a flake, its 6 vertices a quad facing the eye.
#pragma lx_common
layout(location = 0) out vec2 v_q;        // across, along: -1..1
layout(location = 1) out vec3 v_center;   // the drop itself (for the roof test)
layout(location = 2) out float v_fade;
layout(location = 3) flat out float v_snow;
void main() {
    int inst = gl_InstanceIndex;
    int nrain = int(u.cloud_c.z + 0.5);
    bool snow = inst >= nrain;
    float k = float(snow ? inst - nrain : inst);
    vec3 h = vec3(lx_hash12(vec2(k, 1.7)), lx_hash12(vec2(k, 13.1)), lx_hash12(vec2(k, 29.3)));
    float B = snow ? 18.0 : 22.0, H = snow ? 12.0 : 16.0;
    float t = u.camera_pos.w;
    vec2 wind = u.weather_b.xy;
    float fall = snow ? 1.0 + h.x * 0.6 : 8.0 + h.x * 2.5;   // m/s
    vec3 vel = vec3(wind.x * (snow ? 0.6 : 0.3), -fall, wind.y * (snow ? 0.6 : 0.3));
    vec3 p = h * vec3(B, H, B) + vel * t;
    if (snow) p.xz += vec2(sin(t * 1.3 + h.y * 20.0), cos(t * 1.1 + h.z * 20.0)) * 0.3;   // flakes flutter
    vec3 cam = u.camera_pos.xyz;
    vec3 box = vec3(B, H, B);
    vec3 origin = cam - vec3(B * 0.5, H * 0.55, B * 0.5);
    p = origin + mod(p - origin, box);
    vec3 to_cam = cam - p;
    float dist = length(to_cam);
    to_cam /= max(dist, 1e-3);
    vec3 dir = normalize(vel);
    float len = snow ? 0.03 + h.z * 0.02 : 0.25 + fall * 0.02;
    float wid = snow ? len : 0.006 + 0.004 * h.z;
    vec3 side = cross(dir, to_cam);
    side = (dot(side, side) > 1e-6 ? normalize(side) : vec3(1.0, 0.0, 0.0)) * wid;
    vec3 along = snow ? normalize(cross(side, to_cam)) * len : dir * len * 0.5;
    int vi = gl_VertexIndex;
    vec2 c = vi == 0 ? vec2(0, 0) : vi == 1 ? vec2(1, 0) : vi == 2 ? vec2(1, 1) : vi == 3 ? vec2(0, 0) : vi == 4 ? vec2(1, 1) : vec2(0, 1);
    vec2 q = c * 2.0 - 1.0;
    vec3 world = p + side * q.x + along * q.y;
    v_q = q;
    v_center = p;
    v_snow = snow ? 1.0 : 0.0;
    v_fade = smoothstep(0.25, 1.0, dist) * (1.0 - smoothstep(B * 0.3, B * 0.5, dist));
    gl_Position = lx_to_vulkan(u.view_proj * vec4(world, 1.0));
}
