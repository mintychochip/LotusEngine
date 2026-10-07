#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
layout(location = 4) in vec2 in_uv;
layout(location = 6) in float in_roughness;

layout(set = 1, binding = 0) uniform VertexUniforms {
    mat4 view_projection;
    mat4 view;
} u;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_color;
layout(location = 3) out vec3 v_view_position;
layout(location = 5) out vec2 v_uv;
layout(location = 6) out float v_roughness;

void main()
{
    v_world_position = in_position;
    v_normal         = in_normal;
    v_color          = in_color;
    v_uv             = in_uv;
    v_roughness      = in_roughness;
    v_view_position  = (u.view * vec4(in_position, 1.0)).xyz;
    gl_Position      = u.view_projection * vec4(in_position, 1.0);
}
