#version 450

// Directional shadow-map depth pass. Places each vertex in the world exactly
// as lit.vert does (the instance's model matrix; the identity for the
// immediate stream), then transforms it by the light's view-projection. Depth
// is written automatically, so the fragment shader is an empty main. Only the
// position and the model matrix are consumed -- the pipeline keeps the full
// lit vertex layout so the same buffers bind here.
//
// Uniform block reuses the lit vertex layout (set 1, binding 0): the renderer
// uploads light_vp in view_projection and the light view in view.

layout(location = 0) in vec3 in_position;
layout(location = 8) in mat4 in_model; // per instance, locations 8..11

layout(set = 1, binding = 0) uniform VertexUniforms {
    mat4 view_projection;
    mat4 view;
} u;

void main()
{
    vec3 world_position = (in_model * vec4(in_position, 1.0)).xyz;
    gl_Position = u.view_projection * vec4(world_position, 1.0);
}
