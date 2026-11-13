#version 450
// Pre-filters one face of one mip of a reflection probe: GGX importance sampling of the levels
// above it, so mip m holds the environment as seen by a surface of roughness m / (mips - 1).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform samplerCube u_src;   // view of mips [0, m)
layout(push_constant) uniform PC { vec4 p; } pc;          // x face, y roughness, z mip 0 size, w highest readable lod

const float PI = 3.14159265;

vec3 face_dir(int f, vec2 uv) {
    float u = uv.x * 2.0 - 1.0, v = uv.y * 2.0 - 1.0;
    if (f == 0) return vec3(1.0, -v, -u);
    if (f == 1) return vec3(-1.0, -v, u);
    if (f == 2) return vec3(u, 1.0, v);
    if (f == 3) return vec3(u, -1.0, -v);
    if (f == 4) return vec3(u, -v, 1.0);
    return vec3(-u, -v, -1.0);
}

vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), float(bitfieldReverse(i)) * 2.3283064365386963e-10); }

void main() {
    vec3 n = normalize(face_dir(int(pc.p.x), v_uv));
    float a = max(pc.p.y * pc.p.y, 1e-3);
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    float texel_sa = 4.0 * PI / (6.0 * pc.p.z * pc.p.z);
    const uint N = 32u;
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    for (uint i = 0u; i < N; i++) {
        vec2 xi = hammersley(i, N);
        float phi = 2.0 * PI * xi.x;
        float cos_t = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
        float sin_t = sqrt(1.0 - cos_t * cos_t);
        vec3 h = tx * (cos(phi) * sin_t) + ty * (sin(phi) * sin_t) + n * cos_t;
        vec3 l = 2.0 * dot(n, h) * h - n;
        float ndl = dot(n, l);
        if (ndl <= 0.0) continue;
        // read from a blurrier level when each sample covers many texels (avoids fireflies)
        float d = (cos_t * cos_t * (a * a - 1.0) + 1.0);
        float pdf = (a * a / (PI * d * d)) * 0.25;
        float sample_sa = 1.0 / (float(N) * pdf + 1e-4);
        float lod = clamp(0.5 * log2(sample_sa / texel_sa) + 1.0, 0.0, pc.p.w);
        sum += textureLod(u_src, l, lod).rgb * ndl;
        wsum += ndl;
    }
    o_color = vec4(sum / max(wsum, 1e-4), 1.0);
}
