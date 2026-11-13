// PBR forward shading. Built twice: pbr.frag (one output) and pbr_mrt.frag (LX_MRT = 1), which also
// writes the normal/roughness/specular-weight buffers the screen-space reflection pass reads.
#pragma lx_include reflections.glsl
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tangent;
layout(location = 4) in vec4 v_tint;
layout(location = 5) in float v_view_depth;
layout(location = 0) out vec4 o_color;
#if LX_MRT
layout(location = 1) out vec4 o_normal;  // world normal * 0.5 + 0.5
layout(location = 2) out vec4 o_spec;    // rgb: weight of the reflected light, a: roughness
#endif

layout(set = 1, binding = 0) uniform sampler2D u_albedo_map;
layout(set = 1, binding = 1) uniform sampler2D u_normal_map;

layout(push_constant) uniform Material {
    vec4 albedo;
    vec4 params;    // metallic, roughness, uv scale, has normal map
    vec4 emissive;  // rgb, strength
} m;

const float PI = 3.14159265;

float d_ggx(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float v_smith(float ndv, float ndl, float a) {
    float k = a * 0.5;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl / max(4.0 * ndv * ndl, 1e-4);
}
vec3 f_schlick(float vdh, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0); }

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float metallic, float rough) {
    vec3 h = normalize(v + l);
    float ndl = max(dot(n, l), 0.0), ndv = max(dot(n, v), 1e-4), ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0);
    float a = max(rough * rough, 0.002);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 f = f_schlick(vdh, f0);
    vec3 spec = d_ggx(ndh, a) * v_smith(ndv, ndl, a) * f;
    vec3 kd = (1.0 - f) * (1.0 - metallic);
    return (kd * albedo / PI + spec) * ndl;
}

float shadow_factor(vec3 world, vec3 n) {
    if (u.sun_dir.w < 0.5 || u.counts.y == 0) return 1.0;
    int cascade = u.counts.y - 1;
    for (int i = 0; i < u.counts.y; i++) {
        if (v_view_depth < u.cascade_splits[i]) { cascade = i; break; }
    }
    if (v_view_depth > u.cascade_splits[u.counts.y - 1]) return 1.0;
    float texel = 1.0 / float(u.counts.z);
    vec3 offset_pos = world + n * (0.02 + 0.03 * float(cascade));
    vec4 c = u.cascade_vp[cascade] * vec4(offset_pos, 1.0);
    vec3 ndc = c.xyz / c.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float depth = ndc.z * 0.5 + 0.5 - 0.0005;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || depth > 1.0) return 1.0;
    float sum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sum += texture(u_shadow, vec4(uv + vec2(x, y) * texel * 1.5, float(cascade), depth));
    return sum / 9.0;
}

vec3 gi_irradiance(vec3 world, vec3 n) {
    vec3 g = (world + n * u.gi_cell.x * 0.5 - u.gi_origin.xyz) / (u.gi_cell.xyz * u.gi_res.xyz);
    vec4 r = texture(u_gi_r, g), gg = texture(u_gi_g, g), b = texture(u_gi_b, g);
    // L1 SH irradiance (coeff order: L0, y, z, x) with clamped-cosine convolution
    vec4 basis = vec4(PI * 0.282095, (2.0 * PI / 3.0) * 0.488603 * vec3(n.y, n.z, n.x));
    return max(vec3(dot(r, basis), dot(gg, basis), dot(b, basis)), vec3(0.0)) * u.fog_color.w;
}

