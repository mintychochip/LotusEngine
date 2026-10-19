#version 450

// Fullscreen-triangle vertex stage for the SSAO pass. No vertex buffers: the
// triangle is generated from gl_VertexIndex, so the pipeline binds zero vertex
// state. UVs follow the SDL_GPU sampling convention (v = 0.5 - ndc.y * 0.5:
// every backend renders Y-flipped, so NDC +Y is framebuffer row 0, which
// samples as v = 0). Same pattern as fxaa.vert.

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p       = vec2(float((gl_VertexIndex << 1) & 2),
                        float(gl_VertexIndex & 2));
    gl_Position  = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    v_uv         = vec2(p.x, 1.0 - p.y);
}
