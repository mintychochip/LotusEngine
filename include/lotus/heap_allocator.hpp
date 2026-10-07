#pragma once

#include "align.hpp"
#include "allocator.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <cstdlib>
#include <new>

namespace lotus {
/**
 * @brief Fixed-budget heap. Allocations may be freed in any order.
 *
 * HeapAllocator(size) can satisfy one fresh alloc(size, 16) when size > 0.
 * capacity() is size rounded up to 16 bytes. The backing block does not grow.
 * Single-threaded. The caller synchronizes.
 * allocated() is the sum of outstanding requested sizes. remaining() is the
 * sum of free payload and does not include headers.
 */
class HeapAllocator : public Allocator {
	struct Header {
		uint64_t size_and_flags;
		uint64_t prev_size;
		uint64_t user_size;
		uint32_t magic;
		uint32_t reserved;
	};

	struct FreeNode {
		FreeNode* prev;
		FreeNode* next;
	};

	static constexpr uint64_t kAllocated = 1;
	static constexpr uint64_t kGranule   = 16;
	static constexpr uint64_t kMinBlock  = 48;
	static constexpr uint64_t kSentinel  = sizeof(Header);
	static constexpr uint32_t kMagic     = 0xC0FFEE01u;
	static constexpr int kSmallBins      = 29;
	static constexpr int kLargeLogs      = 62 - 9 + 1;
	static constexpr int kBinCount       = kSmallBins + kLargeLogs * 4;
	static constexpr int kBinWords       = (kBinCount + 63) / 64;

	static_assert(sizeof(Header) == 32, "heap header is the granule unit");
	static_assert(kSentinel == 32, "sentinel is one header");
	static_assert(kMinBlock >= sizeof(Header) + sizeof(FreeNode), "free blocks hold a list node");

public:
	using Allocator::alloc;

	explicit HeapAllocator(size_t size)
	        : raw_{nullptr},
	          region_{nullptr},
	          region_size_{0},
	          free_physical_{0},
	          capacity_{0},
	          live_bytes_{0},
	          free_payload_{0}
	{
		bins_.fill(nullptr);
		nonempty_.fill(0);
		if (size == 0) {
			return;
		}
		if (size > std::numeric_limits<size_t>::max() - (kGranule - 1)) {
			throw std::bad_alloc();
		}

		capacity_            = align_up(size, static_cast<size_t>(kGranule));
		const uint64_t block = worst_physical(capacity_, static_cast<size_t>(kGranule));
		if (block == 0 || block > std::numeric_limits<uint64_t>::max() - kSentinel * 2) {
			throw std::bad_alloc();
		}
		free_physical_ = block;
		region_size_   = static_cast<size_t>(block + kSentinel * 2);
		const size_t total = region_size_ + static_cast<size_t>(kGranule);
		raw_               = std::malloc(total);
		if (!raw_) {
			throw std::bad_alloc();
		}

		void* aligned = raw_;
		size_t space  = total;
		if (!std::align(static_cast<size_t>(kGranule), region_size_, aligned, space)) {
			std::free(raw_);
			raw_ = nullptr;
			throw std::bad_alloc();
		}
		region_ = static_cast<unsigned char*>(aligned);
		build_initial_blocks();
		check();
	}

	~HeapAllocator() override
	{
		std::free(raw_);
	}

	HeapAllocator(const HeapAllocator&)            = delete;
	HeapAllocator& operator=(const HeapAllocator&) = delete;
	HeapAllocator(HeapAllocator&&)                 = delete;
	HeapAllocator& operator=(HeapAllocator&&)      = delete;

	void* alloc(size_t bytes, size_t alignment) override
	{
		if (!is_pow2(alignment) || bytes == 0 || !region_) {
			return nullptr;
		}
		const uint64_t need = worst_physical(bytes, alignment);
		if (need == 0) {
			return nullptr;
		}

		int bin = bin_for_block(need);
		while (bin < kBinCount) {
			const int found = next_nonempty(bin);
			if (found < 0) {
				return nullptr;
			}
			Header* header = header_of(bins_[static_cast<size_t>(found)]);
			if (size_of(header) < needed_physical(header, bytes, alignment)) {
				bin = found + 1;
				continue;
			}
			unlink(header);
			void* ptr = carve(header, bytes, alignment);
			check();
			return ptr;
		}
		return nullptr;
	}

