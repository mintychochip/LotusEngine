#version 450

// Screen-space ambient occlusion from the linear-depth MRT (location 1 of the
// lit pass, R32F view depth in world units). 12 taps on two rings plus the
// center: depth differences that exceed a slope-aware tolerance count as
// occluded. Output is single-channel AO in [0, 1]; the lit pass samples it
// and scales ONLY ambient/hemisphere/bounce, never the sun.
//
// DEPTH_FAR (1.0e9, see lotus::kDepthFar) is the "nothing here" sentinel the
// clear color and the sky dome write: taps reading it never occlude, so the
// sky and empty pixels stay at AO 1. Quarter-res rendering is allowed: UVs
// are resolution-independent, and the lit pass samples with gl_FragCoord over
// its own size.

// Must match lotus::kDepthFar (linear-depth "nothing here" sentinel).
const float DEPTH_FAR = 1.0e9;

layout(location = 0) in vec2 v_uv;

layout(set = 2, binding = 0) uniform sampler2D depth_map;

layout(set = 3, binding = 0) uniform SsaoUniforms {
    vec4 texel; // xy: 1/width, 1/height; zw reserved
    vec4 params; // x: radius (UV units); y: bias (depth units);
                 // z: strength; w: reserved
} u;

layout(location = 0) out vec4 out_color;

void main()
{
    float center = texture(depth_map, v_uv).r;
    if (center >= DEPTH_FAR * 0.5) {
        out_color = vec4(1.0);
        return;
    }
    float radius   = max(u.params.x, 1e-6);
    float bias     = max(u.params.y, 0.0);
    float strength = clamp(u.params.z, 0.0, 2.0);

    // 12 taps: 4 inner-ring + 8 outer-ring, rotated per pixel by a cheap
    // hash so banding does not line up across the frame.
    vec2 ring4[4];
    ring4[0] = vec2(1.0, 0.0);
    ring4[1] = vec2(-1.0, 0.0);
    ring4[2] = vec2(0.0, 1.0);
    ring4[3] = vec2(0.0, -1.0);
    vec2 ring8[8];
    ring8[0] = vec2(1.0, 1.0);
    ring8[1] = vec2(-1.0, 1.0);
    ring8[2] = vec2(1.0, -1.0);
    ring8[3] = vec2(-1.0, -1.0);
    ring8[4] = vec2(1.0, 0.0);
    ring8[5] = vec2(-1.0, 0.0);
    ring8[6] = vec2(0.0, 1.0);
    ring8[7] = vec2(0.0, -1.0);

    float hash = fract(sin(dot(floor(gl_FragCoord.xy),
                                   vec2(12.9898, 78.233))) * 43758.5453);
    float ca   = cos(hash * 6.2831853);
    float sa   = sin(hash * 6.2831853);
    mat2 rot   = mat2(ca, -sa, sa, ca);

    float occ = 0.0;
    float taps = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 off = rot * ring4[i] * radius * 0.5;
        float s  = texture(depth_map, v_uv + off * u.texel.xy).r;
        if (s < DEPTH_FAR * 0.5) {
            // A nearer sample occludes: it sits between the camera and us.
            occ += (s < center - bias) ? 1.0 : 0.0;
            taps += 1.0;
        }
    }
    for (int i = 0; i < 8; ++i) {
        vec2 off = rot * ring8[i] * radius;
        float s  = texture(depth_map, v_uv + off * u.texel.xy).r;
        if (s < DEPTH_FAR * 0.5) {
            occ += (s < center - bias) ? 1.0 : 0.0;
            taps += 1.0;
        }
    }
    float ao = taps > 0.0 ? 1.0 - clamp(occ / taps * strength, 0.0, 1.0)
                          : 1.0;
    out_color = vec4(clamp(ao, 0.0, 1.0));
}
