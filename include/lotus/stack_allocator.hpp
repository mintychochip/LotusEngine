#pragma once

#include "region_allocator.hpp"

namespace lotus {
/**
 * @brief Stack-style allocator with LIFO deallocation.
 * Supports pushing and popping allocation markers.
 */
template <size_t MaxAllocations>
class StackAllocator : public RegionAllocator {
	static_assert(MaxAllocations > 0, "StackAllocator needs room for at least one marker");

public:
	using Allocator::alloc;
	explicit StackAllocator(size_t size, size_t alignment)
	        : RegionAllocator{size, alignment},
	          marker_index_{0}
	{}

	/**
	 * @brief Allocates memory with alignment and stores a marker.
	 *
	 * A marker is stored only when the cursor moves. alloc(0) on an
	 * already-aligned cursor does not consume a marker.
	 */
	void* alloc(size_t bytes, size_t alignment) override
	{
		if (!is_pow2(alignment)) {
			return nullptr;
		}

		const uptr mask    = static_cast<uptr>(alignment) - 1;
		const uptr aligned = (top_ + mask) & ~mask;
		if (aligned < top_) {
			return nullptr;
		}
		const size_t padding = static_cast<size_t>(aligned - top_);
		if (padding > remaining() || bytes > remaining() - padding) {
			return nullptr;
		}
		if (bytes == 0 && padding == 0) {
			return reinterpret_cast<void*>(aligned);
		}
		if (marker_index_ >= MaxAllocations) {
			return nullptr;
		}

		markers[marker_index_] = static_cast<size_t>(top_ - base_);
		++marker_index_;
		top_ = aligned + bytes;
		return reinterpret_cast<void*>(aligned);
	}

	/**
	 * @brief Pops the last allocation, reverting to the previous marker.
	 * Does nothing when the stack is empty. Does not run destructors.
	 */
	void pop()
	{
		if (marker_index_ == 0) {
			return;
		}
		--marker_index_;
		top_ = base_ + markers[marker_index_];
	}

	/**
	 * @brief Rewinds the region and drops every marker.
	 */
	void reset() override
	{
		RegionAllocator::reset();
		marker_index_ = 0;
	}

private:
	size_t markers[MaxAllocations]{}; // Offsets of the cursor before each allocation
	size_t marker_index_;             // Number of stored markers
};
} // namespace lotus
