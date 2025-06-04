#pragma once

#include "region_allocator.hpp"

namespace lotus {
/**
 * @brief Allocator that allocates memory linearly from a fixed-size region.
 *
 * Does not support deallocation of individual allocations. Memory is allocated
 * in a stack-like fashion from a pre-allocated block.
 */
class LinearAllocator : public RegionAllocator {
public:
	/**
	 * @brief Constructs a LinearAllocator with given size and alignment.
	 * @param size Total size of the memory region.
	 * @param alignment Default alignment to use.
	 */
	explicit LinearAllocator(size_t size, size_t alignment)
	        : RegionAllocator{size, alignment} {};

	/**
	 * @brief Allocate a block of memory with the given size and alignment.
	 * @param bytes Number of bytes to allocate.
	 * @param alignment Alignment requirement (must be odd, though this may
	 * be a logic issue).
	 * @return Pointer to the aligned memory block, or nullptr on failure.
	 */
	void* alloc(size_t bytes, size_t alignment) override
	{
		if (!(alignment & 1)) {
			return nullptr;
		}

		uptr aligned   = (top_ + alignment - 1) & ~(alignment - 1);
		size_t padding = aligned - top_;

		if (remaining() < padding + bytes) {
			return nullptr;
		}

		top_ = aligned + bytes;
		return reinterpret_cast<void*>(aligned);
	}
};
} // namespace lotus
