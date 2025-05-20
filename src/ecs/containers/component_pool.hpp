#pragma once

#include <mutex>
#include <shared_mutex>
#include "core/allocator.hpp"

class ComponentPool
{
public:
    explicit ComponentPool(LinearAllocator &allocator, u32 capacity, u32 component_size, u32 component_alignment)
        : capacity_{capacity},
          size_{0},
          component_size_{component_size},
          component_alignment_{component_alignment}
    {
        void *raw_data = allocator.alloc(capacity_ * component_size_, component_alignment_);
        data_ = reinterpret_cast<char *>(raw_data);
        dense_ = allocator.alloc<u32>(capacity_);
        sparse_ = allocator.alloc<int>(capacity_);
        for (u32 i = 0; i < capacity_; ++i)
        {
            dense_[i] = 0;
            sparse_[i] = -1;
        }
    }

    ComponentPool(ComponentPool&& pool) noexcept 
        : size_{pool.size_},
        capacity_{pool.capacity_},
        component_size_{pool.component_size_},
        component_alignment_{pool.component_alignment_},
        data_{pool.data_},
        dense_{pool.dense_},
        sparse_{pool.sparse_}
    {
        pool.size_ = 0;
        pool.capacity_ = 0;
        pool.data_ = nullptr;
        pool.dense_ = nullptr;
        pool.sparse_ = nullptr;
    }

    char *assign(u32 entity_id)
    {
        if (sparse_[entity_id] != -1)
            return nullptr;
        std::unique_lock lock{mutex_};
        u32 index = size_++;
        dense_[index] = entity_id;
        sparse_[entity_id] = index;
        char *dst = data_ + index * component_size_;
        std::memset(dst, 0, component_size_);
        return dst;
    }

    void remove(u32 entity_id)
    {
        int index = sparse_[entity_id];
        if (index == -1)
            return;
        std::unique_lock lock{mutex_};
        u32 last = size_ - 1;
        u32 last_entity = dense_[last];
        std::memcpy(
            data_ + index * component_size_,
            data_ + last * component_size_,
            component_size_);
        dense_[index] = last_entity;
        sparse_[last_entity] = index;
        sparse_[entity_id] = -1;
        --size_;
    }

    char *get(u32 entity_id)
    {
        std::shared_lock lock{mutex_};
        if (sparse_[entity_id] == -1)
            return nullptr;
        return data_ + sparse_[entity_id] * component_size_;
    }

    inline auto capacity() const
    {
        return capacity_;
    }

    inline auto size()
    {
        std::shared_lock lock{mutex_};
        return size_;
    }

    class Iterator
    {
    public:
        Iterator(char *ptr, u32 component_size, std::shared_mutex &mutex_) : ptr_{ptr}, component_size_{component_size}, index_{0}, lock_{mutex_} {}

        Iterator &operator++()
        {
            ++index_;
            ptr_ += component_size_;
            return *this;
        }

        char *operator*() const
        {
            return ptr_;
        }

        bool operator!=(const Iterator &rhs) const
        {
            return ptr_ != rhs.ptr_;
        }

    private:
        char *ptr_;
        u32 component_size_, index_;
        std::shared_lock<std::shared_mutex> lock_;
    };

    inline Iterator begin()
    {
        return Iterator{data_, component_size_, mutex_};
    }

    inline Iterator end()
    {
        return Iterator{data_ + size_ * component_size_, component_size_, mutex_};
    }

private:
    u32 size_,               
        capacity_,            
        component_size_,     
        component_alignment_,
        *dense_;            
    char *data_;
    int *sparse_;
    std::shared_mutex mutex_;
};