#pragma once

#include <SDL3/SDL.h>

#include "lotus/application.hpp"
#include "lotus/rpg/codecs.hpp"
#include "lotus/rpg/components.hpp"
#include "lotus/rpg/systems.hpp"
#include "lotus/rpg/ui.hpp"

namespace lotus::rpg {

/// Main RPG game application. Extends the engine Application with
/// play mode, RPG systems, and a demo scene.
class RPGGame : public Application {
public:
    explicit RPGGame(GpuSettings settings = {});

protected:
    void on_ready(Window& window, GpuRenderer& gpu) override;
    void on_update(float dt) override;
    void on_ui(float dt) override;
    void on_scene_committed(Scene& scene) override;
    void on_shutdown() override;

private:
    void enter_play_mode();
    void exit_play_mode();
    void handle_game_input();
    void create_demo_scene();
    void setup_player();
    void setup_npcs();
    void setup_enemies();
    void setup_items();
    void setup_quests();

    bool play_mode_{false};
    Entity player_{NullEntity};
    RPGUI rpg_ui_;

    // Game state
    float game_time_{0.0f};
    int kill_count_{0};

    // Key state tracking for edge detection
    bool prev_keys_[SDL_SCANCODE_COUNT]{};

    // Camera orbit drag state
    bool camera_mouse_down_{false};
    float camera_last_mouse_x_{0.0f};
    float camera_last_mouse_y_{0.0f};
};

} // namespace lotus::rpg
