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
// Interface is unchanged: same varyings, same set/binding layout for the
// albedo sampler, same two outputs (location 1 linear depth feeds the
// occlusion pyramid contract). FragmentUniforms only GAINS trailing members
// (hemi colours, shadow params, light VP, fog, bounce, blob, ssao), so old
// uniform data stays prefix-compatible. Extra samplers (shadow map binding 1,
// ssao map binding 2) bind white when their feature is off, reading as lit.
//
// Hemisphere ambient: mix(ground, sky, t) with t from world normal Y,
// divided by HEMI_REFERENCE so the default sky/ground (== the reference gray,
// == the legacy ambient default) reproduce the legacy flat floor exactly.
// The legacy `ambient` keeps multiplying the result as the floor.
//
// Directional shadows: shadow UVs compute in-frag from the uniform light-VP
// matrix (v_world_position was already available, so no vertex stream change).
// A 5x5 PCF with constant bias + normal-offset scales the direct lobe only;
// ambient is never shadowed. shadow_params.x == 0 restores the exact legacy
// unshadowed path. UV flip: the renderer uses a flipped viewport (a negative
// height), so NDC +Y lands on framebuffer row 0, which samples as v = 0.
//
// Decode-then-multiply: vertex color and texel are each sRGB-encoded, so each
// is decoded to linear first and then multiplied (decoding the product washed
// mid-tones out). Matches lotus::pbr::decode_albedo.
//
// Fog: linear-space exponential fog (1 - exp(-density * view_depth)) mixed
// between outgoing lighting and ACES, tinted by fog_color (derived per frame
// from the scene sky/sun hues, never hardcoded). fog_params.x == 0 restores
// the legacy path. The sky dome is unfogged (writes DEPTH_FAR).
//
// Bounce fill: a third directional from normalize(sun * vec3(-1,0,-1)) with a
// warm color (~sun * albedo tint, default (0.40,0.28,0.20) scaled by
// intensity). dot+clamp term modulated by the AO factor like the hemisphere,
// added to the direct lobe unshadowed (per IQ, never-AO-the-sun applies to
// the sun only; bounce is fill so no shadow map). bounce_params.x == 0
// restores the legacy path.
//
// Blob shadows: up to 4 analytic spheres (xyz center, w radius) gathered per
// frame from BlobCaster components (center from the entity Transform).
// blob_tap gives 1 far away and ~0.3 under the center with a soft edge scaled
// by the sun angular radius; the product multiplies shadow_factor() on the
// direct lobe only.
//
// SSAO: the fullscreen ssao pass samples the linear-depth MRT and writes
// single-channel AO; this stage samples it via gl_FragCoord and scales ONLY
// ambient/hemisphere/bounce (never the sun). ssao_params.x == 0 (or a white
// binding) restores the legacy path.

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_color;
layout(location = 3) in vec3 v_view_position;
layout(location = 5) in vec2 v_uv;
layout(location = 6) in float v_roughness;
layout(location = 7) in vec3 v_emissive;
layout(location = 8) in float v_metallic;

layout(set = 2, binding = 0) uniform sampler2D albedo;
layout(set = 2, binding = 1) uniform sampler2D shadow_map;
layout(set = 2, binding = 2) uniform sampler2D ssao_map;
layout(set = 2, binding = 3) uniform sampler2D normal_map;

