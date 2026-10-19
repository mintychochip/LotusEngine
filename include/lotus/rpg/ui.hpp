#pragma once

#include <functional>
#include <string>

#include "lotus/ecs.hpp"
#include "lotus/rpg/components.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus::rpg {

/// RPG HUD and UI panels. Rendered as an ImGui overlay during play mode.
class RPGUI {
public:
    RPGUI() = default;

    /// Draw the full RPG UI overlay. Call during play mode only.
    void draw(Scene& scene, Entity player);

    /// Toggle inventory panel visibility.
    void toggle_inventory() { show_inventory_ = !show_inventory_; }
    /// Toggle quest log visibility.
    void toggle_quest_log() { show_quest_log_ = !show_quest_log_; }
    /// Toggle character stats panel.
    void toggle_character() { show_character_ = !show_character_; }

    bool inventory_visible() const { return show_inventory_; }
    bool quest_log_visible() const { return show_quest_log_; }
    bool character_visible() const { return show_character_; }

    using QuickSlotCallback = std::function<void(int consumable_slot_index)>;
    void set_quick_slot_callback(QuickSlotCallback callback)
    {
        quick_slot_callback_ = std::move(callback);
    }

private:
    void draw_health_bar(Scene& scene, Entity player);
    void draw_mana_bar(Scene& scene, Entity player);
    void draw_xp_bar(Scene& scene, Entity player);
    void draw_inventory_panel(Scene& scene, Entity player);
    void draw_quest_log(Scene& scene, Entity player);
    void draw_character_panel(Scene& scene, Entity player);
    void draw_dialogue_box(Scene& scene, Entity player);
    void draw_interact_prompt(Scene& scene, Entity player);
    void draw_combat_text(Scene& scene, Entity player);
    void draw_minimap(Scene& scene, Entity player);
    void draw_quick_slots(Scene& scene, Entity player);

    bool show_inventory_{false};
    bool show_quest_log_{false};
    bool show_character_{false};
    QuickSlotCallback quick_slot_callback_;
};

} // namespace lotus::rpg
