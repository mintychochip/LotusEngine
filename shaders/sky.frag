#version 450

// Procedural sky background: zenith->horizon gradient from sky_color
// (darkened upward), warm horizon glow around the sun azimuth, sun disk plus
// a small halo from dot(view_dir, sun); below-horizon pixels ease into
// ground_color so the dome meets the ground plane without a hard line.
//
// Exposure plus the Narkowicz ACES fit and sRGB encode mirror lit.frag, so
// the dome meets lit geometry without a tone break. Location 1 writes the
// far sentinel, so the occlusion-pyramid min reduction ignores sky pixels --
// the same contract the clear colour it replaces upholds.

layout(location = 0) in vec3 v_world_position;

layout(set = 3, binding = 0) uniform SkyFragmentUniforms {
    vec4 camera_position; // xyz
    vec4 sky_color;       // rgb, linear
    vec4 ground_color;    // rgb, linear
    vec4 sun_direction;   // xyz, unit, toward the sun
    vec4 sun_color;       // rgb + intensity in .a
    vec4 params;          // x: exposure; y: horizon_warmth; zw reserved
} u;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_linear_depth;

// Must match lotus::kDepthFar (linear-depth "nothing here" sentinel).
const float DEPTH_FAR = 1.0e9;

vec3 linear_to_srgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}

// Narkowicz ACES filmic fit: cheap, monotonic, bounded to [0, 1].
vec3 aces_tone_map(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

void main()
{
    vec3 dir = v_world_position - u.camera_position.xyz;
    dir /= max(length(dir), 1e-6);
    vec3 sun    = normalize(u.sun_direction.xyz);
    float sun_dot = dot(dir, sun);

    // Zenith (darkened sky_color) -> horizon (sky_color) gradient.
    vec3 horizon = u.sky_color.rgb;
    vec3 zenith  = u.sky_color.rgb * 0.28;
    float up     = clamp(dir.y, 0.0, 1.0);
    vec3 sky     = mix(horizon, zenith, pow(up, 0.55));

    // Warm glow hugging the horizon around the sun's azimuth, scaled by
    // horizon_warmth (0 disables it, 1 is neutral).
    vec2 flat_dir = dir.xz;
    vec2 flat_sun = sun.xz;
    // Guard the poles (and an overhead sun), where xz degenerates.
    float flat_len = max(length(flat_dir) * length(flat_sun), 1e-4);
    float azimuth  = max(dot(flat_dir, flat_sun) / flat_len, 0.0);
    float band     = exp(-abs(dir.y) * 5.0);
    sky += u.sun_color.rgb * (pow(azimuth, 3.0) * band * 0.35 * u.params.y);

    // Sun disk via smoothstep on the sun cosine, plus a tight halo and a
    // broad glow that also fades with horizon_warmth.
    float disk = smoothstep(0.99988, 0.99996, sun_dot);
    float halo = pow(max(sun_dot, 0.0), 700.0) * 0.6 +
                 pow(max(sun_dot, 0.0), 24.0) * 0.10 *
                         (0.5 + 0.5 * u.params.y);
    sky += u.sun_color.rgb * u.sun_color.a * (disk * 4.0 + halo);

    // Below the horizon, ease into ground_color so the dome meets the ground
    // plane without a hard line.
    float down = clamp(-dir.y, 0.0, 1.0);
    sky        = mix(sky, u.ground_color.rgb, smoothstep(0.0, 0.22, down));

    vec3 mapped      = aces_tone_map(sky * max(u.params.x, 0.0));
    out_color        = vec4(linear_to_srgb(mapped), 1.0);
    out_linear_depth = vec4(DEPTH_FAR, DEPTH_FAR, DEPTH_FAR, DEPTH_FAR);
}
