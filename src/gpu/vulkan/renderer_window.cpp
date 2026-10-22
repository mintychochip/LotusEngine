// Windowed path: swapchain, present and the ImGui backend.
//
// Behavioural reference: src/gpu/sdlgpu/renderer_sdlgpu.cpp (begin_frame's
// acquire, end_frame's submit, init_ui_backend/render_ui). The swapchain
// honours FIFO by default with LOTUS_PRESENT_MODE=mailbox|immediate opting
// into the tearing/latency modes when the surface offers them, carries
// max(3, minImageCount) images, and is rebuilt on OUT_OF_DATE/SUBOPTIMAL and
// on resize. The shaders sRGB-encode, so the format is never an _SRGB one:
// B8G8R8A8_UNORM preferred, R8G8B8A8_UNORM fallback.
//
// The session's colour format follows the swapchain (see
// pipeline_color_format_): offscreen sessions never come here, so their
// pipelines stay byte-identical to the verified path.

#include "gpu/vulkan/renderer_vulkan.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace lotus {
namespace {

/// Number of swapchain images: triple buffering even when the surface would
/// allow fewer, so the two frame slots never stall on acquire.
constexpr uint32_t kDesiredImageCount = 3;

/// Descriptors the ImGui backend may hold: the font atlas plus the viewport
/// texture and headroom for one retired epoch.
constexpr uint32_t kUiDescriptorPoolSize = 64;

/// Loads Vulkan entry points for the ImGui backend through the context's
/// instance loader (nothing links libvulkan). The backend asks for instance-
/// and device-level names alike, and vkGetInstanceProcAddr resolves both;
/// vkGetDeviceProcAddr is invalid for instance-level names.
PFN_vkVoidFunction ui_vulkan_loader(const char* name, void* user)
{
	auto* context = static_cast<vk::Context*>(user);
	if (context->api.GetInstanceProcAddr == nullptr) {
		return nullptr;
	}
	return context->api.GetInstanceProcAddr(context->instance, name);
}

/// ImGui result hook: failures throw like every other vk::check in the
/// backend instead of aborting or going silent.
void ui_check_result(VkResult result)
{
	if (result != VK_SUCCESS) {
		throw std::runtime_error(
		        std::string{"ImGui Vulkan backend failed: "} +
		        vk::result_name(result));
	}
}

/// Present mode honouring LOTUS_PRESENT_MODE, FIFO when unset, unknown or
/// unsupported (FIFO is the only mode a surface must offer).
VkPresentModeKHR choose_present_mode(const vk::Api& api,
                                     VkPhysicalDevice device,
                                     VkSurfaceKHR surface)
{
	uint32_t count = 0;
	vk::check(api.GetPhysicalDeviceSurfacePresentModesKHR(device, surface,
	                                                      &count, nullptr),
	          "vkGetPhysicalDeviceSurfacePresentModesKHR");
	std::vector<VkPresentModeKHR> modes(count);
	vk::check(api.GetPhysicalDeviceSurfacePresentModesKHR(device, surface,
	                                                      &count, modes.data()),
	          "vkGetPhysicalDeviceSurfacePresentModesKHR");
	const char* wanted = std::getenv("LOTUS_PRESENT_MODE");
	VkPresentModeKHR preferred = VK_PRESENT_MODE_FIFO_KHR;
	if (wanted != nullptr) {
		if (std::strcmp(wanted, "mailbox") == 0) {
			preferred = VK_PRESENT_MODE_MAILBOX_KHR;
		} else if (std::strcmp(wanted, "immediate") == 0) {
			preferred = VK_PRESENT_MODE_IMMEDIATE_KHR;
		}
	}
	if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
		return preferred;
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

/// B8G8R8A8_UNORM preferred, R8G8B8A8_UNORM fallback, sRGB-nonlinear colour
/// space preferred within a format; never an _SRGB format (the shaders
/// sRGB-encode already).
VkSurfaceFormatKHR choose_surface_format(const vk::Api& api,
                                         VkPhysicalDevice device,
                                         VkSurfaceKHR surface)
{
	uint32_t count = 0;
	vk::check(api.GetPhysicalDeviceSurfaceFormatsKHR(device, surface,
	                                                 &count, nullptr),
	          "vkGetPhysicalDeviceSurfaceFormatsKHR");
	std::vector<VkSurfaceFormatKHR> formats(count);
	vk::check(api.GetPhysicalDeviceSurfaceFormatsKHR(device, surface,
	                                                 &count, formats.data()),
	          "vkGetPhysicalDeviceSurfaceFormatsKHR");
	if (formats.empty()) {
		throw std::runtime_error("surface offers no formats");
	}
	constexpr VkFormat kPreferred[2] = {VK_FORMAT_B8G8R8A8_UNORM,
	                                    VK_FORMAT_R8G8B8A8_UNORM};
	for (VkFormat candidate : kPreferred) {
		for (const VkSurfaceFormatKHR& format : formats) {
			if (format.format == candidate &&
			    format.colorSpace ==
			            VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				return format;
			}
		}
	}
	for (VkFormat candidate : kPreferred) {
		for (const VkSurfaceFormatKHR& format : formats) {
			if (format.format == candidate) {
				return format;
			}
		}
	}
	return formats[0];
}

} // namespace

vk::Image GpuRendererVulkan::windowed_image() const
{
	vk::Image image{};
	image.handle     = swapchain_images_[swapchain_index_];
	image.view       = swapchain_views_[swapchain_index_];
	image.format     = swapchain_format_;
	image.width      = swapchain_width_;
	image.height     = swapchain_height_;
	image.mip_levels = 1;
	return image;
}

void GpuRendererVulkan::on_swapchain_format(VkFormat format)
{
	if (format == pipeline_color_format_) {
		return;
	}
	// Called from create_swapchain, which has already idled the device, so
	// destroying pipelines here is safe.
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	if (lit_pipeline_ != VK_NULL_HANDLE) {
		api.DestroyPipeline(device, lit_pipeline_, nullptr);
		lit_pipeline_ = VK_NULL_HANDLE;
	}
	if (sky_pipeline_ != VK_NULL_HANDLE) {
		api.DestroyPipeline(device, sky_pipeline_, nullptr);
		sky_pipeline_ = VK_NULL_HANDLE;
	}
	sky_format_ = VK_FORMAT_UNDEFINED;
	// SSAO keeps its own R8G8B8A8 target and pipeline in both sessions.
	destroy_fxaa();
	pipeline_color_format_ = format;
	create_lit_pipeline();
	// The viewport images are in the old format: retire them and forget the
	// size, so the application re-resizes (and refresh_viewport_ui_texture
	// re-registers the new view) on the next frame.
	if (viewport_ui_set_ != VK_NULL_HANDLE && ui_ready_) {
		ImGui_ImplVulkan_RemoveTexture(viewport_ui_set_);
		viewport_ui_set_ = VK_NULL_HANDLE;
	}
	retire_image(viewport_color_);
	viewport_color_ = vk::Image{};
	retire_image(viewport_depth_);
	viewport_depth_ = vk::Image{};
	retire_image(viewport_linear_);
	viewport_linear_ = vk::Image{};
	viewport_color_state_  = vk::ImageState{};
	viewport_depth_state_  = vk::ImageState{};
	viewport_linear_state_ = vk::ImageState{};
	viewport_width_        = 0;
	viewport_height_       = 0;
	ui_pipeline_ready_ = false;
	ui_pipeline_format_ = VK_FORMAT_UNDEFINED;
}

void GpuRendererVulkan::create_swapchain(uint32_t width, uint32_t height)
{
	const vk::Api& api         = context_.api;
	const VkDevice device      = context_.device;
	const VkSurfaceKHR surface = context_.surface;
	if (api.CreateSwapchainKHR == nullptr || surface == VK_NULL_HANDLE) {
		throw std::runtime_error("swapchain unavailable on this device");
	}
	VkSurfaceCapabilitiesKHR caps{};
	vk::check(api.GetPhysicalDeviceSurfaceCapabilitiesKHR(
	                  context_.physical_device, surface, &caps),
	          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	VkExtent2D extent{width, height};
	if (caps.currentExtent.width != UINT32_MAX) {
		// The surface dictates the size (some window managers do).
		extent = caps.currentExtent;
	} else {
		extent.width = std::clamp(extent.width, caps.minImageExtent.width,
		                          caps.maxImageExtent.width);
		extent.height = std::clamp(extent.height,
		                           caps.minImageExtent.height,
		                           caps.maxImageExtent.height);
	}
	if (extent.width == 0 || extent.height == 0) {
		throw std::runtime_error("window has no pixels to present to");
	}
	const VkSurfaceFormatKHR surface_format =
	        choose_surface_format(api, context_.physical_device, surface);
	const VkPresentModeKHR present_mode =
	        choose_present_mode(api, context_.physical_device, surface);
	uint32_t image_count =
	        std::max(kDesiredImageCount, caps.minImageCount);
	if (caps.maxImageCount > 0) {
		image_count = std::min(image_count, caps.maxImageCount);
	}
	VkCompositeAlphaFlagBitsKHR alpha =
	        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if ((caps.supportedCompositeAlpha & alpha) == 0) {
		constexpr VkCompositeAlphaFlagBitsKHR kFallback[4] = {
		        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
		        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
		        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		};
		for (VkCompositeAlphaFlagBitsKHR candidate : kFallback) {
			if ((caps.supportedCompositeAlpha & candidate) != 0) {
				alpha = candidate;
				break;
			}
		}
	}
	// Frames in flight still reference the old images, views and present
	// semaphores, so nothing below may be destroyed before the device is
	// idle. A resize keeps the format, so the pipeline rebuild cannot be
	// relied on to wait.
	wait_idle();
	// Pipelines, viewport and the UI pipeline follow the new format.
	on_swapchain_format(surface_format.format);
	for (VkImageView view : swapchain_views_) {
		api.DestroyImageView(device, view, nullptr);
	}
	swapchain_views_.clear();
	swapchain_images_.clear();
	swapchain_states_.clear();
	for (VkSemaphore semaphore : present_semaphores_) {
		api.DestroySemaphore(device, semaphore, nullptr);
	}
	present_semaphores_.clear();
	const VkSwapchainKHR old = swapchain_;
	swapchain_               = VK_NULL_HANDLE;
	VkSwapchainCreateInfoKHR create{};
	create.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface          = surface;
	create.minImageCount    = image_count;
	create.imageFormat      = surface_format.format;
	create.imageColorSpace  = surface_format.colorSpace;
	create.imageExtent      = extent;
	create.imageArrayLayers = 1;
	create.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform     = (caps.supportedTransforms &
	                           VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
	                                  ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
	                                  : caps.currentTransform;
	create.compositeAlpha   = alpha;
	create.presentMode      = present_mode;
	create.clipped          = VK_TRUE;
	create.oldSwapchain     = old;
	VkSwapchainKHR created_swapchain{VK_NULL_HANDLE};
	const VkResult create_result = api.CreateSwapchainKHR(
	        device, &create, nullptr, &created_swapchain);
	// Retired by the call even when the new swapchain could not be made.
	if (old != VK_NULL_HANDLE) {
		api.DestroySwapchainKHR(device, old, nullptr);
	}
	vk::check(create_result, "vkCreateSwapchainKHR");
	swapchain_ = created_swapchain;
	uint32_t created = 0;
	vk::check(api.GetSwapchainImagesKHR(device, swapchain_, &created,
	                                    nullptr),
	          "vkGetSwapchainImagesKHR");
	swapchain_images_.resize(created);
	vk::check(api.GetSwapchainImagesKHR(device, swapchain_, &created,
	                                    swapchain_images_.data()),
	          "vkGetSwapchainImagesKHR");
	for (VkImage image : swapchain_images_) {
		VkImageViewCreateInfo view_info{};
		view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image    = image;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format   = surface_format.format;
		view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		view_info.subresourceRange.baseMipLevel   = 0;
		view_info.subresourceRange.levelCount     = 1;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount     = 1;
		VkImageView view{VK_NULL_HANDLE};
		vk::check(api.CreateImageView(device, &view_info, nullptr, &view),
		          "vkCreateImageView");
		swapchain_views_.push_back(view);
		swapchain_states_.emplace_back();
	}
	VkSemaphoreCreateInfo semaphore_info{};
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (VkSemaphore& semaphore : acquire_semaphores_) {
		if (semaphore == VK_NULL_HANDLE) {
			vk::check(api.CreateSemaphore(device, &semaphore_info, nullptr,
			                              &semaphore),
			          "vkCreateSemaphore");
		}
	}
	// One per image, not per frame: see present_semaphores_.
	present_semaphores_.assign(swapchain_images_.size(),
	                           VkSemaphore{VK_NULL_HANDLE});
	for (VkSemaphore& semaphore : present_semaphores_) {
		vk::check(api.CreateSemaphore(device, &semaphore_info, nullptr,
		                              &semaphore),
		          "vkCreateSemaphore");
	}
	swapchain_format_  = surface_format.format;
	swapchain_width_   = extent.width;
	swapchain_height_  = extent.height;
	swapchain_index_   = 0;
	swapchain_rebuild_ = false;
	create_ui_pipeline();
}

bool GpuRendererVulkan::acquire_swapchain_image()
{
	for (int attempt = 0; attempt < 2; ++attempt) {
		const VkResult result = context_.api.AcquireNextImageKHR(
		        context_.device, swapchain_, UINT64_MAX,
		        acquire_semaphores_[slot_index_], VK_NULL_HANDLE,
		        &swapchain_index_);
		if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
			if (result == VK_SUBOPTIMAL_KHR) {
				swapchain_rebuild_ = true;
			}
			return true;
		}
		if (result == VK_ERROR_OUT_OF_DATE_KHR && attempt == 0) {
			// The window changed under us: rebuild at the stored size and
			// try once more; anything else leaves the frame skipped.
			create_swapchain(swapchain_width_, swapchain_height_);
			continue;
		}
		if (result == VK_TIMEOUT || result == VK_NOT_READY) {
			return false;
		}
		vk::check(result, "vkAcquireNextImageKHR");
		return false;
	}
	return false;
}

void GpuRendererVulkan::begin_frame(SDL_Window* window, float clear_r,
                                    float clear_g, float clear_b)
{
	clear_r_ = clear_r;
	clear_g_ = clear_g;
	clear_b_ = clear_b;
	// Offscreen sessions never come here (they use begin_frame_offscreen):
	// record the clear colour and report no frame.
	if (!windowed_ || !device_valid_ || window == nullptr) {
		return;
	}
	poll_pyramid();
	int pixels_w = 0;
	int pixels_h = 0;
	SDL_GetWindowSizeInPixels(window, &pixels_w, &pixels_h);
	if (pixels_w <= 0 || pixels_h <= 0) {
		// Minimized: no swapchain image exists, the frame is skipped and
		// has_frame() stays false.
		return;
	}
	const uint32_t want_w = static_cast<uint32_t>(pixels_w);
	const uint32_t want_h = static_cast<uint32_t>(pixels_h);
	if (swapchain_ == VK_NULL_HANDLE || swapchain_rebuild_ ||
	    want_w != swapchain_width_ || want_h != swapchain_height_) {
		try {
			create_swapchain(want_w, want_h);
		} catch (const std::exception& error) {
			if (swapchain_format_ == VK_FORMAT_UNDEFINED) {
				// No swapchain was ever made: this surface and driver
				// cannot present at all, which retrying will not change,
				// and a window that runs but never draws is no fallback.
				throw;
			}
			// A rebuild can fail while the window is mid-resize: skip the
			// frame and retry, saying so once per distinct failure.
			if (swapchain_error_ != error.what()) {
				swapchain_error_ = error.what();
				std::cerr << "swapchain: " << error.what()
				          << "; skipping frames until it can be rebuilt\n";
			}
			return;
		}
		swapchain_error_.clear();
	}
	// The slot's previous frame must have finished before its acquire
	// semaphore is signalled again, so its fence is waited first. The slot
	// is only taken (fence reset) once the image is in hand: a skipped
	// acquire must leave the fence signalled, or the next frame would wait
	// on it forever.
	FrameSlot& next = slots_[slot_index_];
	wait_slot(next);
	if (!acquire_swapchain_image()) {
		return;
	}
	// The acquire reads the image and the submit waits on it at
	// kAcquireWaitStage: start the image's tracked state there, so its first
	// transition (a write) names that stage as its source.
	swapchain_states_[swapchain_index_].stage  = kAcquireWaitStage;
	swapchain_states_[swapchain_index_].access = 0;
	FrameSlot& slot = recycle_slot(next);
	slot_           = &slot;
	cmd_            = slot.cmd;
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk::check(context_.api.BeginCommandBuffer(cmd_, &begin),
	          "vkBeginCommandBuffer");
	begin_geometry_frame();
	shadow_rendered_ = false;
	sky_drawn_       = false;
	ensure_depth_target(swapchain_width_, swapchain_height_);
	ensure_pyramid(swapchain_width_, swapchain_height_);
}

void GpuRendererVulkan::end_frame_windowed()
{
	vk::Image windowed = windowed_image();
	image_present_src(*barriers_, cmd_, windowed,
	                  swapchain_states_[swapchain_index_]);
	barriers_->flush(cmd_);
	end_frame_submit(acquire_semaphores_[slot_index_],
	                 present_semaphores_[swapchain_index_]);
	present_swapchain();
}

void GpuRendererVulkan::present_swapchain()
{
	VkPresentInfoKHR present{};
	present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores    = &present_semaphores_[swapchain_index_];
	present.swapchainCount     = 1;
	present.pSwapchains        = &swapchain_;
	present.pImageIndices      = &swapchain_index_;
	const VkResult result =
	        context_.api.QueuePresentKHR(context_.queue, &present);
	if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
		// Rebuilt in the next begin_frame, before acquiring.
		swapchain_rebuild_ = true;
	} else if (result != VK_SUCCESS) {
		vk::check(result, "vkQueuePresentKHR");
	}
}

void GpuRendererVulkan::destroy_swapchain()
{
	if (!device_valid_) {
		swapchain_ = VK_NULL_HANDLE;
		return;
	}
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	for (VkImageView view : swapchain_views_) {
		api.DestroyImageView(device, view, nullptr);
	}
	swapchain_views_.clear();
	swapchain_images_.clear();
	swapchain_states_.clear();
	if (swapchain_ != VK_NULL_HANDLE) {
		api.DestroySwapchainKHR(device, swapchain_, nullptr);
		swapchain_ = VK_NULL_HANDLE;
	}
	for (VkSemaphore& semaphore : acquire_semaphores_) {
		if (semaphore != VK_NULL_HANDLE) {
			api.DestroySemaphore(device, semaphore, nullptr);
			semaphore = VK_NULL_HANDLE;
		}
	}
	for (VkSemaphore semaphore : present_semaphores_) {
		api.DestroySemaphore(device, semaphore, nullptr);
	}
	present_semaphores_.clear();
	swapchain_format_  = VK_FORMAT_UNDEFINED;
	swapchain_width_   = 0;
	swapchain_height_  = 0;
	swapchain_index_   = 0;
	swapchain_rebuild_ = false;
}

bool GpuRendererVulkan::init_ui_backend()
{
	if (ui_ready_) {
		return true;
	}
	if (!device_valid_) {
		return false;
	}
	// The backend resolves every Vulkan call through the context (nothing
	// links libvulkan); dynamic rendering needs no render pass.
	if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,
	                                    ui_vulkan_loader, &context_)) {
		return false;
	}
	try {
		ImGui_ImplVulkan_InitInfo info{};
		info.ApiVersion         = VK_API_VERSION_1_3;
		info.Instance           = context_.instance;
		info.PhysicalDevice     = context_.physical_device;
		info.Device             = context_.device;
		info.QueueFamily        = context_.queue_family;
		info.Queue              = context_.queue;
		info.DescriptorPoolSize = kUiDescriptorPoolSize;
		info.MinImageCount      = GpuRenderer::kFramesInFlight;
		info.ImageCount         = GpuRenderer::kFramesInFlight;
		info.UseDynamicRendering = true;
		info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		// The swapchain format is known only after the first acquire, so
		// the main pipeline is created then (create_ui_pipeline); leaving
		// the rendering info empty skips it here.
		info.CheckVkResultFn = ui_check_result;
		if (!ImGui_ImplVulkan_Init(&info)) {
			return false;
		}
	} catch (const std::exception&) {
		ImGui_ImplVulkan_Shutdown();
		return false;
	}
	ui_ready_ = true;
	if (viewport_color_.handle != VK_NULL_HANDLE) {
		refresh_viewport_ui_texture();
	}
	if (swapchain_ != VK_NULL_HANDLE) {
		create_ui_pipeline();
	}
	return true;
}

