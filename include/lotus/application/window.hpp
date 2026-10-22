#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lotus/gpu/present_target.hpp"
#include "lotus/input/input.hpp"

struct GLFWwindow;

namespace lotus {

/// A platform window the Vulkan renderer presents to, on GLFW. The first
/// window initialises GLFW and the last one's destruction terminates it, so no
/// caller manages the library.
///
/// While a window exists, SIGINT and SIGTERM (Ctrl-C, `kill`) close it instead
/// of killing the process, so the application shuts down in order. A signal
/// the process already ignores or handles is left as it is.
///
/// LOTUS_WINDOW_PLATFORM=wayland|x11|null picks GLFW's platform; unset, GLFW
/// follows the session (XDG_SESSION_TYPE) and otherwise tries Wayland, then
/// X11. `null` needs no display and cannot present, which is what lets the
/// window code be tested.
class Window final : public PresentTarget {
public:
	/// Throws std::runtime_error with GLFW's message when no window can be
	/// made (no display, an unavailable platform).
	Window(int width, int height, const std::string& title);
	~Window() override;

	Window(const Window&)            = delete;
	Window& operator=(const Window&) = delete;

	/// Pumps the event queue. Events update input() as the system delivers
	/// them, then reach \p on_event (if set) in that order, so inside the
	/// callback input() already reflects the whole batch.
	void poll_events(const std::function<void(const Event&)>& on_event = {});

	bool should_close() const { return close_requested_; }

	/// The GLFW window, for the editor's ImGui backend.
	GLFWwindow* handle() const { return handle_; }

	/// The size the window was created with. A resize is seen through
	/// pixel_size().
	int width() const { return width_; }
	int height() const { return height_; }

	/// What is held right now, as of the last poll_events().
	const Input& input() const { return input_; }

	void pixel_size(int& out_w, int& out_h) const override;
	std::vector<std::string> vulkan_instance_extensions() const override;
	std::uint64_t create_vulkan_surface(void* instance,
	                                    void* get_instance_proc) override;

private:
	// GLFW callbacks: translate to engine events and hand them to dispatch().
	static void on_key(GLFWwindow* handle, int key, int scancode, int action,
	                   int mods);
	static void on_mouse_button(GLFWwindow* handle, int button, int action,
	                            int mods);
	static void on_cursor(GLFWwindow* handle, double x, double y);
	static void on_cursor_enter(GLFWwindow* handle, int entered);
	static void on_close(GLFWwindow* handle);
	// Lets the window test feed events without a display.
	friend struct WindowTestAccess;

	void dispatch(const Event& event);

	GLFWwindow* handle_{nullptr};
	int width_;
	int height_;
	Input input_;
	std::vector<Event> pending_;
	bool close_requested_{false};
	bool cursor_known_{false};
	float cursor_x_{0.0f};
	float cursor_y_{0.0f};
};

} // namespace lotus