vec3 env_brdf_approx(vec3 f0, float rough, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

void main() {
    vec2 uv = v_uv * m.params.z;
    vec4 tex = texture(u_albedo_map, uv);
    vec3 albedo = m.albedo.rgb * tex.rgb * v_tint.rgb;
    float metallic = clamp(m.params.x, 0.0, 1.0);
    float rough = clamp(m.params.y, 0.04, 1.0);
    vec3 n = normalize(v_normal);
    if (m.params.w > 0.5) {
        vec3 t = normalize(v_tangent.xyz - n * dot(n, v_tangent.xyz));
        vec3 bt = cross(n, t) * v_tangent.w;
        vec3 tn = texture(u_normal_map, uv).xyz * 2.0 - 1.0;
        n = normalize(mat3(t, bt, n) * tn);
    }
    vec3 v = normalize(u.camera_pos.xyz - v_world);
    if (dot(n, v) < 0.0) n = -n; // two-sided lighting for thin geometry like planes
    vec3 color = vec3(0.0);
    if (u.sun_color.w > 0.5)
        color += brdf(n, v, -u.sun_dir.xyz, albedo, metallic, rough) * u.sun_color.rgb * shadow_factor(v_world, n);
    for (int i = 0; i < u.counts.x; i++) {
        LxLightData L = u.lights[i];
        vec3 to_l = L.pos_range.xyz - v_world;
        float d2 = max(dot(to_l, to_l), 1e-4);
        vec3 l = to_l * inversesqrt(d2);
        float range = L.pos_range.w;
        float window = clamp(1.0 - pow(d2 / (range * range), 2.0), 0.0, 1.0);
        float att = window * window / d2;
        if (L.color_type.w > 1.5) {
            float cd = dot(-l, normalize(L.dir_cos.xyz));
            att *= smoothstep(L.dir_cos.w, mix(L.dir_cos.w, 1.0, 0.25), cd);
        }
        color += brdf(n, v, l, albedo, metallic, rough) * L.color_type.rgb * att;
    }
    // indirect: virtual-photon GI volume (or hemisphere ambient when GI is off)
    vec3 irradiance;
    float ambient = u.sky_zenith.w;
    if (u.gi_origin.w > 0.5) irradiance = gi_irradiance(v_world, n);
    else irradiance = mix(u.ground_color.rgb, u.sky_zenith.rgb, n.y * 0.5 + 0.5) * ambient * PI;
    float ndv = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 spec_env = env_brdf_approx(f0, rough, ndv);
    vec3 kd = (1.0 - spec_env) * (1.0 - metallic);
    color += kd * albedo / PI * irradiance;
    // specular ambient: probes (or the sky), pre-filtered by roughness
    vec3 r = reflect(-v, n);
    float local_w;
    vec3 env = lx_environment_reflection(v_world, r, rough, local_w);
    // specular occlusion: compare the photon GI arriving from the reflection direction with what
    // open sky would deliver from there, so only reflections that really look into geometry darken.
    // Placed probes captured the real surroundings, so they need it much less.
    float occl = 1.0;
    if (u.gi_origin.w > 0.5) {
        vec3 gi_r = gi_irradiance(v_world, r);
        vec3 open_sky = mix(u.sky_horizon.rgb, u.sky_zenith.rgb, 0.5) * ambient * PI * clamp(0.5 + 0.5 * r.y, 0.05, 1.0);
        occl = clamp(dot(gi_r, vec3(0.333)) / max(dot(open_sky, vec3(0.333)), 1e-3), 0.0, 1.0);
        occl = mix(0.35, 1.0, sqrt(occl)); /* GI is low-frequency: never fully black out a reflection */
        occl = mix(occl, 1.0, local_w * 0.7);
    }
    vec3 spec_weight = clamp(spec_env * occl, 0.0, 1.0);
    color += spec_weight * env;
    color += m.emissive.rgb * m.emissive.w;
    float fog = 1.0 - exp(-u.ground_color.w * length(u.camera_pos.xyz - v_world));
    color = mix(color, u.fog_color.rgb * ambient, fog);
    o_color = vec4(color, 1.0);
#if LX_MRT
    o_normal = vec4(n * 0.5 + 0.5, 1.0);
    o_spec = vec4(spec_weight * (1.0 - fog), rough);
#endif
}
