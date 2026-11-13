// Sky. Built as sky.frag and sky_mrt.frag (also clears the SSR targets).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
#if LX_MRT
layout(location = 1) out vec4 o_normal;
layout(location = 2) out vec4 o_spec;
#endif
void main() {
    // Vulkan screen v=0 is the top; GL NDC y=+1 is the top
    vec2 ndc = vec2(v_uv.x * 2.0 - 1.0, 1.0 - v_uv.y * 2.0);
    vec4 far_p = u.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(far_p.xyz / far_p.w - u.camera_pos.xyz);
    vec3 c = lx_sky_radiance(dir);
    if (u.sun_color.w > 0.5) {
        float s = max(dot(dir, -u.sun_dir.xyz), 0.0);
        c += u.sun_color.rgb * (pow(s, 2000.0) * 40.0 + pow(s, 12.0) * 0.08);
    }
    o_color = vec4(c, 1.0);
#if LX_MRT
    o_normal = vec4(0.0);
    o_spec = vec4(0.0);   // the sky reflects nothing
#endif
}
