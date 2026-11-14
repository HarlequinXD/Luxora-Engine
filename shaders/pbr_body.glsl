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
layout(set = 1, binding = 2) uniform sampler2D u_roughness_map;  // G channel (grayscale or ORM-packed)
layout(set = 1, binding = 3) uniform sampler2D u_metallic_map;   // B channel
layout(set = 1, binding = 4) uniform sampler2D u_ao_map;         // R channel
layout(set = 1, binding = 5) uniform sampler2D u_height_map;     // R channel, white = high

layout(push_constant) uniform Material {
    vec4 albedo;
    vec4 params;    // metallic, roughness, uv scale, normal strength (0 = no normal map, < 0 = DirectX green)
    vec4 emissive;  // rgb, strength
    vec4 params2;   // height scale (0 = no parallax), ao strength, flags, -
} m;

const int MF_ROUGH_MAP = 1, MF_METAL_MAP = 2, MF_AO_MAP = 4, MF_SMOOTHNESS = 8, MF_PARALLAX_SHADOWS = 16;

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

/* Point and spot light shadows from the atlas. A point light has 6 tiles (cube faces, picked by the
 * major axis); a spot light has one looking down its cone. The projection is the renderer's perspective
 * camera written out, so no matrices are needed. Normal offset grows with distance (one texel). */
const vec3 LX_CUBE_F[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
const vec3 LX_CUBE_U[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));
float local_shadow(int i, vec3 world, vec3 ng) {
    vec4 s0 = u.light_shadow[i * 2];
    if (s0.x < 0.0) return 1.0;
    vec4 s1 = u.light_shadow[i * 2 + 1];
    vec3 lp = u.lights[i].pos_range.xyz;
    vec3 d = world - lp;
    float tile = s0.x;
    vec3 f, up;
    if (u.lights[i].color_type.w > 1.5) {
        f = normalize(u.lights[i].dir_cos.xyz);
        up = s1.xyz;
    } else {
        vec3 a = abs(d);
        int face = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0 : 1) : a.y >= a.z ? (d.y >= 0.0 ? 2 : 3) : (d.z >= 0.0 ? 4 : 5);
        f = LX_CUBE_F[face];
        up = LX_CUBE_U[face];
        tile += float(face);
    }
    float zn = s0.y, zf = s0.z, th = s0.w, res = s1.w;
    float z = dot(d, f);
    if (z <= zn) return 1.0;
    vec3 p = world + ng * (2.0 * z * th / res) * 1.5;   // about a texel and a half along the normal
    d = p - lp;
    z = max(dot(d, f), zn * 1.001);
    vec3 r = normalize(cross(f, up));
    vec2 ndc = vec2(dot(d, r), dot(d, up)) / (z * th);
    if (any(greaterThan(abs(ndc), vec2(1.0)))) return 1.0;   // outside a spot light's tile
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float depth = zf * (1.0 - zn / (z * 0.998)) / (zf - zn);
    if (depth >= 1.0) return 1.0;
    const float TILES = 8.0;
    float margin = 1.5 / res;
    uv = clamp(uv, vec2(margin), vec2(1.0 - margin));
    vec2 base = (vec2(mod(tile, TILES), floor(tile / TILES)) + uv) / TILES;
    float texel = 1.0 / (TILES * res);
    float sum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sum += texture(u_local_shadow, vec3(base + vec2(x, y) * texel, depth));
    return sum / 9.0;
}

/* The GI volume around a surface point: the 8 surrounding cells, trilinear, leaving out the cells that can't
 * see the point (a wall in between, or buried inside solid geometry), so light doesn't leak through walls.
 * Returns the blended L1 SH of the red, green and blue channels. Same rule as the CPU (lx_gi_irradiance). */
