#pragma once 

#include <cstdint>
#include <bitset>
#include <vector>
#include "allocator.hpp"
#define MAX_COMPONENTS 64
using ComponentMask = std::bitset<MAX_COMPONENTS>;
using u32 = uint32_t;
inline size_t next_component_id = 0;

template<typename T>
size_t get_component_id() {
    static size_t id = next_component_id++;
    return id;
}

struct Entity
{
    const u32 id;
};

struct Archetype
{
    ComponentMask mask;
    std::vector<u32> entities;
    std::vector<void*> component_arrays;

    template<typename T>
    void add_component_array(PoolAllocator<T> allocator)
    {
        T* memory = allocator.alloc();
        component_arrays[get_component_id<T>()] = static_cast<void*>(memory);
    }

    template<typename T>
    T* get_array()
    {
        return reinterpret_cast<T*>(component_arrays[get_component_id<T>()]);
    }
};