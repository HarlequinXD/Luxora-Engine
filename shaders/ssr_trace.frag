#version 450
// Screen-space reflections, traced at half resolution: march the reflected ray across the depth
// buffer in screen space (perspective-correct), refine the hit with a binary search, and fetch
// the lit colour there. Output is premultiplied by confidence so it can be filtered and blended.
#pragma lx_common
#pragma lx_include ssr_common.glsl
layout(set = 1, binding = 0) uniform sampler2D u_hdr;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_ssr;

float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

void main() {
    o_ssr = vec4(0.0);
    vec2 uv = v_uv;
    float d = textureLod(u_depth, uv, 0.0).r;
    if (d >= 1.0 || u.proj[3][3] > 0.5) return;            // sky, or an orthographic camera
    vec4 sp = textureLod(u_gspec, uv, 0.0);
    float rough = sp.a;
    if (max(sp.r, max(sp.g, sp.b)) < 0.02 || rough > pc.a.w) return;
    vec3 n = normalize(textureLod(u_gnormal, uv, 0.0).xyz * 2.0 - 1.0);
    vec3 p = ssr_world_pos(uv, d);
    vec3 view_dir = normalize(p - u.camera_pos.xyz);
    vec3 r = reflect(view_dir, n);
    float facing = 1.0 - smoothstep(0.35, 0.85, dot(r, -view_dir));  // rays back at the camera rarely hit anything on screen
    if (facing <= 0.0) return;

    float dist = length(p - u.camera_pos.xyz);
    vec3 p0 = p + n * (0.01 + 0.002 * dist);
    float len = pc.a.x;
    float near = u.proj[3][2] / (u.proj[2][2] - 1.0);
    float vz0 = (u.view * vec4(p0, 1.0)).z, vzr = (mat3(u.view) * r).z;
    if (vzr > 1e-4) len = min(len, max((-near * 1.5 - vz0) / vzr, 0.0));  // stay in front of the camera
    if (len < 0.05) return;
    vec4 h0 = u.view_proj * vec4(p0, 1.0), h1 = u.view_proj * vec4(p0 + r * len, 1.0);
    float k0 = 1.0 / h0.w, k1 = 1.0 / h1.w;
    vec2 s0 = ssr_uv(h0), s1 = ssr_uv(h1);
    // stop where the ray leaves the screen
    vec2 dir = s1 - s0;
    float tmax = 1.0;
    if (dir.x > 0.0) tmax = min(tmax, (1.0 - s0.x) / dir.x); else if (dir.x < 0.0) tmax = min(tmax, -s0.x / dir.x);
    if (dir.y > 0.0) tmax = min(tmax, (1.0 - s0.y) / dir.y); else if (dir.y < 0.0) tmax = min(tmax, -s0.y / dir.y);
    float pixels = length(dir * tmax * pc.b.zw);
    int steps = int(clamp(pixels * 0.5, 4.0, pc.a.z));
    float jitter = ign(gl_FragCoord.xy);
    float prev_t = 0.0, hit_t = -1.0;
    for (int i = 1; i <= steps; i++) {
        float t = tmax * (float(i) - jitter * 0.95) / float(steps);
        vec2 s = mix(s0, s1, t);
        float ray_z = 1.0 / mix(k0, k1, t);
        float scene_z = ssr_view_depth(textureLod(u_depth, s, 0.0).r);
        float diff = ray_z - scene_z;
        if (diff > 0.0 && diff < pc.a.y * (1.0 + ray_z * 0.04)) {
            float lo = prev_t, hi = t;
            for (int k = 0; k < 5; k++) {
                float mid = 0.5 * (lo + hi);
                vec2 sm = mix(s0, s1, mid);
                if (1.0 / mix(k0, k1, mid) > ssr_view_depth(textureLod(u_depth, sm, 0.0).r)) hi = mid; else lo = mid;
            }
            hit_t = hi;
            break;
        }
        prev_t = t;
    }
    if (hit_t < 0.0) return;
    vec2 s = mix(s0, s1, hit_t);
    vec3 nh = textureLod(u_gnormal, s, 0.0).xyz * 2.0 - 1.0;
    if (dot(nh, r) > 0.2) return;                                     // hit the back of something
    float edge = smoothstep(0.0, 0.07, min(min(s.x, 1.0 - s.x), min(s.y, 1.0 - s.y)));
    float travel = 1.0 - smoothstep(0.65, 1.0, hit_t);
    float gloss = 1.0 - smoothstep(pc.a.w * 0.6, pc.a.w, rough);
    float conf = edge * travel * gloss * facing;
    o_ssr = vec4(textureLod(u_hdr, s, 0.0).rgb * conf, conf);
}
