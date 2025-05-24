#version 330 core

out vec4 FragColor;

in vec2 color;

void main() {
    FragColor = vec4(color, 0.2, 1.0); // Output a solid red color
}
