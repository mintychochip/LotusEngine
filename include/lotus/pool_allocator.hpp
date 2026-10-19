#pragma once

#include "align.hpp"
#include "allocator.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

namespace lotus {
/**
 * @brief Fixed-count pool of T slots.
 *
 * alloc() returns one uninitialized slot. destroy() runs the destructor
 * and returns the slot. Single-threaded. The caller synchronizes.
 * A request for more than one T does not fit.
 */
template <typename T>
class PoolAllocator : public Allocator {
	struct FreeNode {
		FreeNode* next;
	};

	static constexpr size_t kAlignment = alignof(T) > alignof(FreeNode) ? alignof(T) : alignof(FreeNode);
	static constexpr size_t kStride    = align_up(sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode),
	                                               kAlignment);

public:
	using Allocator::alloc;

	explicit PoolAllocator(size_t count)
	        : raw_{nullptr},
	          base_{nullptr},
	          bits_{nullptr},
	          head_{nullptr},
	          count_{count},
	          live_{0}
	{
		if (count_ == 0) {
			return;
		}
		if (kStride == 0 || count_ > static_cast<size_t>(-1) / kStride) {
			throw std::bad_alloc();
		}

		const size_t slot_bytes = kStride * count_;
		const size_t bit_words  = (count_ + 63) / 64;
		const size_t bit_bytes  = bit_words * sizeof(uint64_t);
		if (slot_bytes > static_cast<size_t>(-1) - bit_bytes - kAlignment) {
			throw std::bad_alloc();
		}

		const size_t total = slot_bytes + bit_bytes + kAlignment;
		raw_               = std::malloc(total);
		if (!raw_) {
			throw std::bad_alloc();
		}

		void* aligned = raw_;
		size_t space  = total;
		if (!std::align(kAlignment, slot_bytes, aligned, space)) {
			std::free(raw_);
			raw_ = nullptr;
			throw std::bad_alloc();
		}

		base_ = static_cast<unsigned char*>(aligned);
		bits_ = reinterpret_cast<uint64_t*>(base_ + slot_bytes);
		std::memset(bits_, 0, bit_bytes);

		head_ = nullptr;
		for (size_t index = count_; index-- > 0;) {
			auto* slot = base_ + index * kStride;
			FreeNode node{head_};
			std::memcpy(slot, &node, sizeof(node));
			head_ = reinterpret_cast<FreeNode*>(slot);
		}
	}

	~PoolAllocator() override
	{
		std::free(raw_);
	}

	PoolAllocator(const PoolAllocator&)            = delete;
	PoolAllocator& operator=(const PoolAllocator&) = delete;
	PoolAllocator(PoolAllocator&&)                 = delete;
	PoolAllocator& operator=(PoolAllocator&&)      = delete;

	/**
	 * @brief Returns one uninitialized slot, or nullptr when the pool is empty.
	 */
	T* alloc()
	{
		if (!head_) {
			return nullptr;
		}

		FreeNode node{};
		std::memcpy(&node, head_, sizeof(node));
		T* slot = reinterpret_cast<T*>(head_);
		head_   = node.next;

		const size_t index = index_of(slot);
		bits_[index / 64] |= uint64_t{1} << (index % 64);
		++live_;
#ifndef NDEBUG
		std::memset(static_cast<void*>(slot), 0xCD, kStride);
#endif
		return slot;
	}

	/**
	 * @brief Returns a slot without running its destructor.
	 * free(nullptr) does nothing. A double-free or foreign pointer is ignored.
	 */
	void free(T* ptr)
	{
		if (!ptr) {
			return;
		}
		if (!owns(ptr) || !live(index_of(ptr))) {
			assert(false && "lotus allocator");
			return;
		}
		release(ptr);
	}

	/**
	 * @brief Runs the destructor once, then frees the slot.
	 */
	void destroy(T* ptr)
	{
		if (!ptr) {
			return;
		}
		if (!owns(ptr) || !live(index_of(ptr))) {
			assert(false && "lotus allocator");
			return;
		}
		ptr->~T();
		release(ptr);
	}

	void* alloc(size_t bytes, size_t alignment) override
	{
		if (!is_pow2(alignment) || bytes == 0 || bytes > sizeof(T) || alignment > alignof(T)) {
			return nullptr;
		}
		return alloc();
	}

	size_t remaining() const override
	{
		return (count_ - live_) * kStride;
	}

	size_t allocated() const override
	{
		return live_ * kStride;
	}

	/**
	 * @brief Number of slots.
	 */
	size_t capacity() const
	{
		return count_;
	}

	size_t stride() const
	{
		return kStride;
	}

private:
	void release(T* ptr)
	{
		const size_t index = index_of(ptr);
		bits_[index / 64] &= ~(uint64_t{1} << (index % 64));
		--live_;
#ifndef NDEBUG
		std::memset(static_cast<void*>(ptr), 0xDD, kStride);
#endif
		FreeNode node{head_};
		std::memcpy(static_cast<void*>(ptr), &node, sizeof(node));
		head_ = reinterpret_cast<FreeNode*>(ptr);
	}

	bool owns(const void* ptr) const
	{
		if (!ptr || !base_ || count_ == 0) {
			return false;
		}
		const auto* address = static_cast<const unsigned char*>(ptr);
		if (address < base_) {
			return false;
		}
		const size_t offset = static_cast<size_t>(address - base_);
		const size_t bytes  = kStride * count_;
		return offset < bytes && offset % kStride == 0;
	}

	size_t index_of(const void* ptr) const
	{
		return static_cast<size_t>(static_cast<const unsigned char*>(ptr) - base_) / kStride;
	}

	bool live(size_t index) const
	{
		return (bits_[index / 64] >> (index % 64)) & 1ull;
	}

	void* raw_;
	unsigned char* base_;
	uint64_t* bits_;
	FreeNode* head_;
	size_t count_;
	size_t live_;
};
} // namespace lotus