	/**
	 * @brief Frees a block from alloc or realloc.
	 * free(nullptr) does nothing. A double-free or foreign pointer is ignored.
	 */
	void free(void* ptr)
	{
		if (!ptr) {
			return;
		}
		if (!valid_user(ptr)) {
			assert(false && "lotus allocator");
			return;
		}
		release(header_from(ptr));
		check();
	}

	/**
	 * @brief Grows, shrinks, or relocates a block.
	 * realloc(nullptr, bytes, alignment) allocates. realloc(ptr, 0, alignment)
	 * frees ptr and returns nullptr. On failure the old block is unchanged.
	 */
	void* realloc(void* ptr, size_t bytes, size_t alignment)
	{
		if (!ptr) {
			return alloc(bytes, alignment);
		}
		if (!is_pow2(alignment)) {
			return nullptr;
		}
		if (!valid_user(ptr)) {
			assert(false && "lotus allocator");
			return nullptr;
		}
		if (bytes == 0) {
			free(ptr);
			return nullptr;
		}

		Header* header     = header_from(ptr);
		const bool aligned = reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
		if (aligned && bytes <= header->user_size) {
			shrink(header, ptr, bytes);
			check();
			return ptr;
		}
		if (aligned && grow(header, ptr, bytes)) {
			check();
			return ptr;
		}

		const size_t old_bytes = static_cast<size_t>(header->user_size);
		void* fresh            = alloc(bytes, alignment);
		if (!fresh) {
			return nullptr;
		}
		std::memcpy(fresh, ptr, old_bytes);
		free(ptr);
		return fresh;
	}

	/**
	 * @brief Returns the heap to one free block. Does not run destructors.
	 */
	void reset()
	{
		if (!region_) {
			live_bytes_   = 0;
			free_payload_ = 0;
			return;
		}
		bins_.fill(nullptr);
		nonempty_.fill(0);
		live_bytes_   = 0;
		free_payload_ = 0;
		build_initial_blocks();
		check();
	}

	size_t remaining() const override
	{
		return static_cast<size_t>(free_payload_);
	}

	size_t allocated() const override
	{
		return static_cast<size_t>(live_bytes_);
	}

