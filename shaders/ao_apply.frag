#version 450
// Ambient occlusion applied (full resolution, added to the picture): a depth-aware blur of the half-resolution
// occlusion, taken off the ambient light only (bounce and sky light), never off the sun or lamps.
#pragma lx_common
#pragma lx_include post_common.glsl
layout(location = 0) out vec4 o_color;
// u_in0 occlusion (half res), u_in1 depth, u_in2 ambient light; pc.a: x strength, yz half-res texel size

void main() {
    float d = textureLod(u_in1, v_uv, 0.0).r;
    if (d >= 1.0) { o_color = vec4(0.0); return; }
    vec3 cam = u.camera_pos.xyz;
    float dc = length(post_world(v_uv, d) - cam);
    float tol = dc * 0.03 + 0.05;
    float sum = 0.0, wsum = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 uv = v_uv + vec2(float(x), float(y)) * pc.a.yz;
            float sd = textureLod(u_in1, uv, 0.0).r;
            if (sd >= 1.0) continue;
            float ds = length(post_world(uv, sd) - cam);
            float w = exp(-abs(ds - dc) / tol) * (x == 0 && y == 0 ? 2.0 : 1.0);
            sum += textureLod(u_in0, uv, 0.0).r * w;
            wsum += w;
        }
    float ao = wsum > 0.0 ? sum / wsum : 1.0;
    float occ = 1.0 - pow(clamp(ao, 0.0, 1.0), max(pc.a.x, 0.0));
    vec3 amb = textureLod(u_in2, v_uv, 0.0).rgb;
    o_color = vec4(-amb * occ, 0.0);   // added: takes that much ambient light away
}