void GpuRendererVulkan::create_ui_pipeline()
{
	if (!ui_ready_ || swapchain_ == VK_NULL_HANDLE) {
		return;
	}
	if (ui_pipeline_ready_ && ui_pipeline_format_ == swapchain_format_) {
		return;
	}
	// The backend deep-copies the format array, so the member outlives the
	// call either way.
	ImGui_ImplVulkan_PipelineInfo pipeline_info{};
	pipeline_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	pipeline_info.PipelineRenderingCreateInfo.sType =
	        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	pipeline_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	pipeline_info.PipelineRenderingCreateInfo.pColorAttachmentFormats =
	        &swapchain_format_;
	pipeline_info.PipelineRenderingCreateInfo.depthAttachmentFormat =
	        VK_FORMAT_UNDEFINED;
	pipeline_info.PipelineRenderingCreateInfo.stencilAttachmentFormat =
	        VK_FORMAT_UNDEFINED;
	ImGui_ImplVulkan_CreateMainPipeline(&pipeline_info);
	ui_pipeline_format_ = swapchain_format_;
	ui_pipeline_ready_  = true;
}

void GpuRendererVulkan::shutdown_ui_backend()
{
	if (!ui_ready_) {
		return;
	}
	if (device_valid_) {
		context_.api.DeviceWaitIdle(context_.device);
	}
	// The backend owns its descriptor pool (DescriptorPoolSize path) and
	// destroys it; the viewport set dies with it.
	ImGui_ImplVulkan_Shutdown();
	ui_ready_          = false;
	ui_pipeline_ready_ = false;
	ui_pipeline_format_ = VK_FORMAT_UNDEFINED;
	viewport_ui_set_   = VK_NULL_HANDLE;
}

