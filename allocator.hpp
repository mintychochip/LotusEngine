#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <stdlib.h>
#include <iostream>
#include <vector>

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
            block_size_ = sizeof(T);
            memory_ = new char[capacity_ * block_size_];
            freelist_.reserve(capacity_);
            for (int i = 0; i < count; ++i) {
                freelist_.emplace_back(memory_ + i * block_size_);
            }
            freeindex_ = 0;
        }
        ~PoolAllocator()
        {
            delete[] memory_;
        }

        T* alloc()
        {
            if (freeindex_ >= capacity_)
                return nullptr;
            char* result = freelist_[freeindex_];
            ++freeindex_;
            return reinterpret_cast<T*>(result);
        }

        void free(T* ptr)
        {
            if (freeindex_ == 0)
                return;
            --freeindex_;
            freelist_[freeindex_] = reinterpret_cast<char*>(ptr);
        }
    private:
        char* memory_;
        u32 block_size_, freeindex_, capacity_;
        std::vector<char*> freelist_;
};