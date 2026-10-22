#include "lotus/application/window.hpp"

// Vulkan's header first: GLFW then declares its Vulkan functions with the real
// types instead of its stand-ins.
#include <vulkan/vulkan.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include "core/application/glfw_input.hpp"

namespace lotus {

namespace {

/// Windows alive. GLFW is initialised while this is above zero.
int g_windows = 0;

/// Set by the SIGINT/SIGTERM handler; poll_events turns it into a close
/// request.
volatile std::sig_atomic_t g_quit_signal = 0;

void on_quit_signal(int)
{
	g_quit_signal = 1;
}

/// A signal that asks a process to end, and whether this file took it over.
struct QuitSignal {
	int number;
	bool taken;
};
QuitSignal g_quit_signals[] = {{SIGINT, false}, {SIGTERM, false}};

/// Makes Ctrl-C and `kill` close the window, so the application shuts down in
/// order. A signal the process already ignores or handles is left to whoever
/// set that up.
void take_quit_signals()
{
	g_quit_signal = 0;
	for (QuitSignal& quit : g_quit_signals) {
		const auto previous = std::signal(quit.number, on_quit_signal);
		quit.taken          = previous == SIG_DFL;
		if (!quit.taken) {
			std::signal(quit.number, previous);
		}
	}
}

void give_back_quit_signals()
{
	for (QuitSignal& quit : g_quit_signals) {
		if (quit.taken) {
			std::signal(quit.number, SIG_DFL);
			quit.taken = false;
		}
	}
}

std::string glfw_error()
{
	const char* description = nullptr;
	glfwGetError(&description);
	return description != nullptr ? description : "unknown error";
}

/// The running executable's file name ("lotus", "lotus_rpg"): the name SDL gave
/// windows, and what window rules and task bars match on. Linux reads it from
/// /proc; elsewhere, or when that fails, "lotus" (only Wayland and X11 use it).
std::string executable_name()
{
	std::error_code error;
	const std::filesystem::path exe =
	        std::filesystem::read_symlink("/proc/self/exe", error);
	if (!error && exe.has_filename()) {
		return exe.filename().string();
	}
	return "lotus";
}

/// Applies LOTUS_WINDOW_PLATFORM as GLFW's platform hint.
void apply_platform_hint()
{
	const char* env = std::getenv("LOTUS_WINDOW_PLATFORM");
	if (env == nullptr || *env == '\0') {
		return;
	}
	std::string name{env};
	std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	if (name == "wayland") {
		glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
	} else if (name == "x11") {
		glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
	} else if (name == "null") {
		glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
	} else {
		throw std::runtime_error(
		        "LOTUS_WINDOW_PLATFORM must be wayland, x11 or null, not '" +
		        name + "'");
	}
}

void acquire_glfw()
{
	if (g_windows == 0) {
		apply_platform_hint();
		if (glfwInit() != GLFW_TRUE) {
			throw std::runtime_error("cannot initialise GLFW: " +
			                         glfw_error());
		}
		take_quit_signals();
	}
	++g_windows;
}

void release_glfw()
{
	if (--g_windows == 0) {
		give_back_quit_signals();
		glfwTerminate();
	}
}

Window* window_of(GLFWwindow* handle)
{
	return static_cast<Window*>(glfwGetWindowUserPointer(handle));
}

} // namespace

Window::Window(int width, int height, const std::string& title)
        : width_{width},
          height_{height}
{
	acquire_glfw();
	// Vulkan does the drawing: GLFW must not make an OpenGL context.
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	// How compositors and window managers know the application (window rules,
	// task bars, icons). SDL named windows after the executable; GLFW leaves
	// the name empty unless told.
	const std::string app_id = executable_name();
	glfwWindowHintString(GLFW_WAYLAND_APP_ID, app_id.c_str());
	glfwWindowHintString(GLFW_X11_CLASS_NAME, app_id.c_str());
	glfwWindowHintString(GLFW_X11_INSTANCE_NAME, app_id.c_str());
	handle_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
	if (handle_ == nullptr) {
		const std::string why = glfw_error();
		release_glfw();
		throw std::runtime_error("cannot create a window: " + why);
	}
	glfwSetWindowUserPointer(handle_, this);
	glfwSetKeyCallback(handle_, on_key);
	glfwSetMouseButtonCallback(handle_, on_mouse_button);
	glfwSetCursorPosCallback(handle_, on_cursor);
	glfwSetCursorEnterCallback(handle_, on_cursor_enter);
	glfwSetWindowCloseCallback(handle_, on_close);
}

Window::~Window()
{
	if (handle_ != nullptr) {
		glfwDestroyWindow(handle_);
		handle_ = nullptr;
		release_glfw();
	}
}

void Window::poll_events(const std::function<void(const Event&)>& on_event)
{
	glfwPollEvents();
	if (g_quit_signal != 0) {
		close_requested_ = true;
	}
	std::vector<Event> events;
	events.swap(pending_);
	if (on_event) {
		for (const Event& event : events) {
			on_event(event);
		}
	}
}

void Window::pixel_size(int& out_w, int& out_h) const
{
	glfwGetFramebufferSize(handle_, &out_w, &out_h);
}

std::vector<std::string> Window::vulkan_instance_extensions() const
{
	std::uint32_t count = 0;
	const char** names  = glfwGetRequiredInstanceExtensions(&count);
	if (names == nullptr) {
		throw std::runtime_error("no Vulkan surface support: " + glfw_error());
	}
	return std::vector<std::string>(names, names + count);
}

std::uint64_t Window::create_vulkan_surface(void* instance,
                                            void* /*get_instance_proc*/)
{
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	const VkResult result = glfwCreateWindowSurface(
	        static_cast<VkInstance>(instance), handle_, nullptr, &surface);
	if (result != VK_SUCCESS) {
		throw std::runtime_error("cannot create a Vulkan surface: " +
		                         glfw_error() + " (VkResult " +
		                         std::to_string(static_cast<int>(result)) + ")");
	}
	return std::bit_cast<std::uint64_t>(surface);
}

void Window::dispatch(const Event& event)
{
	input_.apply(event);
	pending_.push_back(event);
}

void Window::on_key(GLFWwindow* handle, int key, int /*scancode*/, int action,
                    int mods)
{
	const Key engine_key = key_from_glfw(key);
	if (engine_key == Key::Unknown) {
		return;
	}
	window_of(handle)->dispatch(KeyEvent{engine_key, mods_from_glfw(mods),
	                                     action != GLFW_RELEASE,
	                                     action == GLFW_REPEAT});
}

void Window::on_mouse_button(GLFWwindow* handle, int button, int action,
                             int mods)
{
	const MouseButton engine_button = mouse_button_from_glfw(button);
	if (engine_button == MouseButton::Count) {
		return;
	}
	window_of(handle)->dispatch(MouseButtonEvent{
	        engine_button, action == GLFW_PRESS, mods_from_glfw(mods)});
}

void Window::on_cursor(GLFWwindow* handle, double x, double y)
{
	Window& window = *window_of(handle);
	MouseMoveEvent event;
	event.x       = static_cast<float>(x);
	event.y       = static_cast<float>(y);
	event.buttons = window.input_.mouse_buttons();
	if (window.cursor_known_) {
		event.dx = event.x - window.cursor_x_;
		event.dy = event.y - window.cursor_y_;
	}
	window.cursor_known_ = true;
	window.cursor_x_     = event.x;
	window.cursor_y_     = event.y;
	window.dispatch(event);
}

void Window::on_cursor_enter(GLFWwindow* handle, int entered)
{
	// The cursor can come back anywhere: the next position is a fresh start,
	// not a jump from where it left.
	if (entered != 0) {
		window_of(handle)->cursor_known_ = false;
	}
}

void Window::on_close(GLFWwindow* handle)
{
	window_of(handle)->close_requested_ = true;
}

} // namespace lotus
