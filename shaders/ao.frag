#version 450
// Screen-space ambient occlusion (half resolution): samples in the hemisphere above each surface point, checked
// against the depth buffer. Writes how open each point is (1 = nothing nearby, 0 = buried).
#pragma lx_common
#pragma lx_include post_common.glsl
layout(location = 0) out float o_ao;
// u_in0 depth, u_in1 world normals; pc.a: x radius (m)

void main() {
    float d = textureLod(u_in0, v_uv, 0.0).r;
    vec4 nn = textureLod(u_in1, v_uv, 0.0);
    if (d >= 1.0 || nn.a == 0.0) { o_ao = 1.0; return; }
    vec3 n = normalize(nn.xyz * 2.0 - 1.0);
    vec3 cam = u.camera_pos.xyz;
    vec3 p = post_world(v_uv, d);
    float dist = length(p - cam);
    float R = pc.a.x;
    p += n * (0.01 + dist * 0.0015);   // off the surface: its own depth isn't an occluder
    float pd = length(p - cam);
    vec3 t = normalize(cross(abs(n.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), n));
    vec3 b = cross(n, t);
    float noise = post_noise(gl_FragCoord.xy);
    float occ = 0.0, total = 0.0;
    const int N = 12;
    for (int i = 0; i < N; i++) {
        float xi = (float(i) + 0.5) / float(N);
        float phi = noise * 6.2831853 + float(i) * 2.3999632;
        float r = sqrt(xi);
        vec3 dir = t * (cos(phi) * r) + b * (sin(phi) * r) + n * sqrt(max(1.0 - xi, 0.0));   // cosine-weighted
        float len = R * mix(0.1, 1.0, fract(noise * 1.618 + float(i) * 0.618034));
        len *= len / R;   // more samples close by: small creases matter most
        vec3 s = p + dir * len;
        vec4 c = u.view_proj * vec4(s, 1.0);
        if (c.w <= 1e-4) continue;
        vec2 suv = vec2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) continue;
        float sd = textureLod(u_in0, suv, 0.0).r;
        total += 1.0;
        if (sd >= 1.0) continue;
        float qd = length(post_world(suv, sd) - cam), sdist = length(s - cam);
        if (qd < sdist - 0.02 * len - 0.005) {
            // occluded, unless the thing in front is far in front (a pole across the view doesn't darken the wall behind)
            occ += smoothstep(0.0, 1.0, R / max(pd - qd, 1e-3));
        }
    }
    o_ao = total > 0.0 ? 1.0 - occ / total : 1.0;
}
