// PBR forward shading. Built twice: pbr.frag (one output) and pbr_mrt.frag (LX_MRT = 1), which also
// writes the normal/roughness/specular-weight buffers the screen-space reflection pass reads.
#pragma lx_include reflections.glsl
layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_tangent;
layout(location = 4) in vec4 v_tint;
layout(location = 5) in float v_view_depth;
layout(location = 6) in vec2 v_uv2;   // lightmap UVs
layout(location = 7) in vec3 v_lm;    // lightmap texel (z = layer, -1 = not lightmapped)
layout(location = 0) out vec4 o_color;
#if LX_MRT
layout(location = 1) out vec4 o_normal;  // world normal * 0.5 + 0.5
layout(location = 2) out vec4 o_spec;    // rgb: weight of the reflected light, a: roughness
layout(location = 3) out vec4 o_ambient; // the bounce and sky light on screen: what ambient occlusion darkens
#endif

layout(set = 1, binding = 0) uniform sampler2D u_albedo_map;
layout(set = 1, binding = 1) uniform sampler2D u_normal_map;
layout(set = 1, binding = 2) uniform sampler2D u_roughness_map;  // G channel (grayscale or ORM-packed)
layout(set = 1, binding = 3) uniform sampler2D u_metallic_map;   // B channel
layout(set = 1, binding = 4) uniform sampler2D u_ao_map;         // R channel
layout(set = 1, binding = 5) uniform sampler2D u_height_map;     // R channel, white = high
layout(set = 1, binding = 6) uniform sampler2DArray u_layer_a;   // terrain: each layer's colour (sRGB) + roughness
layout(set = 1, binding = 7) uniform sampler2DArray u_layer_b;   // terrain: normal xy, height, ao

layout(push_constant) uniform Material {
    vec4 albedo;
    vec4 params;    // metallic, roughness, uv scale, normal strength (0 = no normal map, < 0 = DirectX green)
    vec4 emissive;  // rgb, strength
    vec4 params2;   // height scale (0 = no parallax), ao strength, flags, -
    vec4 tile_a;    // terrain: metres one repeat of layers 0-3 covers
    vec4 tile_b;    // terrain: layers 4-5, how many layers, parallax depth (m, 0 = off)
} m;

const int MF_ROUGH_MAP = 1, MF_METAL_MAP = 2, MF_AO_MAP = 4, MF_SMOOTHNESS = 8, MF_PARALLAX_SHADOWS = 16, MF_TERRAIN = 32;
const int MF_GLASS = 256, MF_ADDITIVE = 512;   // see-through kinds (pbr_blend.frag); Fade is the rest
// Variants: LX_CUTOUT (pbr_cut / pbr_mrt_cut) cuts away pixels under the material's alpha cutoff;
// LX_BLEND (pbr_blend) is drawn after the solid world and writes premultiplied colour + how much it hides behind it.

/* Terrain: the vertices carry six layer weights (uv: 0-1, uv2: 2-3, tangent.w: 4, the rest: 5). Each layer is
   sampled from above in world space (or from three sides on steep slopes), and they are blended by height so rock
   shows between grass rather than smearing into it.
   Parallax: the ground seen from above is ray-marched through the blended layers' heights (stones stand out of the
   soil, cracks sink in) and the result shadows itself from the sun. */
