#pragma once

#include "lotus/allocator.hpp"
#include "window.hpp"

namespace lotus {

class GpuRenderer;

struct GpuSettings {
	bool debug_device{true};
};

class Application {
public:
	explicit Application(GpuSettings settings = {}) : settings_{settings} {}
	Window* create_window(Allocator& allocator, int width, int height,
	                      const std::string& title)
	{
		return allocator.construct<Window>(width, height, title);
	}
	virtual ~Application() = default;

	void run(int width = 800, int height = 600,
	         const std::string& title = "lotus");

protected:
	void on_start();
	virtual void on_ready(Window& window, GpuRenderer& gpu) = 0;
	virtual void on_update(float dt)                         = 0;
	/// Upload buffers / uniforms before the render pass begins.
	virtual void on_prepare_frame(GpuRenderer& gpu) {}
	virtual void on_render(GpuRenderer& gpu) = 0;
	virtual void on_shutdown()                               = 0;
	GpuSettings settings_;
};

// Legacy aliases while migrating call sites.
using GLWindow      = Window;
using GLProfile     = GpuSettings;
using GLApplication = Application;

} // namespace lotus