layout(set = 3, binding = 0) uniform FragmentUniforms {
    vec4 light_direction;
    vec4 light_color;    // rgb + intensity in .a
    vec4 ambient_color;
    vec4 camera_position;
    vec4 tone_params;    // x: exposure; yzw reserved
    vec4 hemi_sky;       // rgb: sky (up) hemisphere colour
    vec4 hemi_ground;    // rgb: ground (down) hemisphere colour
    vec4 shadow_params;  // x: enabled (1/0); y: bias (shadow-depth units);
                         // z: normal offset (world units); w: shadow texel
    mat4 light_vp;       // light view-projection for shadow UVs
    vec4 fog_params;     // x: enabled (1/0); y: density; zw reserved
    vec4 fog_color;      // rgb: linear-space fog color from sky/sun hues
    vec4 bounce_color;   // rgb + intensity in .a
    vec4 bounce_params;  // x: enabled (1/0); yzw reserved
    vec4 blob_spheres[4]; // xyz: center (world); w: radius
    vec4 blob_params;    // x: count; y: sun angular radius; zw reserved
    vec4 ssao_params;    // x: enabled (1/0); yzw reserved
    vec4 ssao_texel;     // xy: 1/width, 1/height; zw reserved
    vec4 environment_params; // x: analytic environment specular intensity
    vec4 point_positions[8]; // xyz: world position; w: range
    vec4 point_colors[8];    // rgb colour; a: intensity
    vec4 point_params;       // x: active count
} u;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_linear_depth;

const float PI = 3.14159265;
// Base reflectance of a dielectric at normal incidence (plastic/glass ~0.04).
const vec3 DIELECTRIC_F0 = vec3(0.04);
// Floor on alpha = roughness^2: at roughness 0 the GGX denominator is 0/0.
// Mirrors lotus::pbr::kMinAlpha in include/lotus/gpu/renderer.hpp.
const float MIN_ALPHA = 0.001;
// Hemisphere colours are stored premultiplied by the legacy ambient floor;
// dividing by this reference gray reproduces the legacy floor exactly for
// default documents. Mirrors lotus::pbr::kHemiReference.
const vec3 HEMI_REFERENCE = vec3(0.15, 0.15, 0.18);
// Sun angular radius soft-edge scale for blob shadows.
// Mirrors lotus::pbr::kBlobSunRadius.
const float BLOB_SUN_RADIUS = 0.05;

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

// 5x5 PCF over the shadow map: 1 fully lit, 0 fully shadowed. Fragments
// outside the light frustum read as lit so a too-tight ortho box fails open,
// never into darkness.
float shadow_factor(vec3 world_position, vec3 n)
{
    if (u.shadow_params.x < 0.5) {
        return 1.0;
    }
    vec4 clip = u.light_vp * vec4(world_position + n * u.shadow_params.z, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 suv = clip.xyz / clip.w;
    // The viewport is Y-flipped: NDC +Y is row 0.
    suv.xy = vec2(suv.x * 0.5 + 0.5, 0.5 - suv.y * 0.5);
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0 ||
        suv.z < 0.0 || suv.z > 1.0) {
        return 1.0;
    }
    float texel = u.shadow_params.w;
    float bias  = u.shadow_params.y;
    float lit   = 0.0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            float occluder =
                    texture(shadow_map, suv.xy + vec2(dx, dy) * texel).r;
            // Mirrors lotus::pbr::shadow_tap.
            lit += step(suv.z - bias, occluder);
        }
    }
    return lit / 25.0;
}

// Analytic blob tap for one sphere: 1 far away, ~0.3 under the center, soft
// edge scaled by the sun angular radius. Mirrors lotus::pbr::blob_tap.
float blob_tap(vec3 world_position, vec3 center, float radius)
{
    if (radius <= 0.0) {
        return 1.0;
    }
    float dist = length(world_position - center);
    float soft = radius * BLOB_SUN_RADIUS + 0.05;
    float lo   = radius - soft;
    float hi   = radius + soft;
    float t    = hi <= lo ? (dist >= radius ? 1.0 : 0.0)
                          : clamp((dist - lo) / (hi - lo), 0.0, 1.0);
    float eased = t * t * (3.0 - 2.0 * t);
    float above  = world_position.y - center.y;
    float fade   = above <= 0.0 ? 1.0
                                : clamp(1.0 - above / (radius * 4.0 + 1e-4),
                                        0.0, 1.0);
    return 1.0 - (1.0 - (0.3 + 0.7 * eased)) * fade;
}

