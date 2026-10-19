#version 450

// Directional shadow-map depth pass. Transforms the world-space lit vertex
// stream by the light's view-projection; depth is written automatically, so
// the fragment shader is an empty main. Only in_position is consumed -- the
// pipeline keeps the full lit vertex layout so the same buffers bind here.
//
// Uniform block reuses the lit vertex layout (set 1, binding 0): the renderer
// uploads light_vp in view_projection and the light view in view.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
layout(location = 4) in vec2 in_uv;
layout(location = 6) in float in_roughness;

layout(set = 1, binding = 0) uniform VertexUniforms {
    mat4 view_projection;
    mat4 view;
} u;

void main()
{
    gl_Position = u.view_projection * vec4(in_position, 1.0);
}
