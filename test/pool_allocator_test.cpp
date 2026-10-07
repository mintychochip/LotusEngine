#include <gtest/gtest.h>

#include "lotus/pool_allocator.hpp"

using namespace lotus;

namespace {

int g_destroy_count = 0;

struct Counter {
	~Counter() { ++g_destroy_count; }
};

} // namespace

TEST(PoolAllocatorTest, ExhaustionAndReuse)
{
	PoolAllocator<int> pool{2};
	int* a = pool.alloc();
	int* b = pool.alloc();
	EXPECT_NE(a, nullptr);
	EXPECT_NE(b, nullptr);
	EXPECT_EQ(pool.alloc(), nullptr);
	pool.free(a);
	int* c = pool.alloc();
	EXPECT_NE(c, nullptr);
	EXPECT_EQ(pool.remaining(), 0u);
}

TEST(PoolAllocatorTest, DestroyRunsDestructorOnce)
{
	g_destroy_count = 0;
	PoolAllocator<Counter> pool{1};
	Counter* object = pool.construct<Counter>();
	ASSERT_NE(object, nullptr);
	pool.destroy(object);
	EXPECT_EQ(g_destroy_count, 1);
	EXPECT_EQ(pool.remaining(), pool.capacity() * pool.stride());
}

TEST(PoolAllocatorTest, OversizedAllocReturnsNull)
{
	PoolAllocator<int> pool{4};
	EXPECT_EQ(pool.alloc(sizeof(int) * 2, alignof(int)), nullptr);
}
