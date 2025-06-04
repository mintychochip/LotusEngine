#pragma once

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <string>

#include "application.hpp"
#include "lotus/global.hpp"

namespace lotus {
class GLWindow {

public:
	GLWindow(int width, int height, const std::string& title,
	         GLWindow* shared_with = nullptr)
	        : width_{width},
	          height_{height},
	          title_{title}
	{
		if (!Global::flag(LOTUS_GLFW_INITIALIZED)) {
			throw std::runtime_error("glfw is not initialized");
		}
		handle_ = glfwCreateWindow(width,
		                           height,
		                           title.c_str(),
		                           nullptr,
		                           shared_with ? shared_with->handle()
		                                       : nullptr);
	}

	bool set_current() const
	{
		glfwMakeContextCurrent(handle_);
		return gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
	}

	void swap_buffers() const
	{
		glfwSwapBuffers(handle_);
	}

	bool should_close() const
	{
		return glfwWindowShouldClose(handle_);
	}

	GLFWwindow* handle() const
	{
		return handle_;
	}

	int width() const
	{
		return width_;
	}

	void width(int width)
	{
		width_ = width;
	}

	int height() const
	{
		return height_;
	}

	void height(int height)
	{
		height_ = height;
	}

	~GLWindow()
	{
		glfwDestroyWindow(handle_);
	}

private:
	GLFWwindow* handle_;
	int width_, height_;
	std::string title_;
};
} // namespace lotus