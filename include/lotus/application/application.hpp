#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include "lotus/allocator.hpp"
#include "window.hpp"
namespace lotus {
struct GLProfile {
	int major, minor;
	GLenum gl_profile;
};
class GLApplication {
public:
	GLApplication(const GLProfile& profile) : profile_{profile} {}
	GLWindow* create_window(Allocator& allocator, int width, int height,
	                        const std::string& title)
	{
		GLWindow* window = allocator.construct<GLWindow>(width, height, title);
		return window;
	}
	~GLApplication() {}
	void run();

protected:
	void on_start();
	virtual void on_update()   = 0;
	virtual void on_render()   = 0;
	virtual void on_shutdown() = 0;
	GLProfile profile_;
};
} // namespace lotus