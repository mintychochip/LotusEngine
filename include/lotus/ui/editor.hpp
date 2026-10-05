#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>

#include "lotus/ecs.hpp"

namespace lotus {

class GpuRenderer;
class SceneManager;
class Scene;

/// Dear ImGui editor: scene hierarchy, entity inspector, stats, and scene-file
/// controls (save / hot reload).
///
/// Structural components (Transform, Hierarchy, Name) are owned by Scene and
/// are not removable from the UI; optional components (Sprite, Camera2D) can
/// be added and removed.
class Editor {
public:
	Editor() = default;
	~Editor();

	Editor(const Editor&)            = delete;
	Editor& operator=(const Editor&) = delete;

	/// Pull-based controls for the scene file panel.
	struct SceneControls {
		std::function<std::string()> path;
		std::function<std::string()> status;
		std::function<void()> on_save;
		std::function<void()> on_reload;
	};

	bool init(SDL_Window* window, GpuRenderer& gpu);
	void shutdown();

	void set_scene_controls(SceneControls controls);

	void process_event(const SDL_Event& event);

	/// Starts an ImGui frame. No-op when hidden.
	void begin_frame();
	/// Builds the panels. No-op when hidden.
	void draw(SceneManager& scenes, float dt);
	/// Renders the accumulated UI into its own render pass. No-op when hidden.
	void render(GpuRenderer& gpu);

	bool visible() const { return visible_; }
	void set_visible(bool value) { visible_ = value; }
	void toggle() { visible_ = !visible_; }

	/// True when the UI changed the scene since the last save.
	bool dirty() const { return dirty_; }
	void clear_dirty() { dirty_ = false; }
	bool auto_save() const { return auto_save_; }

private:
	void draw_hierarchy(Scene& scene);
	void draw_entity_node(Scene& scene, Entity entity);
	void draw_inspector(Scene& scene);
	void draw_stats(SceneManager& scenes, float dt);
	void draw_scene_panel();

	bool initialized_{false};
	bool visible_{true};
	bool dirty_{false};
	bool auto_save_{false};
	Entity selected_{NullEntity};
	SceneControls scene_controls_;
};

} // namespace lotus
