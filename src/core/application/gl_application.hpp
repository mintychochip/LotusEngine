#include <iostream>
#include <atomic>
#include <glad/glad.h>
#include <glfw/glfw3.h>
#include "core/allocator.hpp"
#include "core/application/gl_window.hpp"
#include "core/renderer/shader.hpp"
#include "core/renderer/buffer.hpp"
class GLApplication
{
public:
    static GLApplication &get()
    {
        static GLApplication instance;
        return instance;
    }

    void on_start()
    {
        if (!glfwInit())
        {
            std::cerr << "failed to initialize glfw\n"
                      << std::endl;
            exit(-1);
        }
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    }

    void on_close()
    {
        glfwDestroyWindow(window_->get_handle());
        glfwTerminate();
    }

    void run()
    {
        on_start();

        LinearAllocator pallocator{KB(50)}; // more space for 2 windows

        window_ = GLWindow::create(pallocator, 800, 600, "Simone", nullptr, nullptr);
        window_->set_current(); // Make context current once

        Shader vertex = Shader::create("shaders/test.vert", VERTEX);
        Shader fragment = Shader::create("shaders/test.frag", FRAGMENT);
        ShaderProgram program = ShaderProgram::create(vertex, fragment);

        if (shader_::invalid_program(program))
        {
            std::cerr << "shader program creation failed\n";
            return; // Gracefully exit
        }

        float vertices[] = {
            0.0f, 0.5f,
            -0.5f, -0.5f,
            0.5f, -0.5f
        };

        VertexAttributeLayout layout;
        layout.addAttribute(2, GL_FLOAT, GL_FALSE);

        VertexBuffer *vbo = VertexBuffer::create(pallocator, 1, sizeof(vertices), vertices, GL_STATIC_DRAW);
        vbo->bind(0);
        VertexArray *vao = VertexArray::create(pallocator, 1, layout);
        if (!window_->get_handle())
        {
            std::cerr << "Failed to create one or more windows\n";
            return; // Gracefully exit
        }

        double lastTime = glfwGetTime();
        int frameCount = 0;
        while (!window_->should_close())
        {
            window_->clear();

            program.bind();
            vao->bind(0);

            glDrawArrays(GL_TRIANGLES, 0, 3);

            window_->swap_buffers();
            glfwPollEvents();
        }

        vao->destroy(); // Clean up VAO
        vbo->destroy();
        on_close(); // Handle clean-up
    }

private:
    std::atomic<bool> done_;
    GLWindow *window_;
};