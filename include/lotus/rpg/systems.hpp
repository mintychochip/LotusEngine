#pragma once

#include <string>
#include <vector>

#include "lotus/ecs.hpp"
#include "lotus/rpg/components.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus::rpg {

/// Result of a combat round.
struct CombatResult {
    int damage_dealt{0};
    int damage_taken{0};
    bool target_killed{false};
    bool attacker_killed{false};
};

/// Runs combat between attacker and target. Returns the result.
CombatResult resolve_combat(Scene& scene, Entity attacker, Entity target);

/// Awards XP to the player, handles level-ups.
void award_xp(Scene& scene, Entity player, int amount);

/// Adds an item to an entity's inventory.
void give_item(Scene& scene, Entity entity, const Item& item);

/// Removes an item from an entity's inventory.
bool take_item(Scene& scene, Entity entity, const std::string& item_name,
               int count = 1);

/// Starts a dialogue with an NPC.
void start_dialogue(Scene& scene, Entity npc, Entity player);

/// Advances the current dialogue. Returns false when dialogue ends.
bool advance_dialogue(Scene& scene, Entity player);

/// Starts a quest for the player.
void start_quest(Scene& scene, Entity player, const Quest& quest);

/// Updates quest progress. Call when a relevant event occurs.
void update_quest_progress(Scene& scene, Entity player,
                           const std::string& quest_id,
                           const std::string& objective_desc, int amount = 1);

/// Advances active-quest objectives that track the victim's base name and
/// checks completion. Returns true when any objective advanced. Only the
/// player may advance quests.
bool advance_kill_objectives(Scene& scene, Entity killer, Entity victim,
                             int amount = 1);

/// Checks if a quest is complete and awards rewards.
void check_quest_completion(Scene& scene, Entity player,
                            const std::string& quest_id);

/// Updates AI for all entities with AI component.
void update_ai(Scene& scene, float dt);

/// Updates all combat cooldowns.
void update_combat(Scene& scene, float dt);

/// Updates projectiles.
void update_projectiles(Scene& scene, float dt);

/// Finds the nearest enemy to an entity within range.
Entity find_nearest_enemy(Scene& scene, Entity entity, float range);

/// Finds the nearest interactable entity to the player.
Entity find_nearest_interactable(Scene& scene, Entity player);

/// Checks if an entity is an enemy (has AI + Health).
bool is_enemy(Scene& scene, Entity entity);

/// Checks if an entity is an NPC (has NPC component).
bool is_npc(Scene& scene, Entity entity);

/// Checks if an entity is the player (has Stats + Inventory + Health).
bool is_player(Scene& scene, Entity entity);

/// Gets the player entity (first entity with Stats + Inventory).
Entity get_player(Scene& scene);

/// Calculates total defense including equipment.
int total_defense(Scene& scene, Entity entity);

/// Calculates total attack damage including equipment.
int total_attack(Scene& scene, Entity entity);

/// Applies damage to an entity, considering defense.
void apply_damage(Scene& scene, Entity target, int damage);

/// Heals an entity.
void apply_heal(Scene& scene, Entity target, int amount);

/// Checks if an entity can be interacted with.
bool can_interact(Scene& scene, Entity player, Entity target);

/// Gets interaction prompt text.
std::string get_interact_prompt(Scene& scene, Entity target);

/// Loots a container.
void loot_container(Scene& scene, Entity player, Entity container);

/// Spawns a projectile.
void spawn_projectile(Scene& scene, Entity owner, const glm::vec3& direction,
                      int damage, float speed);

/// Updates all RPG systems. Call once per frame.
void rpg_system_update(Scene& scene, float dt);

} // namespace lotus::rpg
