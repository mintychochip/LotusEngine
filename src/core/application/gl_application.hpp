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
#include <glm/glm.hpp>
#include "core/allocator.hpp"
#include "core/application/gl_window.hpp"
#include "core/renderer/shader.hpp"
#include "core/renderer/texture.hpp"
#include "core/renderer/vertex_batch.hpp"
#include "core/renderer/buffer.hpp"
#include "core/renderer/vertex_array.hpp"
#include "core/renderer/drawable.hpp"
#include "core/renderer/camera.hpp"
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
        glfwWindowHint(GLFW_SAMPLES,16);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        srand(std::time(nullptr));
        Global::get().width(1024).height(768);
        std::cout << "Aspect = " << Global::get().get_aspect() << "\n";

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
        window_ = GLWindow::create(pallocator, Global::get().width(),Global::get().height(), "Simone", nullptr, nullptr);
        window_->set_current(); // Make context current once
        glViewport(0,0,Global::get().width(),Global::get().height());

        Shader vertex{"shaders/test.vert", GL_VERTEX_SHADER};
        Shader fragment{"shaders/test.frag", GL_FRAGMENT_SHADER};
        ShaderProgram program = ShaderProgram::create(vertex, fragment);

        if (shader_::invalid_program(program))
        {
            std::cerr << "shader program creation failed\n";
            return; // Gracefully exit
        }

        StackAllocator allocator{KB(1024)};
        lotus::AttributeLayout layout{allocator.alloc<lotus::Attribute_>(1), 1, sizeof(lotus::Quad::VertexType)};
        layout.add(2, GL_FLOAT, GL_FALSE, 0);
        lotus::VertexBatch<lotus::Quad> batch(
            allocator.alloc<lotus::Quad::VertexType>(9), 9, 
            allocator.alloc<lotus::Quad::QuadUniformData>(9), 9,
            allocator.alloc<u32>(9), 9,
            GL_STATIC_DRAW, layout);

        if (!window_->get_handle())
        {
            std::cerr << "Failed to create one or more windows\n";
            return; // Gracefully exit
        }
        glEnable(GL_MULTISAMPLE);
        double lastTime = glfwGetTime();
        int frameCount = 0;
    //    glDisable(GL_CULL_FACE); // for testing

        batch.push_vertex({{-0.3f,  0.3f}}); 
        batch.push_vertex({{ 0.3f,  0.3f}}); 
        batch.push_vertex({{ 0.3f, -0.3f}}); 
        batch.push_vertex({{-0.3f, -0.3f}});
        batch.push_element({2, 1, 0});
        batch.push_element({0, 3, 2});
        auto camera = lotus::Camera(glm::vec3{0.0f,0.0f,15.0f});
        glm::mat4 model = glm::mat4(1.0f);

glm::mat4 projection = glm::perspective(glm::radians(45.0f), 1024.0f / 768.0f, 0.1f, 100.0f);
        batch.push_uniform({color()});

        glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
        while (!window_->should_close())
        {
            window_->clear();
            program.bind();
            program.set_mat4("model", model);
            program.set_mat4("view", camera.view_matrix());
            program.set_mat4("projection", projection);
            program.set_float3("aColor", glm::vec3(1.0f, 0.0f, 1.0f)); // magenta again
            // batch.clear(lotus::BatchBufferType::UNIFORM_BUFFER);
            batch.forward();
            batch.draw();
            auto pos = camera.get_pos() + glm::vec3(0.0f,0.0f,-0.01f);
            camera.set_pos(pos);
            window_->swap_buffers();
            glfwPollEvents();
            
        }
        on_close(); // Handle clean-up
    }

private:
    GLApplication() {}
    std::atomic<bool> done_;
    GLWindow *window_;
};