	/**
	 * @brief Usable payload promised at construction, rounded up to 16 bytes.
	 */
	size_t capacity() const
	{
		return capacity_;
	}

private:
	static uint64_t align_up_u64(uint64_t value, uint64_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	static uint64_t size_of(const Header* header)
	{
		return header->size_and_flags & ~kAllocated;
	}

	static int floor_log2(uint64_t value)
	{
		return 63 - static_cast<int>(std::countl_zero(value));
	}

	static int bin_for_block(uint64_t size)
	{
		if (size < 512) {
			if (size < kMinBlock) {
				size = kMinBlock;
			}
			return static_cast<int>((size - kMinBlock) / kGranule);
		}
		if (size >= (uint64_t{1} << 62)) {
			return kBinCount - 1;
		}
		const int log     = floor_log2(size);
		int quarter       = static_cast<int>((size - (uint64_t{1} << log)) >> (log - 2));
		if (quarter > 3) {
			quarter = 3;
		}
		return kSmallBins + (log - 9) * 4 + quarter;
	}

	static uint64_t worst_physical(size_t bytes, size_t alignment)
	{
		const uint64_t base = sizeof(Header) + sizeof(size_t);
		const uint64_t gap  = static_cast<uint64_t>(alignment - 1);
		if (bytes > std::numeric_limits<uint64_t>::max() - base - gap) {
			return 0;
		}
		const uint64_t aligned = align_up_u64(base + bytes + gap, kGranule);
		return aligned < kMinBlock ? kMinBlock : aligned;
	}

	uint64_t needed_physical(const Header* header, size_t bytes, size_t alignment) const
	{
		const auto block    = reinterpret_cast<uintptr_t>(header);
		const auto earliest = block + sizeof(Header) + sizeof(size_t);
		const auto user     = (earliest + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
		const uint64_t back = static_cast<uint64_t>(user - block);
		if (bytes > std::numeric_limits<uint64_t>::max() - back) {
			return std::numeric_limits<uint64_t>::max();
		}
		const uint64_t used = align_up_u64(back + bytes, kGranule);
		return used < kMinBlock ? kMinBlock : used;
	}

	int next_nonempty(int start) const
	{
		if (start < 0 || start >= kBinCount) {
			return -1;
		}
		int word             = start / 64;
		const int bit        = start % 64;
		uint64_t bits        = nonempty_[static_cast<size_t>(word)] >> bit;
		if (bits != 0) {
			const int index = start + static_cast<int>(std::countr_zero(bits));
			return index < kBinCount ? index : -1;
		}
		for (++word; word < kBinWords; ++word) {
			bits = nonempty_[static_cast<size_t>(word)];
			if (bits != 0) {
				const int index = word * 64 + static_cast<int>(std::countr_zero(bits));
				return index < kBinCount ? index : -1;
			}
		}
		return -1;
	}

	Header* header_of(FreeNode* node) const
	{
		return reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(node) - sizeof(Header));
	}

	FreeNode* node_of(Header* header) const
	{
		return reinterpret_cast<FreeNode*>(reinterpret_cast<unsigned char*>(header) + sizeof(Header));
	}

	Header* header_from(void* ptr) const
	{
		const size_t back = reinterpret_cast<size_t*>(ptr)[-1];
		return reinterpret_cast<Header*>(static_cast<unsigned char*>(ptr) - back);
	}

	bool valid_user(void* ptr) const
	{
		if (!region_ || !ptr) {
			return false;
		}
		const auto address = reinterpret_cast<uintptr_t>(ptr);
		const auto region  = reinterpret_cast<uintptr_t>(region_);
		if (address < region + sizeof(size_t) || address >= region + region_size_) {
			return false;
		}
		if (address % alignof(size_t) != 0) {
			return false;
		}
		const size_t back = reinterpret_cast<size_t*>(ptr)[-1];
		if (back < sizeof(Header) + sizeof(size_t) || back > address - region) {
			return false;
		}
		auto* header = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(ptr) - back);
		if (reinterpret_cast<uintptr_t>(header) % kGranule != 0 || header->magic != kMagic) {
			return false;
		}
		if ((header->size_and_flags & kAllocated) == 0 || header->user_size == 0) {
			return false;
		}
		const uint64_t size = size_of(header);
		if (size < kMinBlock || size % kGranule != 0 || back >= size) {
			return false;
		}
		auto* start = reinterpret_cast<unsigned char*>(header);
		if (start < region_ || start + size >= region_ + region_size_) {
			return false;
		}
		const auto* next = reinterpret_cast<Header*>(start + size);
		return next->magic == kMagic && next->prev_size == size;
	}

	void mark_bin(int bin)
	{
		nonempty_[static_cast<size_t>(bin / 64)] |= uint64_t{1} << (bin % 64);
	}

	void clear_bin_if_empty(int bin)
	{
		if (!bins_[static_cast<size_t>(bin)]) {
			nonempty_[static_cast<size_t>(bin / 64)] &= ~(uint64_t{1} << (bin % 64));
		}
	}

	void insert_free(Header* header)
	{
		const uint64_t size    = size_of(header);
		header->size_and_flags = size;
		header->user_size      = 0;
		header->magic          = kMagic;
		const int bin          = bin_for_block(size);
		auto* node             = new (node_of(header)) FreeNode{};
		node->prev             = nullptr;
		node->next             = bins_[static_cast<size_t>(bin)];
		if (node->next) {
			node->next->prev = node;
		}
		bins_[static_cast<size_t>(bin)] = node;
		mark_bin(bin);
		free_payload_ += size - sizeof(Header);
	}

	void unlink(Header* header)
	{
		const uint64_t size = size_of(header);
		const int bin       = bin_for_block(size);
		FreeNode* node      = node_of(header);
		if (node->prev) {
			node->prev->next = node->next;
		} else {
			bins_[static_cast<size_t>(bin)] = node->next;
		}
		if (node->next) {
			node->next->prev = node->prev;
		}
		clear_bin_if_empty(bin);
		free_payload_ -= size - sizeof(Header);
	}

