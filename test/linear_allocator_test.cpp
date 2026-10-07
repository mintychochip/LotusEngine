#include "lotus/linear_allocator.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <type_traits>

using namespace lotus;

TEST(LinearAllocator, AlignsPowerOfTwoRequests)
{
	LinearAllocator allocator(1024, alignof(std::max_align_t));

	for (size_t alignment = 1; alignment <= 32; alignment *= 2) {
		void* ptr = allocator.alloc(8, alignment);
		ASSERT_NE(ptr, nullptr) << alignment;
		EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0u);
	}
	EXPECT_EQ(allocator.allocated() + allocator.remaining(), allocator.capacity());
}

TEST(LinearAllocator, RejectsNonPowerOfTwoAlignment)
{
	LinearAllocator allocator(256, 16);
	EXPECT_EQ(allocator.alloc(8, 0), nullptr);
	EXPECT_EQ(allocator.alloc(8, 3), nullptr);
	EXPECT_EQ(allocator.alloc(8, 24), nullptr);
	EXPECT_EQ(allocator.allocated(), 0u);
}

TEST(LinearAllocator, ReturnsNullWhenFull)
{
	LinearAllocator allocator(64, 16);
	ASSERT_NE(allocator.alloc(64, 16), nullptr);
	EXPECT_EQ(allocator.alloc(1, 1), nullptr);
}

TEST(LinearAllocator, ResetRewindsTheRegion)
{
	LinearAllocator allocator(128, 16);
	ASSERT_NE(allocator.alloc(80, 8), nullptr);
	allocator.reset();
	EXPECT_EQ(allocator.allocated(), 0u);
	EXPECT_EQ(allocator.remaining(), allocator.capacity());
	EXPECT_NE(allocator.alloc(80, 8), nullptr);
}

TEST(LinearAllocator, ConstructReturnsNullWhenFull)
{
	LinearAllocator allocator(16, 16);
	ASSERT_NE(allocator.alloc(16, 1), nullptr);

	struct Big {
		char bytes[64];
	};
	EXPECT_EQ(allocator.construct<Big>(), nullptr);
}

TEST(LinearAllocator, TypedAllocUsesTypeAlignment)
{
	LinearAllocator allocator(256, 16);
	double* value = allocator.alloc<double>();
	ASSERT_NE(value, nullptr);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(value) % alignof(double), 0u);
	*value = 1.5;
	EXPECT_DOUBLE_EQ(*value, 1.5);
}

TEST(LinearAllocator, IsNotCopyableOrMovable)
{
	static_assert(!std::is_copy_constructible_v<LinearAllocator>);
	static_assert(!std::is_move_constructible_v<LinearAllocator>);
}
