#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec3 aColor;
out vec3 color;
// 0.0f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f

void main() {
    color = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
}