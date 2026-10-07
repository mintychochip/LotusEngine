#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <string>
#include <vector>

#include "lotus/ecs.hpp"
#include "lotus/scene/history.hpp"

namespace lotus {

class Console;
class Keybinds;
class GpuRenderer;
class SceneManager;
class Scene;

/// Dear ImGui editor: scene hierarchy, entity inspector, stats, console, and
/// keybind reference, plus scene-file controls (save / hot reload).
///
/// Structural components (Transform, Hierarchy, Name) are owned by Scene and
/// are not removable from the UI; optional components (Sprite, Camera2D,
/// Script) can be added and removed.
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

	/// Pull-based controls for the prefab panel. The Application owns the
	/// prefab directory and the active scene, so the editor asks rather than
	/// reaching for either.
	struct PrefabControls {
		std::function<std::vector<std::string>()> names;
		/// Saves \p root and its descendants as a named prefab. Returns false
		/// and fills \p error on failure.
		std::function<bool(Entity root, const std::string& name,
		                   std::string* error)>
		        create_from;
		/// Instantiates \p name under \p parent. NullEntity on failure.
		std::function<Entity(const std::string& name, Entity parent)> instantiate;
	};

	bool init(SDL_Window* window, GpuRenderer& gpu);
	void shutdown();

	void set_scene_controls(SceneControls controls);
	void set_prefab_controls(PrefabControls controls)
	{
		prefab_controls_ = std::move(controls);
	}
	void set_console(Console* console) { console_ = console; }
	void set_keybinds(Keybinds* keybinds, std::function<void()> on_save);

	void process_event(const SDL_Event& event);

	/// True when ImGui has keyboard focus (so keybinds must not fire).
	bool captures_keyboard() const;

	/// Starts an ImGui frame. No-op when hidden.
	void begin_frame();
	/// Builds the panels. No-op when hidden.
	void draw(SceneManager& scenes, float dt);
	/// Renders the accumulated UI into its own render pass. No-op when hidden.
	void render(GpuRenderer& gpu);

	bool visible() const { return visible_; }
	void set_visible(bool value) { visible_ = value; }
	void toggle() { visible_ = !visible_; }

	bool console_visible() const { return console_visible_; }
	void toggle_console() { console_visible_ = !console_visible_; }

	/// True when the UI changed the scene since the last save.
	bool dirty() const { return dirty_; }
	void clear_dirty() { dirty_ = false; }
	bool auto_save() const { return auto_save_; }

	/// Drops the undo history (e.g. after a hot reload replaced the scene).
	void reset_history() { history_scene_ = nullptr; }
	/// Size of the viewport panel in pixels. The application queries this
	/// to size the offscreen render target.
	struct ViewportSize {
		uint32_t width{0};
		uint32_t height{0};
	};
	ViewportSize viewport_size() const { return viewport_size_; }



	bool can_undo() const { return history_.can_undo(); }
	bool can_redo() const { return history_.can_redo(); }

	void request_undo() { undo_requested_ = true; }
	void request_redo() { redo_requested_ = true; }

private:
	void draw_viewport(Scene& scene);
	void draw_hierarchy(Scene& scene);
	void draw_entity_node(Scene& scene, Entity entity);
	void draw_inspector(Scene& scene);
	void draw_stats(SceneManager& scenes, float dt);
	void draw_scene_panel();
	void draw_console();
	void draw_keybinds();
	void draw_prefabs(Scene& scene);
	void draw_viewport();


	bool initialized_{false};
	bool visible_{true};
	bool dirty_{false};
	bool auto_save_{false};
	bool edited_{false};
	bool undo_requested_{false};
	bool redo_requested_{false};
	Entity selected_{NullEntity};
	SceneControls scene_controls_;
	PrefabControls prefab_controls_;
	char prefab_name_[64]{"prefab"};
	std::string prefab_error_;

	SceneHistory history_;
	const Scene* history_scene_{nullptr};

	Console* console_{nullptr};
	Keybinds* keybinds_{nullptr};
	std::function<void()> save_keybinds_;
	bool console_visible_{false};
	bool refocus_console_{false};
	bool console_scroll_{false};
	char console_input_[256]{};
	std::vector<std::string> console_history_;
	int console_history_pos_{-1};
	ViewportSize viewport_size_;
	GpuRenderer* gpu_{nullptr};

	// Viewport interaction. gizmo_axis_ is 0/1/2 for X/Y/Z while a handle is
	// hovered or dragged, -1 when idle. The drag is incremental: each frame
	// applies the mouse movement since the previous frame along the axis.
	int gizmo_axis_{-1};
	bool gizmo_dragging_{false};
	float gizmo_last_mouse_x_{0.0f};
	float gizmo_last_mouse_y_{0.0f};
};

} // namespace lotus