	void build_initial_blocks()
	{
		auto* first = new (region_) Header{};
		first->size_and_flags = kSentinel | kAllocated;
		first->prev_size      = 0;
		first->user_size      = 0;
		first->magic          = kMagic;

		auto* middle = new (region_ + kSentinel) Header{};
		middle->size_and_flags = free_physical_;
		middle->prev_size      = kSentinel;
		middle->user_size      = 0;
		middle->magic          = kMagic;

		auto* last = new (region_ + kSentinel + static_cast<size_t>(free_physical_)) Header{};
		last->size_and_flags = kSentinel | kAllocated;
		last->prev_size      = free_physical_;
		last->user_size      = 0;
		last->magic          = kMagic;

		insert_free(middle);
	}

	void* carve(Header* header, size_t bytes, size_t alignment)
	{
		auto* block            = reinterpret_cast<unsigned char*>(header);
		const uint64_t total   = size_of(header);
		const auto block_addr  = reinterpret_cast<uintptr_t>(block);
		const auto earliest    = block_addr + sizeof(Header) + sizeof(size_t);
		const auto user        = (earliest + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
		const uint64_t back    = static_cast<uint64_t>(user - block_addr);
		uint64_t used          = align_up_u64(back + bytes, kGranule);
		if (used < kMinBlock) {
			used = kMinBlock;
		}

		auto* after = reinterpret_cast<Header*>(block + total);
		if (total - used >= kMinBlock) {
			header->size_and_flags = used | kAllocated;
			auto* rest             = new (block + used) Header{};
			rest->size_and_flags   = total - used;
			rest->prev_size        = used;
			rest->user_size        = 0;
			rest->magic            = kMagic;
			after->prev_size       = total - used;
			insert_free(rest);
		} else {
			header->size_and_flags = total | kAllocated;
		}

		header->user_size = bytes;
		header->magic     = kMagic;
		reinterpret_cast<size_t*>(user)[-1] = static_cast<size_t>(back);
		live_bytes_ += bytes;
#ifndef NDEBUG
		std::memset(reinterpret_cast<void*>(user), 0xCD, bytes);
#endif
		return reinterpret_cast<void*>(user);
	}

	void release(Header* header)
	{
		live_bytes_ -= header->user_size;
		header->user_size = 0;
#ifndef NDEBUG
		const uint64_t payload = size_of(header) - sizeof(Header);
		std::memset(reinterpret_cast<unsigned char*>(header) + sizeof(Header), 0xDD,
		            static_cast<size_t>(payload));
#endif
		header->size_and_flags = size_of(header);

		auto* next = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) + size_of(header));
		if ((next->size_and_flags & kAllocated) == 0) {
			const uint64_t next_size = size_of(next);
			unlink(next);
			const uint64_t grown = size_of(header) + next_size;
			header->size_and_flags = grown;
			auto* after = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) + grown);
			after->prev_size = grown;
		}

		if (header->prev_size != 0) {
			auto* prev = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) - header->prev_size);
			if ((prev->size_and_flags & kAllocated) == 0) {
				const uint64_t prev_size = size_of(prev);
				unlink(prev);
				const uint64_t grown = prev_size + size_of(header);
				prev->size_and_flags = grown;
				auto* after = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(prev) + grown);
				after->prev_size = grown;
				header           = prev;
			}
		}
		insert_free(header);
	}

	bool grow(Header* header, void* ptr, size_t bytes)
	{
		const uint64_t back = reinterpret_cast<size_t*>(ptr)[-1];
		uint64_t used       = align_up_u64(back + bytes, kGranule);
		if (used < kMinBlock) {
			used = kMinBlock;
		}
		const uint64_t total = size_of(header);
		if (used <= total) {
			return false;
		}
		auto* next = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) + total);
		if ((next->size_and_flags & kAllocated) != 0) {
			return false;
		}
		const uint64_t next_size = size_of(next);
		if (total + next_size < used) {
			return false;
		}

		unlink(next);
		const uint64_t combined = total + next_size;
		header->size_and_flags  = combined | kAllocated;
		auto* after = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) + combined);
		after->prev_size = combined;

		if (combined - used >= kMinBlock) {
			header->size_and_flags = used | kAllocated;
			auto* rest             = new (reinterpret_cast<unsigned char*>(header) + used) Header{};
			rest->size_and_flags   = combined - used;
			rest->prev_size        = used;
			rest->user_size        = 0;
			rest->magic            = kMagic;
			after->prev_size       = combined - used;
			insert_free(rest);
		}

		live_bytes_ -= header->user_size;
		live_bytes_ += bytes;
		header->user_size = bytes;
		header->magic     = kMagic;
		return true;
	}

	void shrink(Header* header, void* ptr, size_t bytes)
	{
		live_bytes_ -= header->user_size;
		live_bytes_ += bytes;
		header->user_size      = bytes;
		const uint64_t back    = reinterpret_cast<size_t*>(ptr)[-1];
		uint64_t used          = align_up_u64(back + bytes, kGranule);
		if (used < kMinBlock) {
			used = kMinBlock;
		}
		const uint64_t total = size_of(header);
		if (used >= total || total - used < kMinBlock) {
			return;
		}

		header->size_and_flags = used | kAllocated;
		auto* rest             = new (reinterpret_cast<unsigned char*>(header) + used) Header{};
		rest->size_and_flags   = total - used;
		rest->prev_size        = used;
		rest->user_size        = 0;
		rest->magic            = kMagic;

		auto* after = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) + total);
		if ((after->size_and_flags & kAllocated) == 0) {
			const uint64_t after_size = size_of(after);
			unlink(after);
			const uint64_t grown = (total - used) + after_size;
			rest->size_and_flags = grown;
			auto* tail = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(rest) + grown);
			tail->prev_size = grown;
		} else {
			after->prev_size = total - used;
		}
		insert_free(rest);
	}

	bool audit() const
	{
		if (!region_) {
			return capacity_ == 0 && live_bytes_ == 0 && free_payload_ == 0 && raw_ == nullptr;
		}

		uint64_t seen          = 0;
		uint64_t user_sum      = 0;
		uint64_t free_sum      = 0;
		uint64_t expected_prev = 0;
		bool prev_free         = false;
		bool first             = true;
		auto* header           = reinterpret_cast<Header*>(region_);
		const auto* end        = region_ + region_size_;

		while (reinterpret_cast<unsigned char*>(header) < end) {
			if (reinterpret_cast<uintptr_t>(header) % kGranule != 0 || header->magic != kMagic) {
				return false;
			}
			const uint64_t size = size_of(header);
			if (size == 0 || size % kGranule != 0) {
				return false;
			}
			if (reinterpret_cast<unsigned char*>(header) + size > end) {
				return false;
			}
			if (first) {
				if (header->prev_size != 0) {
					return false;
				}
			} else if (header->prev_size != expected_prev) {
				return false;
			}

			const bool allocated = (header->size_and_flags & kAllocated) != 0;
			if (!allocated && prev_free) {
				return false;
			}
			if (!allocated) {
				if (size < kMinBlock) {
					return false;
				}
				free_sum += size - sizeof(Header);
			} else if (size != kSentinel && size < kMinBlock) {
				return false;
			} else {
				user_sum += header->user_size;
			}

			prev_free     = !allocated;
			expected_prev = size;
			first         = false;
			seen += size;
			header = reinterpret_cast<Header*>(reinterpret_cast<unsigned char*>(header) + size);
		}
		return seen == region_size_ && user_sum == live_bytes_ && free_sum == free_payload_;
	}

	void check() const
	{
#ifndef NDEBUG
		assert(audit() && "lotus allocator");
#endif
	}

	void* raw_;
	unsigned char* region_;
	size_t region_size_;
	uint64_t free_physical_;
	size_t capacity_;
	uint64_t live_bytes_;
	uint64_t free_payload_;
	std::array<FreeNode*, kBinCount> bins_{};
	std::array<uint64_t, kBinWords> nonempty_{};
};
} // namespace lotus
