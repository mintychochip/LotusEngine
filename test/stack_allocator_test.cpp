#include "lotus/stack_allocator.hpp"
#include <cstdint>
#include <gtest/gtest.h>


using namespace lotus;

class StackAllocatorTest : public ::testing::Test {
protected:
	static constexpr size_t kSize      = 1024;
	static constexpr size_t kAlignment = alignof(std::max_align_t);

	StackAllocator<128>* allocator;

	void SetUp() override
	{
		allocator = new StackAllocator<128>(kSize, kAlignment);
	}

	void TearDown() override
	{
		delete allocator;
	}
};

TEST_F(StackAllocatorTest, InitialState)
{
	EXPECT_EQ(allocator->allocated(), 0u);
	EXPECT_EQ(allocator->remaining(), kSize);
}

TEST_F(StackAllocatorTest, AllocateSingleBlock)
{
	void* ptr = allocator->alloc(128, alignof(std::max_align_t));
	ASSERT_NE(ptr, nullptr);
	EXPECT_EQ(allocator->allocated(), 128u);
	EXPECT_EQ(allocator->remaining(), kSize - 128);
}

TEST_F(StackAllocatorTest, MultipleAllocations)
{
	void* p1 = allocator->alloc(100, 8);
	ASSERT_NE(p1, nullptr);
	size_t a1 = allocator->allocated();

	void* p2 = allocator->alloc(200, 8);
	ASSERT_NE(p2, nullptr);
	size_t a2 = allocator->allocated();

	// p2 must come after p1 + 100 at minimum
	EXPECT_GE(reinterpret_cast<uintptr_t>(p2),
	          reinterpret_cast<uintptr_t>(p1) + 100);

	// Allocation increases
	EXPECT_GT(a2, a1);
	EXPECT_EQ(a2 + allocator->remaining(), kSize);
}

TEST_F(StackAllocatorTest, AlignmentCheck)
{
	for (size_t alignment = 8; alignment <= 64; alignment *= 2) {
		void* ptr = allocator->alloc(64, alignment);
		ASSERT_NE(ptr, nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(ptr) % alignment, 0u);
	}
}

TEST_F(StackAllocatorTest, PopDeallocatesLast)
{
	void* p1    = allocator->alloc(128, 8);
	void* p2    = allocator->alloc(128, 8);
	size_t used = allocator->allocated();
	allocator->pop();
	EXPECT_LT(allocator->allocated(), used);
	EXPECT_EQ(allocator->remaining(), kSize - 128);
}

TEST_F(StackAllocatorTest, PopBeyondInitialDoesNothingHarmful)
{
	for (int i = 0; i < 10; ++i) {
		allocator->alloc(32, 8);
	}

	for (int i = 0; i < 15; ++i) {
		allocator->pop(); // extra pops
	}

	EXPECT_GE(allocator->remaining(), kSize - 320);
}

TEST_F(StackAllocatorTest, ResetClearsAllAllocations)
{
	allocator->alloc(300, 8);
	allocator->alloc(200, 8);
	allocator->reset();
	EXPECT_EQ(allocator->allocated(), 0u);
	EXPECT_EQ(allocator->remaining(), kSize);
}

TEST_F(StackAllocatorTest, AllocateZeroBytes)
{
	void* ptr = allocator->alloc(0, 8);
	EXPECT_NE(ptr, nullptr); // still a valid pointer
	EXPECT_EQ(allocator->allocated(), 0u);
}

TEST_F(StackAllocatorTest, AllocateMoreThanAvailableReturnsNull)
{
	void* ptr = allocator->alloc(kSize + 1, 8);
	EXPECT_EQ(ptr, nullptr);
}

TEST_F(StackAllocatorTest, ConstructObject)
{
	struct TestStruct {
		int a   = 42;
		float b = 3.14f;
	};

	TestStruct* obj = allocator->construct<TestStruct>();
	ASSERT_NE(obj, nullptr);
	EXPECT_EQ(obj->a, 42);
	EXPECT_FLOAT_EQ(obj->b, 3.14f);
}

TEST_F(StackAllocatorTest, MaxMarkersLimit)
{
	std::vector<void*> ptrs;
	for (int i = 0; i < 128; ++i) {
		ptrs.push_back(allocator->alloc(4, 4));
		ASSERT_NE(ptrs.back(), nullptr);
	}

	// 129th allocation might fail due to marker limit
	void* ptr = allocator->alloc(4, 4);
	EXPECT_EQ(ptr, nullptr); // depending on internal implementation
}
