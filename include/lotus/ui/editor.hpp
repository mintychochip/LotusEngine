#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "lotus/ecs.hpp"

namespace lotus {

class AssetRegistry;
class Console;
class Keybinds;
class GpuRenderer;
class PhysicsWorld;
class SceneManager;
class Scene;

/// Read-only scene viewer (Dear ImGui): viewport with camera orbit, scene
/// hierarchy, read-only entity inspector, stats, scene-file status, asset
/// list, keybind reference, and console.
///
/// The viewer never mutates the scene or the disk: every widget is
/// display-only, selection is the only state it keeps, and all saving,
/// reloading, spawning, and prefab work lives in Application behind console
/// commands (the agent/CLI surface).
class Editor {
public:
	Editor() = default;
	~Editor();

	Editor(const Editor&)            = delete;
	Editor& operator=(const Editor&) = delete;

	/// Pull-based read-only status for the scene file panel.
	struct SceneControls {
		std::function<std::string()> path;
		std::function<std::string()> status;
	};

	/// Pull-based read-only status for the asset panel.
	struct AssetControls {
		std::function<const AssetRegistry*()> registry;
		std::function<std::string()> status;
		/// Unknown asset keys referenced by the active scene (sorted, unique).
		std::function<std::vector<std::string>(Scene* scene)> dangling_refs;
	};

	bool init(SDL_Window* window, GpuRenderer& gpu);
	void shutdown();

	void set_scene_controls(SceneControls controls)
	{
		scene_controls_ = std::move(controls);
	}
	void set_asset_controls(AssetControls controls)
	{
		asset_controls_ = std::move(controls);
	}
	/// Optional physics backend for viewport picking (raycast vs colliders).
	void set_physics(PhysicsWorld* physics) { physics_ = physics; }
	void set_console(Console* console) { console_ = console; }
	/// Read-only keybind reference; the viewer never saves keybinds.
	void set_keybinds(Keybinds* keybinds) { keybinds_ = keybinds; }

	void process_event(const SDL_Event& event);

	/// True when ImGui has keyboard focus (so keybinds must not fire).
	bool captures_keyboard() const;

	/// True when ImGui wants the mouse (so a right-drag orbits the editor
	/// viewport instead of the game camera). False when hidden.
	bool captures_mouse() const;

	/// Starts an ImGui frame. No-op when hidden.
	void begin_frame();
	/// Builds the panels. No-op when hidden. Never mutates the scene.
	void draw(SceneManager& scenes, float dt);
	/// Renders the accumulated UI into its own render pass. No-op when hidden.
	void render(GpuRenderer& gpu);

	bool visible() const { return visible_; }
	void set_visible(bool value) { visible_ = value; }
	void toggle() { visible_ = !visible_; }

	bool console_visible() const { return console_visible_; }
	void toggle_console() { console_visible_ = !console_visible_; }

	/// Size of the viewport panel in pixels. The application queries this
	/// to size the offscreen render target.
	struct ViewportSize {
		uint32_t width{0};
		uint32_t height{0};
	};
	ViewportSize viewport_size() const { return viewport_size_; }

private:
	void draw_viewport(Scene& scene);
	void draw_hierarchy(Scene& scene);
	void draw_entity_node(Scene& scene, Entity entity);
	void draw_inspector(Scene& scene);
	void draw_stats(SceneManager& scenes, float dt);
	void draw_scene_panel();
	void draw_assets(Scene* scene);
	void draw_keybinds();
	void draw_console();

	bool initialized_{false};
	bool frame_open_{false};
	bool visible_{true};
	Entity selected_{NullEntity};
	SceneControls scene_controls_;
	AssetControls asset_controls_;

	PhysicsWorld* physics_{nullptr};
	Console* console_{nullptr};
	Keybinds* keybinds_{nullptr};
	bool console_visible_{false};
	bool refocus_console_{false};
	bool console_scroll_{false};
	char console_input_[256]{};
	std::vector<std::string> console_history_;
	int console_history_pos_{-1};
	ViewportSize viewport_size_;
	GpuRenderer* gpu_{nullptr};
};

} // namespace lotus
