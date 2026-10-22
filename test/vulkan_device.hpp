#pragma once

// Environment checks for the tests that need a Vulkan device.
//
// A machine without a Vulkan driver (a CI runner, a container) cannot run the
// GPU tests, and that says nothing about the engine, so those tests skip and
// say why. The check does not go through the renderer: it asks the Vulkan
// loader directly whether a device exists, and skips only when none does. A
// machine that has a device but on which the renderer fails to initialise
// still FAILS the tests; skipping on the renderer's own exception would turn
// every such regression into a silent skip.
//
// LOTUS_REQUIRE_VULKAN=1 turns "no device" into a failure, for environments
// (CI) that promise a device and must not quietly lose the GPU tests when it
// goes missing.

#include <gtest/gtest.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lotus::test {

/// What the machine's Vulkan loader offers the tests. An empty string means
/// the need is met; otherwise it says why not.
struct VulkanProbe {
	/// A Vulkan device the renderer could use (Vulkan 1.3) exists.
	std::string no_device;
	/// A window on SDL's offscreen video driver can get a headless surface.
	std::string no_headless_surface;
};

namespace detail {

/// Asks a loaded Vulkan loader, given SDL's vkGetInstanceProcAddr.
inline VulkanProbe probe_loader(PFN_vkGetInstanceProcAddr get_instance_proc)
{
	VulkanProbe probe;
	const auto fail = [&probe](const std::string& why) {
		probe.no_device           = why;
		probe.no_headless_surface = why;
		return probe;
	};
	if (get_instance_proc == nullptr) {
		return fail("SDL provides no vkGetInstanceProcAddr");
	}
	const auto enumerate_extensions =
	        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
	                get_instance_proc(VK_NULL_HANDLE,
	                                  "vkEnumerateInstanceExtensionProperties"));
	const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
	        get_instance_proc(VK_NULL_HANDLE, "vkCreateInstance"));
	if (enumerate_extensions == nullptr || create_instance == nullptr) {
		return fail("the Vulkan loader lacks its global entry points");
	}

	// What a window needs comes from the instance's extensions.
	uint32_t extension_count = 0;
	enumerate_extensions(nullptr, &extension_count, nullptr);
	std::vector<VkExtensionProperties> extensions(extension_count);
	enumerate_extensions(nullptr, &extension_count, extensions.data());
	extensions.resize(extension_count);
	const auto offers = [&extensions](const char* name) {
		for (const VkExtensionProperties& extension : extensions) {
			if (std::strcmp(extension.extensionName, name) == 0) {
				return true;
			}
		}
		return false;
	};
	if (!offers(VK_KHR_SURFACE_EXTENSION_NAME) ||
	    !offers(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
		probe.no_headless_surface =
		        "the Vulkan loader does not offer VK_EXT_headless_surface "
		        "(NVIDIA's driver does not implement it)";
	}

	// A device needs a bare instance to be counted.
	VkApplicationInfo application{};
	application.sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.apiVersion = VK_API_VERSION_1_0;
	VkInstanceCreateInfo create{};
	create.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create.pApplicationInfo = &application;
	VkInstance instance{VK_NULL_HANDLE};
	if (const VkResult result = create_instance(&create, nullptr, &instance);
	    result != VK_SUCCESS) {
		probe.no_device = "vkCreateInstance failed (VkResult " +
		                  std::to_string(static_cast<int>(result)) + ")";
		return probe;
	}
	const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
	        get_instance_proc(instance, "vkDestroyInstance"));
	const auto enumerate_devices =
	        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
	                get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
	const auto get_properties =
	        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
	                get_instance_proc(instance,
	                                  "vkGetPhysicalDeviceProperties"));
	if (destroy_instance == nullptr || enumerate_devices == nullptr ||
	    get_properties == nullptr) {
		probe.no_device = "the Vulkan loader lacks its instance entry points";
		return probe;
	}
	uint32_t device_count = 0;
	enumerate_devices(instance, &device_count, nullptr);
	std::vector<VkPhysicalDevice> devices(device_count);
	enumerate_devices(instance, &device_count, devices.data());
	devices.resize(device_count);
	// The renderer needs dynamic rendering and synchronization2, which Vulkan
	// 1.3 makes mandatory, so the version is the question to ask.
	bool has_vulkan_13 = false;
	std::string found;
	for (const VkPhysicalDevice device : devices) {
		VkPhysicalDeviceProperties properties{};
		get_properties(device, &properties);
		has_vulkan_13 = has_vulkan_13 ||
		                properties.apiVersion >= VK_API_VERSION_1_3;
		found += found.empty() ? "" : ", ";
		found += properties.deviceName;
		found += " " +
		         std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) +
		         "." +
		         std::to_string(VK_API_VERSION_MINOR(properties.apiVersion));
	}
	destroy_instance(instance, nullptr);
	if (devices.empty()) {
		probe.no_device = "the Vulkan loader reports no physical device";
	} else if (!has_vulkan_13) {
		probe.no_device =
		        "no device supports Vulkan 1.3 (found " + found + ")";
	}
	return probe;
}