struct GiSH { vec4 r, g, b; };
GiSH gi_mix(GiSH a, GiSH b, float t) { return GiSH(mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t)); }
// level -1: the scene grid; 0, 1: the camera grids (stacked along z in their textures)
GiSH gi_gather_level(int level, vec3 q) {
    ivec3 res;
    vec3 g;
    int zoff = 0;
    if (level < 0) {
        res = ivec3(u.gi_res.xyz);
        g = (q - u.gi_origin.xyz) / u.gi_cell.xyz;
    } else {
        res = ivec3(u.gi_casc_res.x);
        g = (q - u.gi_casc[level].xyz) / u.gi_casc[level].w;
        zoff = level * u.gi_casc_res.x;
    }
    vec3 t = g - 0.5;
    ivec3 base = ivec3(floor(t));
    vec3 f = t - vec3(base);
    GiSH vis_sum = GiSH(vec4(0.0), vec4(0.0), vec4(0.0)), plain_sum = GiSH(vec4(0.0), vec4(0.0), vec4(0.0));
    float wv = 0.0, wp = 0.0;
    for (int k = 0; k < 8; k++) {
        ivec3 o = ivec3(k & 1, (k >> 1) & 1, k >> 2);
        ivec3 c = base + o;
        if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, res))) continue;
        vec3 ww = mix(1.0 - f, f, vec3(o));
        float w = ww.x * ww.y * ww.z;
        ivec3 at = c + ivec3(0, 0, zoff);
        vec4 cr, cg, cb, va;
        vec2 vb;
        if (level < 0) {
            cr = texelFetch(u_gi_r, at, 0); cg = texelFetch(u_gi_g, at, 0); cb = texelFetch(u_gi_b, at, 0);
            va = texelFetch(u_gi_vis_a, at, 0) * 1.5; vb = texelFetch(u_gi_vis_b, at, 0).xy * 1.5;
        } else {
            cr = texelFetch(u_gc_r, at, 0); cg = texelFetch(u_gc_g, at, 0); cb = texelFetch(u_gc_b, at, 0);
            va = texelFetch(u_gc_vis_a, at, 0) * 1.5; vb = texelFetch(u_gc_vis_b, at, 0).xy * 1.5;
        }
        plain_sum.r += cr * w; plain_sum.g += cg * w; plain_sum.b += cb * w;
        wp += w;
        vec3 d = g - (vec3(c) + 0.5);
        vec3 wall = vec3(d.x >= 0.0 ? va.x : va.y, d.y >= 0.0 ? va.z : va.w, d.z >= 0.0 ? vb.x : vb.y);
        if (any(lessThan(wall, abs(d) - 0.02))) continue;
        vis_sum.r += cr * w; vis_sum.g += cg * w; vis_sum.b += cb * w;
        wv += w;
    }
    if (wv > 1e-6) return GiSH(vis_sum.r / wv, vis_sum.g / wv, vis_sum.b / wv);
    if (wp > 1e-6) return GiSH(plain_sum.r / wp, plain_sum.g / wp, plain_sum.b / wp);
    return GiSH(vec4(0.0), vec4(0.0), vec4(0.0));
}
// how far inside a camera grid a point is: 1 well inside, fading to 0 over the last 3 cells at its edge
float gi_casc_fade(int level, vec3 q) {
    if (u.gi_casc[level].w <= 0.0) return 0.0;
    vec3 g = (q - u.gi_casc[level].xyz) / u.gi_casc[level].w;
    vec3 edge = min(g, vec3(u.gi_casc_res.x) - g);
    return clamp((min(min(edge.x, edge.y), edge.z) - 1.0) / 3.0, 0.0, 1.0);
}
/* The GI around a surface point: the finest grid that covers it, blending into the coarser ones near its edge.
 * Within each grid, the 8 surrounding cells, trilinear, leaving out cells that can't see the point (a wall in
 * between, or buried inside solid geometry), so light doesn't leak through walls (as lx_gi_irradiance). */
// where along the normal a surface reads the GI: a quarter cell out
vec3 gi_at(vec3 world, vec3 ng, float cell) { return world + ng * (cell * 0.25); }
GiSH gi_gather(vec3 world, vec3 ng) {
    float f0 = gi_casc_fade(0, gi_at(world, ng, u.gi_casc[0].w));
    if (f0 >= 1.0) return gi_gather_level(0, gi_at(world, ng, u.gi_casc[0].w));
    float f1 = gi_casc_fade(1, gi_at(world, ng, u.gi_casc[1].w));
    GiSH coarse;
    if (f1 >= 1.0) coarse = gi_gather_level(1, gi_at(world, ng, u.gi_casc[1].w));
    else {
        coarse = gi_gather_level(-1, gi_at(world, ng, u.gi_cell.x));
        if (f1 > 0.0) coarse = gi_mix(coarse, gi_gather_level(1, gi_at(world, ng, u.gi_casc[1].w)), f1);
    }
    return f0 > 0.0 ? gi_mix(coarse, gi_gather_level(0, gi_at(world, ng, u.gi_casc[0].w)), f0) : coarse;
}
/* Irradiance from one channel's L1 SH (coeff order L0, y, z, x), Geomerics' non-negative reconstruction.
 * The plain linear form a + b.n goes negative (clamped to black) for surfaces facing away from strongly
 * directional light, which cut hard-edged dark shapes into curved surfaces such as faces. This one has the same
 * average and main direction, never goes negative, and matches light from a single direction exactly
 * (the same as lx_gi_eval_l1 on the CPU). */
