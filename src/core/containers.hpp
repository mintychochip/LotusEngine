#pragma once

#include <cstdint>
#include "allocator.hpp"
using u32 = uint32_t;
struct FreeBlock
{
    FreeBlock *next;
};
