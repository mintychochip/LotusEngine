#version 450

// Lit geometry vertex stage.
//
// Geometry is uploaded already transformed into world space (the CPU applies
// the model matrix when building the batch), so the model matrix here is
// identity and the input position *is* the world position. That keeps the
// whole scene in a single draw call.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;

layout(set = 1, binding = 0) uniform VertexUniforms {
    mat4 view_projection;
} u;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_color;

void main()
{
    v_world_position = in_position;
    v_normal         = in_normal;
    v_color          = in_color;
    gl_Position      = u.view_projection * vec4(in_position, 1.0);
}
