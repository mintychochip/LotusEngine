#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "lotus/ecs.hpp"
#include "lotus/scene/components.hpp"

namespace lotus::rpg {

/// Core RPG stats for any character (player or NPC).
struct Stats {
    int level{1};
    int xp{0};
    int xp_to_next{100};
    int strength{10};
    int dexterity{10};
    int intelligence{10};
    int vitality{10};

    int max_health() const { return 50 + vitality * 5 + level * 10; }
    int max_mana() const { return 20 + intelligence * 3 + level * 5; }
    int attack_damage() const { return 5 + strength * 2; }
    int defense() const { return vitality; }
};

/// Health pool. When current reaches 0 the entity dies.
struct Health {
    int current{100};
    int max{100};
    bool is_alive{true};

    void take_damage(int amount)
    {
        current = std::max(0, current - amount);
        if (current == 0) {
            is_alive = false;
        }
    }
    void heal(int amount)
    {
        current = std::min(max, current + amount);
    }
};

/// Mana pool for abilities.
struct Mana {
    int current{50};
    int max{50};

    bool spend(int amount)
    {
        if (current < amount) {
            return false;
        }
        current -= amount;
        return true;
    }
    void restore(int amount) { current = std::min(max, current + amount); }
};

/// Item that can exist in an inventory or on the ground.
struct Item {
    std::string name;
    std::string description;
    std::string type; // "weapon", "armor", "consumable", "quest", "misc"
    int value{0};     // gold value
    int stack_size{1};
    int quantity{1};
    int bonus_damage{0};
    int bonus_defense{0};
    int bonus_health{0};
    int bonus_mana{0};
};

/// Inventory: a collection of items plus gold.
struct Inventory {
    std::vector<Item> items;
    int gold{0};

    void add(const Item& item)
    {
        if (item.stack_size > 1) {
            for (Item& existing : items) {
                if (existing.name == item.name &&
                    existing.quantity < existing.stack_size) {
                    const int space = existing.stack_size - existing.quantity;
                    const int to_add = std::min(space, item.quantity);
                    existing.quantity += to_add;
                    Item remainder = item;
                    remainder.quantity -= to_add;
                    if (remainder.quantity > 0) {
                        items.push_back(remainder);
                    }
                    return;
                }
            }
        }
        items.push_back(item);
    }

    bool remove(const std::string& name, int count = 1)
    {
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (it->name == name) {
                it->quantity -= count;
                if (it->quantity <= 0) {
                    items.erase(it);
                }
                return true;
            }
        }
        return false;
    }

    const Item* find(const std::string& name) const
    {
        for (const Item& item : items) {
            if (item.name == name) {
                return &item;
            }
        }
        return nullptr;
    }

    std::size_t count() const { return items.size(); }
};

/// Quest state machine.
enum class QuestState : std::uint8_t {
    Locked,
    Available,
    Active,
    Completed,
    TurnedIn,
};

struct QuestObjective {
    std::string description;
    int required{1};
    int current{0};
    bool is_completed() const { return current >= required; }
};

struct Quest {
    std::string id;
    std::string name;
    std::string description;
    QuestState state{QuestState::Available};
    std::vector<QuestObjective> objectives;
    int xp_reward{0};
    int gold_reward{0};
    std::vector<Item> item_rewards;

    bool is_complete() const
    {
        for (const QuestObjective& obj : objectives) {
            if (!obj.is_completed()) {
                return false;
            }
        }
        return true;
    }
};

/// Dialogue system for NPC conversations.
struct Dialogue {
    std::string npc_name;
    std::vector<std::string> lines;
    int current_line{0};
    bool is_active{false};

    const std::string& current_text() const
    {
        static const std::string empty;
        if (current_line >= 0 &&
            current_line < static_cast<int>(lines.size())) {
            return lines[current_line];
        }
        return empty;
    }
    bool advance()
    {
        ++current_line;
        if (current_line >= static_cast<int>(lines.size())) {
            is_active = false;
            return false;
        }
        return true;
    }
    void start()
    {
        is_active = true;
        current_line = 0;
    }
};

/// Combat state for real-time combat.
struct Combat {
    int damage{10};
    int defense{0};
    float attack_cooldown{1.0f};
    float cooldown_timer{0.0f};
    bool is_in_combat{false};
    float aggro_range{8.0f};
    float attack_range{2.0f};
    Entity target{NullEntity};

    bool can_attack() const { return cooldown_timer <= 0.0f; }
    void attack()
    {
        cooldown_timer = attack_cooldown;
        is_in_combat = true;
    }
    void update(float dt)
    {
        if (cooldown_timer > 0.0f) {
            cooldown_timer -= dt;
        }
    }
};

/// NPC marker with role and faction.
struct NPC {
    std::string name;
    std::string role;
    std::string faction;
    bool is_interactable{true};
};

/// Loot container (on dead enemies or chests).
struct Loot {
    std::vector<Item> items;
    int gold{0};
    bool is_looted{false};
};

/// Interactable: player can press E near this entity.
struct Interactable {
    float radius{3.0f};
    std::string prompt{"Press E to interact"};
    bool is_active{true};
};

/// AI state for enemies.
enum class AIState : std::uint8_t {
    Idle,
    Patrol,
    Chase,
    Attack,
    Flee,
    Dead,
};

struct AI {
    AIState state{AIState::Idle};
    float aggro_range{10.0f};
    float attack_range{2.0f};
    float move_speed{3.0f};
    Entity target{NullEntity};
    float detection_timer{0.0f};
};

/// Projectile for ranged combat/spells.
struct Projectile {
    int damage{10};
    float speed{20.0f};
    float lifetime{3.0f};
    float age{0.0f};
    Entity owner{NullEntity};
    bool is_active{true};
};

/// Damage flash effect timer.
struct DamageFlash {
    float timer{0.0f};
    float duration{0.15f};
};

/// Experience gain notification.
struct ExperienceGain {
    int amount{0};
    float timer{2.0f};
};

} // namespace lotus::rpg
