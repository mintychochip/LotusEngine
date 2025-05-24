#pragma once

#include <glad/glad.h>
#include <glfw/glfw3.h>
#include <string>
#include <cstdint>
#include <iostream>

#include "core/allocator.hpp"

class GLWindow
{
public:
    static GLWindow *create(LinearAllocator &allocator, int x, int y, const std::string &title,
                            GLFWmonitor *monitor, GLFWwindow *window)
    {
        GLFWwindow *w = glfwCreateWindow(x, y, title.c_str(), monitor, window);
        if (!w)
        {
            return nullptr;
        }
        return allocator.construct<GLWindow>(x, y, w);
    }

    void close(bool force)
    {
        glfwSetWindowShouldClose(window_, force);
    }

    int should_close()
    {
        return glfwWindowShouldClose(window_);
    }

    void swap_buffers()
    {
        glfwSwapBuffers(window_);
    }

    void clear()
    {
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void set_title(const std::string &title)
    {
        glfwSetWindowTitle(window_, title.c_str());
    }

    void set_size(int width, int height)
    {
        width_ = width;
        height_ = height;
        glfwSetWindowSize(window_, width_, height_);
    }

    void get_size(int &width, int &height)
    {
        width = width_;
        height = height_;
    }

    int set_current()
    {
        glfwMakeContextCurrent(window_);
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            std::cerr << "failed to initialize GLAD\n";
            return 0;
        }
        return 1;
    }

    GLFWwindow *get_handle()
    {
        return window_;
    }

private:
    friend class LinearAllocator;
    GLWindow(int x, int y, GLFWwindow *window) : width_{x}, height_{y}, window_{window} {}
    int width_, height_;
    GLFWwindow *window_;
};