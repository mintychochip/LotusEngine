#include <iostream>
#include <atomic>
#include <gl/gl.h>
#include <glfw/glfw3.h>
#include "core/allocator.hpp"
#include "core/application/window/window.h"
#include "core/application/window/gl_window.hpp"
class Application
{
    public:
    static Application &get()
    {
        static Application instance;
        return instance;
    }
    void on_start()
    {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
        if (!glfwInit())
        {
            std::cerr << "failed to initialize glfw\n"
                      << std::endl;
            exit(-1);
        }
    }
    void run()
    {
        on_start();

        LinearAllocator pallocator{KB(50)};  // more space for 2 windows

        IWindow<GLFWwindow>* win1 = GLWindow::create(pallocator, 800, 600, "Window 1", nullptr, nullptr);
        IWindow<GLFWwindow>* win2 = GLWindow::create(pallocator, 640, 480, "Window 2", nullptr, nullptr);

        if (!win1 || !win2) {
            std::cerr << "Failed to create one or more windows\n";
            glfwTerminate();
            return;
        }

        glfwMakeContextCurrent(win1->handle());
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            std::cerr << "Failed to initialize GLAD\n";
            glfwTerminate();
            return;
        }

        // ✅ Main loop for both windows
        while (!win1->should_close() || !win2->should_close()) {
            glfwPollEvents();  // handles input for all windows

            // --- Window 1 ---
            if (!win1->should_close()) {
                glfwMakeContextCurrent(win1->handle());
                win1->clear();
                // render win1
                win1->swap_buffers();
            }

            // --- Window 2 ---
            if (!win2->should_close()) {
                glfwMakeContextCurrent(win2->handle());
                win2->clear();
                // render win2
                win2->swap_buffers();
            }
        }

        glfwDestroyWindow(win1->handle());
        glfwDestroyWindow(win2->handle());
        glfwTerminate();
    }
    private:
        Application() {}
        std::atomic<bool> done_;
};