/// Loads the loader through SDL, as the renderer does, and asks it.
inline VulkanProbe probe_vulkan()
{
	// SDL needs its video subsystem to load the loader. The offscreen driver
	// needs no display.
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
		// Not a Vulkan question: the fixture's own SDL init reports it.
		return {};
	}
	VulkanProbe probe;
	if (SDL_Vulkan_LoadLibrary(nullptr)) {
		probe = probe_loader(reinterpret_cast<PFN_vkGetInstanceProcAddr>(
		        SDL_Vulkan_GetVkGetInstanceProcAddr()));
		SDL_Vulkan_UnloadLibrary();
	} else {
		probe.no_device =
		        std::string{"SDL cannot load Vulkan: "} + SDL_GetError();
		probe.no_headless_surface = probe.no_device;
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	return probe;
}

} // namespace detail

/// The probe's answer, worked out once per process.
inline const VulkanProbe& vulkan_probe()
{
	static const VulkanProbe probe = detail::probe_vulkan();
	return probe;
}

/// LOTUS_REQUIRE_VULKAN is set, and not to 0.
inline bool vulkan_required()
{
	const char* value = std::getenv("LOTUS_REQUIRE_VULKAN");
	return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

} // namespace lotus::test

/// Skips the current test when the machine has no Vulkan device the renderer
/// could use, or fails it when LOTUS_REQUIRE_VULKAN is set. Use it first thing
/// in SetUp or a test body (a function returning void).
#define LOTUS_SKIP_WITHOUT_VULKAN_DEVICE()                                     \
	do {                                                                   \
		const std::string& lotus_problem_ =                            \
		        ::lotus::test::vulkan_probe().no_device;               \
		if (!lotus_problem_.empty()) {                                 \
			if (::lotus::test::vulkan_required()) {                \
				FAIL() << "LOTUS_REQUIRE_VULKAN is set, but "  \
				          "there is no usable Vulkan device: " \
				       << lotus_problem_;                      \
			}                                                      \
			GTEST_SKIP() << "no usable Vulkan device: "            \
			             << lotus_problem_;                        \
		}                                                              \
	} while (false)

/// Skips the current test when a window on SDL's offscreen video driver cannot
/// get a headless surface, which is a limit of the driver, not a bug. Unlike a
/// missing device, LOTUS_REQUIRE_VULKAN does not turn it into a failure.
#define LOTUS_SKIP_WITHOUT_HEADLESS_SURFACE()                                  \
	do {                                                                   \
		const std::string& lotus_problem_ =                            \
		        ::lotus::test::vulkan_probe().no_headless_surface;     \
		if (!lotus_problem_.empty()) {                                 \
			GTEST_SKIP() << lotus_problem_;                        \
		}                                                              \
	} while (false)
