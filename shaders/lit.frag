#version 450

// Blinn-Phong: ambient + directional diffuse + directional specular.

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_color;
layout(location = 3) in vec3 v_view_position;

layout(set = 3, binding = 0) uniform FragmentUniforms {
    vec4 light_direction; // xyz: unit vector pointing toward the light
    vec4 light_color;     // rgb: colour, a: intensity
    vec4 ambient_color;   // rgb: ambient term
    vec4 camera_position; // xyz: for the specular half-vector
} u;

layout(location = 0) out vec4 out_color;

// Linear view depth, feeding the Hi-Z pyramid. Distance along the camera
// forward axis, in world units. Cleared to 1e9 each frame; must match
// lotus::kDepthFar.
layout(location = 1) out vec4 out_linear_depth;

void main()
{
    vec3 n = normalize(v_normal);
    vec3 l = normalize(u.light_direction.xyz);
    vec3 v = normalize(u.camera_position.xyz - v_world_position);
    vec3 h = normalize(l + v);

    float diffuse  = max(dot(n, l), 0.0);
    float specular = pow(max(dot(n, h), 0.0), 32.0) * 0.25;

    vec3 radiance = u.light_color.rgb * u.light_color.a;
    vec3 lit = v_color * (u.ambient_color.rgb + radiance * diffuse) +
               radiance * specular;

    out_color = vec4(lit, 1.0);
    out_linear_depth = vec4(-v_view_position.z, 0.0, 0.0, 1.0);
}
