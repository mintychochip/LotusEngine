#include <iostream>
#include <string>
#include "allocator.hpp"
#include "math.hpp"
#include "component_pool.hpp"
struct Transform
{
    Vector2<float> pos;
};

int main()
{
    StackAllocator allocator{1024};
    int count = 0;
    while (allocator.allocated() < allocator.size())
    {
        int *alloc = allocator.alloc<int>();
        *alloc = count++;
        std::cout << *alloc << std::endl;
    }
    LinearAllocator pallocator {MB(25)};
    ComponentPool<Transform> pool (pallocator,32);
    Transform* tc = pool.assign(1);
    tc->pos[0] = 1;
    tc->pos[1] = 1.5f;

    std::cout << tc->pos << std::endl;
    pool.remove(1);
    tc = pool.assign(1);
    tc->pos[0] = 2;
    std::cout << tc->pos << std::endl;

    return 0;
}