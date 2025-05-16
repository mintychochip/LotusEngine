#include <iostream>
#include <string>
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
    PoolAllocator<Particle> particle_allocator {1024};
    for (int i = 0; i < 10; i++) {
        PoolAllocation<Particle> alloc = particle_allocator.alloc(1024);
        std::cout << alloc.member << ' ' << alloc.nblocks << std::endl;
        particle_allocator.free(alloc);
    }    
    return 0;
}