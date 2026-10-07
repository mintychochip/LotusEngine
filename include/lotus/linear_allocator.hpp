#pragma once

#include "region_allocator.hpp"

namespace lotus {
/**
 * @brief Bump allocator over a fixed region.
 *
 * Individual allocations cannot be freed. reset() rewinds the whole region.
 * Single-threaded. The caller synchronizes.
 */
class LinearAllocator : public RegionAllocator {
public:
	using Allocator::alloc;

	/**
	 * @brief Constructs a LinearAllocator with the given region size.
	 * @param size Bytes available to allocate.
	 * @param alignment Alignment of the region base. Must be a power of two.
	 */
	explicit LinearAllocator(size_t size, size_t alignment)
	        : RegionAllocator{size, alignment}
	{}

	/**
	 * @brief Allocate a block with the given size and alignment.
	 * @param alignment Must be a non-zero power of two.
	 * @return Aligned pointer, or nullptr when the request does not fit.
	 */
	void* alloc(size_t bytes, size_t alignment) override
	{
		if (!is_pow2(alignment)) {
			return nullptr;
		}

		const uptr aligned = align_cursor(top_, alignment);
		if (aligned < top_) {
			return nullptr;
		}
		const size_t padding = static_cast<size_t>(aligned - top_);
		if (padding > remaining() || bytes > remaining() - padding) {
			return nullptr;
		}

		top_ = aligned + bytes;
		return reinterpret_cast<void*>(aligned);
	}

private:
	static uptr align_cursor(uptr cursor, size_t alignment)
	{
		const uptr mask = static_cast<uptr>(alignment) - 1;
		return (cursor + mask) & ~mask;
	}
};
} // namespace lotus
