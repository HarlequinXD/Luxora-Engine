#version 450
// Screen-space reflections, traced at half resolution: march the reflected ray across the depth
// buffer in screen space (perspective-correct), refine the hit with a binary search, and fetch
// the lit colour there. Output is premultiplied by confidence so it can be filtered and blended.
#pragma lx_common
#pragma lx_include ssr_common.glsl
const float PI = 3.14159265;
#pragma lx_include gi_common.glsl
layout(set = 1, binding = 0) uniform sampler2D u_hdr;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_ssr;

float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

/* ---- reflection cards: each nearby object's six sides, small snapshots in an atlas (src/gfx/cards.c) ----
 * Where the screen has nothing for a ray (off the edge, behind the camera, hidden behind something), the ray walks
 * a coarse grid of the objects around the camera and marches each one's box: a point is inside the object where
 * every axis's front and back cards say so (its visual hull). The hit takes the colour of the card that sees it best
 * and is lit by the sun (with its shadows) and the Live GI. */
layout(set = 3, binding = 0, std430) readonly buffer CardInst { vec4 grid_origin; ivec4 grid_res; vec4 inst[]; } ci;   // per object: 3 rows world -> box 0..1, then (first tile, ...)
layout(set = 3, binding = 1, std430) readonly buffer CardCells { uvec2 cells[]; } cc;   // per grid cell: first index, count
layout(set = 3, binding = 2, std430) readonly buffer CardIdx { uint idx[]; } cx;
layout(set = 3, binding = 3) uniform sampler2D u_card_albedo;
layout(set = 3, binding = 4) uniform sampler2D u_card_depth;
const int CARD_RES = 32, CARD_ROW = 64;
const float CARD_SLACK = 1.0 / 32.0;

