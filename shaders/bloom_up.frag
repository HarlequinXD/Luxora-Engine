#version 450
// Bloom, coming back up: the smaller level spread with a 3x3 tent and added to the level above.
#pragma lx_common
#pragma lx_include post_common.glsl
layout(location = 0) out vec4 o_color;
// u_in0 the smaller level; pc.a: xy its texel size

void main() {
    vec2 t = pc.a.xy;
    vec3 c = textureLod(u_in0, v_uv, 0.0).rgb * 4.0;
    c += (textureLod(u_in0, v_uv + vec2(-t.x, 0.0), 0.0).rgb + textureLod(u_in0, v_uv + vec2(t.x, 0.0), 0.0).rgb +
          textureLod(u_in0, v_uv + vec2(0.0, -t.y), 0.0).rgb + textureLod(u_in0, v_uv + vec2(0.0, t.y), 0.0).rgb) * 2.0;
    c += textureLod(u_in0, v_uv + vec2(-t.x, -t.y), 0.0).rgb + textureLod(u_in0, v_uv + vec2(t.x, -t.y), 0.0).rgb +
         textureLod(u_in0, v_uv + vec2(-t.x, t.y), 0.0).rgb + textureLod(u_in0, v_uv + vec2(t.x, t.y), 0.0).rgb;
    o_color = vec4(c / 16.0, 1.0);   // added to the level above
}
