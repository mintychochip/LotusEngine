#include <iostream>
#include "allocator.hpp"

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
    return 0;
}