#include <gtest/gtest.h>

#include <cstring>

#include "lotus/heap_allocator.hpp"

using namespace lotus;

TEST(HeapAllocatorTest, AllocFreeAndReuse)
{
	HeapAllocator heap{4096};
	void* a = heap.alloc(64, 16);
	void* b = heap.alloc(128, 16);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	heap.free(a);
	void* c = heap.alloc(64, 16);
	EXPECT_NE(c, nullptr);
	heap.free(b);
	heap.free(c);
}

TEST(HeapAllocatorTest, InPlaceRealloc)
{
	HeapAllocator heap{512};
	void* block = heap.alloc(64, 16);
	ASSERT_NE(block, nullptr);
	std::memset(block, 0xAB, 64);
	void* grown = heap.realloc(block, 80, 16);
	EXPECT_EQ(grown, block);
	EXPECT_EQ(static_cast<unsigned char*>(block)[0], 0xAB);
	heap.free(grown);
}

TEST(HeapAllocatorTest, ResetReturnsAllMemory)
{
	HeapAllocator heap{512};
	ASSERT_NE(heap.alloc(200, 16), nullptr);
	ASSERT_NE(heap.alloc(200, 16), nullptr);
	heap.reset();
	EXPECT_EQ(heap.allocated(), 0u);
	EXPECT_GT(heap.remaining(), 0u);
}
