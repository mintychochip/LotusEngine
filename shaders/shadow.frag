#version 450

// Depth-only fragment stage for the shadow map. The depth value is produced
// by fixed-function rasterization from shadow.vert's gl_Position; nothing is
// written here. Kept as a separate file (rather than reusing another empty
// fragment stage) so the shadow pipeline declares exactly zero samplers and
// zero fragment uniform buffers.

void main()
{
}