float gi_eval_l1(vec4 c, vec3 n) {
    float a = PI * 0.282095 * c.x;                                   // average irradiance
    if (a <= 0.0) return 0.0;
    vec3 b = (2.0 * PI / 3.0) * 0.488603 * vec3(c.w, c.y, c.z);     // its change across directions (x, y, z)
    float lb = length(b);
    if (lb < 1e-6 * a) return a;
    float rho = min(lb / (2.0 * a), 1.0);                            // 0 even light .. 1 all from one direction
    float q = 0.5 * (1.0 + dot(b / lb, n));
    float p = 1.0 + 2.0 * rho;
    float k = (1.0 - rho) / (1.0 + rho);
    return a * (k + (1.0 - k) * (p + 1.0) * pow(q, p));
}
vec3 gi_eval(GiSH sh, vec3 n) {
    return vec3(gi_eval_l1(sh.r, n), gi_eval_l1(sh.g, n), gi_eval_l1(sh.b, n)) * u.fog_color.w;
}

vec3 env_brdf_approx(vec3 f0, float rough, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

/* ---- parallax occlusion mapping ----
 * Ray-marches the height map in tangent space: the step count adapts to the viewing angle (more steps
 * at grazing angles, where the offset is largest), the hit is refined by interpolating between the
 * last two layers, texture gradients come from the original UVs (no mip seams), and the effect fades
 * out with distance so far-away surfaces pay nothing. The depth found also drives self-shadowing. */
float pom_height(vec2 uv, vec2 dx, vec2 dy) { return textureGrad(u_height_map, uv, dx, dy).r; }

vec2 parallax_uv(vec2 uv, vec3 v_ts, float scale, vec2 dx, vec2 dy, out float hit_depth) {
    hit_depth = 0.0;
    float nz = max(v_ts.z, 0.08);
    float layers = mix(48.0, 10.0, clamp(v_ts.z, 0.0, 1.0));
    float layer = 1.0 / layers;
    vec2 delta = -(v_ts.xy / nz) * scale * layer;   // UV shift per layer, away from the eye
    vec2 cur_uv = uv;
    float cur_depth = 0.0;
    float surf = 1.0 - pom_height(cur_uv, dx, dy);
    vec2 prev_uv = cur_uv;
    float prev_surf = surf, prev_depth = 0.0;
    for (int i = 0; i < 64 && cur_depth < surf; i++) {
        prev_uv = cur_uv; prev_surf = surf; prev_depth = cur_depth;
        cur_uv += delta;
        cur_depth += layer;
        surf = 1.0 - pom_height(cur_uv, dx, dy);
    }
    // linear refinement between the layer above and below the surface
    float after = surf - cur_depth, before = prev_surf - prev_depth;
    float t = clamp(before / max(before - after, 1e-5), 0.0, 1.0);
    hit_depth = mix(prev_depth, cur_depth, t);
    return mix(prev_uv, cur_uv, t);
}

/* Soft self-shadowing: march from the hit point towards the light and see how far the height field
 * rises above the ray. */
float parallax_shadow(vec2 uv, float depth, vec3 l_ts, float scale, vec2 dx, vec2 dy) {
    if (l_ts.z <= 0.0) return 0.0;
    if (depth <= 0.001) return 1.0;
    const int STEPS = 16;
    float layer = depth / float(STEPS);
    vec2 delta = (l_ts.xy / max(l_ts.z, 0.08)) * scale * layer;
    float shadow = 0.0;
    vec2 p = uv;
    float d = depth;
    for (int i = 1; i <= STEPS; i++) {
        p += delta;
        d -= layer;
        float surf = 1.0 - pom_height(p, dx, dy);
        float above = d - surf;   // > 0: the height field is above the ray
        shadow = max(shadow, above * (1.0 - float(i) / float(STEPS)) * 12.0);
    }
    return 1.0 - clamp(shadow, 0.0, 1.0);
}

void main() {
    vec2 uv = v_uv * m.params.z;
    vec2 uv_dx = dFdx(uv), uv_dy = dFdy(uv);
    vec3 v = normalize(u.camera_pos.xyz - v_world);
    // geometric normal facing the viewer (planes are drawn two-sided)
    vec3 ng = normalize(v_normal);
    if (dot(ng, v) < 0.0) ng = -ng;
    vec3 t = v_tangent.xyz - ng * dot(ng, v_tangent.xyz);
    t = dot(t, t) > 1e-8 ? normalize(t) : normalize(cross(ng, abs(ng.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 bt = cross(ng, t) * (v_tangent.w < 0.0 ? -1.0 : 1.0) * (dot(normalize(v_normal), ng) < 0.0 ? -1.0 : 1.0);
    mat3 tbn = mat3(t, bt, ng);
    int flags = int(m.params2.z + 0.5);

    float pom_depth = 0.0, self_shadow = 1.0;
    float height_scale = m.params2.x;
    if (height_scale > 0.0) {
        float fade = 1.0 - smoothstep(25.0, 45.0, v_view_depth);
        if (fade > 0.0) {
            vec3 v_ts = transpose(tbn) * v;
            uv = parallax_uv(uv, v_ts, height_scale * fade, uv_dx, uv_dy, pom_depth);
            if ((flags & MF_PARALLAX_SHADOWS) != 0 && u.sun_color.w > 0.5) {
                vec3 l_ts = transpose(tbn) * -u.sun_dir.xyz;
                self_shadow = mix(1.0, parallax_shadow(uv, pom_depth, l_ts, height_scale * fade, uv_dx, uv_dy), fade);
            }
        }
    }

    vec4 tex = textureGrad(u_albedo_map, uv, uv_dx, uv_dy);
    vec3 albedo = m.albedo.rgb * tex.rgb * v_tint.rgb;
    float metallic = m.params.x, rough = m.params.y;
    if ((flags & MF_ROUGH_MAP) != 0) {
        float r = textureGrad(u_roughness_map, uv, uv_dx, uv_dy).g;
        rough *= (flags & MF_SMOOTHNESS) != 0 ? 1.0 - r : r;
    }
    if ((flags & MF_METAL_MAP) != 0) metallic *= textureGrad(u_metallic_map, uv, uv_dx, uv_dy).b;
    metallic = clamp(metallic, 0.0, 1.0);
    rough = clamp(rough, 0.04, 1.0);
    float ao = 1.0;
    if ((flags & MF_AO_MAP) != 0) ao = mix(1.0, textureGrad(u_ao_map, uv, uv_dx, uv_dy).r, clamp(m.params2.y, 0.0, 1.0));
    ao *= 1.0 - pom_depth * 0.35 * clamp(height_scale * 10.0, 0.0, 1.0);   // deep crevices get less ambient light

    vec3 n = ng;
    float nstrength = m.params.w;
    if (nstrength != 0.0) {
        vec3 tn = textureGrad(u_normal_map, uv, uv_dx, uv_dy).xyz * 2.0 - 1.0;
        if (nstrength < 0.0) tn.y = -tn.y;     // DirectX-style normal map
        tn.xy *= abs(nstrength);
        n = normalize(tbn * tn);
        // a bumped normal can face away from the eye at grazing angles; bend it back so it doesn't
        // reflect the ground below the surface (black speckles)
        float nv = dot(n, v);
        if (nv < 0.05) n = normalize(n + v * (0.05 - nv));
    }
    vec3 color = vec3(0.0);
    if (u.sun_color.w > 0.5)
        color += brdf(n, v, -u.sun_dir.xyz, albedo, metallic, rough) * u.sun_color.rgb * shadow_factor(v_world, ng) * self_shadow;
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
        if (att > 0.0) att *= local_shadow(i, v_world, ng);
        color += brdf(n, v, l, albedo, metallic, rough) * L.color_type.rgb * att;
    }
    // indirect: virtual-photon GI volume (or hemisphere ambient when GI is off)
    vec3 irradiance;
    float ambient = u.sky_zenith.w;
    GiSH gi_sh = GiSH(vec4(0.0), vec4(0.0), vec4(0.0));
    if (u.gi_origin.w > 0.5) { gi_sh = gi_gather(v_world, ng); irradiance = gi_eval(gi_sh, n); }
    else irradiance = mix(u.ground_color.rgb, u.sky_zenith.rgb, n.y * 0.5 + 0.5) * ambient * PI;
    float ndv = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 spec_env = env_brdf_approx(f0, rough, ndv);
    vec3 kd = (1.0 - spec_env) * (1.0 - metallic);
    color += kd * albedo / PI * irradiance * ao;
    // specular ambient: probes (or the sky), pre-filtered by roughness
    vec3 r = reflect(-v, n);
    float local_w;
    vec3 env = lx_environment_reflection(v_world, r, rough, local_w);
    // specular occlusion: compare the photon GI arriving from the reflection direction with what
    // open sky would deliver from there, so only reflections that really look into geometry darken.
    // Placed probes captured the real surroundings, so they need it much less.
    float occl = 1.0;
    if (u.gi_origin.w > 0.5) {
        vec3 gi_r = gi_eval(gi_sh, r);
        vec3 open_sky = mix(u.sky_horizon.rgb, u.sky_zenith.rgb, 0.5) * ambient * PI * clamp(0.5 + 0.5 * r.y, 0.05, 1.0);
        occl = clamp(dot(gi_r, vec3(0.333)) / max(dot(open_sky, vec3(0.333)), 1e-3), 0.0, 1.0);
        occl = mix(0.35, 1.0, sqrt(occl)); /* GI is low-frequency: never fully black out a reflection */
        occl = mix(occl, 1.0, local_w * 0.7);
    }
    // specular occlusion from the AO map (Lagarde & de Rousiers)
    float spec_ao = clamp(pow(ndv + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
    vec3 spec_weight = clamp(spec_env * occl * spec_ao, 0.0, 1.0);
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
