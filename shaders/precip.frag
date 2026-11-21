#version 450
#pragma lx_common
layout(location = 0) in vec2 v_q;
layout(location = 1) in vec3 v_center;
layout(location = 2) in float v_fade;
layout(location = 3) flat in float v_snow;
layout(location = 0) out vec4 o_color;
void main() {
    // nothing falls under a roof: the precipitation map sees the first surface from above
    if (u.weather_b.w > 0.5) {
        vec4 c = u.precip_vp * vec4(v_center, 1.0);
        vec3 ndc = c.xyz / c.w;
        vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        float depth = ndc.z * 0.5 + 0.5 - 0.002;
        if (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0))) && depth < 1.0 &&
            texture(u_shadow, vec4(uv, float(u.counts.w), depth)) < 0.5) discard;
    }
    bool snow = v_snow > 0.5;
    float a = snow ? smoothstep(1.0, 0.2, length(v_q)) * 0.85
                   : (1.0 - abs(v_q.x)) * smoothstep(1.0, 0.4, abs(v_q.y)) * 0.32;
    a *= v_fade * clamp((snow ? u.weather_a.y : u.weather_a.x) * 1.5 + 0.2, 0.0, 1.0);
    if (a < 0.003) discard;
    // lit like a small bright object: the sky all round, a little sun, and lightning
    vec3 sky = mix(u.sky_horizon.rgb, u.sky_zenith.rgb, 0.5) * u.sky_zenith.w * 3.14159;
    vec3 light = sky * (snow ? 0.32 : 0.45) + u.sun_color.rgb * (snow ? 0.22 : 0.06) * u.sun_color.w;
    light += vec3(0.7, 0.75, 0.9) * u.sky_misc.z * 1.5;
    o_color = vec4(light * (snow ? vec3(1.0) : vec3(0.85, 0.9, 1.0)), a);
}
