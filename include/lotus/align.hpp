#pragma once

#include <cstddef>

namespace lotus {
/**
 * @brief True when value is a non-zero power of two.
 */
constexpr bool is_pow2(size_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

/**
 * @brief Rounds value up to a multiple of alignment.
 * alignment must be a power of two.
 */
constexpr size_t align_up(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace lotus
