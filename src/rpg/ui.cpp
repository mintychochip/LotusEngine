#include "lotus/rpg/ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "lotus/rpg/systems.hpp"
#include "lotus/scene/components.hpp"

namespace lotus::rpg {
namespace {

ImVec4 health_color()
{
    return ImVec4{0.85f, 0.2f, 0.2f, 1.0f};
}

ImVec4 mana_color()
{
    return ImVec4{0.2f, 0.4f, 0.9f, 1.0f};
}

ImVec4 xp_color()
{
    return ImVec4{0.9f, 0.8f, 0.2f, 1.0f};
}

ImVec4 bar_background()
{
    return ImVec4{0.15f, 0.15f, 0.15f, 0.8f};
}

void draw_bar(float fraction, const ImVec2& size, const ImVec4& color,
              const char* label)
{
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, bar_background());
    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), size, label);
    ImGui::PopStyleColor(2);
}

} // namespace

void RPGUI::draw(Scene& scene, Entity player)
{
    if (player == NullEntity || !scene.registry().valid(player)) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;

    // HUD: bottom-left health/mana/xp bars
    constexpr float bar_width = 200.0f;
    constexpr float bar_spacing = 4.0f;
    const float hud_x = 20.0f;
    const float hud_y = screen_size.y - 100.0f;

    ImGui::SetNextWindowPos(ImVec2(hud_x, hud_y));
    ImGui::SetNextWindowSize(ImVec2(bar_width + 20, 120));
    ImGui::Begin("RPG_HUD", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs);

    draw_health_bar(scene, player);
    ImGui::Dummy(ImVec2(0, bar_spacing));
    draw_mana_bar(scene, player);
    ImGui::Dummy(ImVec2(0, bar_spacing));
    draw_xp_bar(scene, player);

    ImGui::End();

    // Top-left: character info
    ImGui::SetNextWindowPos(ImVec2(20, 20));
    ImGui::SetNextWindowSize(ImVec2(200, 60));
    ImGui::Begin("RPG_CharInfo", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs);

    const auto& stats = scene.registry().get<Stats>(player);
    const auto& name = scene.registry().get<Name>(player);
    ImGui::Text("%s  Lv.%d", name.value.c_str(), stats.level);
    ImGui::Text("XP: %d / %d", stats.xp, stats.xp_to_next);

    ImGui::End();

    // Top-right: minimap
    draw_minimap(scene, player);

    // Bottom-center: quick slots
    draw_quick_slots(scene, player);

    // Center: interact prompt
    draw_interact_prompt(scene, player);

    // Dialogue box
    draw_dialogue_box(scene, player);

    // Combat text
    draw_combat_text(scene, player);

    // Panels
    if (show_inventory_) {
        draw_inventory_panel(scene, player);
    }
    if (show_quest_log_) {
        draw_quest_log(scene, player);
    }
    if (show_character_) {
        draw_character_panel(scene, player);
    }
}

void RPGUI::draw_health_bar(Scene& scene, Entity player)
{
    const auto& health = scene.registry().get<Health>(player);
    const float fraction = health.max > 0
            ? static_cast<float>(health.current) / health.max
            : 0.0f;
    char label[64];
    std::snprintf(label, sizeof(label), "HP %d / %d", health.current,
                  health.max);
    draw_bar(fraction, ImVec2(200, 20), health_color(), label);
}

void RPGUI::draw_mana_bar(Scene& scene, Entity player)
{
    const auto& mana = scene.registry().get<Mana>(player);
    const float fraction =
            mana.max > 0 ? static_cast<float>(mana.current) / mana.max : 0.0f;
    char label[64];
    std::snprintf(label, sizeof(label), "MP %d / %d", mana.current,
                  mana.max);
    draw_bar(fraction, ImVec2(200, 20), mana_color(), label);
}

void RPGUI::draw_xp_bar(Scene& scene, Entity player)
{
    const auto& stats = scene.registry().get<Stats>(player);
    const float fraction = stats.xp_to_next > 0
            ? static_cast<float>(stats.xp) / stats.xp_to_next
            : 0.0f;
    char label[64];
    std::snprintf(label, sizeof(label), "XP %d / %d", stats.xp,
                  stats.xp_to_next);
    draw_bar(fraction, ImVec2(200, 16), xp_color(), label);
}

