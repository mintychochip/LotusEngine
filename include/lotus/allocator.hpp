#pragma once

#include "types.hpp"
#include <iostream>
#include <utility>

namespace lotus {
/**
 * @brief Abstract base class for memory allocators.
 */
class Allocator {
public:
	/**
	 * @brief Allocate a block of memory with the given size and alignment.
	 */
	virtual void* alloc(size_t bytes, size_t alignment) = 0;

	template <typename T>
	T* alloc(size_t count = 1)
	{
		void* memory = alloc(sizeof(T) * count, alignof(T));
		return reinterpret_cast<T*>(memory);
	}

	/**
	 * @brief Construct an object of type T in allocated memory.
	 */
	template <typename T, typename... Args>
	T* construct(Args&&... args)
	{
		void* memory = alloc(sizeof(T), alignof(T));
		return new (memory) T(std::forward<Args>(args)...);
	}

	/**
	 * @brief Returns number of bytes left.
	 */
	virtual size_t remaining() const = 0;

	/**
	 * @brief Returns number of bytes allocated.
	 */
	virtual size_t allocated() const = 0;

	virtual ~Allocator() = default;
};

class Allocation {
public:
	virtual ~Allocation() = default;

private:
	void* memory_;
	size_t size_;
};
} // namespace lotus