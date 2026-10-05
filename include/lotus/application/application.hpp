#pragma once

#include "lotus/application/window.hpp"
#include "lotus/scene/scene_manager.hpp"
#include "lotus/ui/editor.hpp"

namespace lotus {

class GpuRenderer;

struct GpuSettings {
	bool debug_device{true};
};

/// Owns the window, GPU renderer, scene manager, and editor, and runs the loop.
class Application {
public:
	explicit Application(GpuSettings settings = {}) : settings_{settings} {}
	virtual ~Application() = default;

	void run(int width = 800, int height = 600,
	         const std::string& title = "lotus");

	SceneManager& scenes() { return scenes_; }
	Editor& editor() { return editor_; }

protected:
	void on_start();
	/// Called once after the GPU device and editor exist.
	virtual void on_ready(Window& window, GpuRenderer& gpu) = 0;
	virtual void on_update(float dt)                       = 0;
	virtual void on_shutdown()                             = 0;

	GpuSettings settings_;

private:
	SceneManager scenes_;
	Editor editor_;
};

// Legacy aliases while migrating call sites.
using GLWindow      = Window;
using GLProfile     = GpuSettings;
using GLApplication = Application;

} // namespace lotus
