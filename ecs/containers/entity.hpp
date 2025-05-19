#pragma once

#include <cstdint>
#include <bitset>

struct Entity
{
    using u32 = uint32_t;
    using component_mask = std::bitset<32>; //CHANGE THIS NUMBER LATER
    u32 id;
    component_mask mask;
    constexpr Entity(u32 id) : id{id} {}
};