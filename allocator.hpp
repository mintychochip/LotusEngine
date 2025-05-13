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
class PoolAllocator
{
public:
    PoolAllocator(u32 count) : capacity_{count}
    {
        u32 alignment = alignof(T);
        std::cout << alignment << std::endl;
        block_size_ = ((sizeof(T) + alignment - 1) / alignment) * alignment;
        size_t total_size = block_size_ * capacity_ + alignment;
        raw_memory_ = new char[total_size];
        void* raw_ptr = static_cast<void*>(raw_memory_);
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

    T *alloc()
    {
        if (head_ == nullptr)
            return nullptr;
        T *ptr = reinterpret_cast<T *>(head_);
        head_ = head_->next;
        return ptr;
    }

    void free(T *ptr)
    {
        if (ptr == nullptr)
            return;
        FreeBlock *block = reinterpret_cast<FreeBlock *>(ptr);
        block->next = head_;
        head_ = block;
    }

    u32 capacity() const
    {
        return capacity_;
    }

private:
    char *raw_memory_;
    char *memory_;
    u32 block_size_, capacity_;
    FreeBlock *head_;
};