#include "gpu/vk/vk_context.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace lotus::vk {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL
debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
               VkDebugUtilsMessageTypeFlagsEXT /*types*/,
               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
	static_cast<Context*>(user)->note_validation_message();
	std::fprintf(stderr, "vulkan: %s\n", data->pMessage);
	return VK_FALSE; // never abort the call that triggered the message
}

int device_rank(VkPhysicalDeviceType type)
{
	switch (type) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 1;
	default: return 0;
	}
}

} // namespace

Context::~Context()
{
	shutdown();
}

void Context::init(const ContextOptions& options)
{
	if (valid()) {
		return;
	}
	if (!SDL_Vulkan_LoadLibrary(nullptr)) {
		throw std::runtime_error(std::string{"no Vulkan loader: "} +
		                         SDL_GetError());
	}
	loaded_library_ = true;
	try {
		create(options);
	} catch (...) {
		shutdown();
		throw;
	}
}

void Context::create(const ContextOptions& options)
{
	api.load_global(reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	        SDL_Vulkan_GetVkGetInstanceProcAddr()));

	uint32_t loader_version = 0;
	check(api.EnumerateInstanceVersion(&loader_version),
	      "vkEnumerateInstanceVersion");
	if (loader_version < VK_API_VERSION_1_3) {
		throw std::runtime_error(
		        "Vulkan 1.3 is required, the loader supports " +
		        std::to_string(VK_VERSION_MAJOR(loader_version)) + "." +
		        std::to_string(VK_VERSION_MINOR(loader_version)));
	}

	// Validation needs the layer and the debug-utils extension; without both
	// there is nothing to enable, which is not an error.
	const char* env     = std::getenv("LOTUS_VK_VALIDATION");
	const bool env_on   = env != nullptr && *env != '\0' &&
	                      std::strcmp(env, "0") != 0;
	const bool want_sync = env != nullptr && std::strcmp(env, "sync") == 0;
	bool validation      = options.validation || env_on;

	const auto instance_extensions = [&](const char* layer) {
		uint32_t count = 0;
		api.EnumerateInstanceExtensionProperties(layer, &count, nullptr);
		std::vector<VkExtensionProperties> list(count);
		api.EnumerateInstanceExtensionProperties(layer, &count, list.data());
		return list;
	};
	const auto has_extension = [](const std::vector<VkExtensionProperties>& list,
	                              const char* name) {
		return std::any_of(list.begin(), list.end(), [&](const auto& e) {
			return std::strcmp(e.extensionName, name) == 0;
		});
	};

	std::vector<const char*> layers;
	std::vector<const char*> extensions;
	bool sync_validation = false;
	if (validation) {
		uint32_t count = 0;
		api.EnumerateInstanceLayerProperties(&count, nullptr);
		std::vector<VkLayerProperties> available(count);
		api.EnumerateInstanceLayerProperties(&count, available.data());
		const bool layer_present =
		        std::any_of(available.begin(), available.end(), [](const auto& l) {
			        return std::strcmp(l.layerName, kValidationLayer) == 0;
		        });
		validation = layer_present &&
		             has_extension(instance_extensions(nullptr),
		                           VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		if (validation) {
			layers.push_back(kValidationLayer);
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			sync_validation =
			        want_sync &&
			        has_extension(instance_extensions(kValidationLayer),
			                      VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
			if (sync_validation) {
				extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
			}
		}
	}
	if (options.window != nullptr) {
		Uint32 count            = 0;
		const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&count);
		if (sdl == nullptr) {
			throw std::runtime_error(std::string{"no Vulkan surface support: "} +
			                         SDL_GetError());
		}
		extensions.insert(extensions.end(), sdl, sdl + count);
	}

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "lotus";
	app.pEngineName      = "lotus";
	app.apiVersion       = VK_API_VERSION_1_3;

	const VkValidationFeatureEnableEXT enabled_features[] = {
	        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
	VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
	features.enabledValidationFeatureCount = 1;
	features.pEnabledValidationFeatures    = enabled_features;

	VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance_info.pNext                   = sync_validation ? &features : nullptr;
	instance_info.pApplicationInfo        = &app;
	instance_info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
	instance_info.ppEnabledLayerNames     = layers.data();
	instance_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	instance_info.ppEnabledExtensionNames = extensions.data();
	check(api.CreateInstance(&instance_info, nullptr, &instance),
	      "vkCreateInstance");
	api.load_instance(instance);

	if (validation && api.CreateDebugUtilsMessengerEXT != nullptr) {
		VkDebugUtilsMessengerCreateInfoEXT messenger{
		        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		messenger.messageSeverity =
		        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messenger.pfnUserCallback = debug_callback;
		messenger.pUserData       = this;
		check(api.CreateDebugUtilsMessengerEXT(instance, &messenger, nullptr,
		                                       &messenger_),
		      "vkCreateDebugUtilsMessengerEXT");
	}

	if (options.window != nullptr &&
	    !SDL_Vulkan_CreateSurface(options.window, instance, nullptr, &surface)) {
		throw std::runtime_error(std::string{"cannot create a Vulkan surface: "} +
		                         SDL_GetError());
	}

	pick_physical_device(options.window != nullptr);

	// Everything below was verified by pick_physical_device.
	VkPhysicalDeviceVulkan13Features features13{
	        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	features13.dynamicRendering = VK_TRUE;
	features13.synchronization2 = VK_TRUE;
	VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features2.pNext                       = &features13;
	features2.features.depthClamp         = VK_TRUE;
	features2.features.independentBlend   = VK_TRUE;

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue_info.queueFamilyIndex = queue_family;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &priority;

	std::vector<const char*> device_extensions;
	if (options.window != nullptr) {
		device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
	VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	device_info.pNext                   = &features2;
	device_info.queueCreateInfoCount    = 1;
	device_info.pQueueCreateInfos       = &queue_info;
	device_info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	device_info.ppEnabledExtensionNames = device_extensions.data();
	check(api.CreateDevice(physical_device, &device_info, nullptr, &device),
	      "vkCreateDevice");
	api.load_device(device);
	api.GetDeviceQueue(device, queue_family, 0, &queue);

	VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool_info.queueFamilyIndex = queue_family;
	check(api.CreateCommandPool(device, &pool_info, nullptr, &one_shot_pool_),
	      "vkCreateCommandPool");
	VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	check(api.CreateFence(device, &fence_info, nullptr, &one_shot_fence_),
	      "vkCreateFence");
}

void Context::pick_physical_device(bool windowed)
{
	uint32_t count = 0;
	check(api.EnumeratePhysicalDevices(instance, &count, nullptr),
	      "vkEnumeratePhysicalDevices");
	std::vector<VkPhysicalDevice> devices(count);
	check(api.EnumeratePhysicalDevices(instance, &count, devices.data()),
	      "vkEnumeratePhysicalDevices");

	int best_rank = -1;
	std::string rejected;
	for (VkPhysicalDevice candidate : devices) {
		VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		api.GetPhysicalDeviceProperties2(candidate, &props);
		const auto reject = [&](const char* why) {
			rejected += std::string{"\n  "} + props.properties.deviceName + ": " + why;
		};
		if (props.properties.apiVersion < VK_API_VERSION_1_3) {
			reject("does not support Vulkan 1.3");
			continue;
		}

		VkPhysicalDeviceVulkan13Features f13{
		        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
		api.GetPhysicalDeviceFeatures2(candidate, &f2);
		if (!f13.dynamicRendering || !f13.synchronization2 ||
		    !f2.features.depthClamp || !f2.features.independentBlend) {
			reject("lacks dynamicRendering, synchronization2, depthClamp or "
			       "independentBlend");
			continue;
		}

		if (windowed) {
			uint32_t extension_count = 0;
			api.EnumerateDeviceExtensionProperties(candidate, nullptr,
			                                       &extension_count, nullptr);
			std::vector<VkExtensionProperties> extensions(extension_count);
			api.EnumerateDeviceExtensionProperties(candidate, nullptr,
			                                       &extension_count,
			                                       extensions.data());
			const bool swapchain = std::any_of(
			        extensions.begin(), extensions.end(), [](const auto& e) {
				        return std::strcmp(e.extensionName,
				                           VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
			        });
			if (!swapchain) {
				reject("has no VK_KHR_swapchain");
				continue;
			}
		}

		uint32_t family_count = 0;
		api.GetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
		std::vector<VkQueueFamilyProperties> families(family_count);
		api.GetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
		                                           families.data());
		uint32_t chosen_family = UINT32_MAX;
		for (uint32_t family = 0; family < family_count; ++family) {
			constexpr VkQueueFlags kNeeded =
			        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
			if ((families[family].queueFlags & kNeeded) != kNeeded) {
				continue;
			}
			if (windowed) {
				VkBool32 present = VK_FALSE;
				api.GetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface,
				                                       &present);
				if (!present) {
					continue;
				}
			}
			chosen_family = family;
			break;
		}
		if (chosen_family == UINT32_MAX) {
			reject(windowed ? "has no graphics+compute queue that can present"
			                : "has no graphics+compute queue");
			continue;
		}

		const int rank = device_rank(props.properties.deviceType);
		if (rank > best_rank) {
			best_rank       = rank;
			physical_device = candidate;
			queue_family    = chosen_family;
		}
	}
	if (physical_device == VK_NULL_HANDLE) {
		throw std::runtime_error("no suitable Vulkan device" + rejected);
	}

	VkPhysicalDeviceDriverProperties driver{
	        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
	api.GetPhysicalDeviceProperties2(physical_device, &props);
	properties  = props.properties;
	driver_name = driver.driverName;
	api.GetPhysicalDeviceMemoryProperties(physical_device, &memory);
}

void Context::shutdown()
{
	if (device != VK_NULL_HANDLE) {
		api.DeviceWaitIdle(device);
		if (one_shot_fence_ != VK_NULL_HANDLE) {
			api.DestroyFence(device, one_shot_fence_, nullptr);
		}
		if (one_shot_pool_ != VK_NULL_HANDLE) {
			api.DestroyCommandPool(device, one_shot_pool_, nullptr);
		}
		api.DestroyDevice(device, nullptr);
	}
	one_shot_fence_ = VK_NULL_HANDLE;
	one_shot_pool_  = VK_NULL_HANDLE;
	device          = VK_NULL_HANDLE;
	queue           = VK_NULL_HANDLE;
	physical_device = VK_NULL_HANDLE;
	if (surface != VK_NULL_HANDLE) {
		SDL_Vulkan_DestroySurface(instance, surface, nullptr);
		surface = VK_NULL_HANDLE;
	}
	if (messenger_ != VK_NULL_HANDLE) {
		api.DestroyDebugUtilsMessengerEXT(instance, messenger_, nullptr);
		messenger_ = VK_NULL_HANDLE;
	}
	if (instance != VK_NULL_HANDLE) {
		api.DestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
	if (loaded_library_) {
		SDL_Vulkan_UnloadLibrary();
		loaded_library_ = false;
	}
}

void Context::submit_and_wait(const std::function<void(VkCommandBuffer)>& record)
{
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool        = one_shot_pool_;
	allocate.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	VkCommandBuffer cmd{VK_NULL_HANDLE};
	check(api.AllocateCommandBuffers(device, &allocate, &cmd),
	      "vkAllocateCommandBuffers");
	try {
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		check(api.BeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
		record(cmd);
		check(api.EndCommandBuffer(cmd), "vkEndCommandBuffer");

		VkCommandBufferSubmitInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
		cmd_info.commandBuffer = cmd;
		VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
		submit.commandBufferInfoCount = 1;
		submit.pCommandBufferInfos    = &cmd_info;
		check(api.QueueSubmit2(queue, 1, &submit, one_shot_fence_), "vkQueueSubmit2");
		check(api.WaitForFences(device, 1, &one_shot_fence_, VK_TRUE, UINT64_MAX),
		      "vkWaitForFences");
		check(api.ResetFences(device, 1, &one_shot_fence_), "vkResetFences");
	} catch (...) {
		api.FreeCommandBuffers(device, one_shot_pool_, 1, &cmd);
		throw;
	}
	api.FreeCommandBuffers(device, one_shot_pool_, 1, &cmd);
}

} // namespace lotus::vk
