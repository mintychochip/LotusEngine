#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "core/renderer/shader.hpp" // your shader header
#include "core/renderer/buffer.hpp"
#include "core/application/gl_application.hpp"
#include "core/renderer/buffer.hpp"
void framebuffer_size_callback(GLFWwindow *window, int width, int height)
{
    glViewport(0, 0, width, height);
}

int main() {
    std::cout << glGetString(GL_VERSION) << std::endl;
    GLApplication &application = GLApplication::get();
    application.run();
    // Initialize GLFW
    // if (!glfwInit()) {
    //     std::cerr << "Failed to initialize GLFW" << std::endl;
    //     return -1;
    // }

    // // Create a windowed mode window and its OpenGL context
    // GLFWwindow *window = glfwCreateWindow(800, 600, "OpenGL Debug", NULL, NULL);
    // if (!window) {
    //     std::cerr << "Failed to create GLFW window" << std::endl;
    //     glfwTerminate();
    //     return -1;
    // }

    // // Make the window's context current
    // glfwMakeContextCurrent(window);
    // gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    // // Check OpenGL version
    // // std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // // Create and compile shader
    // Shader vertex = Shader::create("shaders/test.vert",VERTEX);
    // Shader fragment = Shader::create("shaders/test.frag",FRAGMENT);
    // ShaderProgram program = ShaderProgram::create(vertex,fragment);
    // // Create a simple VAO and VBO for testing

    // LinearAllocator allocator {KB(50)};
    // VertexAttributeLayout layout;
    // layout.addAttribute(3,GL_FLOAT,GL_FALSE);
    // GLuint VBO;
    // glGenBuffers(1, &VBO);

    // glBindBuffer(GL_ARRAY_BUFFER, VBO);

    // VertexArray* vao = VertexArray::create(allocator,1,layout);

    // float vertices[] = {
    //     0.0f, 0.5f, 0.0f,
    //     -0.5f, -0.5f, 0.0f,
    //     0.5f, -0.5f, 0.0f
    // };

    // glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    // // Loop until the user closes the window
    // while (!glfwWindowShouldClose(window)) {
    //     glClear(GL_COLOR_BUFFER_BIT);

    //     program.bind();
    //     vao->bind();
    //     // Draw
    //     glDrawArrays(GL_TRIANGLES, 0, 3);

    //     // vao->unbind();
    //     glfwSwapBuffers(window);
    //     glfwPollEvents();
    // }

    // // Cleanup
    // glDeleteBuffers(1, &VBO);
    // glfwDestroyWindow(window);
    // glfwTerminate();

    return 0;
}
