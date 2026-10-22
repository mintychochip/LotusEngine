#include "lotus/rpg/game.hpp"

namespace lotus::rpg {

RPGGame::RPGGame(GpuSettings settings) : Application(settings)
{
	register_component_codecs();
}

} // namespace lotus::rpg

#include <memory>
#include "lotus/scene/mesh.hpp"
#include "lotus/gpu/renderer.hpp"


using namespace lotus;
using namespace lotus::rpg;
using namespace lotus::mesh;
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;
using namespace lotus::rpg;

namespace {

/// Creates a player entity with all RPG components.
Entity create_player(Scene& scene, const std::string& name,
                    const glm::vec3& position)
{
    const Entity player = scene.create(name);
    auto& transform = scene.registry().get<Transform>(player);
    transform.position = position;

    scene.registry().emplace<Stats>(player);
    scene.registry().emplace<Health>(player, Health{100, 100, true});
    scene.registry().emplace<Mana>(player, Mana{50, 50});
    scene.registry().emplace<Inventory>(player);
    scene.registry().emplace<Combat>(player, Combat{10, 0, 1.0f, 0.0f,
                                                    false, 8.0f, 2.0f,
                                                    NullEntity});
    scene.registry().emplace<Dialogue>(player);
    scene.registry().emplace<Quest>(player);
    scene.registry().emplace<CharacterController>(player);
    scene.registry().emplace<Interactable>(player, Interactable{3.0f,
                                                                "Press E to interact",
                                                                true});

    // Starting items
    auto& inventory = scene.registry().get<Inventory>(player);
    inventory.add(Item{"Iron Sword", "A sturdy iron sword.", "weapon",
                       50, 1, 1, 5, 0, 0, 0});
    inventory.add(Item{"Health Potion", "Restores 30 HP.", "consumable",
                       10, 10, 1, 0, 0, 30, 0});
    inventory.add(Item{"Mana Potion", "Restores 20 MP.", "consumable",
                       10, 10, 1, 0, 0, 0, 20});
    inventory.gold = 100;

    return player;
}

/// Creates an NPC with dialogue.
Entity create_npc(Scene& scene, const std::string& name,
                  const glm::vec3& position, const std::string& role,
                  const std::vector<std::string>& dialogue_lines)
{
    const Entity npc = scene.create(name);
    auto& transform = scene.registry().get<Transform>(npc);
    transform.position = position;

    scene.registry().emplace<NPC>(npc, NPC{name, role, "village", true});
    scene.registry().emplace<Interactable>(npc, Interactable{3.0f,
                                                             "Press E to talk",
                                                             true});
    scene.registry().emplace<Dialogue>(npc);
    auto& dialogue = scene.registry().get<Dialogue>(npc);
    dialogue.npc_name = name;
    dialogue.lines = dialogue_lines;

    // Visual representation
    scene.registry().emplace<MeshSource>(npc, MeshSource{"builtin:cylinder", ""});
    scene.registry().emplace<Material>(npc, Material{{0.3f, 0.6f, 0.9f},
                                                     "", 0.5f});

    return npc;
}

/// Creates an enemy with AI and combat.
Entity create_enemy(Scene& scene, const std::string& name,
                    const glm::vec3& position, int health, int damage)
{
    const Entity enemy = scene.create(name);
    auto& transform = scene.registry().get<Transform>(enemy);
    transform.position = position;

    scene.registry().emplace<Stats>(enemy, Stats{1, 0, 100, 8, 8, 5, 8});
    scene.registry().emplace<Health>(enemy, Health{health, health, true});
    scene.registry().emplace<Combat>(enemy, Combat{damage, 2, 1.5f, 0.0f,
                                                    false, 10.0f, 2.0f,
                                                    NullEntity});
    scene.registry().emplace<AI>(enemy);
    scene.registry().emplace<Loot>(enemy);

    auto& loot = scene.registry().get<Loot>(enemy);
    loot.items.push_back(Item{"Monster Hide", "A tough hide.", "misc",
                               5, 5, 1, 0, 0, 0, 0});
    loot.gold = 10;

    // Visual representation
    scene.registry().emplace<MeshSource>(enemy, MeshSource{"builtin:sphere", ""});
    scene.registry().emplace<Material>(enemy, Material{{0.9f, 0.3f, 0.3f},
                                                       "", 0.5f});

    return enemy;
}

/// Creates a lootable chest.
Entity create_chest(Scene& scene, const std::string& name,
                    const glm::vec3& position)
{
    const Entity chest = scene.create(name);
    auto& transform = scene.registry().get<Transform>(chest);
    transform.position = position;

    scene.registry().emplace<Interactable>(chest, Interactable{2.5f,
                                                                "Press E to open",
                                                                true});
    scene.registry().emplace<Loot>(chest);

    auto& loot = scene.registry().get<Loot>(chest);
    loot.items.push_back(Item{"Gold Ring", "A shiny gold ring.", "misc",
                               50, 1, 1, 0, 0, 0, 0});
    loot.items.push_back(Item{"Health Potion", "Restores 30 HP.", "consumable",
                               10, 10, 1, 0, 0, 30, 0});
    loot.gold = 25;

    // Visual representation
    scene.registry().emplace<MeshSource>(chest, MeshSource{"builtin:cube", ""});
    scene.registry().emplace<Material>(chest, Material{{0.8f, 0.6f, 0.2f},
                                                       "", 0.3f});

    return chest;
}

/// Creates a quest item on the ground.
Entity create_quest_item(Scene& scene, const std::string& name,
                         const glm::vec3& position)
{
    const Entity item = scene.create(name);
    auto& transform = scene.registry().get<Transform>(item);
    transform.position = position;

    scene.registry().emplace<Interactable>(item, Interactable{2.0f,
                                                               "Press E to pick up",
                                                               true});
    scene.registry().emplace<Loot>(item);

    auto& loot = scene.registry().get<Loot>(item);
    loot.items.push_back(Item{name, "A quest item.", "quest", 0, 1, 1,
                               0, 0, 0, 0});

    // Visual representation
    scene.registry().emplace<MeshSource>(item, MeshSource{"builtin:pyramid", ""});
    scene.registry().emplace<Material>(item, Material{{0.9f, 0.9f, 0.3f},
                                                       "", 0.2f});

    return item;
}

} // namespace

