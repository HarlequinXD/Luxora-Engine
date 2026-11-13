// Environment reflections: reflection probes (set 2) with the procedural sky as the fallback.
// Included by the PBR shader and the SSR composite so both compute exactly the same value.
layout(set = 2, binding = 0) uniform samplerCube u_probes[LX_PROBE_SLOTS];

// Box projection: intersect the reflection ray with the probe's box and look the hit point up
// from the capture point, so reflections of walls and floors line up with the real geometry.
vec3 lx_probe_dir(int i, vec3 p, vec3 r) {
    if (u.probe_max[i].w < 0.5) return r;
    vec3 inv = 1.0 / max(abs(r), vec3(1e-5)) * sign(r + vec3(1e-7));
    vec3 t_max = max((u.probe_max[i].xyz - p) * inv, (u.probe_min[i].xyz - p) * inv);
    float t = min(min(t_max.x, t_max.y), t_max.z);
    return p + r * max(t, 0.0) - u.probe_pos[i].xyz;
}

// 1 well inside the box, fading to 0 across the blend distance at its faces.
float lx_probe_weight(int i, vec3 p) {
    vec3 d = min(p - u.probe_min[i].xyz, u.probe_max[i].xyz - p);
    float m = min(min(d.x, d.y), d.z);
    return clamp(m / max(u.probe_min[i].w, 1e-3), 0.0, 1.0);
}

// Pre-filtered radiance reflected towards the viewer along r for a surface of this roughness.
// local_weight reports how much came from placed probes (they already contain occlusion).
vec3 lx_environment_reflection(vec3 p, vec3 r, float rough, out float local_weight) {
    float lod = rough * max(u.refl_params.x - 1.0, 0.0);
    vec3 base = (u.refl_params.y > 0.5 && u.probe_pos[0].w > 0.0)
              ? textureLod(u_probes[0], r, lod).rgb * u.probe_pos[0].w
              : lx_sky_glossy(r, rough);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    if (u.refl_params.y > 0.5) {
        for (int i = 1; i < LX_PROBE_SLOTS; i++) {
            if (u.probe_pos[i].w <= 0.0) continue;
            float w = lx_probe_weight(i, p);
            if (w <= 0.0) continue;
            acc += textureLod(u_probes[i], lx_probe_dir(i, p, r), lod).rgb * u.probe_pos[i].w * w;
            wsum += w;
        }
    }
    if (wsum > 1.0) { acc /= wsum; wsum = 1.0; }
    local_weight = wsum;
    return acc + base * (1.0 - wsum);
}
