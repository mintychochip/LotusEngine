#version 450

// Per vertex (slot 0 and 1): a mesh vertex. For an instanced mesh these are
// local-space with colour 1 and roughness 1; for the immediate stream (sprites,
// hand-built meshes) they are already world-space and the instance below is
// the identity.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
layout(location = 4) in vec2 in_uv;
layout(location = 6) in float in_roughness;

// Per instance (slot 2, input rate INSTANCE): the model matrix (a mat4 takes
// locations 8..11) and the material multipliers, rgb colour then roughness.
layout(location = 8) in mat4 in_model;
layout(location = 12) in vec4 in_shade;
layout(location = 13) in vec4 in_material; // rgb emissive, a metallic

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
layout(location = 7) out vec3 v_emissive;
layout(location = 8) out float v_metallic;

void main()
{
    vec3 world_position = (in_model * vec4(in_position, 1.0)).xyz;

    // The normal matrix is the upper 3x3 of the model matrix: right for
    // rotation and uniform scale, as the CPU path this replaces always was.
    // A missing normal is stored as zero and, like a degenerate one, falls
    // back to +Z untransformed.
    vec3 rotated = mat3(in_model) * in_normal;
    float len2   = dot(rotated, rotated);
    vec3 normal  = len2 > 0.0 ? rotated * inversesqrt(len2) : vec3(0.0, 0.0, 1.0);

    v_world_position = world_position;
    v_normal         = normal;
    v_color          = in_color * in_shade.rgb;
    v_uv             = in_uv;
    v_roughness      = in_roughness * in_shade.a;
    v_emissive       = in_material.rgb;
    v_metallic       = in_material.a;
    v_view_position  = (u.view * vec4(world_position, 1.0)).xyz;
    gl_Position      = u.view_projection * vec4(world_position, 1.0);
}
