#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "gpu/vk/vk_api.hpp"

struct SDL_Window;

namespace lotus::vk {

struct ContextOptions {
	/// The window to present to, or null for offscreen use. A window means the
	/// instance carries SDL's surface extensions, the device is one that can
	/// present to it, and the context owns the surface.
	SDL_Window* window{nullptr};
	/// Ask for the Khronos validation layer. Honoured only where the layer is
	/// installed. LOTUS_VK_VALIDATION=1 asks too, and =sync adds
	/// synchronization validation.
	bool validation{false};
};

/// The Vulkan instance, the one device and queue the renderer draws with, and
/// the loaded function table. Vulkan 1.3 is required: dynamic rendering and
/// synchronization2 are what the passes and barriers are written against, and
/// depthClamp and independentBlend are what the pipeline state needs.
class Context {
public:
	Context() = default;
	~Context();
	Context(const Context&)            = delete;
	Context& operator=(const Context&) = delete;

	/// Loads the loader through SDL (SDL's video subsystem must be
	/// initialised), creates the instance, picks the best suitable physical
	/// device (discrete over integrated over virtual over software) and
	/// creates the device. Throws std::runtime_error, leaving nothing behind,
	/// when there is no Vulkan 1.3 device.
	void init(const ContextOptions& options);
	void shutdown();
	bool valid() const { return device != VK_NULL_HANDLE; }

	Api api;
	VkInstance instance{VK_NULL_HANDLE};
	VkSurfaceKHR surface{VK_NULL_HANDLE};
	VkPhysicalDevice physical_device{VK_NULL_HANDLE};
	VkDevice device{VK_NULL_HANDLE};
	VkQueue queue{VK_NULL_HANDLE};
	uint32_t queue_family{0};
	VkPhysicalDeviceProperties properties{};
	VkPhysicalDeviceMemoryProperties memory{};
	/// The driver's own name: "NVIDIA", "llvmpipe", "Intel open-source Mesa
	/// driver".
	std::string driver_name;

	/// True when the validation layer is on and reporting.
	bool validation_enabled() const { return messenger_ != VK_NULL_HANDLE; }
	/// Warnings and errors the validation layer has reported so far.
	uint32_t validation_messages() const { return validation_messages_; }
	void note_validation_message() { ++validation_messages_; }

	/// Records commands with \p record, submits them and waits for them to
	/// finish. For work that happens rarely and must be complete before the
	/// caller goes on (texture uploads, readbacks); never part of a frame.
	void submit_and_wait(const std::function<void(VkCommandBuffer)>& record);

private:
	void create(const ContextOptions& options);
	void pick_physical_device(bool windowed);

	bool loaded_library_{false};
	VkDebugUtilsMessengerEXT messenger_{VK_NULL_HANDLE};
	VkCommandPool one_shot_pool_{VK_NULL_HANDLE};
	VkFence one_shot_fence_{VK_NULL_HANDLE};
	std::atomic<uint32_t> validation_messages_{0};
};

} // namespace lotus::vk
