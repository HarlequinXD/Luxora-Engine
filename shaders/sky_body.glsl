// Sky. Built as sky.frag and sky_mrt.frag (also clears the SSR targets). Drawn at the far plane after the
// opaque geometry, so only pixels that show sky pay for it.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
#if LX_MRT
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_spec;
#endif

/* ---- Day-night: the air lit by sun and moon in one march ---- */
vec3 dn_air(vec3 d, out vec3 trans) {
    float haze = u.sky_params.z, height = u.sky_sun.w;
    const int N = 16;
    vec3 s = u.sky_sun.xyz, m = u.sky_moon.xyz;
    float Es = u.sky_params.y, Em = u.sky_moon.w;
    float r0 = LX_RG + max(height, 1.0);
    float b = r0 * d.y;
    float t_top = -b + sqrt(max(0.0, b * b - (r0 * r0 - LX_RT * LX_RT)));
    float dg = b * b - (r0 * r0 - LX_RG * LX_RG);
    float L = (d.y < 0.0 && dg > 0.0) ? -b - sqrt(dg) : t_top;
    float g2 = LX_MIE_G * LX_MIE_G, iso = LX_MS_BOOST / (4.0 * LX_SPI);
    float mus_ = dot(d, s), mum = dot(d, m);
    float prs = 3.0 / (16.0 * LX_SPI) * (1.0 + mus_ * mus_) + iso, prm = 3.0 / (16.0 * LX_SPI) * (1.0 + mum * mum) + iso;
    float pms = 3.0 / (8.0 * LX_SPI) * (1.0 - g2) * (1.0 + mus_ * mus_) / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * LX_MIE_G * mus_, 1.5)) + iso;
    float pmm = 3.0 / (8.0 * LX_SPI) * (1.0 - g2) * (1.0 + mum * mum) / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * LX_MIE_G * mum, 1.5)) + iso;
    float bms = LX_MIE_S * haze, bme = bms * 1.11;
    float odr = 0.0, odm = 0.0;
    vec3 S = vec3(0.0), M = vec3(0.0);
    bool moon = Em > 0.0;
    for (int i = 0; i < N; i++) {
        float a0 = float(i) / float(N), a1 = float(i + 1) / float(N);
        float t0 = L * a0 * a0, t1 = L * a1 * a1, dt = t1 - t0, t = 0.5 * (t0 + t1);
        vec3 p = vec3(d.x * t, r0 + d.y * t, d.z * t);
        float r = length(p), h = max(r - LX_RG, 0.0);
        vec3 up = p / r;
        float rr = exp(-h / LX_HR), rm = exp(-h / LX_HM);
        float cr = odr + rr * dt * 0.5, cm = odm + rm * dt * 0.5;
        odr += rr * dt;
        odm += rm * dt;
        float hr = h / LX_HR, hm = h / LX_HM;
        float ms = dot(up, s);
        vec3 T = exp(-(LX_BR + LX_BO) * (cr + LX_HR * lx_chapman(LX_RG / LX_HR, hr, ms)) - bme * (cm + LX_HM * lx_chapman(LX_RG / LX_HM, hm, ms)));
        S += T * (LX_BR * rr * prs + bms * rm * pms) * dt;
        if (moon) {
            float mm = dot(up, m);
            vec3 Tm = exp(-(LX_BR + LX_BO) * (cr + LX_HR * lx_chapman(LX_RG / LX_HR, hr, mm)) - bme * (cm + LX_HM * lx_chapman(LX_RG / LX_HM, hm, mm)));
            M += Tm * (LX_BR * rr * prm + bms * rm * pmm) * dt;
        }
    }
    trans = exp(-(LX_BR + LX_BO) * odr - bme * odm);
    vec3 glow = vec3(0.012, 0.016, 0.03) * (0.15 + u.sky_params.w * 0.1);   // airglow: nights are never pure black
    return (S * Es + M * Em * vec3(0.8, 0.9, 1.15)) * LX_SKY_GAIN + glow;
}

float dn_hg(float c, float g) { float g2 = g * g; return (1.0 - g2) / (4.0 * LX_SPI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5)); }