void RPGUI::draw_inventory_panel(Scene& scene, Entity player)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;
    const float panel_w = 350.0f;
    const float panel_h = 400.0f;

    ImGui::SetNextWindowPos(
            ImVec2(screen_size.x - panel_w - 20, screen_size.y - panel_h - 20));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h));
    ImGui::Begin("Inventory", nullptr, ImGuiWindowFlags_NoCollapse);

    const auto& inventory = scene.registry().get<Inventory>(player);
    ImGui::Text("Gold: %d", inventory.gold);
    ImGui::Separator();

    if (inventory.items.empty()) {
        ImGui::TextDisabled("Empty");
    } else {
        for (const Item& item : inventory.items) {
            ImGui::PushID(item.name.c_str());
            ImGui::Text("%s", item.name.c_str());
            if (item.quantity > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("x%d", item.quantity);
            }
            ImGui::TextDisabled("  %s", item.description.c_str());
            ImGui::TextDisabled("  Type: %s  Value: %dg", item.type.c_str(),
                                item.value);
            if (item.bonus_damage > 0) {
                ImGui::TextColored(ImVec4(1, 0.5, 0.3, 1),
                                    "  +%d damage", item.bonus_damage);
            }
            if (item.bonus_defense > 0) {
                ImGui::TextColored(ImVec4(0.3, 0.5, 1, 1),
                                    "  +%d defense", item.bonus_defense);
            }
            if (item.bonus_health > 0) {
                ImGui::TextColored(ImVec4(0.3, 1, 0.3, 1),
                                    "  +%d health", item.bonus_health);
            }
            if (item.bonus_mana > 0) {
                ImGui::TextColored(ImVec4(0.3, 0.3, 1, 1),
                                    "  +%d mana", item.bonus_mana);
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }

    ImGui::End();
}

void RPGUI::draw_quest_log(Scene& scene, Entity player)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;
    const float panel_w = 350.0f;
    const float panel_h = 350.0f;

    ImGui::SetNextWindowPos(ImVec2(20, screen_size.y - panel_h - 20));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h));
    ImGui::Begin("Quest Log", nullptr, ImGuiWindowFlags_NoCollapse);

    const auto& quest = scene.registry().get<Quest>(player);

    if (quest.state == QuestState::Locked ||
        quest.state == QuestState::Available) {
        ImGui::TextDisabled("No active quests");
    } else {
        ImGui::Text("%s", quest.name.c_str());
        ImGui::TextDisabled("State: %s",
                            quest.state == QuestState::Active
                                    ? "Active"
                                    : quest.state == QuestState::Completed
                                    ? "Complete"
                                    : "Turned In");
        ImGui::TextWrapped("%s", quest.description.c_str());
        ImGui::Separator();

        for (const QuestObjective& obj : quest.objectives) {
            if (obj.is_completed()) {
                ImGui::TextColored(ImVec4(0.3, 1, 0.3, 1),
                                    "[x] %s (%d/%d)",
                                    obj.description.c_str(), obj.current,
                                    obj.required);
            } else {
                ImGui::TextColored(ImVec4(1, 0.8, 0.3, 1),
                                    "[ ] %s (%d/%d)",
                                    obj.description.c_str(), obj.current,
                                    obj.required);
            }
        }

        ImGui::Separator();
        ImGui::Text("Rewards:");
        ImGui::TextColored(ImVec4(1, 0.8, 0.2, 1), "  %d XP  %d gold",
                            quest.xp_reward, quest.gold_reward);
        for (const Item& item : quest.item_rewards) {
            ImGui::TextColored(ImVec4(0.5, 0.8, 1, 1), "  %s",
                                item.name.c_str());
        }
    }

    ImGui::End();
}

void RPGUI::draw_character_panel(Scene& scene, Entity player)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;
    const float panel_w = 300.0f;
    const float panel_h = 300.0f;

    ImGui::SetNextWindowPos(ImVec2(screen_size.x / 2 - panel_w / 2, 80));
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h));
    ImGui::Begin("Character", nullptr, ImGuiWindowFlags_NoCollapse);

    const auto& stats = scene.registry().get<Stats>(player);
    const auto& health = scene.registry().get<Health>(player);
    const auto& mana   = scene.registry().get<Mana>(player);

    ImGui::Text("Level %d", stats.level);
    ImGui::Separator();
    ImGui::Text("HP: %d / %d", health.current, health.max);
    ImGui::Text("MP: %d / %d", mana.current, mana.max);
    ImGui::Text("XP: %d / %d", stats.xp, stats.xp_to_next);
    ImGui::Separator();
    ImGui::Text("Strength:     %d", stats.strength);
    ImGui::Text("Dexterity:    %d", stats.dexterity);
    ImGui::Text("Intelligence: %d", stats.intelligence);
    ImGui::Text("Vitality:     %d", stats.vitality);
    ImGui::Separator();
    ImGui::Text("Attack:  %d", total_attack(scene, player));
    ImGui::Text("Defense: %d", total_defense(scene, player));

    ImGui::End();
}

