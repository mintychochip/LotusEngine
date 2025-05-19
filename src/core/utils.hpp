#pragma once

#include <cstdint>

using u32 = uint32_t;
constexpr u32 KB(u32 x) { return x * 1024; }
constexpr u32 MB(u32 x) { return x * 1024 * 1024; }