/* clouds along d: rgb the light they send to the eye, a how much of the sky behind still shows */
vec4 dn_clouds(vec3 d, float jitter) {
    int N = int(u.cloud_a.w + 0.5);
    if (N <= 0 || u.cloud_a.x <= 0.001 || d.y <= 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    float base = u.cloud_b.x, slab = u.cloud_b.y;
    float thick = slab * lx_cloud_top(1.0, u.cloud_a.z);   // as high as any cloud of this type reaches: samples go where clouds are
    float hc = u.camera_pos.y;
    if (hc > base) return vec4(0.0, 0.0, 0.0, 1.0);   // above the cloud base: the slab isn't drawn
    float r0 = LX_RG + max(hc, 0.0);
    float b = r0 * d.y;
    float rb = LX_RG + base, rt = rb + thick;
    float t0 = -b + sqrt(b * b - r0 * r0 + rb * rb);
    float t1 = -b + sqrt(b * b - r0 * r0 + rt * rt);
    const float FAR = 60000.0;
    if (t0 > FAR) return vec4(0.0, 0.0, 0.0, 1.0);
    t1 = min(min(t1, t0 + thick * 5.0), FAR);
    float dt = (t1 - t0) / float(N);
    /* the light on the clouds: the sun by day, the moon by night, through the air above */
    bool day = u.sky_sun.y > -0.06;
    vec3 ldir = day ? u.sky_sun.xyz : u.sky_moon.xyz;
    float mid = base + thick * 0.5;
    vec3 lcol = day ? u.sky_params.y * lx_sun_trans(mid, ldir.y, u.sky_params.z) * smoothstep(-0.06, 0.02, ldir.y)
                    : u.sky_moon.w * lx_sun_trans(mid, ldir.y, u.sky_params.z) * vec3(0.8, 0.9, 1.1) * smoothstep(-0.02, 0.05, ldir.y);
    float c = dot(d, ldir);
    float phase = mix(dn_hg(c, 0.6), dn_hg(c, -0.25), 0.35);
    vec3 amb = u.sky_zenith.rgb * u.sky_zenith.w;
    int LS = int(u.cloud_c.x + 0.5);
    float lstep = thick * 0.6 / float(max(LS, 1));
    float flash = u.sky_misc.z;
    vec3 flash_dir = u.bolt.xyz;
    float ks = lx_cloud_sigma();
    float T = 1.0;
    vec3 Lsum = vec3(0.0);
    float first_t = -1.0;
    for (int i = 0; i < N; i++) {
        float t = t0 + (float(i) + jitter) * dt;
        float alt = sqrt(r0 * r0 + 2.0 * r0 * t * d.y + t * t) - LX_RG;
        float h = clamp((alt - base) / slab, 0.0, 1.0);
        vec3 p = vec3(u.camera_pos.x + d.x * t, alt, u.camera_pos.z + d.z * t);
        float dens = lx_cloud_density(p, h, true);
        if (dens <= 0.002) continue;
        if (first_t < 0.0) first_t = t;
        float sigma = dens * ks;
        /* light through the cloud above this point */
        float od = 0.0;
        for (int k = 0; k < LS; k++) {
            vec3 q = p + ldir * (lstep * (float(k) + 0.5));
            float hq = clamp((q.y - base) / slab, 0.0, 1.0);
            od += lx_cloud_density(q, hq, false) * ks * lstep;
        }
        float tl = max(exp(-od), exp(-od * 0.25) * 0.7);   // light scattered many times reaches deeper
        float powder = 1.0 - exp(-sigma * 2.0 * thick * 0.25);
        float hy = clamp(h * slab / thick, 0.0, 1.0);   // how far up the clouds: bases are darker, tops catch the sky
        vec3 S = lcol * phase * tl * mix(0.6, 1.0, powder) + amb * mix(0.3, 1.0, hy);
        if (flash > 0.0) S += vec3(0.8, 0.85, 1.0) * flash * 6.0 * pow(max(dot(normalize(vec3(d.x, 0.0, d.z)), flash_dir), 0.0), 6.0) * (1.0 - hy);
        float a = 1.0 - exp(-sigma * dt);
        Lsum += T * S * a;
        T *= 1.0 - a;
        if (T < 0.02) break;
    }
    if (first_t < 0.0) return vec4(0.0, 0.0, 0.0, 1.0);
    /* far clouds take the colour of the horizon haze, but stay just as solid: what they hide stays hidden */
    float fade = exp(-first_t / 28000.0) * smoothstep(0.0, 0.04, d.y);
    vec3 haze = u.sky_horizon.rgb * u.sky_zenith.w * (1.0 - T);
    return vec4(mix(haze, Lsum, fade), T);
}

/* stars: one at most in each cell of a cube around the sky, turning about the pole through the night */
vec3 dn_stars(vec3 dir) {
    vec3 k = u.sky_pole.xyz;
    float a = u.sky_misc.w;
    vec3 d = dir * cos(a) + cross(k, dir) * sin(a) + k * dot(k, dir) * (1.0 - cos(a));
    vec3 ad = abs(d);
    vec2 uv;
    float face;
    if (ad.x > ad.y && ad.x > ad.z) { uv = d.yz / ad.x; face = d.x > 0.0 ? 0.0 : 1.0; }
    else if (ad.y > ad.z) { uv = d.xz / ad.y; face = d.y > 0.0 ? 2.0 : 3.0; }
    else { uv = d.xy / ad.z; face = d.z > 0.0 ? 4.0 : 5.0; }
    vec2 g = (uv * 0.5 + 0.5) * 220.0;
    vec2 cell = floor(g) + face * 517.0;
    float h = lx_hash12(cell);
    if (h > 0.045) return vec3(0.0);
    vec2 pos = vec2(lx_hash12(cell + 7.31), lx_hash12(cell + 19.17)) * 0.7 + 0.15;
    float dist = length(fract(g) - pos);
    float mag = pow(lx_hash12(cell + 3.71), 8.0);
    float b = smoothstep(0.16 + mag * 0.1, 0.0, dist) * (0.06 + 2.5 * mag);
    float tw = 0.7 + 0.3 * sin(u.camera_pos.w * (2.0 + h * 60.0) + h * 400.0);
    float temp = lx_hash12(cell + 11.3);
    vec3 col = mix(vec3(1.0, 0.82, 0.65), vec3(0.7, 0.8, 1.0), temp);
    return col * b * tw;
}

/* the moon's disc: lit on the side facing the sun, so it shows its phase */
vec3 dn_moon(vec3 dir) {
    vec3 m = u.sky_moon.xyz;
    const float R = 0.0095;
    float cd = dot(dir, m);
    if (cd < cos(R * 1.6)) return vec3(0.0);
    vec3 right = normalize(cross(m, abs(m.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 upv = cross(right, m);
    vec2 q = vec2(dot(dir, right), dot(dir, upv)) / R;
    float r2 = dot(q, q);
    vec3 halo = vec3(0.6, 0.7, 0.9) * 0.02 * smoothstep(1.6, 1.0, sqrt(r2));
    if (r2 >= 1.0) return halo * u.sky_misc.y;
    vec3 n = q.x * right + q.y * upv - sqrt(1.0 - r2) * m;
    float lit = clamp(dot(n, u.sky_sun.xyz) * 4.0 + 0.1, 0.0, 1.0);
    float maria = lx_vnoise2(q * 3.0 + 4.0) * 0.6 + lx_vnoise2(q * 9.0) * 0.4;
    float alb = mix(0.55, 1.0, smoothstep(0.35, 0.65, maria));
    float edge = smoothstep(1.0, 0.92, sqrt(r2));
    return (vec3(1.0, 0.97, 0.92) * 2.2 * alb * lit + vec3(0.02, 0.025, 0.035)) * edge + halo;
}

/* a lightning bolt under the clouds, towards bolt.xyz */
vec3 dn_bolt(vec3 dir) {
    float age = u.bolt.w;
    if (age > 0.4) return vec3(0.0);
    float on = age < 0.06 ? 1.0 : age < 0.12 ? 0.15 : age < 0.2 ? 0.9 : age < 0.26 ? 0.1 : age < 0.32 ? 0.6 : 0.0;
    if (on <= 0.0) return vec3(0.0);
    vec3 fl = normalize(vec3(dir.x, 0.0, dir.z));
    float az = atan(dot(cross(u.bolt.xyz, fl), vec3(0.0, 1.0, 0.0)), dot(u.bolt.xyz, fl));
    float el = asin(clamp(dir.y, -1.0, 1.0));
    float top = atan(u.cloud_b.x / max(u.bolt_b.y, 100.0));
    if (el < -0.005 || el > top || abs(az) > 0.25) return vec3(0.0);
    float k = el / max(top, 1e-3);
    float seed = u.bolt_b.x;
    float x = 0.0, amp = 0.05 * top;
    for (int i = 0; i < 4; i++) {
        float f = exp2(float(i)) * 6.0;
        x += (lx_vnoise2(vec2(k * f, seed + float(i) * 13.0)) - 0.5) * amp;
        amp *= 0.55;
    }
    float w = 0.0012 + 0.0006 * k;
    float core = exp(-pow((az - x) / w, 2.0));
    float glow = exp(-abs(az - x) / (w * 12.0)) * 0.15;
    return vec3(0.75, 0.82, 1.0) * (core * 40.0 + glow * 6.0) * on;
}

vec3 dn_sky(vec3 dir) {
    vec3 trans;
    vec3 c = dn_air(dir, trans);
    if (dir.y < 0.0) c += u.ground_color.rgb * trans;   // the ground towards the horizon
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec4 cl = dn_clouds(dir, jitter);
    /* what lies beyond the air: stars, the moon, the sun's disc (behind the clouds) */
    vec3 space = vec3(0.0);
    if (dir.y > -0.02) {
        space += dn_stars(dir) * u.sky_params.w * u.sky_misc.y;
        space += dn_moon(dir);
        float sc = dot(dir, u.sky_sun.xyz);
        const float SR = 0.0093;
        if (sc > cos(SR)) {
            float x = acos(clamp(sc, -1.0, 1.0)) / SR;
            float limb = 1.0 - 0.6 * (1.0 - sqrt(max(1.0 - x * x, 0.0)));
            space += vec3(1.0) * 60.0 * limb * (u.sky_params.y / 3.5);
        }
    }
    c += space * trans * cl.a * cl.a;   // a thin veil of cloud already hides a disc's edge
    c = c * cl.a + cl.rgb;
    c += dn_bolt(dir) * smoothstep(-0.01, 0.0, dir.y);
    c += vec3(0.7, 0.75, 0.9) * u.sky_misc.z * 0.25 * (1.0 - cl.a * 0.7);   // the whole sky brightens in a flash
    return c;
}

void main() {
    // Vulkan screen v=0 is the top; GL NDC y=+1 is the top
    vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0);
    vec4 far_p = u.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(far_p.xyz / far_p.w - u.camera_pos.xyz);
    vec3 c;
    if (lx_day_night()) c = dn_sky(dir);
    else {
        c = lx_sky_radiance(dir);
        if (u.sun_color.w > 0.5) {
            float s = max(dot(dir, -u.sun_dir.xyz), 0.0);
            c += u.sun_color.rgb * (pow(s, 2000.0) * 40.0 + pow(s, 12.0) * 0.08);
        }
    }
    if (lx_day_night() && u.weather_b.z > 0.0) {   // mist hides the horizon
        float k = clamp(u.weather_b.z * 1.2, 0.0, 1.0) * (1.0 - smoothstep(-0.05, 0.25 + u.weather_b.z * 0.4, dir.y));
        c = mix(c, u.fog_color.rgb * u.sky_zenith.w, k);
    }
    o_color = vec4(c, 1.0);
#if LX_MRT
    o_normal = vec4(0.0);
    o_spec = vec4(0.0);   // the sky reflects nothing
#endif
}
