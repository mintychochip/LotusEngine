#pragma once

#include <SDL3/SDL.h>

#include "lotus/ecs.hpp"

namespace lotus {

class GpuRenderer;
class SceneManager;
class Scene;

/// Dear ImGui debug overlay: scene hierarchy, entity inspector, stats.
/// Owns the ImGui context and the SDL3/SDL_GPU backends.
class Editor {
public:
	Editor() = default;
	~Editor();

	Editor(const Editor&)            = delete;
	Editor& operator=(const Editor&) = delete;

	bool init(SDL_Window* window, GpuRenderer& gpu);
	void shutdown();

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

private:
	void draw_hierarchy(Scene& scene);
	void draw_entity_node(Scene& scene, Entity entity);
	void draw_inspector(Scene& scene);
	void draw_stats(SceneManager& scenes, float dt);

	bool initialized_{false};
	bool visible_{true};
	Entity selected_{NullEntity};
};

} // namespace lotus
