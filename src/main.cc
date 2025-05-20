#include <iostream>
#include <string>
#include <glfw/glfw3.h>
#include "core/allocator.hpp"
#include "core/math.hpp"
#include "ecs/registry.hpp"
#include "core/application/application.hpp"
#include <stb_image.h>
struct Transform
{
    Vector2<float> pos;
};

struct Physics
{
    Vector2<float> vel;
};

void test()
{
    LinearAllocator pallocator {KB(25)};
    ComponentManager component_manager {pallocator,256};
    EntityManager emanager {256};
    Entity entity = emanager.create();
    auto [transform,physics] = component_manager.assign<Transform,Physics>(entity);
    for (int i = 0; i < 255; i++) {
        std::cout << emanager.active() << ' ' << emanager.free() << std::endl;
        entity = emanager.create();
        emanager.remove(entity);
    }
    emanager.remove(entity);
    std::cout << emanager.create().id;
    std::cout << transform->pos << std::endl;
    std::cout << pallocator.remaining() << std::endl;
}
int main()
{
    Application& application = Application::get();
    application.Run();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW\n";
        return -1;
    }

    GLFWwindow* window = glfwCreateWindow(800, 600, "GLFW Window", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create window\n";
        glfwTerminate();
        return -1;
    }
    

    glfwMakeContextCurrent(window);

    while (!glfwWindowShouldClose(window)) {
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    test();
    return 0;
}