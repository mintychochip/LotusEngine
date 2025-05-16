#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <memory>
#include "containers.hpp"

using u32 = uint32_t;
class StackAllocator
{
public:
    explicit StackAllocator(u32 size) : size_{size}
    {
        memory_ = new char[size];
        top_ = memory_;
    }
    ~StackAllocator()
    {
        delete[] memory_;
    }
    template <typename T>
    T *alloc()
    {
        auto size = sizeof(T);
        if (top_ + size > memory_ + size_)
        {
            std::cout << "here";
            return nullptr;
        }
        T *ptr = reinterpret_cast<T *>(top_);
        top_ += size;
        return ptr;
    }
    template <typename T>
    void free()
    {
        auto dealloc_size = sizeof(T);
        if (top_ - dealloc_size < memory_)
        {
            return;
        }
        top_ -= dealloc_size;
    }

    u32 size() const
    {
        return size_;
    }

    u32 allocated() const
    {
        return static_cast<u32>(top_ - memory_);
    }

    u32 remaining() const
    {
        return size_ - allocated();
    }

private:
    char *memory_, *top_;
    u32 size_;
};

template <typename T>
union PoolAllocationBlock
{
    T *member;
    FreeBlock *block;
};

template <typename T>
struct PoolAllocation
{
    u32 nblocks;
    T *member;
    FreeBlock *block;
    PoolAllocation(u32 nblocks, PoolAllocationBlock<T> block) : nblocks{nblocks}, member{block.member}, block{block.block} {}    
};

template <typename T>
class PoolAllocator
{
public:
    explicit PoolAllocator(u32 count) : capacity_{count}, allocations_{0}
    {
        u32 alignment = alignof(T);
        block_size_ = std::max(sizeof(T), sizeof(FreeBlock));
        size_t total_size = block_size_ * capacity_ + alignment;
        raw_memory_ = new char[total_size];
        void *raw_ptr = static_cast<void *>(raw_memory_);
        memory_ = static_cast<char *>(std::align(alignment, block_size_ * capacity_, raw_ptr, total_size));

        head_ = nullptr;
        for (u32 i = 0; i < capacity_; ++i)
        {
            FreeBlock *block = reinterpret_cast<FreeBlock *>(memory_ + i * block_size_);
            block->next = head_;
            head_ = block;
        }
    }

    ~PoolAllocator()
    {
        delete[] raw_memory_;
    }

    PoolAllocation<T> alloc(u32 blocks = 1)
    {
        if (head_ == nullptr || blocks + allocations_ > capacity_)
            return {0, {nullptr}};
        FreeBlock *allocation_head = head_;
        FreeBlock *current = head_;
        for (u32 i = 1; i < blocks; ++i)
        {
            current = current->next;
        }
        head_ = current->next;
        current->next = nullptr;
        allocations_ += blocks;
        PoolAllocationBlock<T> ab;
        ab.block = allocation_head;
        return {blocks, ab};
    }

    void free(PoolAllocation<T> allocation)
    {
        if (allocation.block == nullptr || allocation.nblocks == 0)
            return;
        FreeBlock *start = allocation.block;
        FreeBlock *last = start;
        for (u32 i = 1; i < allocation.nblocks; ++i)
        {
            last = last->next;
        }
        last->next = head_;
        head_ = start;
        allocations_ -= allocation.nblocks;
    }

    u32 capacity() const
    {
        return capacity_;
    }

private:
    char *raw_memory_;
    char *memory_;
    u32 block_size_, capacity_, allocations_;
    FreeBlock *head_;
};