#version 330 core
layout(location = 0) in vec2 aPos;

layout(std140) uniform QuadUniform
{
    vec3 aColor;
    float pad;
};

out vec3 color;

void main() {
    color = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
