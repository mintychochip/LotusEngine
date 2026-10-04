#pragma once

#include "lotus/allocator.hpp"
#include "window.hpp"

namespace lotus {
struct GLProfile {
	int major{3};
	int minor{3};
	bool core_profile{true};
};

class GLApplication {
public:
	explicit GLApplication(const GLProfile& profile) : profile_{profile} {}
	GLWindow* create_window(Allocator& allocator, int width, int height,
	                        const std::string& title)
	{
		return allocator.construct<GLWindow>(width, height, title);
	}
	virtual ~GLApplication() = default;

	/// Runs the main loop until the window closes.
	void run(int width = 800, int height = 600,
	         const std::string& title = "lotus");

protected:
	void on_start();
	/// Called once after the window exists and the OpenGL context is current.
	virtual void on_ready(GLWindow& window) {}
	virtual void on_update(float dt) = 0;
	virtual void on_render() = 0;
	virtual void on_shutdown() = 0;
	GLProfile profile_;
};
} // namespace lotus
