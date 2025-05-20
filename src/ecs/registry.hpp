#pragma once

#include <cstdint>
#include <cstring>
#include <queue>
#include <vector>
#include "core/allocator.hpp"
#include "core/utils.hpp"
#include "containers/component_pool.hpp"
#include "containers/entity.hpp"

constexpr u32 INVALID_ENTITY_ {UINT32_MAX};

bool is_valid(Entity& entity)
{
    return entity.id != INVALID_ENTITY_;
}

class EntityManager
{
    using u32 = uint32_t;
    public:
        explicit EntityManager(u32 capacity) : capacity_{capacity} {}

        Entity create()
        {
            if (!free_.empty()) {
                u32 free_id = free_.front();
                free_.pop();
                return {free_id};
            }
            size_t size = active_.size();
            if (size >= capacity_) {
                return {UINT32_MAX};
            }
            u32 id = static_cast<u32>(size);
            active_.push_back({id});
            return {id};
        }

        void remove(Entity& e)
        {
            if (!is_valid(e)) {
                return;
            }
            free_.push(e.id);
            for (auto it = active_.begin();it != active_.end();) {
                if (e.id == it->id) {
                    active_.erase(it);
                    break;
                }
                ++it;
            }
            e.id = INVALID_ENTITY_;
        }

        inline auto free() const
        {
            return free_.size();
        }

        inline auto active() const
        {
            return active_.size();
        }

    private:
        u32 capacity_;
        std::queue<u32> free_;
        std::vector<Entity> active_;
};

template <typename Tag>
struct ComponentTagGenerator
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

class ComponentManager
{
    struct Tag {};
public:
    explicit ComponentManager(LinearAllocator &allocator, u32 capacity) : allocator_{allocator}, capacity_{capacity} {}

    template <typename... Ts>
    std::enable_if_t<(sizeof...(Ts) > 1), std::tuple<Ts*...>>
    assign(Entity& e)
    {
        return std::make_tuple(assign<Ts>(e)...);
    }

    template <typename T>
    T *assign(Entity& e)
    {
        if (!is_valid(e)) {
            return nullptr;
        }
        u32 cid = generator_.component_id<T>();
        if (cid >= pools_.size())
            pools_.emplace_back(allocator_,capacity_,sizeof(T),alignof(T));
        char* ptr = pools_[cid].assign(e.id);
        e.mask.set(cid);
        return reinterpret_cast<T*>(ptr);
    }

    template <typename T>
    T *get(Entity& e)
    {
        if (!is_valid(e)) {
            return nullptr;
        }
        u32 cid = generator_.component_id<T>();
        if (!e.mask.test(cid)) {
            return nullptr;
        }
        char *ptr = pools_[cid].get(e.id);
        return reinterpret_cast<T*>(ptr);
    }

    template <typename T>
    void remove(Entity& e)
    {
        if (!is_valid(e)) {
            return;
        }
        u32 cid = generator_.component_id<T>();
        e.mask.reset(cid);
        pools_[cid].remove(e.id);
    }

    void clear(Entity& e)
    {
        if (!is_valid(e)) {
            return;
        }
        for (size_t cid = 0; cid < e.mask.size(); ++cid) {
            if (!e.mask.test(cid)) {
                continue;
            }
            pools_[cid].remove(e.id);
        }
        e.mask.reset();
    }

private:
    LinearAllocator &allocator_;
    ComponentTagGenerator<Tag> generator_;
    std::vector<ComponentPool> pools_;
    u32 capacity_;
};