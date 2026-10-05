#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <stdexcept>
#include <string>

#include "lotus/global.hpp"

namespace lotus {

/// SDL window used with SDL3 GPU (Vulkan backend on Linux).
class Window {
public:
	Window(int width, int height, const std::string& title)
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
		                           SDL_WINDOW_RESIZABLE);
		if (!handle_) {
			throw std::runtime_error(SDL_GetError());
		}
	}

	/// Pumps the event queue; \p on_event (if set) sees every event first.
	void poll_events(const std::function<void(const SDL_Event&)>& on_event = {})
	{
		SDL_Event event{};
		while (SDL_PollEvent(&event)) {
			if (on_event) {
				on_event(event);
			}
			if (event.type == SDL_EVENT_QUIT) {
				close_requested_ = true;
			} else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
			           event.window.windowID == SDL_GetWindowID(handle_)) {
				close_requested_ = true;
			}
		}
	}

	bool should_close() const { return close_requested_; }

	SDL_Window* handle() const { return handle_; }

	int width() const { return width_; }
	int height() const { return height_; }

	/// Drawable size in pixels (differs from window size on HiDPI).
	void pixel_size(int& out_w, int& out_h) const
	{
		SDL_GetWindowSizeInPixels(handle_, &out_w, &out_h);
	}

	~Window()
	{
		if (handle_) {
			SDL_DestroyWindow(handle_);
			handle_ = nullptr;
		}
	}

private:
	SDL_Window* handle_{nullptr};
	int width_, height_;
	std::string title_;
	bool close_requested_{false};
};

} // namespace lotus
