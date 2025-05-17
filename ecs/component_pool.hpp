#pragma once

#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <mutex>
#include "allocator.hpp"
#include "utils.hpp"
template <typename T>
class ComponentPool
{
public:
    explicit ComponentPool(LinearAllocator &allocator, u32 capacity)
        : capacity_{capacity},
          size_{0}
    {
        data_ = allocator.alloc<T>(capacity_);
        dense_ = allocator.alloc<u32>(capacity_);
        sparse_ = allocator.alloc<int>(capacity_);
        for (int i = 0; i < capacity_; ++i)
        {
            dense_[i] = 0;
            sparse_[i] = -1;
        }
    }

    T *assign(u32 entity_id)
    {
        if (sparse_[entity_id] != -1)
            return nullptr;
        u32 index = size_;
        {
            std::unique_lock lock(shared_mutex_);
            ++size_;
            dense_[index] = entity_id;
            sparse_[entity_id] = index;
            std::memset(&data_[index], 0, sizeof(T));
        }
        return &data_[index];
    }

    void remove(u32 entity_id)
    {
        int index = sparse_[entity_id];
        if (index == -1)
            return;
        std::unique_lock lock(shared_mutex_);
        u32 last = size_ - 1;
        u32 last_entity = dense_[last];
        data_[index] = data_[last];
        dense_[index] = last_entity;
        sparse_[last_entity] = index;
        sparse_[entity_id] = -1;
        --size_;
    }

    T *get(u32 entity_id)
    {
        if (sparse_[entity_id] == -1)
            return nullptr;
        std::shared_lock lock(shared_mutex_);
        return &data_[sparse_[entity_id]];
    }

    inline u32 capacity() const
    {
        return capacity_;
    }

    inline u32 size() const
    {
        return size_;
    }

private:
    u32 size_, capacity_;
    T *data_;
    u32 *dense_;
    int *sparse_;
    std::shared_mutex shared_mutex_;
};