vec2 card_uv(int a, vec3 q) { return a == 0 ? q.zy : (a == 1 ? q.xz : q.xy); }
ivec2 card_texel(int tile, vec2 uv) {
    return ivec2((tile % CARD_ROW) * CARD_RES, (tile / CARD_ROW) * CARD_RES) + ivec2(clamp(uv * float(CARD_RES), vec2(0.0), vec2(CARD_RES - 1)));
}
float card_depth(int tile, vec2 uv) {
    float v = texelFetch(u_card_depth, card_texel(tile, uv), 0).r;
    return v > 0.999 ? 2.0 : v * (255.0 / 254.0);
}
bool card_inside(int base, vec3 q, out int face) {
    float best = 1e9;
    face = 0;
    for (int a = 0; a < 3; a++) {
        vec2 uv = card_uv(a, q);
        float hi = 1.0 - card_depth(base + a * 2, uv) + CARD_SLACK, lo = card_depth(base + a * 2 + 1, uv) - CARD_SLACK;
        float qa = q[a];
        if (qa > hi || qa < lo) return false;
        if (hi - qa < best) { best = hi - qa; face = a * 2; }
        if (qa - lo < best) { best = qa - lo; face = a * 2 + 1; }
    }
    return true;
}
// one object: the distance along the ray to it (world units, < best), or -1
float card_trace_one(int k, vec3 o, vec3 r, float best, out int face) {
    vec4 r0 = ci.inst[k * 4], r1 = ci.inst[k * 4 + 1], r2 = ci.inst[k * 4 + 2];
    int base = int(ci.inst[k * 4 + 3].x);
    vec3 oq = vec3(dot(r0.xyz, o) + r0.w, dot(r1.xyz, o) + r1.w, dot(r2.xyz, o) + r2.w);
    vec3 dq = vec3(dot(r0.xyz, r), dot(r1.xyz, r), dot(r2.xyz, r));
    vec3 inv = 1.0 / mix(dq, vec3(1e-8), lessThan(abs(dq), vec3(1e-8)));
    vec3 ta = -oq * inv, tb = (1.0 - oq) * inv;
    vec3 tn = min(ta, tb), tf = max(ta, tb);
    float t0 = max(max(tn.x, tn.y), max(tn.z, 0.0)), t1 = min(min(tf.x, tf.y), min(tf.z, best));
    face = 0;
    if (t0 >= t1) return -1.0;
    int f;
    if (t0 <= 0.0 && card_inside(base, oq, f)) return -1.0;   // the ray starts inside this object (it's the reflecting one)
    const int N = 14;
    float prev = t0;
    for (int i = 0; i <= N; i++) {
        float t = t0 + (t1 - t0) * float(i) / float(N);
        if (card_inside(base, oq + dq * t, f)) {
            float lo = prev, hi = t;
            for (int b = 0; b < 5; b++) {
                float mid = 0.5 * (lo + hi);
                if (card_inside(base, oq + dq * mid, f)) hi = mid; else lo = mid;
            }
            card_inside(base, oq + dq * hi, face);
            return hi;
        }
        prev = t;
    }
    return -1.0;
}
float card_shadow(vec3 world, vec3 n) {
    if (u.sun_dir.w < 0.5 || u.counts.y == 0) return 1.0;
    float vd = -(u.view * vec4(world, 1.0)).z;
    if (vd > u.cascade_splits[u.counts.y - 1]) return 1.0;
    int cascade = u.counts.y - 1;
    for (int i = 0; i < u.counts.y; i++)
        if (vd < u.cascade_splits[i]) { cascade = i; break; }
    vec4 c = u.cascade_vp[cascade] * vec4(world + n * (0.05 + 0.05 * float(cascade)), 1.0);
    vec3 ndc = c.xyz / c.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    float depth = ndc.z * 0.5 + 0.5 - 0.001;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || depth > 1.0) return 1.0;
    return texture(u_shadow, vec4(uv, float(cascade), depth));
}
// the reflected ray through the objects around the camera: lit colour (rgb) and how much it found (a)
vec4 card_trace(vec3 o, vec3 r) {
    int count = ci.grid_res.w;
    if (count <= 0) return vec4(0.0);
    ivec3 res = ci.grid_res.xyz;
    float cell = ci.grid_origin.w;
    vec3 g = (o - ci.grid_origin.xyz) / cell;
    ivec3 c = ivec3(floor(g));
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, res))) return vec4(0.0);
    ivec3 stp = ivec3(sign(r));
    vec3 rr = mix(r, vec3(1e-6), lessThan(abs(r), vec3(1e-6)));
    vec3 tdelta = abs(cell / rr);
    vec3 next = ((vec3(c) + max(vec3(stp), 0.0)) * cell + ci.grid_origin.xyz - o) / rr;
    next = mix(next, vec3(1e30), equal(stp, ivec3(0)));
    float best = 1e30;
    int best_k = -1, best_face = 0, tests = 0;
    const int MAX_CELLS = 28, MAX_TESTS = 40;
    float reach = float(MAX_CELLS) * cell;
    for (int i = 0; i < MAX_CELLS; i++) {
        float exit_t = min(min(next.x, next.y), next.z);
        uvec2 cl = cc.cells[(c.y * res.z + c.z) * res.x + c.x];
        for (uint j = 0u; j < cl.y && tests < MAX_TESTS; j++, tests++) {
            int k = int(cx.idx[cl.x + j]);
            int f;
            float t = card_trace_one(k, o, r, best, f);
            if (t >= 0.0 && t < best) { best = t; best_k = k; best_face = f; }
        }
        if (best <= exit_t || tests >= MAX_TESTS) break;
        if (next.x <= next.y && next.x <= next.z) { c.x += stp.x; next.x += tdelta.x; }
        else if (next.y <= next.z) { c.y += stp.y; next.y += tdelta.y; }
        else { c.z += stp.z; next.z += tdelta.z; }
        if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, res))) break;
    }
    if (best_k < 0) return vec4(0.0);
    vec3 hit = o + r * best;
    vec4 rows[3] = vec4[3](ci.inst[best_k * 4], ci.inst[best_k * 4 + 1], ci.inst[best_k * 4 + 2]);
    int base = int(ci.inst[best_k * 4 + 3].x);
    int a = best_face >> 1;
    vec3 q = vec3(dot(rows[0].xyz, hit) + rows[0].w, dot(rows[1].xyz, hit) + rows[1].w, dot(rows[2].xyz, hit) + rows[2].w);
    vec3 albedo = texelFetch(u_card_albedo, card_texel(base + best_face, card_uv(a, q)), 0).rgb;
    vec3 n = normalize(rows[a].xyz) * ((best_face & 1) == 0 ? 1.0 : -1.0);   // the side the card looks at
    if (dot(n, r) > 0.0) n = -n;
    vec3 light = vec3(0.0);
    if (u.sun_color.w > 0.5) {
        float sh = card_shadow(hit, n);
        if (lx_day_night()) sh *= lx_cloud_shadow(hit, -u.sun_dir.xyz);
        light += u.sun_color.rgb * max(dot(n, -u.sun_dir.xyz), 0.0) * sh;
    }
    vec3 sky_ambient = mix(u.ground_color.rgb, u.sky_zenith.rgb, n.y * 0.5 + 0.5) * u.sky_zenith.w * PI;
    float gi_cover = 0.0;
    if (u.gi_origin.w > 0.5) {
        vec3 gg = (hit - u.gi_origin.xyz) / u.gi_cell.xyz;
        vec3 edge = min(gg, u.gi_res.xyz - gg);
        gi_cover = clamp(min(min(edge.x, edge.y), edge.z) - 0.5, 0.0, 1.0);
    }
    vec3 irr = gi_cover > 0.0 ? mix(sky_ambient, gi_eval(gi_gather(hit, n), n), gi_cover) : sky_ambient;
    light += irr;
    float fade = 1.0 - smoothstep(reach * 0.7, reach, best);
    return vec4(albedo / PI * light, fade);
}