float terrain_height(vec2 xz, float tb[6], float tiles[6], int count, vec2 gx, vec2 gy) {
    float h = 0.0, wt = 0.0;
    for (int l = 0; l < 6; l++) {
        if (l >= count || tb[l] <= 0.0) continue;
        float s = 1.0 / tiles[l];
        h += textureGrad(u_layer_b, vec3(xz * s, float(l)), gx * s, gy * s).b * tb[l];
        wt += tb[l];
    }
    return wt > 0.0 ? h / wt : 1.0;
}
void terrain_surface(vec3 ng, vec3 v, inout vec3 albedo, inout float rough, inout float ao, inout vec3 n, inout float shadow) {
    float w[6];
    w[0] = v_uv.x; w[1] = v_uv.y; w[2] = v_uv2.x; w[3] = v_uv2.y; w[4] = v_tangent.w;
    w[5] = max(1.0 - (w[0] + w[1] + w[2] + w[3] + w[4]), 0.0);
    float tiles[6] = float[6](m.tile_a.x, m.tile_a.y, m.tile_a.z, m.tile_a.w, m.tile_b.x, m.tile_b.y);
    int count = int(m.tile_b.z + 0.5);
    vec3 bl = pow(abs(ng), vec3(6.0));
    bl /= bl.x + bl.y + bl.z;
    bool tri = bl.y < 0.97;   // steep: textures from the side too, so cliffs don't stretch
    vec3 p = v_world;
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    // pass 1: each layer's height, to pick which shows
    float score[6];
    float best = -10.0;
    for (int l = 0; l < 6; l++) {
        score[l] = -10.0;
        if (l >= count || w[l] < 0.004) continue;
        float s = 1.0 / tiles[l];
        float h = textureGrad(u_layer_b, vec3(p.xz * s, float(l)), dpx.xz * s, dpy.xz * s).b;
        score[l] = h * 0.6 + w[l];
        best = max(best, score[l]);
    }
    float tb[6];
    for (int l = 0; l < 6; l++) tb[l] = max(score[l] - (best - 0.25), 0.0);
    // parallax on the ground seen from above: where the eye's ray meets the height field
    vec2 top = p.xz;
    float depth_m = m.tile_b.w * (1.0 - smoothstep(25.0, 45.0, v_view_depth)) * smoothstep(0.6, 0.9, bl.y);
    float hit = 0.0;
    if (depth_m > 0.001 && v.y > 0.02) {
        float vy = max(v.y, 0.12);
        float steps = mix(24.0, 8.0, clamp(v.y, 0.0, 1.0));
        float layer = 1.0 / steps;
        vec2 delta = -(v.xz / vy) * depth_m * layer;   // metres across per step down
        vec2 cur = top, prev = top;
        float d = 0.0, pd = 0.0;
        float surf = 1.0 - terrain_height(cur, tb, tiles, count, dpx.xz, dpy.xz), psurf = surf;
        for (int i = 0; i < 32 && d < surf; i++) {
            prev = cur; psurf = surf; pd = d;
            cur += delta;
            d += layer;
            surf = 1.0 - terrain_height(cur, tb, tiles, count, dpx.xz, dpy.xz);
        }
        float after = surf - d, before = psurf - pd;
        float t = clamp(before / max(before - after, 1e-5), 0.0, 1.0);
        hit = mix(pd, d, t);
        top = mix(prev, cur, t);
        // the bumps shadow each other from the sun
        vec3 ls = -u.sun_dir.xyz;   // the same sun the lighting and shadow maps use
        if (u.sun_color.w > 0.5 && ls.y > 0.0 && hit > 0.001) {
            const int SS = 10;
            float sl = hit / float(SS);
            vec2 sd = (ls.xz / max(ls.y, 0.1)) * depth_m * sl;
            float occl = 0.0;
            vec2 q = top;
            float dd = hit;
            for (int i = 1; i <= SS; i++) {
                q += sd;
                dd -= sl;
                float above = dd - (1.0 - terrain_height(q, tb, tiles, count, dpx.xz, dpy.xz));
                occl = max(occl, above * (1.0 - float(i) / float(SS)) * 10.0);
            }
            shadow = 1.0 - clamp(occl, 0.0, 1.0) * bl.y;
        }
    }
    vec3 col = vec3(0.0), nrm = vec3(0.0);
    float r = 0.0, occ = 0.0, total = 0.0;
    for (int l = 0; l < 6; l++) {
        float b = tb[l];
        if (b <= 0.0) continue;
        float s = 1.0 / tiles[l];
        vec4 A = textureGrad(u_layer_a, vec3(top * s, float(l)), dpx.xz * s, dpy.xz * s);
        vec4 B = textureGrad(u_layer_b, vec3(top * s, float(l)), dpx.xz * s, dpy.xz * s);
        vec3 lc = A.rgb * bl.y, ln = vec3(B.x * 2.0 - 1.0, 0.0, B.y * 2.0 - 1.0) * bl.y;
        float lr = A.a * bl.y, lo = B.a * bl.y;
        if (tri) {   // from the x and z sides
            vec4 Ax = textureGrad(u_layer_a, vec3(p.zy * s, float(l)), dpx.zy * s, dpy.zy * s);
            vec4 Bx = textureGrad(u_layer_b, vec3(p.zy * s, float(l)), dpx.zy * s, dpy.zy * s);
            vec4 Az = textureGrad(u_layer_a, vec3(p.xy * s, float(l)), dpx.xy * s, dpy.xy * s);
            vec4 Bz = textureGrad(u_layer_b, vec3(p.xy * s, float(l)), dpx.xy * s, dpy.xy * s);
            lc += Ax.rgb * bl.x + Az.rgb * bl.z;
            lr += Ax.a * bl.x + Az.a * bl.z;
            lo += Bx.a * bl.x + Bz.a * bl.z;
            ln += vec3(0.0, Bx.y * 2.0 - 1.0, Bx.x * 2.0 - 1.0) * bl.x * sign(ng.x) + vec3(Bz.x * 2.0 - 1.0, Bz.y * 2.0 - 1.0, 0.0) * bl.z * sign(ng.z);
        }
        col += lc * b; r += lr * b; occ += lo * b; nrm += ln * b;
        total += b;
    }
    if (total <= 0.0) return;
    albedo = col / total * v_tint.rgb;
    rough = clamp(r / total, 0.04, 1.0);
    ao = occ / total * (1.0 - hit * 0.35);   // deep cracks get less ambient light
    n = normalize(ng + nrm / total);
}

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

