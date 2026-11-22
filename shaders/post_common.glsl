// Shared by the screen passes after the main pass (ambient occlusion, bloom, sun shafts, the final image).
// Set 0 is the scene (camera matrices), set 1 holds up to four input pictures, the push constants each pass's numbers.
layout(location = 0) in vec2 v_uv;
layout(set = 1, binding = 0) uniform sampler2D u_in0;
layout(set = 1, binding = 1) uniform sampler2D u_in1;
layout(set = 1, binding = 2) uniform sampler2D u_in2;
layout(set = 1, binding = 3) uniform sampler2D u_in3;
layout(push_constant) uniform PostPC { vec4 a; vec4 b; vec4 c; vec4 d; } pc;

// the world point at a screen position and depth (the depth buffer holds Vulkan 0..1 depth of GL-style matrices)
vec3 post_world(vec2 uv, float depth) {
    vec4 p = u.inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}
float post_noise(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }
