#include "gpu/vk/vk_api.hpp"

#include <string>

namespace lotus::vk {

void Api::load_global(PFN_vkGetInstanceProcAddr loader)
{
	GetInstanceProcAddr = loader;
#define LOTUS_VK_LOAD(name)                                                    \
	name = reinterpret_cast<PFN_vk##name>(loader(VK_NULL_HANDLE, "vk" #name)); \
	if (name == nullptr) {                                                     \
		throw std::runtime_error("the Vulkan loader has no vk" #name           \
		                         " (Vulkan 1.3 is required)");                 \
	}
	LOTUS_VK_GLOBAL_COMMANDS(LOTUS_VK_LOAD)
#undef LOTUS_VK_LOAD
}

void Api::load_instance(VkInstance instance)
{
#define LOTUS_VK_LOAD(name)                                                    \
	name = reinterpret_cast<PFN_vk##name>(                                     \
	        GetInstanceProcAddr(instance, "vk" #name));                        \
	if (name == nullptr) {                                                     \
		throw std::runtime_error("the Vulkan instance has no vk" #name);       \
	}
#define LOTUS_VK_LOAD_OPTIONAL(name)                                           \
	name = reinterpret_cast<PFN_vk##name>(                                     \
	        GetInstanceProcAddr(instance, "vk" #name));
	LOTUS_VK_INSTANCE_COMMANDS(LOTUS_VK_LOAD)
	LOTUS_VK_INSTANCE_OPTIONAL_COMMANDS(LOTUS_VK_LOAD_OPTIONAL)
#undef LOTUS_VK_LOAD
#undef LOTUS_VK_LOAD_OPTIONAL
}

void Api::load_device(VkDevice device)
{
#define LOTUS_VK_LOAD(name)                                                    \
	name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); \
	if (name == nullptr) {                                                     \
		throw std::runtime_error("the Vulkan device has no vk" #name);         \
	}
#define LOTUS_VK_LOAD_OPTIONAL(name)                                           \
	name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));
	LOTUS_VK_DEVICE_COMMANDS(LOTUS_VK_LOAD)
	LOTUS_VK_DEVICE_OPTIONAL_COMMANDS(LOTUS_VK_LOAD_OPTIONAL)
#undef LOTUS_VK_LOAD
#undef LOTUS_VK_LOAD_OPTIONAL
}

const char* result_name(VkResult result)
{
	switch (result) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
	default: break;
	}
	static thread_local std::string unknown;
	unknown = "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
	return unknown.c_str();
}

void fail(const char* what, VkResult result)
{
	throw std::runtime_error(std::string{what} + " failed: " +
	                         result_name(result));
}

} // namespace lotus::vk
