#pragma once

#include "align.hpp"
#include "allocator.hpp"
#include "types.hpp"
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace lotus {
/**
 * @brief Base class for fixed region-style allocators.
 *
 * Allocates memory linearly from a pre-allocated block.
 */
class RegionAllocator : public Allocator {
public:
	using Allocator::alloc;

	RegionAllocator(const RegionAllocator&)            = delete;
	RegionAllocator& operator=(const RegionAllocator&) = delete;
	RegionAllocator(RegionAllocator&&)                 = delete;
	RegionAllocator& operator=(RegionAllocator&&)      = delete;

	size_t remaining() const override
	{
		return size_ - allocated();
	}

	size_t allocated() const override
	{
		return top_ - base_;
	}

	/**
	 * @brief Returns the current top of the allocation region.
	 */
	uptr top() const
	{
		return top_;
	}

	/**
	 * @brief Returns the aligned base of the memory region.
	 */
	uptr base() const
	{
		return base_;
	}

	/**
	 * @brief Resets the allocator to its base pointer.
	 * All previous allocations become invalid.
	 */
	virtual void reset()
	{
		top_ = base_;
	}

	/**
	 * @brief Returns the total size of the region.
	 */
	size_t capacity() const
	{
		return size_;
	}

	/**
	 * @brief Returns true if the pointer lies within the memory region.
	 */
	bool contains(void* ptr) const
	{
		uptr p = reinterpret_cast<uintptr_t>(ptr);
		return p >= base_ && p < base_ + size_;
	}

protected:
	RegionAllocator(size_t size, size_t alignment) : size_{size}
	{
		if (!is_pow2(alignment)) {
			throw std::invalid_argument("RegionAllocator alignment must be a power of two");
		}
		if (size > static_cast<size_t>(-1) - alignment) {
			throw std::bad_alloc();
		}

		size_t total_space = size + alignment;
		raw_               = std::malloc(total_space);
		if (!raw_) {
			throw std::bad_alloc();
		}

		void* aligned = raw_;
		size_t space  = total_space;

		if (!std::align(alignment, size, aligned, space)) {
			std::free(raw_);
			throw std::runtime_error("RegionAllocator: std::align failed");
		}

		base_ = reinterpret_cast<uptr>(aligned);
		top_  = base_;
	}

	~RegionAllocator()
	{
		std::free(raw_);
	}

	void* raw_; // Unaligned memory buffer
	uptr base_; // Start of aligned memory region
	uptr top_;  // Current allocation offset
	size_t size_;
};
} // namespace lotus
