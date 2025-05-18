#include <iostream>
#include <string>
#include "allocator.hpp"
#include "math.hpp"
#include "component.hpp"
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
    // StackAllocator allocator{1024};
    // int count = 0;
    // while (allocator.allocated() < allocator.size())
    // {
    //     int *alloc = allocator.alloc<int>();
    //     *alloc = count++;
    //     std::cout << *alloc << std::endl;
    // }
    LinearAllocator pallocator {KB(25)};
    ComponentManager component_manager {pallocator,256};
    auto [transform,physics] = component_manager.assign<Transform,Physics>(0);
    std::cout << transform->pos << std::endl;
    // auto tc = component_manager.assign<Transform>(0);
    // std::cout << tc->pos;
    // ComponentPool pool(pallocator, 1024, sizeof(Transform),alignof(Transform));
    // for (int i = 0; i < pool.capacity(); ++i) {
    //     Transform* tc = reinterpret_cast<Transform*>(pool.assign(i));
    //     tc->pos[0] = i;
    //     tc->pos[1] = 15 * i;
    // }

    // for (char* component : pool) {
    //     Transform* tc = reinterpret_cast<Transform*>(component);
    //     std::cout << tc->pos << std::endl;
    // }

    return 0;
}