#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lotus {

/// What a renderer presents to: a platform window or, in tests, a surface with
/// no window system behind it. Vulkan handles cross this interface as opaque
/// pointers and integers so the header needs no Vulkan include.
class PresentTarget {
public:
	virtual ~PresentTarget() = default;

	/// Drawable size in pixels; zero while a window is minimized.
	virtual void pixel_size(int& width, int& height) const = 0;

	/// The Vulkan instance extensions a surface for this target needs: the
	/// generic surface extension plus the platform's. Throws
	/// std::runtime_error when the target cannot present through Vulkan.
	virtual std::vector<std::string> vulkan_instance_extensions() const = 0;

	/// Creates this target's Vulkan surface. \p instance is the VkInstance and
	/// \p get_instance_proc the loader's vkGetInstanceProcAddr. Returns the
	/// VkSurfaceKHR handle, which the caller destroys with vkDestroySurfaceKHR.
	/// Throws std::runtime_error when no surface can be made.
	virtual std::uint64_t create_vulkan_surface(void* instance,
	                                            void* get_instance_proc) = 0;
};

} // namespace lotus
