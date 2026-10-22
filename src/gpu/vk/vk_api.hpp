#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>

namespace lotus::vk {

// Every entry point the renderer calls, listed once per level so the table,
// the loader and the missing-function check cannot drift apart. The "optional"
// lists belong to extensions that are only enabled for windowed rendering or
// validation; code that uses them checks for null.

#define LOTUS_VK_GLOBAL_COMMANDS(X)                                            \
	X(CreateInstance)                                                          \
	X(EnumerateInstanceVersion)                                                \
	X(EnumerateInstanceExtensionProperties)                                    \
	X(EnumerateInstanceLayerProperties)

#define LOTUS_VK_INSTANCE_COMMANDS(X)                                          \
	X(DestroyInstance)                                                         \
	X(EnumeratePhysicalDevices)                                                \
	X(GetPhysicalDeviceProperties2)                                            \
	X(GetPhysicalDeviceFeatures2)                                              \
	X(GetPhysicalDeviceMemoryProperties)                                       \
	X(GetPhysicalDeviceQueueFamilyProperties)                                  \
	X(GetPhysicalDeviceFormatProperties)                                       \
	X(EnumerateDeviceExtensionProperties)                                      \
	X(CreateDevice)                                                            \
	X(GetDeviceProcAddr)

#define LOTUS_VK_INSTANCE_OPTIONAL_COMMANDS(X)                                 \
	X(GetPhysicalDeviceSurfaceSupportKHR)                                      \
	X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                 \
	X(GetPhysicalDeviceSurfaceFormatsKHR)                                      \
	X(GetPhysicalDeviceSurfacePresentModesKHR)                                 \
	X(CreateDebugUtilsMessengerEXT)                                            \
	X(DestroyDebugUtilsMessengerEXT)                                           \
	X(SetDebugUtilsObjectNameEXT)

#define LOTUS_VK_DEVICE_COMMANDS(X)                                            \
	X(DestroyDevice)                                                           \
	X(GetDeviceQueue)                                                          \
	X(DeviceWaitIdle)                                                          \
	X(QueueSubmit2)                                                            \
	X(AllocateMemory)                                                          \
	X(FreeMemory)                                                              \
	X(MapMemory)                                                               \
	X(UnmapMemory)                                                             \
	X(FlushMappedMemoryRanges)                                                 \
	X(InvalidateMappedMemoryRanges)                                            \
	X(BindBufferMemory)                                                        \
	X(BindImageMemory)                                                         \
	X(GetBufferMemoryRequirements)                                             \
	X(GetImageMemoryRequirements)                                              \
	X(CreateBuffer)                                                            \
	X(DestroyBuffer)                                                           \
	X(CreateImage)                                                             \
	X(DestroyImage)                                                            \
	X(CreateImageView)                                                         \
	X(DestroyImageView)                                                        \
	X(CreateSampler)                                                           \
	X(DestroySampler)                                                          \
	X(CreateShaderModule)                                                      \
	X(DestroyShaderModule)                                                     \
	X(CreateDescriptorSetLayout)                                               \
	X(DestroyDescriptorSetLayout)                                              \
	X(CreatePipelineLayout)                                                    \
	X(DestroyPipelineLayout)                                                   \
	X(CreateGraphicsPipelines)                                                 \
	X(CreateComputePipelines)                                                  \
	X(DestroyPipeline)                                                         \
	X(CreateDescriptorPool)                                                    \
	X(DestroyDescriptorPool)                                                   \
	X(ResetDescriptorPool)                                                     \
	X(AllocateDescriptorSets)                                                  \
	X(FreeDescriptorSets)                                                      \
	X(UpdateDescriptorSets)                                                    \
	X(CreateCommandPool)                                                       \
	X(DestroyCommandPool)                                                      \
	X(ResetCommandPool)                                                        \
	X(AllocateCommandBuffers)                                                  \
	X(FreeCommandBuffers)                                                      \
	X(BeginCommandBuffer)                                                      \
	X(EndCommandBuffer)                                                        \
	X(CreateFence)                                                             \
	X(DestroyFence)                                                            \
	X(ResetFences)                                                             \
	X(WaitForFences)                                                           \
	X(GetFenceStatus)                                                          \
	X(CreateSemaphore)                                                         \
	X(DestroySemaphore)                                                        \
	X(CmdBindPipeline)                                                         \
	X(CmdSetViewport)                                                          \
	X(CmdSetScissor)                                                           \
	X(CmdBindDescriptorSets)                                                   \
	X(CmdBindVertexBuffers)                                                    \
	X(CmdBindIndexBuffer)                                                      \
	X(CmdDraw)                                                                 \
	X(CmdDrawIndexed)                                                          \
	X(CmdDispatch)                                                             \
	X(CmdCopyBuffer)                                                           \
	X(CmdCopyBufferToImage)                                                    \
	X(CmdCopyImageToBuffer)                                                    \
	X(CmdBlitImage)                                                            \
	X(CmdPipelineBarrier2)                                                     \
	X(CmdBeginRendering)                                                       \
	X(CmdEndRendering)

#define LOTUS_VK_DEVICE_OPTIONAL_COMMANDS(X)                                   \
	X(CreateSwapchainKHR)                                                      \
	X(DestroySwapchainKHR)                                                     \
	X(GetSwapchainImagesKHR)                                                   \
	X(AcquireNextImageKHR)                                                     \
	X(QueuePresentKHR)

/// The loaded entry points, resolved once from the loader `vk::Loader` opens
/// (vk_loader.hpp), so nothing is linked against libvulkan. Device
/// level commands come from vkGetDeviceProcAddr and skip the loader's
/// per-call dispatch. Called as `api.CmdDraw(...)`: the name is the Vulkan
/// one without its "vk" prefix.
struct Api {
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr{nullptr};

#define LOTUS_VK_DECLARE(name) PFN_vk##name name{nullptr};
	LOTUS_VK_GLOBAL_COMMANDS(LOTUS_VK_DECLARE)
	LOTUS_VK_INSTANCE_COMMANDS(LOTUS_VK_DECLARE)
	LOTUS_VK_INSTANCE_OPTIONAL_COMMANDS(LOTUS_VK_DECLARE)
	LOTUS_VK_DEVICE_COMMANDS(LOTUS_VK_DECLARE)
	LOTUS_VK_DEVICE_OPTIONAL_COMMANDS(LOTUS_VK_DECLARE)
#undef LOTUS_VK_DECLARE

	/// Each loader throws std::runtime_error naming the first required
	/// command that is missing.
	void load_global(PFN_vkGetInstanceProcAddr loader);
	void load_instance(VkInstance instance);
	void load_device(VkDevice device);
};

/// "VK_ERROR_OUT_OF_DEVICE_MEMORY" for the results the renderer can meet,
/// "VkResult(<n>)" otherwise.
const char* result_name(VkResult result);

/// Throws std::runtime_error "<what> failed: <result name>".
[[noreturn]] void fail(const char* what, VkResult result);

inline void check(VkResult result, const char* what)
{
	if (result != VK_SUCCESS) {
		fail(what, result);
	}
}

} // namespace lotus::vk
