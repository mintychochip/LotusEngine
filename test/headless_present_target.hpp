#pragma once

// A present target with no window system behind it: its surface is a
// VK_EXT_headless_surface and its size is whatever the test says. It lets the
// swapchain path run on a machine with no display. Mesa's drivers offer the
// extension; NVIDIA's does not (see vulkan_device.hpp).

#include <vulkan/vulkan.h>

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "lotus/gpu/present_target.hpp"

namespace lotus::test {

class HeadlessPresentTarget final : public PresentTarget {
public:
	HeadlessPresentTarget(int width, int height)
	        : width_{width},
	          height_{height}
	{
	}

	/// The size the next frame sees, as if the user had resized the window.
	void resize(int width, int height)
	{
		width_  = width;
		height_ = height;
	}

	void pixel_size(int& width, int& height) const override
	{
		width  = width_;
		height = height_;
	}

	std::vector<std::string> vulkan_instance_extensions() const override
	{
		return {VK_KHR_SURFACE_EXTENSION_NAME,
		        VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
	}

	std::uint64_t create_vulkan_surface(void* instance,
	                                    void* get_instance_proc) override
	{
		const auto get_proc =
		        reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc);
		const auto vk_instance = static_cast<VkInstance>(instance);
		const auto create =
		        reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
		                get_proc(vk_instance, "vkCreateHeadlessSurfaceEXT"));
		if (create == nullptr) {
			throw std::runtime_error(
			        "the loader has no vkCreateHeadlessSurfaceEXT");
		}
		VkHeadlessSurfaceCreateInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		const VkResult result = create(vk_instance, &info, nullptr, &surface);
		if (result != VK_SUCCESS) {
			throw std::runtime_error(
			        "vkCreateHeadlessSurfaceEXT failed (VkResult " +
			        std::to_string(static_cast<int>(result)) + ")");
		}
		return std::bit_cast<std::uint64_t>(surface);
	}

private:
	int width_;
	int height_;
};

} // namespace lotus::test
