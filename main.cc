#include <iostream>
#include <string>
#include "allocator.hpp"
#include "math.hpp"
#include "registry.hpp"
struct Transform
{
    Vector2<float> pos;
};

struct Physics
{
    Vector2<float> vel;
};
int main()
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
    return 0;
}