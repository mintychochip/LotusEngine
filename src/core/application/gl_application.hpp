/**
**********************************************************************************************************************************************************************************************************************************
* @file:	gl_application.hpp
* @author:
* @date:	2025年05月23日 19:47:52 Friday
* @brief:
**********************************************************************************************************************************************************************************************************************************
**/

#pragma once

#include <iostream>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <glad/glad.h>
#include <glfw/glfw3.h>
#include "core/allocator.hpp"
#include "core/application/gl_window.hpp"
#include "core/renderer/shader.hpp"
#include "core/renderer/texture.hpp"
#include "core/renderer/vertex_batch.hpp"
// #include "core/assets/asset_manager.hpp"
#include "core/renderer/vertex_array.hpp"
#include "core/renderer/vertex_buffer.hpp"
#include "core/renderer/vertex_batch.hpp"

struct Quad
{
    struct Vertex
    {
        glm::vec2 pos;
        glm::vec3 color;
    };

    using VertexType = Vertex;
};

GLuint random_ub()
{
    return rand() % 256;
}

glm::vec3 color()
{
    return glm::vec3(random_ub(), random_ub(), random_ub()) / 255.0f;
}

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
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        srand(std::time(nullptr));
    }

    void on_close()
    {
        glfwDestroyWindow(window_->get_handle());
        glfwTerminate();
    }

    void run()
    {
        on_start();
        LinearAllocator pallocator{KB(1)}; // more space for 2 windows
        window_ = GLWindow::create(pallocator, 800, 600, "Simone", nullptr, nullptr);
        window_->set_current(); // Make context current once
        glViewport(0,0,800,600);
        GLTextureSettings settings{GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT};

        Shader vertex{"shaders/test.vert", GL_VERTEX_SHADER};
        Shader fragment{"shaders/test.frag", GL_FRAGMENT_SHADER};
        ShaderProgram program = ShaderProgram::create(vertex, fragment);

        if (shader_::invalid_program(program))
        {
            std::cerr << "shader program creation failed\n";
            return; // Gracefully exit
        }
        
        StackAllocator allocator{KB(1024)};
        lotus::AttributeLayout layout{allocator.alloc<lotus::Attribute_>(2), 2, sizeof(Quad::VertexType)};
        layout.add(2, GL_FLOAT, GL_FALSE, 0);
        layout.add(3, GL_FLOAT, GL_FALSE, 2 * sizeof(float));
        lotus::VertexBatch<Quad> batch = {allocator.alloc<Quad::VertexType>(9), 9, GL_STATIC_DRAW, layout};

        if (!window_->get_handle())
        {
            std::cerr << "Failed to create one or more windows\n";
            return; // Gracefully exit
        }

        double lastTime = glfwGetTime();
        int frameCount = 0;
        glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
        double lastUpdate = glfwGetTime();       // time of last update
        const double updateInterval = 1.0 / 10.0; // 5 times per second

        while (!window_->should_close())
        {
            double now = glfwGetTime();

            window_->clear();
            program.bind();
            // Only update vertices every 1/5th second
            if (now - lastUpdate >= updateInterval)
            {
                batch.clear();
                auto c = color();
                // Triangle 1 — top center
                batch.push_vertex({{  0.0f,  0.6f}, c});
                batch.push_vertex({{ -0.3f,  0.0f}, c});
                batch.push_vertex({{  0.3f,  0.0f}, c});


                // Triangle 2 — bottom left
                batch.push_vertex({{ -0.3f,  0.0f}, c});
                batch.push_vertex({{ -0.6f, -0.6f}, c});
                batch.push_vertex({{  0.0f, -0.6f}, c});


                // Triangle 3 — bottom right
                batch.push_vertex({{  0.3f,  0.0f}, c});
                batch.push_vertex({{  0.0f, -0.6f}, c});
                batch.push_vertex({{  0.6f, -0.6f}, c});


                batch.forward();

                lastUpdate = now;
            } else {
                batch.draw();
            }


            window_->swap_buffers();
            glfwPollEvents();
        }
        on_close(); // Handle clean-up
        // update.join();
    }

private:
    GLApplication() {}
    std::atomic<bool> done_;
    GLWindow *window_;
};