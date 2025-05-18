#pragma once

#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include "allocator.hpp"
#include "utils.hpp"

template <typename Tag>
struct ComponentIDGenerator
{
    using u32 = uint32_t;
    template <typename T>
    u32 component_id()
    {
        static u32 id = next_id++;
        return id;
    }
    u32 next_id {0};
};

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
        for (int i = 0; i < capacity_; ++i)
        {
            dense_[i] = 0;
            sparse_[i] = -1;
        }
    }

    ComponentPool(ComponentPool&& moved) noexcept 
        : size_{moved.size_},
        capacity_{moved.capacity_},
        component_size_{moved.component_size_},
        component_alignment_{moved.component_alignment_},
        data_{moved.data_},
        dense_{moved.dense_},
        sparse_{moved.sparse_}
    {
        moved.size_ = 0;
        moved.capacity_ = 0;
        moved.data_ = nullptr;
        moved.dense_ = nullptr;
        moved.sparse_ = nullptr;
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

    inline u32 capacity() const
    {
        return capacity_;
    }

    inline u32 size()
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

class ComponentManager
{
    struct Tag {};
public:
    ComponentManager(LinearAllocator &allocator, u32 capacity) : allocator_{allocator}, capacity_{capacity} {}

    template <typename... Ts>
    std::enable_if_t<(sizeof...(Ts) > 1), std::tuple<Ts*...>>
    assign(u32 entity_id)
    {
        return std::make_tuple(assign<Ts>(entity_id)...);
    }

    template <typename T>
    T *assign(u32 entity_id)
    {
        u32 id = generator.component_id<T>();
        if (id >= pools_.size())
            pools_.emplace_back(allocator_,capacity_,sizeof(T),alignof(T));
        char* ptr = pools_[id].assign(entity_id);
        return reinterpret_cast<T*>(ptr);
    }

    template <typename T>
    T *get(u32 entity_id)
    {
        u32 id = generator.component_id<T>();
        char *ptr = pools_[id].get(entity_id);
        return reinterpret_cast<T*>(ptr);
    }

    template <typename T>
    void remove(u32 entity_id)
    {
        u32 id = generator.component_id<T>();
        pools_[id].remove(entity_id);
    }

    inline auto begin()
    {
        return pools_.begin();
    }

    inline auto end()
    {
        return pools_.end();
    }

private:
    LinearAllocator &allocator_;
    ComponentIDGenerator<Tag> generator;
    std::vector<ComponentPool> pools_;
    u32 capacity_;
};

