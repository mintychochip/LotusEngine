#include "lotus/rpg/systems.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "lotus/scene/components.hpp"

namespace lotus::rpg {
namespace {

/// Distance between two entities on the XZ plane.
float distance_xz(Scene& scene, Entity a, Entity b)
{
    const auto& ta = scene.registry().get<Transform>(a);
    const auto& tb = scene.registry().get<Transform>(b);
    const glm::vec3 delta = ta.position - tb.position;
    return std::sqrt(delta.x * delta.x + delta.z * delta.z);
}

/// Distance between two entities in 3D.
float distance_3d(Scene& scene, Entity a, Entity b)
{
    const auto& ta = scene.registry().get<Transform>(a);
    const auto& tb = scene.registry().get<Transform>(b);
    return glm::distance(ta.position, tb.position);
}

/// Move an entity toward a target on the XZ plane.
void move_toward(Scene& scene, Entity entity, const glm::vec3& target,
                 float speed, float dt)
{
    auto& transform = scene.registry().get<Transform>(entity);
    glm::vec3 delta  = target - transform.position;
    delta.y          = 0.0f;
    const float dist = glm::length(delta);
    if (dist > 0.01f) {
        delta = glm::normalize(delta) * speed * dt;
        transform.position += delta;
    }
}

/// Face an entity toward a target.
void face_toward(Scene& scene, Entity entity, const glm::vec3& target)
{
    auto& transform = scene.registry().get<Transform>(entity);
    const glm::vec3 delta = target - transform.position;
    if (glm::length(delta) > 0.001f) {
        transform.rotation.y = std::atan2(delta.x, delta.z);
    }
}

std::string to_lower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string victim_base_name(Scene& scene, Entity victim)
{
    if (!scene.registry().all_of<Name>(victim)) {
        return {};
    }
    std::string name = scene.registry().get<Name>(victim).value;
    while (!name.empty() &&
           std::isdigit(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    return to_lower(name);
}

bool contains_insensitive(const std::string& haystack,
                        const std::string& needle)
{
    if (needle.empty()) {
        return false;
    }
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

} // namespace

CombatResult resolve_combat(Scene& scene, Entity attacker, Entity target)
{
    CombatResult result;

    if (!scene.registry().valid(attacker) ||
        !scene.registry().valid(target)) {
        return result;
    }

    auto& attacker_combat = scene.registry().get<Combat>(attacker);
    auto& target_health   = scene.registry().get<Health>(target);

    if (!attacker_combat.can_attack() || !target_health.is_alive) {
        return result;
    }

    const int attack_damage = total_attack(scene, attacker);
    const int target_defense = total_defense(scene, target);
    const int damage = std::max(1, attack_damage - target_defense);

    attacker_combat.attack();
    target_health.take_damage(damage);
    result.damage_dealt = damage;

    if (!target_health.is_alive) {
        result.target_killed = true;
        if (auto* ai = scene.registry().try_get<AI>(target)) {
            ai->state = AIState::Dead;
        }
        // Award XP to attacker
        if (is_player(scene, attacker)) {
            award_xp(scene, attacker, 20);
        }
    }

    return result;
}

void award_xp(Scene& scene, Entity player, int amount)
{
    auto& stats = scene.registry().get<Stats>(player);
    stats.xp += amount;

    while (stats.xp >= stats.xp_to_next) {
        stats.xp -= stats.xp_to_next;
        ++stats.level;
        stats.xp_to_next = stats.level * 100;

        // Increase stats on level up
        stats.strength += 2;
        stats.dexterity += 2;
        stats.intelligence += 2;
        stats.vitality += 2;

        // Restore health and mana on level up
        auto& health = scene.registry().get<Health>(player);
        auto& mana   = scene.registry().get<Mana>(player);
        health.max = stats.max_health();
        health.current = health.max;
        mana.max = stats.max_mana();
        mana.current = mana.max;
    }
}

void give_item(Scene& scene, Entity entity, const Item& item)
{
    auto& inventory = scene.registry().get<Inventory>(entity);
    inventory.add(item);
}

bool take_item(Scene& scene, Entity entity, const std::string& item_name,
               int count)
{
    auto& inventory = scene.registry().get<Inventory>(entity);
    return inventory.remove(item_name, count);
}

void start_dialogue(Scene& scene, Entity npc, Entity player)
{
    auto& dialogue = scene.registry().get<Dialogue>(player);
    if (const Dialogue* npc_dialogue =
                scene.registry().try_get<Dialogue>(npc);
        npc_dialogue != nullptr && !npc_dialogue->lines.empty()) {
        dialogue.npc_name = npc_dialogue->npc_name;
        dialogue.lines    = npc_dialogue->lines;
    } else {
        const auto& npc_comp = scene.registry().get<NPC>(npc);
        dialogue.npc_name = npc_comp.name;
        dialogue.lines.clear();
        dialogue.lines.push_back("Greetings, traveler.");
        dialogue.lines.push_back("What brings you to these parts?");
    }
    dialogue.start();
}

bool advance_dialogue(Scene& scene, Entity player)
{
    auto& dialogue = scene.registry().get<Dialogue>(player);
    if (!dialogue.is_active) {
        return false;
    }
    return dialogue.advance();
}

void start_quest(Scene& scene, Entity player, const Quest& quest)
{
    auto& player_quests = scene.registry().get<Quest>(player);
    Quest q = quest;
    q.state = QuestState::Active;
    player_quests = q;
}

void update_quest_progress(Scene& scene, Entity player,
                           const std::string& quest_id,
                           const std::string& objective_desc, int amount)
{
    auto& quest = scene.registry().get<Quest>(player);
    if (quest.id != quest_id || quest.state != QuestState::Active) {
        return;
    }
    for (QuestObjective& obj : quest.objectives) {
        if (obj.description == objective_desc && !obj.is_completed()) {
            obj.current += amount;
        }
    }
}

bool advance_kill_objectives(Scene& scene, Entity killer, Entity victim,
                             int amount)
{
    if (!is_player(scene, killer) || amount <= 0) {
        return false;
    }

    const std::string base = victim_base_name(scene, victim);
    if (base.empty()) {
        return false;
    }

    auto* quest = scene.registry().try_get<Quest>(killer);
    if (quest == nullptr || quest->state != QuestState::Active) {
        return false;
    }

    bool advanced = false;
    for (QuestObjective& obj : quest->objectives) {
        if (obj.is_completed()) {
            continue;
        }
        if (!contains_insensitive(obj.description, base)) {
            continue;
        }
        obj.current = std::min(obj.current + amount, obj.required);
        advanced    = true;
    }

    if (advanced) {
        check_quest_completion(scene, killer, quest->id);
    }
    return advanced;
}

void check_quest_completion(Scene& scene, Entity player,
                            const std::string& quest_id)
{
    auto& quest = scene.registry().get<Quest>(player);
    if (quest.id != quest_id || quest.state != QuestState::Active) {
        return;
    }
    if (quest.is_complete()) {
        quest.state = QuestState::Completed;
        award_xp(scene, player, quest.xp_reward);
        auto& inventory = scene.registry().get<Inventory>(player);
        inventory.gold += quest.gold_reward;
        for (const Item& item : quest.item_rewards) {
            inventory.add(item);
        }
    }
}

void update_ai(Scene& scene, float dt)
{
    const auto view = scene.registry().view<AI, Transform>();
    for (Entity entity : view) {
        auto& ai       = view.get<AI>(entity);

        if (ai.state == AIState::Dead) {
            continue;
        }

        // Find player
        const Entity player = get_player(scene);
        if (player == NullEntity) {
            continue;
        }

        const float dist_to_player = distance_xz(scene, entity, player);

        switch (ai.state) {
        case AIState::Idle:
            if (dist_to_player < ai.aggro_range) {
                ai.state = AIState::Chase;
                ai.target = player;
            }
            break;
        case AIState::Chase:
            if (dist_to_player > ai.aggro_range * 1.5f) {
                ai.state = AIState::Idle;
            } else if (dist_to_player <= ai.attack_range) {
                ai.state = AIState::Attack;
            } else {
                const auto& player_transform =
                        scene.registry().get<Transform>(player);
                move_toward(scene, entity, player_transform.position,
                            ai.move_speed, dt);
                face_toward(scene, entity, player_transform.position);
            }
            break;
        case AIState::Attack:
            if (dist_to_player > ai.attack_range * 1.5f) {
                ai.state = AIState::Chase;
            } else {
                face_toward(scene, entity,
                            scene.registry().get<Transform>(player).position);
                if (auto* combat =
                            scene.registry().try_get<Combat>(entity)) {
                    if (combat->can_attack()) {
                        resolve_combat(scene, entity, player);
                    }
                }
            }
            break;
        case AIState::Flee:
        case AIState::Patrol:
        case AIState::Dead:
            break;
        }
    }
}

void update_combat(Scene& scene, float dt)
{
    const auto view = scene.registry().view<Combat>();
    for (Entity entity : view) {
        view.get<Combat>(entity).update(dt);
    }
}

void update_projectiles(Scene& scene, float dt)
{
    const auto view = scene.registry().view<Projectile, Transform>();
    for (Entity entity : view) {
        auto& proj     = view.get<Projectile>(entity);
        proj.age += dt;
        if (proj.age >= proj.lifetime) {
            scene.destroy(entity);
        }
    }
}

Entity find_nearest_enemy(Scene& scene, Entity entity, float range)
{
    Entity nearest = NullEntity;
    float nearest_dist = range;

    const auto view = scene.registry().view<Health, AI>();
    for (Entity other : view) {
        if (other == entity) {
            continue;
        }
        const float dist = distance_xz(scene, entity, other);
        if (dist < nearest_dist) {
            nearest_dist = dist;
            nearest = other;
        }
    }
    return nearest;
}

Entity find_nearest_interactable(Scene& scene, Entity player)
{
    Entity nearest = NullEntity;
    float nearest_dist = std::numeric_limits<float>::max();

    const auto view = scene.registry().view<Interactable, Transform>();
    for (Entity entity : view) {
        const auto& inter = view.get<Interactable>(entity);
        if (!inter.is_active) {
            continue;
        }
        const float dist = distance_3d(scene, player, entity);
        if (dist < inter.radius && dist < nearest_dist) {
            nearest_dist = dist;
            nearest = entity;
        }
    }
    return nearest;
}

bool is_enemy(Scene& scene, Entity entity)
{
    return scene.registry().all_of<AI, Health>(entity);
}

bool is_npc(Scene& scene, Entity entity)
{
    return scene.registry().all_of<NPC>(entity);
}

bool is_player(Scene& scene, Entity entity)
{
    return scene.registry().all_of<Stats, Inventory, Health>(entity);
}

Entity get_player(Scene& scene)
{
    const auto view = scene.registry().view<Stats, Inventory>();
    for (Entity entity : view) {
        if (is_player(scene, entity)) {
            return entity;
        }
    }
    return NullEntity;
}

int total_defense(Scene& scene, Entity entity)
{
    int defense = 0;
    if (auto* stats = scene.registry().try_get<Stats>(entity)) {
        defense += stats->defense();
    }
    if (auto* combat = scene.registry().try_get<Combat>(entity)) {
        defense += combat->defense;
    }
    if (auto* inventory = scene.registry().try_get<Inventory>(entity)) {
        for (const Item& item : inventory->items) {
            defense += item.bonus_defense;
        }
    }
    return defense;
}

int total_attack(Scene& scene, Entity entity)
{
    int attack = 0;
    if (auto* stats = scene.registry().try_get<Stats>(entity)) {
        attack += stats->attack_damage();
    }
    if (auto* combat = scene.registry().try_get<Combat>(entity)) {
        attack += combat->damage;
    }
    if (auto* inventory = scene.registry().try_get<Inventory>(entity)) {
        for (const Item& item : inventory->items) {
            attack += item.bonus_damage;
        }
    }
    return attack;
}

void apply_damage(Scene& scene, Entity target, int damage)
{
    auto& health = scene.registry().get<Health>(target);
    health.take_damage(damage);
    if (!health.is_alive) {
        if (auto* ai = scene.registry().try_get<AI>(target)) {
            ai->state = AIState::Dead;
        }
    }
}

void apply_heal(Scene& scene, Entity target, int amount)
{
    auto& health = scene.registry().get<Health>(target);
    health.heal(amount);
}

bool can_interact(Scene& scene, Entity player, Entity target)
{
    if (!scene.registry().all_of<Interactable>(target)) {
        return false;
    }
    const auto& inter = scene.registry().get<Interactable>(target);
    if (!inter.is_active) {
        return false;
    }
    return distance_3d(scene, player, target) <= inter.radius;
}

std::string get_interact_prompt(Scene& scene, Entity target)
{
    if (auto* inter = scene.registry().try_get<Interactable>(target)) {
        return inter->prompt;
    }
    return "";
}

void loot_container(Scene& scene, Entity player, Entity container)
{
    auto& loot = scene.registry().get<Loot>(container);
    if (loot.is_looted) {
        return;
    }
    loot.is_looted = true;

    auto& inventory = scene.registry().get<Inventory>(player);
    for (const Item& item : loot.items) {
        give_item(scene, player, item);
    }
    inventory.gold += loot.gold;
}

void spawn_projectile(Scene& scene, Entity owner, const glm::vec3& /*direction*/,
                      int damage, float speed)
{
    const Entity proj = scene.create("projectile");
    scene.registry().emplace<Projectile>(
            proj, Projectile{damage, speed, 3.0f, 0.0f, owner, true});
    const auto& owner_transform =
            scene.registry().get<Transform>(owner);
    auto& proj_transform = scene.registry().get<Transform>(proj);
    proj_transform.position = owner_transform.position;
}

void rpg_system_update(Scene& scene, float dt)
{
    update_combat(scene, dt);
    update_ai(scene, dt);
    update_projectiles(scene, dt);
}

} // namespace lotus::rpg