void RPGUI::draw_dialogue_box(Scene& scene, Entity player)
{
    const auto* dialogue = scene.registry().try_get<Dialogue>(player);
    if (!dialogue || !dialogue->is_active) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;
    const float box_w = 500.0f;
    const float box_h = 120.0f;

    ImGui::SetNextWindowPos(
            ImVec2(screen_size.x / 2 - box_w / 2, screen_size.y - box_h - 40));
    ImGui::SetNextWindowSize(ImVec2(box_w, box_h));
    ImGui::Begin("Dialogue", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(ImVec4(1, 0.9, 0.5, 1), "%s:",
                        dialogue->npc_name.c_str());
    ImGui::TextWrapped("%s", dialogue->current_text().c_str());
    ImGui::Separator();
    ImGui::TextDisabled("Press E to continue...");

    ImGui::End();
}

void RPGUI::draw_interact_prompt(Scene& scene, Entity player)
{
    const Entity target = find_nearest_interactable(scene, player);
    if (target == NullEntity) {
        return;
    }

    const std::string prompt = get_interact_prompt(scene, target);
    if (prompt.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;

    ImGui::SetNextWindowPos(
            ImVec2(screen_size.x / 2 - 100, screen_size.y / 2 + 50));
    ImGui::SetNextWindowSize(ImVec2(200, 40));
    ImGui::Begin("InteractPrompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs);

    ImGui::TextColored(ImVec4(1, 1, 0.5, 1), "%s", prompt.c_str());

    ImGui::End();
}

void RPGUI::draw_combat_text(Scene& scene, Entity player)
{
    const auto* xp_gain = scene.registry().try_get<ExperienceGain>(player);
    if (!xp_gain || xp_gain->timer <= 0.0f) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;

    ImGui::SetNextWindowPos(
            ImVec2(screen_size.x / 2 - 50, screen_size.y / 2 - 100));
    ImGui::SetNextWindowSize(ImVec2(100, 30));
    ImGui::Begin("CombatText", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoInputs);

    ImGui::TextColored(ImVec4(1, 0.8, 0.2, 1), "+%d XP",
                        xp_gain->amount);

    ImGui::End();
}

void RPGUI::draw_minimap(Scene& scene, Entity player)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;
    const float map_size = 150.0f;

    ImGui::SetNextWindowPos(ImVec2(screen_size.x - map_size - 20, 20));
    ImGui::SetNextWindowSize(ImVec2(map_size, map_size));
    ImGui::Begin("Minimap", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse);

    const auto& player_transform =
            scene.registry().get<Transform>(player);
    const float scale = 5.0f; // world units per pixel

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 map_min = ImGui::GetWindowPos();
    const ImVec2 map_center =
            ImVec2(map_min.x + map_size / 2, map_min.y + map_size / 2);

    // Draw player dot
    draw_list->AddCircleFilled(map_center, 4.0f,
                               IM_COL32(80, 200, 80, 255));

    // Draw NPCs
    const auto npc_view = scene.registry().view<NPC, Transform>();
    for (Entity npc : npc_view) {
        const auto& npc_transform =
                scene.registry().get<Transform>(npc);
        const glm::vec3 delta = npc_transform.position -
                player_transform.position;
        const ImVec2 pos(map_center.x + delta.x * scale,
                         map_center.y + delta.z * scale);
        if (pos.x >= map_min.x && pos.x <= map_min.x + map_size &&
            pos.y >= map_min.y && pos.y <= map_min.y + map_size) {
            draw_list->AddCircleFilled(pos, 3.0f,
                                       IM_COL32(255, 200, 50, 255));
        }
    }

    // Draw enemies
    const auto enemy_view = scene.registry().view<AI, Transform>();
    for (Entity enemy : enemy_view) {
        const auto& enemy_transform =
                scene.registry().get<Transform>(enemy);
        const glm::vec3 delta = enemy_transform.position -
                player_transform.position;
        const ImVec2 pos(map_center.x + delta.x * scale,
                         map_center.y + delta.z * scale);
        if (pos.x >= map_min.x && pos.x <= map_min.x + map_size &&
            pos.y >= map_min.y && pos.y <= map_min.y + map_size) {
            draw_list->AddCircleFilled(pos, 3.0f,
                                       IM_COL32(255, 50, 50, 255));
        }
    }

    ImGui::End();
}

void RPGUI::draw_quick_slots(Scene& scene, Entity player)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 screen_size = viewport->WorkSize;
    constexpr int num_slots = 6;
    constexpr float slot_size = 50.0f;
    constexpr float slot_spacing = 4.0f;
    const float total_w = num_slots * (slot_size + slot_spacing);

    ImGui::SetNextWindowPos(
            ImVec2(screen_size.x / 2 - total_w / 2, screen_size.y - 80));
    ImGui::SetNextWindowSize(ImVec2(total_w, slot_size + 10));
    ImGui::Begin("QuickSlots", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoBackground);

    const auto& inventory = scene.registry().get<Inventory>(player);
    int slot = 0;
    for (const Item& item : inventory.items) {
        if (slot >= num_slots) {
            break;
        }
        if (item.type == "consumable") {
            ImGui::PushID(slot);
            if (ImGui::Button(item.name.c_str(),
                              ImVec2(slot_size, slot_size)) &&
                quick_slot_callback_) {
                quick_slot_callback_(slot);
            }
            ImGui::PopID();
            ImGui::SameLine();
            ++slot;
        }
    }
    // Fill remaining slots
    for (int i = slot; i < num_slots; ++i) {
        ImGui::PushID(i);
        ImGui::Button("", ImVec2(slot_size, slot_size));
        ImGui::PopID();
        ImGui::SameLine();
    }

    ImGui::End();
}

} // namespace lotus::rpg
