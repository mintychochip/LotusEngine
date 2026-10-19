#version 450

// Procedural sky dome vertex stage. Bufferless fullscreen triangle generated
// from gl_VertexIndex (same pattern as fxaa.vert): the pipeline binds zero
// vertex state. The triangle is pinned at far (z == w, depth 1.0 in the
// zero-to-one range) and is drawn AFTER the scene geometry with LESS_OR_EQUAL
// and no depth write. The renderer rasterises geometry into the lower half of
// the depth range, so only a pixel no geometry touched still holds the
// cleared 1.0 and takes the sky; the fragment stage never runs under a
// surface. The world-space position on the far plane is passed through; the
// fragment stage subtracts the camera position to recover the view
// direction.

layout(set = 1, binding = 0) uniform SkyVertexUniforms {
    mat4 inv_view_projection;
} u;

layout(location = 0) out vec3 v_world_position;

void main()
{
    vec2 p      = vec2(float((gl_VertexIndex << 1) & 2),
                       float(gl_VertexIndex & 2));
    vec4 ndc    = vec4(p * 2.0 - 1.0, 1.0, 1.0);
    vec4 world  = u.inv_view_projection * ndc;
    v_world_position = world.xyz / max(world.w, 1e-6);
    gl_Position      = ndc;
}
