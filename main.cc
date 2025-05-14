#include <iostream>
#include "allocator.hpp"
#include "math.hpp"
struct Particle
{
    int x,y;
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
    PoolAllocator<Particle> particle_allocator {10};   
    for (int i = 0; i < 10; ++i) {
        Particle* alloc = particle_allocator.alloc();
        std::cout << alloc << std::endl;
    }
    using vec2i = Vector2<int>;
    vec2i test {0,1};

    test[0] = 1;
    std::cout << test[0] << std::endl;
    return 0;
}
