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
    
    static GLWindow *create(LinearAllocator& allocator, int x, int y, const std::string &title,
                            GLFWmonitor *monitor, GLFWwindow *window)
    {
        GLFWwindow *w = glfwCreateWindow(x, y, title.c_str(), monitor, window);
        return allocator.construct<GLWindow>(x,y,w);
    }

    void close(bool force)
    {
        if (window_)
            glfwSetWindowShouldClose(window_, force);
    }

    int should_close()
    {
        if (window_)
            return glfwWindowShouldClose(window_);
        return true;
    }

    void swap_buffers()
    {
        if (window_)
            glfwSwapBuffers(window_);
    }

    void clear()
    {
        if (window_)
            glClear(GL_COLOR_BUFFER_BIT);
    }

    void update()
    {
        if (window_)
        {
            glfwSwapBuffers(window_);
            glfwPollEvents();
        }
    }

    void set_title(const std::string &title)
    {
        if (window_)
            glfwSetWindowTitle(window_, title.c_str());
    }

    void set_size(int width, int height)
    {
        width_ = width;
        height_ = height;
        if (window_)
            glfwSetWindowSize(window_, width_, height_);
    }

    void get_size(int &width, int &height)
    {
        width = width_;
        height = height_;
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