// the highlight part of brdf(): baked light shines too
vec3 brdf_spec(vec3 n, vec3 v, vec3 l, vec3 albedo, float metallic, float rough) {
    vec3 h = normalize(v + l);
    float ndl = max(dot(n, l), 0.0), ndv = max(dot(n, v), 1e-4), ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0);
    float a = max(rough * rough, 0.002);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    return d_ggx(ndh, a) * v_smith(ndv, ndl, a) * f_schlick(vdh, f0) * ndl;
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
    if (mod(u.lights[i].color_type.w, 10.0) > 1.5) {
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

#pragma lx_include gi_common.glsl

// Baked Lighting Volume at p (like Bakery Volumes): its light (SH) and shadowmask, filtered between voxels.
// Returns how much the volume covers p: 0 outside, fading in over half a voxel at its faces.
float bvol_at(vec3 p, out GiSH sh, out vec4 mask) {
    sh = GiSH(vec4(0.0), vec4(0.0), vec4(0.0));
    mask = vec4(1.0);
    int count = int(u.bvol_params.x + 0.5);
    for (int i = 0; i < count; i++) {
        vec4 pp = vec4(p, 1.0);
        vec3 q = vec3(dot(u.bvol_rows[i * 3], pp), dot(u.bvol_rows[i * 3 + 1], pp), dot(u.bvol_rows[i * 3 + 2], pp));
        if (any(lessThan(q, vec3(0.0))) || any(greaterThan(q, vec3(1.0)))) continue;
        vec4 info = u.bvol_info[i];   // z offset, nx, ny, nz
        vec3 res = info.yzw;
        vec3 t = q * res;
        vec3 edge = min(t, res - t);
        float cover = clamp(min(min(edge.x, edge.y), edge.z) * 2.0, 0.0, 1.0);
        vec3 size = vec3(textureSize(u_bvol, 0));
        vec3 c = clamp(t, vec3(0.5), res - 0.5);   // stay inside this volume's voxels
        vec2 xy = c.xy / size.xy;
        float z0 = info.x + c.z, D = u.bvol_params.y;
        sh.r = texture(u_bvol, vec3(xy, z0 / size.z));
        sh.g = texture(u_bvol, vec3(xy, (z0 + D) / size.z));
        sh.b = texture(u_bvol, vec3(xy, (z0 + 2.0 * D) / size.z));
        mask = texture(u_bvol, vec3(xy, (z0 + 3.0 * D) / size.z));
        return cover;
    }
    return 0.0;
}
vec3 sh_irradiance(GiSH sh, vec3 n) {
    vec3 e = vec3(gi_eval_l1(sh.r, n), gi_eval_l1(sh.g, n), gi_eval_l1(sh.b, n));
    return (any(isnan(e)) || any(isinf(e))) ? vec3(0.0) : e;
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
    float alpha = m.albedo.a * tex.a * v_tint.a;
#if defined(LX_CUTOUT)
    // smaller mips average the alpha down, so thin cut-outs (leaves, wires) would thin away in the distance: lift it
    alpha *= 1.0 + max(textureQueryLod(u_albedo_map, uv).x, 0.0) * 0.25;
    if (alpha < m.params2.w) discard;
#endif
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
    if ((flags & MF_TERRAIN) != 0) { terrain_surface(ng, v, albedo, rough, ao, n, self_shadow); metallic = 0.0; nstrength = 0.0; }
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
    // weather: rain wets what the sky can reach (darker, glossier, puddles that ripple); snow settles on what faces up
    if (lx_day_night() && (u.weather_a.z > 0.002 || u.weather_a.w > 0.002)) {
        float open_sky = 1.0;
        if (u.weather_b.w > 0.5) {   // under a roof stays dry
            vec4 c = u.precip_vp * vec4(v_world + ng * 0.05, 1.0);
            vec3 ndc = c.xyz / c.w;
            vec2 puv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            if (all(greaterThan(puv, vec2(0.0))) && all(lessThan(puv, vec2(1.0))))
                open_sky = texture(u_shadow, vec4(puv, float(u.counts.w), ndc.z * 0.5 + 0.5 - 0.003));
        }
        float up = max(ng.y, 0.0);
        float wet = u.weather_a.z * open_sky * mix(0.45, 1.0, up);
        if (wet > 0.0) {
            float porous = (1.0 - metallic) * wet;
            albedo *= mix(1.0, 0.55, porous * (0.6 + 0.4 * rough));   // soaked: darker
            rough = mix(rough, 0.12, wet * 0.85);
            // puddles where flat ground dips (noise), filling as it gets wetter
            float pn = lx_fbm2(v_world.xz * 0.35);
            float puddle = smoothstep(1.05 - u.weather_a.z * 0.45, 1.15 - u.weather_a.z * 0.45, pn + 0.2) * smoothstep(0.92, 0.98, up) * open_sky;
            if (puddle > 0.0) {
                albedo *= mix(1.0, 0.6, puddle);
                rough = mix(rough, 0.02, puddle);
                metallic *= 1.0 - puddle;
                vec3 pn3 = ng;
                if (u.weather_a.x > 0.01) {   // raindrops ring the water
                    vec2 cell = floor(v_world.xz * 2.5), f = fract(v_world.xz * 2.5) - 0.5;
                    float h = lx_hash12(cell);
                    float ph = fract(u.camera_pos.w * 1.3 + h);
                    vec2 off = vec2(lx_hash12(cell + 3.1), lx_hash12(cell + 7.7)) - 0.5;
                    float r = length(f - off * 0.5);
                    float ring = sin((r - ph * 0.5) * 60.0) * smoothstep(0.0, 0.1, ph * 0.5 - r + 0.05) * (1.0 - ph) * smoothstep(0.5, 0.3, r);
                    vec2 g = (f - off * 0.5) / max(r, 1e-3) * ring * 0.25 * min(u.weather_a.x * 2.0, 1.0);
                    pn3 = normalize(ng + vec3(g.x, 0.0, g.y));
                }
                n = normalize(mix(n, pn3, puddle));
            }
        }
        float sn = u.weather_a.w * open_sky * smoothstep(0.25, 0.75, n.y) * smoothstep(0.1, 0.5, lx_vnoise2(v_world.xz * 1.7) + u.weather_a.w * 0.6);
        if (sn > 0.0) {
            albedo = mix(albedo, vec3(0.88, 0.9, 0.95), sn);
            rough = mix(rough, 0.65, sn);
            metallic *= 1.0 - sn;
            n = normalize(mix(n, ng, sn * 0.7));
        }
        rough = clamp(rough, 0.02, 1.0);
    }
    vec3 color = vec3(0.0);
    bool lightmapped = v_lm.z >= -0.5;
    // everything else may stand in a Baked Lighting Volume: baked light and baked shadows for characters
    GiSH vol_sh;
    vec4 vol_mask;
    float vol_cover = lightmapped ? 0.0 : bvol_at(v_world + ng * 0.05, vol_sh, vol_mask);
    // a Baked light's light is in the lightmap / volume already: skip (or fade) its real-time light there
    if (u.sun_color.w > 0.5) {
        float w = u.sun_color.w > 1.5 ? (lightmapped ? 0.0 : 1.0 - vol_cover) : 1.0;
        float sh = shadow_factor(v_world, ng) * self_shadow;
        if (lx_day_night()) sh *= lx_cloud_shadow(v_world, -u.sun_dir.xyz);   // cloud shadows drift over the ground
        int sch = int(u.bvol_params.z + 0.5);
        if (u.bvol_params.z > -0.5 && vol_cover > 0.0) sh = min(sh, mix(1.0, vol_mask[sch], vol_cover));   // Mixed sun: baked shadows too
        if (w > 0.0) color += brdf(n, v, -u.sun_dir.xyz, albedo, metallic, rough) * u.sun_color.rgb * sh * w;
    }
    for (int i = 0; i < u.counts.x; i++) {
        LxLightData L = u.lights[i];
        float code = mod(L.color_type.w, 100.0);   // type + 10 for Baked; + 100 * (shadowmask channel + 1)
        bool baked = code > 9.5;
        int mch = int(L.color_type.w / 100.0 + 0.001) - 1;
        if (lightmapped && baked) continue;
        float lw = baked ? 1.0 - vol_cover : 1.0;
        if (lw <= 0.0) continue;
        vec3 to_l = L.pos_range.xyz - v_world;
        float d2 = max(dot(to_l, to_l), 1e-4);
        vec3 l = to_l * inversesqrt(d2);
        float range = L.pos_range.w;
        float window = clamp(1.0 - pow(d2 / (range * range), 2.0), 0.0, 1.0);
        float att = window * window / d2;
        if (mod(L.color_type.w, 10.0) > 1.5) {
            float cd = dot(-l, normalize(L.dir_cos.xyz));
            att *= smoothstep(L.dir_cos.w, mix(L.dir_cos.w, 1.0, 0.25), cd);
        }
        if (att > 0.0) {
            float ls = local_shadow(i, v_world, ng);
            if (mch >= 0 && vol_cover > 0.0) ls = min(ls, mix(1.0, vol_mask[mch], vol_cover));   // baked shadows from static objects
            att *= ls;
        }
        color += brdf(n, v, l, albedo, metallic, rough) * L.color_type.rgb * att * lw;
    }
    // indirect: Live GI probes (or hemisphere ambient when GI is off)
    vec3 irradiance;
    float ambient = u.sky_zenith.w;
    GiSH gi_sh = GiSH(vec4(0.0), vec4(0.0), vec4(0.0));
    vec3 sky_ambient = mix(u.ground_color.rgb, u.sky_zenith.rgb, n.y * 0.5 + 0.5) * ambient * PI;
    float gi_cover = 0.0;   // how far inside the GI grid this point is: outside it there's no GI, so plain sky ambient
    if (u.gi_origin.w > 0.5) {
        vec3 g = (v_world - u.gi_origin.xyz) / u.gi_cell.xyz;
        vec3 edge = min(g, u.gi_res.xyz - g);
        gi_cover = clamp(min(min(edge.x, edge.y), edge.z) - 0.5, 0.0, 1.0);
    }
    if (lightmapped) {
        // baked: irradiance at the surface, turned towards the bumped normal with the stored light direction
        // (the direction map's w is how much the unbumped normal faces it, so an unbumped surface gets exactly the bake)
        vec4 lm = texture(u_lightmap, v_lm);
        vec4 ld = texture(u_lightmap, vec3(v_lm.xy, v_lm.z + 1.0));
        vec3 ldir = ld.xyz * 2.0 - 1.0;
        float turn = clamp((dot(n, ldir) * 0.5 + 0.5) / max(ld.w, 0.05), 0.0, 3.0);
        irradiance = lm.rgb * turn;
        if (lm.a < 0.0) irradiance += (-lm.a) * sky_ambient;   // Day-night bake: the sky it sees, as the sky is now
        gi_cover = 0.0;
        // baked light shines: the part that comes from one direction gets a highlight, rough or glossy as the material
        float dirness = length(ldir);
        if (dirness > 0.05) {
            vec3 bl = ldir / dirness;
            vec3 peak = min(lm.rgb * dirness / max(dot(ng, bl), 0.25), lm.rgb * 4.0);
            color += brdf_spec(n, v, bl, albedo, metallic, rough) * peak * ao;
        }
    } else {
        if (gi_cover > 0.0) {
            gi_sh = gi_gather(v_world, ng);
            irradiance = mix(sky_ambient, gi_eval(gi_sh, n) * gi_dfao(v_world, ng), gi_cover);
        }
        else irradiance = sky_ambient;
        if (vol_cover > 0.0) {
            irradiance = mix(irradiance, sh_irradiance(vol_sh, n), vol_cover);
            // its main light direction shines too (the strongly one-sided part of the light: Baked lights, a bright opening)
            vec3 bv = vec3(dot(vec3(vol_sh.r.w, vol_sh.g.w, vol_sh.b.w), vec3(0.2126, 0.7152, 0.0722)),
                           dot(vec3(vol_sh.r.y, vol_sh.g.y, vol_sh.b.y), vec3(0.2126, 0.7152, 0.0722)),
                           dot(vec3(vol_sh.r.z, vol_sh.g.z, vol_sh.b.z), vec3(0.2126, 0.7152, 0.0722)));
            float l0 = dot(vec3(vol_sh.r.x, vol_sh.g.x, vol_sh.b.x), vec3(0.2126, 0.7152, 0.0722));
            float lb = length(bv);
            if (l0 > 0.0 && lb > 1e-5) {
                vec3 bl = bv / lb;
                float rho = clamp(lb * (2.0 * PI / 3.0) * 0.488603 / (2.0 * PI * 0.282095 * l0), 0.0, 1.0);
                color += brdf_spec(n, v, bl, albedo, metallic, rough) * sh_irradiance(vol_sh, bl) * rho * vol_cover * ao;
            }
        }
    }
    float ndv = max(dot(n, v), 1e-4);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 spec_env = env_brdf_approx(f0, rough, ndv);
    vec3 kd = (1.0 - spec_env) * (1.0 - metallic);
    if (u.sky_misc.z > 0.0) irradiance += vec3(0.7, 0.75, 0.9) * u.sky_misc.z * 1.5 * (0.45 + 0.55 * max(n.y, 0.0));   // lightning
    vec3 ambient_diffuse = kd * albedo / PI * irradiance * ao;
    color += ambient_diffuse;
    // specular ambient: probes (or the sky), pre-filtered by roughness
    vec3 r = reflect(-v, n);
    float local_w;
    vec3 env = lx_environment_reflection(v_world, r, rough, lx_probe0_scale(v_world, r, gi_sh, gi_cover), local_w);
    // specular occlusion: compare the GI arriving from the reflection direction with what
    // open sky would deliver from there, so only reflections that really look into geometry darken.
    // Placed probes captured the real surroundings, so they need it much less.
    float occl = 1.0;
    if (gi_cover > 0.0) {
        vec3 gi_r = gi_eval(gi_sh, r);
        vec3 open_sky = mix(u.sky_horizon.rgb, u.sky_zenith.rgb, 0.5) * ambient * PI * clamp(0.5 + 0.5 * r.y, 0.05, 1.0);
        occl = clamp(dot(gi_r, vec3(0.333)) / max(dot(open_sky, vec3(0.333)), 1e-3), 0.0, 1.0);
        occl = mix(0.35, 1.0, sqrt(occl)); /* GI is low-frequency: never fully black out a reflection */
        occl = mix(occl, 1.0, local_w * 0.7);
        occl = mix(1.0, occl, gi_cover);
    }
    if (lightmapped || vol_cover > 0.0) {   // baked: how much less light arrives than the open sky would give
        vec3 from_r = lightmapped ? irradiance : sh_irradiance(vol_sh, r);   // a volume knows where light comes from
        float open_e = max(dot(lightmapped ? sky_ambient : mix(u.sky_horizon.rgb, u.sky_zenith.rgb, 0.5) * ambient * PI * clamp(0.5 + 0.5 * r.y, 0.05, 1.0), vec3(0.333)), 1e-3);
        float ratio = clamp(dot(from_r, vec3(0.333)) / open_e, 0.0, 1.0);
        float baked_occl = mix(mix(0.35, 1.0, sqrt(ratio)), 1.0, local_w * 0.7);
        occl = mix(occl, baked_occl, lightmapped ? 1.0 : vol_cover);
    }
    // specular occlusion from the AO map (Lagarde & de Rousiers)
    float spec_ao = clamp(pow(ndv + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao, 0.0, 1.0);
    vec3 spec_weight = clamp(spec_env * occl * spec_ao, 0.0, 1.0);
    color += spec_weight * env;
    color += m.emissive.rgb * m.emissive.w;
    float fog = 1.0 - exp(-u.ground_color.w * length(u.camera_pos.xyz - v_world));
    color = mix(color, u.fog_color.rgb * ambient, fog);
#if defined(LX_BLEND)
    float a = clamp(alpha, 0.0, 1.0);
    if ((flags & MF_ADDITIVE) != 0) o_color = vec4(color * a, 0.0);   // adds light, hides nothing
    else if ((flags & MF_GLASS) != 0) {
        // glass: the body fades with alpha, the reflection stays (and hides more of the view at grazing angles)
        vec3 shine = spec_weight * env * (1.0 - fog);
        vec3 body = max(color - shine, vec3(0.0));
        float f = clamp(dot(spec_weight, vec3(0.3333)), 0.0, 1.0) * (1.0 - fog);
        o_color = vec4(body * a + shine, clamp(a + (1.0 - a) * f, 0.0, 1.0));
    } else o_color = vec4(color * a, a);
#else
    o_color = vec4(color, 1.0);
#endif
#if LX_MRT
    o_normal = vec4(n * 0.5 + 0.5, 1.0);
    o_spec = vec4(spec_weight * (1.0 - fog), rough);
    o_ambient = vec4((ambient_diffuse + spec_weight * env * 0.5) * (1.0 - fog), 1.0);
#endif
}
