#pragma once

#include "region_allocator.hpp"

namespace lotus {
/**
 * @brief Stack-style allocator with LIFO deallocation.
 * Supports pushing and popping allocation markers.
 */
template <size_t MaxAllocations>
class StackAllocator : public RegionAllocator {
public:
	using Allocator::alloc;
	explicit StackAllocator(size_t size, size_t alignment)
	        : RegionAllocator{size, alignment},
	          marker_index_{0}
	{}

	/**
	 * @brief Allocates memory with alignment and stores a marker.
	 */
	void* alloc(size_t bytes, size_t alignment) override
	{
		if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
		    marker_index_ >= MaxAllocations) {
			return nullptr;
		}

		uptr aligned   = (top_ + alignment - 1) & ~(alignment - 1);
		size_t padding = aligned - top_;

		if (remaining() < padding + bytes) {
			return nullptr;
		}

		markers[marker_index_] = static_cast<size_t>(top_ - base_);
		++marker_index_;
		top_ = aligned + bytes;
		return reinterpret_cast<void*>(aligned);
	}

	/**
	 * @brief Pops the last allocation, reverting to previous marker.
	 */
	void pop()
	{
		if (marker_index_ == 0) { // empty
			return;
		}
		top_ = base_ + markers[marker_index_--];
	}

private:
	size_t markers[MaxAllocations]; // Stack of marker offsets
	size_t marker_index_;           // Current marker count
};
} // namespace lotus
