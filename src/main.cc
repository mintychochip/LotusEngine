#include <iostream>
#include <string>
#include <glad/glad.h>
#include <glfw/glfw3.h>
#include "core/allocator.hpp"
#include "core/math.hpp"
#include "ecs/registry.hpp"
#include "core/application/gl_application.hpp"
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
    GLApplication& application = GLApplication::get();
    application.run();
    test();
    return 0;
}