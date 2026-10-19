#version 450

// Cook-Torrance metallic-roughness PBR for dielectrics (metallic = 0, F0 = 0.04),
// sRGB decode on input, Narkowicz ACES tone map + sRGB encode on output.
//
// Why dielectric-only: the vertex stream carries a single roughness float per
// vertex (see GpuVertex), so per-material metallic cannot reach this stage
// without a vertex-format change. A dielectric Cook-Torrance BRDF is still a
// strict improvement over the old roughness->shininess Blinn-Phong hack: the
// GGX/Smith/Schlick terms conserve energy by construction (the specular lobe
// integrates to <= 1 for every roughness), and the diffuse lobe is weighted
// by (1 - F) so light is never created at grazing angles.
//
// Interface is unchanged: same varyings, same set/binding layout, same two
// outputs. FragmentUniforms only grows one trailing vec4 (tone_params), so
// old uniform data stays prefix-compatible.

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_color;
layout(location = 3) in vec3 v_view_position;
layout(location = 5) in vec2 v_uv;
layout(location = 6) in float v_roughness;

layout(set = 2, binding = 0) uniform sampler2D albedo;

layout(set = 3, binding = 0) uniform FragmentUniforms {
    vec4 light_direction;
    vec4 light_color;    // rgb + intensity in .a
    vec4 ambient_color;
    vec4 camera_position;
    vec4 tone_params;    // x: exposure; yzw reserved
} u;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_linear_depth;

const float PI = 3.14159265;
// Base reflectance of a dielectric at normal incidence (plastic/glass ~0.04).
const vec3 DIELECTRIC_F0 = vec3(0.04);
// Floor on alpha = roughness^2: at roughness 0 the GGX denominator is 0/0.
// Mirrors lotus::pbr::kMinAlpha in include/lotus/gpu/renderer.hpp.
const float MIN_ALPHA = 0.001;

// Authored albedo (color-picker values, texture texels) is sRGB-encoded;
// lighting math runs in linear, so decode on the way in.
vec3 srgb_to_linear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 linear_to_srgb(vec3 c)
{
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}

// Trowbridge-Reitz GGX normal distribution.
float ndf_ggx(float n_dot_h, float roughness)
{
    float a     = max(roughness * roughness, MIN_ALPHA);
    float a2    = a * a;
    float denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

// Schlick-GGX approximation of the Smith geometry term (direct lighting).
float geometry_schlick_ggx(float n_dot, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return n_dot / (n_dot * (1.0 - k) + k);
}

vec3 fresnel_schlick(float cos_theta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Narkowicz ACES filmic fit: cheap, monotonic, bounded to [0, 1].
vec3 aces_tone_map(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

void main()
{
    vec3 n = normalize(v_normal);
    vec3 l = normalize(u.light_direction.xyz);
    vec3 v = normalize(u.camera_position.xyz - v_world_position);
    vec3 h = normalize(l + v);

    float n_dot_l = max(dot(n, l), 0.0);
    float n_dot_v = max(dot(n, v), 0.0);
    float rough   = clamp(v_roughness, 0.0, 1.0);

    vec3 albedo   = srgb_to_linear(v_color * texture(albedo, v_uv).rgb);
    vec3 radiance = u.light_color.rgb * u.light_color.a;

    // Ambient is a linear-space floor so unlit faces never go pure black.
    vec3 outgoing = u.ambient_color.rgb * albedo;
    if (n_dot_l > 0.0 && n_dot_v > 0.0) {
        float n_dot_h = max(dot(n, h), 0.0);
        float h_dot_v = max(dot(h, v), 0.0);

        float d = ndf_ggx(n_dot_h, rough);
        float g = geometry_schlick_ggx(n_dot_l, rough) *
                  geometry_schlick_ggx(n_dot_v, rough);
        vec3 f = fresnel_schlick(h_dot_v, DIELECTRIC_F0);

        // Energy-conserving split: what Fresnel reflects cannot also diffuse.
        vec3 specular = (d * g / max(4.0 * n_dot_l * n_dot_v, 0.001)) * f;
        vec3 diffuse  = (vec3(1.0) - f) * albedo / PI;
        outgoing += (diffuse + specular) * radiance * n_dot_l;
    }

    vec3 mapped = aces_tone_map(outgoing * max(u.tone_params.x, 0.0));
    out_color   = vec4(linear_to_srgb(mapped), 1.0);

    out_linear_depth = vec4(-v_view_position.z, 0.0, 0.0, 1.0);
}