// screen-space part: premultiplied colour and confidence
vec4 screen_trace(vec3 p, vec3 p0, vec3 r, float rough, float facing) {
    float len = pc.a.x;
    float near = u.proj[3][2] / (u.proj[2][2] - 1.0);
    float vz0 = (u.view * vec4(p0, 1.0)).z, vzr = (mat3(u.view) * r).z;
    if (vzr > 1e-4) len = min(len, max((-near * 1.5 - vz0) / vzr, 0.0));  // stay in front of the camera
    if (len < 0.05 || facing <= 0.0) return vec4(0.0);
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
        // steps start fine and grow with distance: precise contact reflections, long reach
        float f = (float(i) - jitter * 0.95) / float(steps);
        float t = tmax * f * mix(f, 1.0, 0.35);
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
    if (hit_t < 0.0) return vec4(0.0);
    vec2 s = mix(s0, s1, hit_t);
    vec3 nh = textureLod(u_gnormal, s, 0.0).xyz * 2.0 - 1.0;
    if (dot(nh, r) > 0.2) return vec4(0.0);                           // hit the back of something
    float edge = smoothstep(0.0, 0.07, min(min(s.x, 1.0 - s.x), min(s.y, 1.0 - s.y)));
    float travel = 1.0 - smoothstep(0.65, 1.0, hit_t);
    float conf = edge * travel * facing;
    return vec4(textureLod(u_hdr, s, 0.0).rgb * conf, conf);
}

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
    float dist = length(p - u.camera_pos.xyz);
    vec3 p0 = p + n * (0.01 + 0.002 * dist);
    float gloss = 1.0 - smoothstep(pc.a.w * 0.6, pc.a.w, rough);

    vec4 s = screen_trace(p, p0, r, rough, facing);
    vec3 c = s.a > 1e-4 ? s.rgb / s.a : vec3(0.0);
    float conf = s.a;
    if (conf < 0.99) {   // what the screen couldn't show: the reflection cards
        vec4 k = card_trace(p0, r);
        if (k.a > 0.0) {
            float add = k.a * (1.0 - conf);
            c = (c * conf + k.rgb * add) / (conf + add);
            conf += add;
        }
    }
    if (conf <= 0.0) return;
    conf *= gloss;
    // firefly guard: a ray landing on a tiny, extremely bright highlight (the sun on chrome) must not
    // become a sparkle on a glossy floor; rougher surfaces are clamped harder
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float cap = mix(32.0, 4.0, clamp(rough / max(pc.a.w, 0.05), 0.0, 1.0));
    c *= min(1.0, cap / max(lum, 1e-4));
    o_ssr = vec4(c * conf, conf);
}