namespace lotus::rpg {

void RPGGame::on_ready(Window& /*window*/, GpuRenderer& gpu)
{
    gpu.lighting().direction = {0.5f, 0.9f, 0.4f};
    gpu.lighting().intensity = 1.0f;
    gpu.lighting().ambient   = {0.12f, 0.13f, 0.17f};
    gpu.lighting().exposure = 1.0f;

    // Load or create the demo scene
    std::string error;
    if (!load_scene_by_name("rpg_demo", &error)) {
        console().info("creating demo scene: " + error);
        scenes().activate("rpg_demo");
        create_demo_scene();
        adopt_scene_path(scene_file_for("rpg_demo"));
        bind_active_scene();
        // Save the demo scene now that the path is set
        std::string save_error;
        if (save_scene_now(&save_error)) {
            console().info("Demo scene saved");
        } else {
            console().warn("Could not save demo scene: " + save_error);
        }
    }

    // Startup hint
    console().info("=== LOTUS RPG ===");
    console().info("Click the green PLAY button in the editor to start!");
    console().info("WASD: move | E: interact | Space: attack | F1: editor");

    // Create player if not in scene
    Scene* scene = scenes().active();
    if (scene && scene->find("player") == NullEntity) {
        setup_player();
    }

    rpg_ui_.set_quick_slot_callback([this](int consumable_slot_index) {
        Scene* scene = scenes().active();
        if (!scene || player_ == NullEntity ||
            !scene->registry().valid(player_)) {
            return;
        }
        const auto& inventory = scene->registry().get<Inventory>(player_);
        int slot              = 0;
        for (const Item& item : inventory.items) {
            if (item.type != "consumable") {
                continue;
            }
            if (slot != consumable_slot_index) {
                ++slot;
                continue;
            }
            if (!rpg::take_item(*scene, player_, item.name, 1)) {
                console().warn("Could not use item: " + item.name);
                return;
            }
            // Consumables carry their heal in bonus_health; ones without an
            // explicit value (the starting potions) restore 30 HP.
            const int heal_amount =
                    item.bonus_health > 0 ? item.bonus_health : 30;
            rpg::apply_heal(*scene, player_, heal_amount);
            console().info("Used " + item.name + ", restored " +
                           std::to_string(heal_amount) + " HP");
            return;
        }
        console().warn("No consumable in quick slot " +
                       std::to_string(consumable_slot_index));
    });

}

void RPGGame::on_scene_committed(Scene& scene)
{
	player_ = scene.find("player");
}

void RPGGame::on_ui(float /*dt*/)
{
	if (!play_mode_) {
		return;
	}
	Scene* scene = scenes().active();
	if (scene == nullptr || player_ == NullEntity ||
	    !scene->registry().valid(player_)) {
		return;
	}
	rpg_ui_.draw(*scene, player_);
}

void RPGGame::on_update(float dt)
{
    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    if (play_mode_) {
        game_time_ += dt;

        // Run RPG systems
        rpg_system_update(*scene, dt);

        // Camera rotation: right-click drag to orbit around player
        if (Camera3D* camera = scene->active_camera3d()) {
            const float mouse_x   = input().mouse_x();
            const float mouse_y   = input().mouse_y();
            const bool right_down = input().mouse_down(MouseButton::Right);
            if (right_down) {
                if (!camera_mouse_down_) {
                    camera_last_mouse_x_ = mouse_x;
                    camera_last_mouse_y_ = mouse_y;
                    camera_mouse_down_   = true;
                }
                const float dx = (mouse_x - camera_last_mouse_x_) * 0.005f;
                const float dy = (mouse_y - camera_last_mouse_y_) * 0.005f;
                camera_last_mouse_x_ = mouse_x;
                camera_last_mouse_y_ = mouse_y;
                glm::vec3 offset     = camera->position - camera->target;
                const float radius   = glm::length(offset);
                if (radius > 0.001f) {
                    float theta =
                            std::atan2(offset.x, offset.z) - dx;
                    float phi = std::acos(
                            glm::clamp(offset.y / radius, -1.0f, 1.0f));
                    phi = glm::clamp(phi - dy, 0.05f, 3.09f);
                    offset = glm::vec3{
                            radius * std::sin(phi) * std::sin(theta),
                            radius * std::cos(phi),
                            radius * std::sin(phi) * std::cos(theta)};
                    camera->position = camera->target + offset;
                    camera->follow   = false;
                }
            } else {
                camera_mouse_down_ = false;
            }
        }

        // Safety net: reset player if they fall through the world
        if (player_ != NullEntity && scene->registry().valid(player_)) {
            auto& player_transform = scene->registry().get<Transform>(player_);
            if (player_transform.position.y < -10.0f) {
                player_transform.position = {0.0f, 2.0f, 5.0f};
                console().warn("You fell through the world! Resetting position.");
            }
        }


        // Handle game input
        handle_game_input();

        // Check player death
        if (player_ != NullEntity && scene->registry().valid(player_)) {
            const auto& health = scene->registry().get<Health>(player_);
            if (!health.is_alive) {
                exit_play_mode();
                console().warn("You have died! Returning to editor mode.");
            }
        }
    }

    // Rotate the cube prop (existing behavior)
    if (const Entity cube = scene->find("cube");
        cube != NullEntity) {
        scene->registry().get<Transform>(cube).rotation.y += dt * 0.5f;
    }
}

void RPGGame::on_shutdown() {}

void RPGGame::enter_play_mode()
{
    play_mode_ = true;
    editor().set_visible(false);
    console().info("=== PLAY MODE ===");
    console().info("WASD: move | E: interact | I: inventory | Q: quests | C: character");
    console().info("Space: attack | Right-drag: rotate camera | R: reset camera");
    console().info("F1: return to editor");

    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    // Find or create player
    player_ = scene->find("player");
    if (player_ == NullEntity) {
        setup_player();
    }

    // Ensure player has all components
    if (player_ != NullEntity) {
        auto& health = scene->registry().get<Health>(player_);
        health.current = health.max;
        health.is_alive = true;
    }
}

void RPGGame::exit_play_mode()
{
    play_mode_ = false;
    editor().set_visible(true);
    console().info("=== EDITOR MODE ===");
}

void RPGGame::handle_game_input()
{
    Scene* scene = scenes().active();
    if (!scene || player_ == NullEntity || !scene->registry().valid(player_)) {
        return;
    }

    const KeySet& keys = input().keys();
    auto key_pressed = [&](Key key) {
        const auto index = static_cast<std::size_t>(key);
        return keys[index] && !prev_keys_[index];
    };

    // E: interact
    if (key_pressed(Key::E)) {
        const Entity target = rpg::find_nearest_interactable(*scene, player_);
        if (target != NullEntity) {
            if (rpg::is_npc(*scene, target)) {
                rpg::start_dialogue(*scene, target, player_);
            } else if (scene->registry().all_of<Loot>(target)) {
                rpg::loot_container(*scene, player_, target);
                console().info("Looted " +
                              scene->registry().get<Name>(target).value);
            }
        } else if (scene->registry().all_of<Dialogue>(player_)) {
            auto& dialogue = scene->registry().get<Dialogue>(player_);
            if (dialogue.is_active) {
                rpg::advance_dialogue(*scene, player_);
            }
        }
    }

    // Space: attack nearest enemy
    if (key_pressed(Key::Space)) {
        const Entity enemy = rpg::find_nearest_enemy(*scene, player_, 3.0f);
        if (enemy != NullEntity) {
            auto result = rpg::resolve_combat(*scene, player_, enemy);
            if (result.damage_dealt > 0) {
                console().info("Dealt " +
                              std::to_string(result.damage_dealt) + " damage");
            }
            if (result.target_killed) {
                ++kill_count_;
                console().info("Enemy killed! Total: " +
                              std::to_string(kill_count_));
                if (rpg::advance_kill_objectives(*scene, player_, enemy)) {
                    const auto& quest =
                            scene->registry().get<Quest>(player_);
                    for (const QuestObjective& obj : quest.objectives) {
                        console().info("Quest progress: " + obj.description +
                                       " " + std::to_string(obj.current) +
                                       "/" + std::to_string(obj.required));
                    }
                }
            }
        }
    }

    // I: toggle inventory
    if (key_pressed(Key::I)) {
        rpg_ui_.toggle_inventory();
    }

    // Q: toggle quest log
    if (key_pressed(Key::Q)) {
        rpg_ui_.toggle_quest_log();
    }

    // C: toggle character panel
    if (key_pressed(Key::C)) {
        rpg_ui_.toggle_character();
    }

    // R: re-enable camera follow
    if (key_pressed(Key::R)) {
        if (Camera3D* camera = scene->active_camera3d()) {
            camera->follow = true;
            console().info("Camera follow re-enabled");
        }
    }

    // F1: exit play mode
    if (key_pressed(Key::F1)) {
        exit_play_mode();
    }

    // Update previous key state
    prev_keys_ = keys;
}

void RPGGame::create_demo_scene()
{
    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    // Mountainous terrain
    const Entity ground = scene->create("ground");
    auto& ground_transform = scene->registry().get<Transform>(ground);
    ground_transform.position = {0.0f, 0.0f, 0.0f};
    scene->registry().emplace<MeshSource>(ground,
                                           MeshSource{"builtin:terrain", ""});
    scene->registry().emplace<Material>(ground,
                                        Material{{0.35f, 0.45f, 0.25f}, "", 0.85f});
    scene->registry().emplace<RigidBody>(ground,
                                         RigidBody{PhysicsMotionType::Static});
    // builtin:terrain generates a 64x64 grid displaced up to 8 m (mesh.hpp
    // defaults); the heightfield collider matches the visible mountains, so the
    // player walks on the terrain instead of a flat box.
    scene->registry().emplace<HeightFieldCollider>(
            ground, HeightFieldCollider{0.0f, 8.0f});


    // Camera — positioned high above terrain to see the scene
    const Entity camera = scene->create("camera");
    auto& camera_transform = scene->registry().get<Transform>(camera);
    camera_transform.position = {0.0f, 15.0f, 20.0f};
    scene->registry().emplace<Camera3D>(camera, Camera3D{{0.0f, 15.0f, 20.0f},
                                                          {0.0f, 0.0f, 0.0f},
                                                          {0.0f, 1.0f, 0.0f},
                                                          55.0f, 0.1f, 200.0f});
    const Entity wall_n = scene->create("wall_n");
    auto& wall_n_transform = scene->registry().get<Transform>(wall_n);
    wall_n_transform.position = {0.0f, 2.0f, -20.0f};
    wall_n_transform.scale = {20.0f, 4.0f, 1.0f};
    scene->registry().emplace<MeshSource>(wall_n, MeshSource{"builtin:cube", ""});
    scene->registry().emplace<Material>(wall_n,
                                        Material{{0.5f, 0.4f, 0.3f}, "", 0.7f});
    scene->registry().emplace<RigidBody>(wall_n,
                                         RigidBody{PhysicsMotionType::Static});
    scene->registry().emplace<BoxCollider>(wall_n,
                                            BoxCollider{{10.0f, 2.0f, 0.5f}});

    const Entity wall_s = scene->create("wall_s");
    auto& wall_s_transform = scene->registry().get<Transform>(wall_s);
    wall_s_transform.position = {0.0f, 2.0f, 20.0f};
    wall_s_transform.scale = {20.0f, 4.0f, 1.0f};
    scene->registry().emplace<MeshSource>(wall_s, MeshSource{"builtin:cube", ""});
    scene->registry().emplace<Material>(wall_s,
                                        Material{{0.5f, 0.4f, 0.3f}, "", 0.7f});
    scene->registry().emplace<RigidBody>(wall_s,
                                         RigidBody{PhysicsMotionType::Static});
    scene->registry().emplace<BoxCollider>(wall_s,
                                            BoxCollider{{10.0f, 2.0f, 0.5f}});

    const Entity wall_e = scene->create("wall_e");
    auto& wall_e_transform = scene->registry().get<Transform>(wall_e);
    wall_e_transform.position = {20.0f, 2.0f, 0.0f};
    wall_e_transform.scale = {1.0f, 4.0f, 20.0f};
    scene->registry().emplace<MeshSource>(wall_e, MeshSource{"builtin:cube", ""});
    scene->registry().emplace<Material>(wall_e,
                                        Material{{0.5f, 0.4f, 0.3f}, "", 0.7f});
    scene->registry().emplace<RigidBody>(wall_e,
                                         RigidBody{PhysicsMotionType::Static});
    scene->registry().emplace<BoxCollider>(wall_e,
                                            BoxCollider{{0.5f, 2.0f, 10.0f}});

    const Entity wall_w = scene->create("wall_w");
    auto& wall_w_transform = scene->registry().get<Transform>(wall_w);
    wall_w_transform.position = {-20.0f, 2.0f, 0.0f};
    wall_w_transform.scale = {1.0f, 4.0f, 20.0f};
    scene->registry().emplace<MeshSource>(wall_w, MeshSource{"builtin:cube", ""});
    scene->registry().emplace<Material>(wall_w,
                                        Material{{0.5f, 0.4f, 0.3f}, "", 0.7f});
    scene->registry().emplace<RigidBody>(wall_w,
                                         RigidBody{PhysicsMotionType::Static});
    scene->registry().emplace<BoxCollider>(wall_w,
                                            BoxCollider{{0.5f, 2.0f, 10.0f}});

    // Decorative elements
    const Entity tree1 = scene->create("tree1");
    auto& tree1_transform = scene->registry().get<Transform>(tree1);
    tree1_transform.position = {-10.0f, 0.0f, -10.0f};
    tree1_transform.scale = {1.0f, 3.0f, 1.0f};
    scene->registry().emplace<MeshSource>(tree1,
                                           MeshSource{"builtin:cylinder", ""});
    scene->registry().emplace<Material>(tree1,
                                        Material{{0.4f, 0.25f, 0.1f}, "", 0.9f});

    const Entity tree2 = scene->create("tree2");
    auto& tree2_transform = scene->registry().get<Transform>(tree2);
    tree2_transform.position = {12.0f, 0.0f, 8.0f};
    tree2_transform.scale = {1.0f, 2.5f, 1.0f};
    scene->registry().emplace<MeshSource>(tree2,
                                           MeshSource{"builtin:cylinder", ""});
    scene->registry().emplace<Material>(tree2,
                                        Material{{0.4f, 0.25f, 0.1f}, "", 0.9f});

    const Entity rock1 = scene->create("rock1");
    auto& rock1_transform = scene->registry().get<Transform>(rock1);
    rock1_transform.position = {5.0f, 0.3f, -8.0f};
    rock1_transform.scale = {0.8f, 0.6f, 0.8f};
    scene->registry().emplace<MeshSource>(rock1,
                                           MeshSource{"builtin:sphere", ""});
    scene->registry().emplace<Material>(rock1,
                                        Material{{0.5f, 0.5f, 0.5f}, "", 0.9f});

    const Entity rock2 = scene->create("rock2");
    auto& rock2_transform = scene->registry().get<Transform>(rock2);
    rock2_transform.position = {-7.0f, 0.2f, 12.0f};
    rock2_transform.scale = {0.5f, 0.4f, 0.5f};
    scene->registry().emplace<MeshSource>(rock2,
                                           MeshSource{"builtin:sphere", ""});
    scene->registry().emplace<Material>(rock2,
                                        Material{{0.5f, 0.5f, 0.5f}, "", 0.9f});

    // Torch posts
    const Entity torch1 = scene->create("torch1");
    auto& torch1_transform = scene->registry().get<Transform>(torch1);
    torch1_transform.position = {-5.0f, 1.5f, 0.0f};
    scene->registry().emplace<MeshSource>(torch1,
                                           MeshSource{"builtin:cylinder", ""});
    scene->registry().emplace<Material>(torch1,
                                        Material{{0.8f, 0.4f, 0.1f}, "", 0.3f});

    const Entity torch2 = scene->create("torch2");
    auto& torch2_transform = scene->registry().get<Transform>(torch2);
    torch2_transform.position = {5.0f, 1.5f, 0.0f};
    scene->registry().emplace<MeshSource>(torch2,
                                           MeshSource{"builtin:cylinder", ""});
    scene->registry().emplace<Material>(torch2,
                                        Material{{0.8f, 0.4f, 0.1f}, "", 0.3f});

    // Setup game entities
    setup_player();
    setup_npcs();
    setup_enemies();
    setup_items();
    setup_quests();

}

void RPGGame::setup_player()
{
    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    player_ = scene->find("player");
    if (player_ == NullEntity) {
        player_ = create_player(*scene, "player", {0.0f, 0.0f, 5.0f});
    }
    if (!scene->registry().all_of<MeshSource>(player_)) {
        scene->registry().emplace<MeshSource>(
                player_, MeshSource{"builtin:cylinder", ""});
        scene->registry().emplace<Material>(
                player_, Material{{0.2f, 0.75f, 0.35f}, "", 0.4f});
    }
}

void RPGGame::setup_npcs()
{
    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    // Quest giver
    if (scene->find("elder") == NullEntity) {
        create_npc(*scene, "elder", {-3.0f, 0.0f, -3.0f}, "quest_giver",
                   {"Greetings, young adventurer.",
                    "Our village is in danger. Monsters have been attacking.",
                    "Will you help us defeat them?",
                    "Bring me proof of your deeds and I shall reward you."});
    }

    // Merchant
    if (scene->find("merchant") == NullEntity) {
        create_npc(*scene, "merchant", {4.0f, 0.0f, -2.0f}, "merchant",
                   {"Welcome to my shop!",
                    "I have potions and equipment for sale.",
                    "Come back anytime, traveler."});
    }

    // Guard
    if (scene->find("guard") == NullEntity) {
        create_npc(*scene, "guard", {0.0f, 0.0f, -8.0f}, "guard",
                   {"Halt! State your business.",
                    "The village is under my protection.",
                    "Move along, citizen."});
    }

    // Villager
    if (scene->find("villager") == NullEntity) {
        create_npc(*scene, "villager", {-6.0f, 0.0f, 4.0f}, "villager",
                   {"Nice weather today, isn't it?",
                    "I heard there are treasures hidden around here.",
                    "Be careful out there!"});
    }
}

void RPGGame::setup_enemies()
{
    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    // Goblins
    if (scene->find("goblin1") == NullEntity) {
        create_enemy(*scene, "goblin1", {8.0f, 0.0f, -6.0f}, 30, 8);
    }
    if (scene->find("goblin2") == NullEntity) {
        create_enemy(*scene, "goblin2", {-12.0f, 0.0f, -8.0f}, 30, 8);
    }
    if (scene->find("goblin3") == NullEntity) {
        create_enemy(*scene, "goblin3", {15.0f, 0.0f, 10.0f}, 30, 8);
    }

    // Orc (tougher)
    if (scene->find("orc1") == NullEntity) {
        create_enemy(*scene, "orc1", {-15.0f, 0.0f, 15.0f}, 60, 15);
    }
}

void RPGGame::setup_items()
{
    Scene* scene = scenes().active();
    if (!scene) {
        return;
    }

    // Chests
    if (scene->find("chest1") == NullEntity) {
        create_chest(*scene, "chest1", {3.0f, 0.0f, 3.0f});
    }
    if (scene->find("chest2") == NullEntity) {
        create_chest(*scene, "chest2", {-8.0f, 0.0f, -5.0f});
    }

    // Quest items
    if (scene->find("ancient_amulet") == NullEntity) {
        create_quest_item(*scene, "ancient_amulet", {10.0f, 0.5f, -12.0f});
    }
}

void RPGGame::setup_quests()
{
    Scene* scene = scenes().active();
    if (!scene || player_ == NullEntity) {
        return;
    }

    // Main quest: kill goblins
    Quest kill_quest;
    kill_quest.id = "kill_goblins";
    kill_quest.name = "Goblin Threat";
    kill_quest.description = "Defeat the goblins attacking the village.";
    kill_quest.objectives.push_back(
            QuestObjective{"Defeat goblins", 3, 0});
    kill_quest.xp_reward = 100;
    kill_quest.gold_reward = 50;
    kill_quest.item_rewards.push_back(
            Item{"Goblin Slayer Blade", "A sharp blade.", "weapon",
                 100, 1, 1, 10, 0, 0, 0});

    auto& quest = scene->registry().get<Quest>(player_);
    quest = kill_quest;
    quest.state = QuestState::Active;
}

} // namespace lotus::rpg