void GpuRendererVulkan::ui_backend_new_frame()
{
	if (ui_ready_) {
		ImGui_ImplVulkan_NewFrame();
	}
}

void GpuRendererVulkan::render_ui(ImDrawData* draw_data)
{
	if (!ui_ready_ || !ui_pipeline_ready_ || draw_data == nullptr ||
	    draw_data->CmdListsCount == 0 || cmd_ == VK_NULL_HANDLE ||
	    slot_ == nullptr || !windowed_target()) {
		return;
	}
	// The viewport texture the editor samples: transition it for the read
	// the draw below performs. Re-register it first when the view changed
	// under us (resize re-registers eagerly; this covers a viewport that
	// appeared after the backend was created).
	if (viewport_color_.handle != VK_NULL_HANDLE) {
		if (viewport_ui_set_ == VK_NULL_HANDLE) {
			refresh_viewport_ui_texture();
		}
		if (viewport_ui_set_ != VK_NULL_HANDLE) {
			image_shader_read(*barriers_, cmd_, viewport_color_, 0, 1,
			                  viewport_color_state_);
		}
	}
	vk::Image windowed = windowed_image();
	image_as_color_attachment(*barriers_, cmd_, windowed, 0, 1,
	                          swapchain_states_[swapchain_index_]);
	barriers_->flush(cmd_);
	// The backend records (viewport/scissor, binds, draws) but opens no
	// pass itself: draw over the scene, keeping what is there.
	VkRenderingAttachmentInfo color{};
	color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView   = windowed.view;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rendering{};
	rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea =
	        VkRect2D{{0, 0}, {swapchain_width_, swapchain_height_}};
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments    = &color;
	context_.api.CmdBeginRendering(cmd_, &rendering);
	ImGui_ImplVulkan_RenderDrawData(draw_data, cmd_);
	context_.api.CmdEndRendering(cmd_);
}

uint64_t GpuRendererVulkan::viewport_ui_texture() const
{
	return reinterpret_cast<uint64_t>(viewport_ui_set_);
}

void GpuRendererVulkan::refresh_viewport_ui_texture()
{
	if (!ui_ready_ || viewport_color_.handle == VK_NULL_HANDLE) {
		return;
	}
	if (viewport_ui_set_ != VK_NULL_HANDLE) {
		// Frames in flight may still read the old set: drain first.
		wait_idle();
		ImGui_ImplVulkan_RemoveTexture(viewport_ui_set_);
		viewport_ui_set_ = VK_NULL_HANDLE;
	}
	viewport_ui_set_ = ImGui_ImplVulkan_AddTexture(
	        viewport_color_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace lotus