float blob_shadow(vec3 world_position)
{
    float count = clamp(u.blob_params.x, 0.0, 4.0);
    float shadow = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (float(i) >= count) {
            break;
        }
        shadow *= blob_tap(world_position, u.blob_spheres[i].xyz,
                            u.blob_spheres[i].w);
    }
    return shadow;
}

// Screen-space AO: 1 when disabled (or bound to white), sampled otherwise.
// Scales fill only, never the sun.
float ssao_factor()
{
    if (u.ssao_params.x < 0.5) {
        return 1.0;
    }
    vec2 uv = gl_FragCoord.xy * u.ssao_texel.xy;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return 1.0;
    }
    return clamp(texture(ssao_map, uv).r, 0.0, 1.0);
}

void main()
{
    vec3 geometric_n = normalize(v_normal);
    vec3 n = geometric_n;

    // Tangent-space normal mapping with an analytic TBN from screen-space
    // derivatives. The flat-normal sentinel carries alpha 0, so materials
    // without a normal map take the exact legacy path.
    vec4 normal_texel = texture(normal_map, v_uv);
    if (normal_texel.a > 0.5) {
        vec3 dp1 = dFdx(v_world_position);
        vec3 dp2 = dFdy(v_world_position);
        vec2 duv1 = dFdx(v_uv);
        vec2 duv2 = dFdy(v_uv);
        vec3 dp2perp = cross(dp2, geometric_n);
        vec3 dp1perp = cross(geometric_n, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
        float invmax = inversesqrt(max(dot(T, T), dot(B, B)) + 1e-12);
        T *= invmax;
        B *= invmax;
        vec3 tangent_n = normal_texel.xyz * 2.0 - 1.0;
        vec3 mapped_n = mat3(T, B, geometric_n) * tangent_n;
        if (dot(mapped_n, mapped_n) > 1e-8) {
            n = normalize(mapped_n);
        }
    }

    vec3 l = normalize(u.light_direction.xyz);
    vec3 v = normalize(u.camera_position.xyz - v_world_position);
    vec3 h = normalize(l + v);

    float n_dot_l = max(dot(n, l), 0.0);
    float n_dot_v = max(dot(n, v), 0.0);
    float rough   = clamp(v_roughness, 0.0, 1.0);
    float metallic = clamp(v_metallic, 0.0, 1.0);

    // Decode-then-multiply: each sRGB input decodes first, then multiplies.
    vec3 albedo = srgb_to_linear(v_color) *
                  srgb_to_linear(texture(albedo, v_uv).rgb);
    vec3 radiance = u.light_color.rgb * u.light_color.a;

    // Metallic workflow: metals have no diffuse lobe and tint F0 with their
    // base colour. Dielectrics keep the legacy 0.04 F0.
    vec3 f0 = mix(DIELECTRIC_F0, albedo, metallic);
    vec3 diffuse_albedo = albedo * (1.0 - metallic);
    float occ = ssao_factor();

    // Hemisphere ambient floor (unchanged for dielectrics).
    float hemi_t = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemi = mix(u.hemi_ground.rgb, u.hemi_sky.rgb, hemi_t) /
                HEMI_REFERENCE;
    vec3 outgoing = u.ambient_color.rgb * hemi * diffuse_albedo * occ;

    // Bounce fill from the mirrored sun, unshadowed.
    if (u.bounce_params.x > 0.5) {
        vec3 bounce_dir = normalize(l * vec3(-1.0, 0.0, -1.0));
        float n_dot_b = max(dot(n, bounce_dir), 0.0);
        if (n_dot_b > 0.0) {
            vec3 bounce_radiance = u.bounce_color.rgb * u.bounce_color.a;
            outgoing += diffuse_albedo * bounce_radiance * n_dot_b * occ / PI;
        }
    }

    // Analytic sky/ground environment specular. Cheap enough for every
    // material and enough to give water, leather and metal a reflected
    // environment instead of a flat fill light.
    if (u.environment_params.x > 0.0) {
        vec3 r = reflect(-v, n);
        float up = clamp(r.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 env = mix(srgb_to_linear(u.hemi_ground.rgb),
                       srgb_to_linear(u.hemi_sky.rgb), up);
        vec3 f_env = fresnel_schlick(n_dot_v, f0);
        float gloss = (1.0 - rough) * (1.0 - rough);
        outgoing += env * f_env * gloss * u.environment_params.x * occ;
    }

    // Direct sun.
    if (n_dot_l > 0.0 && n_dot_v > 0.0) {
        float n_dot_h = max(dot(n, h), 0.0);
        float h_dot_v = max(dot(h, v), 0.0);
        float d = ndf_ggx(n_dot_h, rough);
        float g = geometry_schlick_ggx(n_dot_l, rough) *
                  geometry_schlick_ggx(n_dot_v, rough);
        vec3 f = fresnel_schlick(h_dot_v, f0);
        vec3 specular = (d * g / max(4.0 * n_dot_l * n_dot_v, 0.001)) * f;
        vec3 diffuse = (vec3(1.0) - f) * diffuse_albedo / PI;
        outgoing += (diffuse + specular) * radiance * n_dot_l *
                    shadow_factor(v_world_position, n) *
                    blob_shadow(v_world_position);
    }

    // Point lights. Attenuation is a smooth windowed inverse square; no
    // shadows (they are fill lights, and the sun owns the shadow map).
    int point_count = int(clamp(u.point_params.x, 0.0, 8.0));
    for (int i = 0; i < 8; ++i) {
        if (i >= point_count) {
            break;
        }
        vec3 to_light = u.point_positions[i].xyz - v_world_position;
        float dist = length(to_light);
        float range = max(u.point_positions[i].w, 0.001);
        if (dist >= range) {
            continue;
        }
        vec3 pl = to_light / max(dist, 1e-5);
        vec3 ph = normalize(pl + v);
        float pl_n_dot_l = max(dot(n, pl), 0.0);
        float pl_n_dot_v = max(dot(n, v), 0.0);
        if (pl_n_dot_l <= 0.0 || pl_n_dot_v <= 0.0) {
            continue;
        }
        float window = clamp(1.0 - pow(dist / range, 4.0), 0.0, 1.0);
        float atten = (window * window) / (dist * dist + 1.0);
        float pl_n_dot_h = max(dot(n, ph), 0.0);
        float pl_h_dot_v = max(dot(ph, v), 0.0);
        float pd = ndf_ggx(pl_n_dot_h, rough);
        float pg = geometry_schlick_ggx(pl_n_dot_l, rough) *
                   geometry_schlick_ggx(pl_n_dot_v, rough);
        vec3 pf = fresnel_schlick(pl_h_dot_v, f0);
        vec3 pspec = (pd * pg / max(4.0 * pl_n_dot_l * pl_n_dot_v, 0.001)) * pf;
        vec3 pdiff = (vec3(1.0) - pf) * diffuse_albedo / PI;
        vec3 point_color = srgb_to_linear(u.point_colors[i].rgb) *
                           u.point_colors[i].a;
        outgoing += (pdiff + pspec) * point_color * pl_n_dot_l * atten;
    }

    // Emissive surfaces (flames, fireflies, lantern glass).
    outgoing += srgb_to_linear(v_emissive);

    // Linear-space exponential fog between lighting and ACES.
    if (u.fog_params.x > 0.5) {
        float view_depth = max(-v_view_position.z, 0.0);
        float fog_amount =
                1.0 - exp(-max(u.fog_params.y, 0.0) * view_depth);
        fog_amount = clamp(fog_amount, 0.0, 1.0);
        outgoing = mix(outgoing, u.fog_color.rgb, fog_amount);
    }

    vec3 mapped = aces_tone_map(outgoing * max(u.tone_params.x, 0.0));
    out_color = vec4(linear_to_srgb(mapped), 1.0);

    out_linear_depth = vec4(-v_view_position.z, 0.0, 0.0, 1.0);
}
