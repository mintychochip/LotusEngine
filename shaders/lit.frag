#version 450

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_color;
layout(location = 3) in vec3 v_view_position;
layout(location = 5) in vec2 v_uv;

layout(set = 2, binding = 0) uniform sampler2D albedo;

layout(set = 3, binding = 0) uniform FragmentUniforms {
    vec4 light_direction;
    vec4 light_color;
    vec4 ambient_color;
    vec4 camera_position;
} u;

layout(location = 0) out vec4 out_color;
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
    vec3 albedo_rgb = texture(albedo, v_uv).rgb;
    vec3 lit = v_color * albedo_rgb * (u.ambient_color.rgb + radiance * diffuse) +
               radiance * specular;

    out_color = vec4(lit, 1.0);
    out_linear_depth = vec4(-v_view_position.z, 0.0, 0.0, 1.0);
}
