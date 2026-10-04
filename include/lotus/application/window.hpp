#pragma once

#include <glad/glad.h>
#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

#include "lotus/global.hpp"

namespace lotus {
class GLWindow {
public:
	GLWindow(int width, int height, const std::string& title)
	        : width_{width},
	          height_{height},
	          title_{title}
	{
		if (!Global::flag(LOTUS_SDL_INITIALIZED)) {
			throw std::runtime_error("SDL is not initialized");
		}
		handle_ = SDL_CreateWindow(title.c_str(),
		                           width,
		                           height,
		                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
		if (!handle_) {
			throw std::runtime_error(SDL_GetError());
		}
		context_ = SDL_GL_CreateContext(handle_);
		if (!context_) {
			throw std::runtime_error(SDL_GetError());
		}
	}

	bool set_current() const
	{
		if (!SDL_GL_MakeCurrent(handle_, context_)) {
			return false;
		}
		return gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress));
	}

	void swap_buffers() const
	{
		SDL_GL_SwapWindow(handle_);
	}

	void poll_events()
	{
		SDL_Event event{};
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT) {
				close_requested_ = true;
			} else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
			           event.window.windowID == SDL_GetWindowID(handle_)) {
				close_requested_ = true;
			}
		}
	}

	bool should_close() const
	{
		return close_requested_;
	}

	SDL_Window* handle() const
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
		if (context_) {
			SDL_GL_DestroyContext(context_);
			context_ = nullptr;
		}
		if (handle_) {
			SDL_DestroyWindow(handle_);
			handle_ = nullptr;
		}
	}

private:
	SDL_Window* handle_{nullptr};
	SDL_GLContext context_{nullptr};
	int width_, height_;
	std::string title_;
	bool close_requested_{false};
};
} // namespace lotus
