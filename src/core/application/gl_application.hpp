#include <iostream>
#include <atomic>
#include <gl/gl.h>
#include <glfw/glfw3.h>
#include "core/allocator.hpp"
#include "core/application/gl_window.hpp"
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
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        if (!glfwInit())
        {
            std::cerr << "failed to initialize glfw\n"
                      << std::endl;
            exit(-1);
        }
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

        if (!window_->get_handle())
        {
            std::cerr << "Failed to create one or more windows\n";
            glfwTerminate();
            exit(1);
        }

        glfwMakeContextCurrent(window_->get_handle());
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
        {
            std::cerr << "Failed to initialize GLAD\n";
            glfwTerminate();
            return;
        }
        while (!window_->should_close())
        {
            window_->swap_buffers();
            glfwPollEvents();
        }
        on_close();
    }

private:
    std::atomic<bool> done_;
    GLWindow* window_;
};