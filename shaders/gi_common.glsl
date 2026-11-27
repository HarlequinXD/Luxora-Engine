// Live GI lookups shared by the lit surfaces (pbr_body) and the reflection tracer (ssr_trace).
// Needs PI defined before it.
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
/* Distance-field occlusion: the probes are half a metre apart, so on their own they miss the light blocked right
 * next to a surface (the foot of a wall, under a character, a corner). A few steps out along the normal through the
 * camera level's distance field show how much is close by: the nearer, the darker. */
float gi_dfao(vec3 p, vec3 n) {
    int level = -1;
    for (int l = 0; l < 2 && level < 0; l++) {
        if (u.gi_casc[l].w <= 0.0) continue;
        vec3 g = (p - u.gi_casc[l].xyz) / u.gi_casc[l].w;
        float r = u.gi_casc_res.x;
        if (all(greaterThan(g, vec3(1.5))) && all(lessThan(g, vec3(r - 1.5)))) level = l;
    }
    if (level < 0) return 1.0;
    float cell = u.gi_casc[level].w, half_cell = cell * 0.5, r2 = u.gi_casc_res.x * 2.0;
    float occ = 0.0, wsum = 0.0;
    for (int i = 1; i <= 4; i++) {
        float d = half_cell * (0.6 + 0.9 * float(i));
        vec3 g = (p + n * d - u.gi_casc[level].xyz) / half_cell;   // in voxels
        g = clamp(g, vec3(0.5), vec3(r2 - 0.5));
        float s = texture(u_gi_sdf, vec3(g.xy / r2, (g.z + float(level) * r2) / (2.0 * r2))).r * half_cell;
        float w = 1.0 / float(i);
        occ += w * clamp((d - s - 0.4 * half_cell) / d, 0.0, 1.0);   // a little slack: the field is only half a cell fine
        wsum += w;
    }
    return clamp(1.0 - 0.7 * occ / wsum, 0.3, 1.0);
}
vec3 gi_eval(GiSH sh, vec3 n) {
    vec3 e = vec3(gi_eval_l1(sh.r, n), gi_eval_l1(sh.g, n), gi_eval_l1(sh.b, n)) * u.fog_color.w;
    return (any(isnan(e)) || any(isinf(e))) ? vec3(0.0) : e;   // a broken GI cell must not blacken the screen
}

/* The automatic reflection probe is one picture from one spot near the camera. Away from that spot it can show
 * light that isn't there (a lamp it was captured beside, reflected in a dark field outside the window). Compare the
 * GI arriving along the reflection here with what arrived at the capture spot: where less light comes from that
 * direction here, the probe is dimmed to match (never brightened). Close to the spot the probe is trusted as is. */
float lx_probe0_scale(vec3 p, vec3 r, GiSH here, float cover) {
    if (u.refl_params.z < 0.5 || cover <= 0.0) return 1.0;
    float far = smoothstep(1.5, 6.0, length(p - u.probe_pos[0].xyz)) * cover;
    if (far <= 0.0) return 1.0;
    const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);
    float la = dot(vec3(gi_eval_l1(here.r, r), gi_eval_l1(here.g, r), gi_eval_l1(here.b, r)), LUM);
    float lb = dot(vec3(gi_eval_l1(u.probe0_gi[0], r), gi_eval_l1(u.probe0_gi[1], r), gi_eval_l1(u.probe0_gi[2], r)), LUM);
    float ratio = clamp((la + 1e-4) / (lb + 1e-4), 0.0, 1.0);
    return mix(1.0, ratio, far);
}
