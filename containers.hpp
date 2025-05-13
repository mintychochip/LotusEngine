#include <cstdint>

using u32 = uint32_t;
struct FreeBlock
{
    FreeBlock *next;
};