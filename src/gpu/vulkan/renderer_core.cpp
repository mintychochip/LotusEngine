// Device, frame ring, targets, frame lifecycle and capture.

#include "gpu/vulkan/renderer_vulkan.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace lotus {
namespace {

constexpr uint32_t kInitialVertexBytes = 128 * 1024;
constexpr uint32_t kInitialInstances   = 1024;
constexpr uint64_t kInitialUniforms    = 64 * 1024;

constexpr VkFormat kColorFormat   = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kDepthFormat   = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kPyramidFormat = VK_FORMAT_R32_SFLOAT;

uint64_t align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

GpuRendererVulkan::~GpuRendererVulkan()
{
	shutdown();
}

void GpuRendererVulkan::init(PresentTarget* target, bool debug_device)
{
	(void)debug_device;
	if (device_valid_) {
		return;
	}
	// A target means the context carries the surface extensions and picks a
	// device that can present; null stays offscreen. The swapchain itself is
	// created lazily in begin_frame, when the drawable size is known.
	windowed_ = target != nullptr;
	vk::ContextOptions options;
	options.target     = target;
	options.validation = false;   // LOTUS_VK_VALIDATION=1 still asks inside
	context_.init(options);
	memory_.init(context_);
	device_valid_ = true;
	device_name_ = std::string(
	        reinterpret_cast<const char*>(context_.properties.deviceName));
	device_driver_ = context_.driver_name;
	uniform_alignment_ =
	        std::max<uint64_t>(context_.properties.limits.minUniformBufferOffsetAlignment,
	                           1);
	barriers_ = std::make_unique<vk::BarrierBatch>(context_.api);
	create_samplers();
	create_white_texture();
	create_flat_normal_texture();
	layouts_.create(context_);
	create_descriptor_cache();
	create_pipelines();
	for (FrameSlot& slot : slots_) {
		create_slot(slot);
	}
}

void GpuRendererVulkan::shutdown()
{
	if (!device_valid_) {
		return;
	}
	context_.api.DeviceWaitIdle(context_.device);
	// The UI backend first (its pool may reference the viewport view),
	// then the swapchain (its views), while the wait above still holds.
	shutdown_ui_backend();
	destroy_swapchain();
	// A frame may be open; its command buffer dies with the pool.
	cmd_          = nullptr;
	slot_         = nullptr;
	scene_pass_open_    = false;
	viewport_pass_open_ = false;
	pass_redirected_    = false;
	pass_color_view_    = VK_NULL_HANDLE;
	abandon_geometry_staging();
	geometry_draws_.clear();
	pending_meshes_.clear();
	destroy_meshes();
	destroy_textures();
	destroy_shadows();
	destroy_occlusion();
	destroy_fxaa();
	destroy_ssao();
	destroy_viewport();
	// Retired images are destroyed here, after the wait above: nothing can
	// still reference them. destroy_fxaa/ssao/viewport only retired theirs.
	for (auto& drop : retired_targets_) {
		drop();
	}
	retired_targets_.clear();
	// Deferred frees still queued on the slots (replaced buffers, sets
	// returned to the cache pool) run now, while the pool and everything
	// else they touch is alive.
	for (FrameSlot& slot : slots_) {
		destroy_garbage(slot);
	}
	destroy_pipelines();
	layouts_.destroy(context_);
	destroy_descriptor_cache();
	destroy_samplers();
	destroy_targets();
	for (FrameSlot& slot : slots_) {
		destroy_slot(slot);
	}
	barriers_.reset();
	memory_.shutdown();
	context_.shutdown();
	device_valid_ = false;
	offscreen_   = false;
	windowed_    = false;
}

void GpuRendererVulkan::init_offscreen(uint32_t width, uint32_t height)
{
	if (device_valid_) {
		return;
	}
	if (width == 0 || height == 0) {
		throw std::runtime_error("offscreen size must be non-zero");
	}
	init(nullptr, false);
	offscreen_       = true;
	offscreen_width_  = width;
	offscreen_height_ = height;
	offscreen_color_ = create_target_image(
	        "offscreen", kColorFormat, width, height, 1,
	        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	offscreen_color_state_ = vk::ImageState{};
	occlusion_enabled_ = create_occlusion_pipeline();
	if (!occlusion_enabled_) {
		std::cerr << "occlusion: compute pipeline unavailable, "
		             "continuing without depth culling\n";
	}
	ensure_depth_target(width, height);
	ensure_pyramid(width, height);
}

void GpuRendererVulkan::create_slot(FrameSlot& slot)
{
	const vk::Api& api     = context_.api;
	const VkDevice device  = context_.device;
	VkCommandPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = context_.queue_family;
	vk::check(api.CreateCommandPool(device, &pool_info, nullptr, &slot.pool),
	          "vkCreateCommandPool");
	VkCommandBufferAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc.commandPool        = slot.pool;
	alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	vk::check(api.AllocateCommandBuffers(device, &alloc, &slot.cmd),
	          "vkAllocateCommandBuffers");
	VkFenceCreateInfo fence_info{};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	vk::check(api.CreateFence(device, &fence_info, nullptr, &slot.fence),
	          "vkCreateFence");
	const VkDescriptorPoolSize sizes[3] = {
	        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 128},
	        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048},
	        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
	};
	VkDescriptorPoolCreateInfo pool_create{};
	pool_create.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_create.maxSets       = 1024;
	pool_create.poolSizeCount = 3;
	pool_create.pPoolSizes    = sizes;
	vk::check(api.CreateDescriptorPool(device, &pool_create, nullptr,
	                                   &slot.descriptors),
	          "vkCreateDescriptorPool");
	slot.instances = memory_.create_buffer(
	        vk::MemoryClass::Dynamic,
	        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	        static_cast<uint64_t>(kInitialInstances) * sizeof(GpuInstance),
	        "instances");
	slot.instance_capacity = kInitialInstances;
	slot.immediate = memory_.create_buffer(
	        vk::MemoryClass::Dynamic, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	        kInitialVertexBytes, "immediate");
	slot.immediate_uv = memory_.create_buffer(
	        vk::MemoryClass::Dynamic, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	        kInitialVertexBytes / 2u, "immediate_uv");
	slot.immediate_capacity    = kInitialVertexBytes;
	slot.immediate_uv_capacity = kInitialVertexBytes / 2u;
	slot.uniforms = memory_.create_buffer(vk::MemoryClass::Dynamic,
	                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
	                                      kInitialUniforms, "uniforms");
	slot.uniform_capacity = kInitialUniforms;
	vertex_capacity_      = kInitialVertexBytes;
}

void GpuRendererVulkan::destroy_slot(FrameSlot& slot)
{
	destroy_garbage(slot);
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	if (slot.descriptors != VK_NULL_HANDLE) {
		api.DestroyDescriptorPool(device, slot.descriptors, nullptr);
		slot.descriptors = VK_NULL_HANDLE;
	}
	if (slot.fence != VK_NULL_HANDLE) {
		api.DestroyFence(device, slot.fence, nullptr);
		slot.fence = VK_NULL_HANDLE;
	}
	if (slot.pool != VK_NULL_HANDLE) {
		api.DestroyCommandPool(device, slot.pool, nullptr);
		slot.pool = VK_NULL_HANDLE;
		slot.cmd  = VK_NULL_HANDLE;
	}
	if (slot.instances.handle != VK_NULL_HANDLE) {
		memory_.destroy_buffer(slot.instances);
	}
	if (slot.immediate.handle != VK_NULL_HANDLE) {
		memory_.destroy_buffer(slot.immediate);
	}
	if (slot.immediate_uv.handle != VK_NULL_HANDLE) {
		memory_.destroy_buffer(slot.immediate_uv);
	}
	if (slot.uniforms.handle != VK_NULL_HANDLE) {
		memory_.destroy_buffer(slot.uniforms);
	}
	slot.instance_capacity = 0;
	slot.immediate_capacity = 0;
	slot.immediate_uv_capacity = 0;
	slot.uniform_capacity = 0;
}

void GpuRendererVulkan::destroy_garbage(FrameSlot& slot)
{
	for (auto& drop : slot.garbage) {
		drop();
	}
	slot.garbage.clear();
}

void GpuRendererVulkan::wait_slot(FrameSlot& slot)
{
	// The slot's fence was signalled before its first use (created signalled)
	// and after every submit, so waiting here throttles to kFramesInFlight.
	vk::check(context_.api.WaitForFences(context_.device, 1, &slot.fence,
	                                     VK_TRUE, UINT64_MAX),
	          "vkWaitForFences");
	// The wait just proved this slot's frame finished, and the queue runs
	// in order, so every frame up to its serial is done. Without this, a
	// GPU-bound loop never observes completion in poll_pyramid and the
	// pyramid never rebuilds.
	if (slot.serial > completed_serial_) {
		completed_serial_ = slot.serial;
	}
}

FrameSlot& GpuRendererVulkan::recycle_slot(FrameSlot& slot)
{
	vk::check(context_.api.ResetFences(context_.device, 1, &slot.fence),
	          "vkResetFences");
	destroy_garbage(slot);
	// Retired targets join this slot's garbage: its fence was just waited
	// on, and a full ring turn passes before the slot is reused, so no
	// submitted command buffer can still reference them.
	for (auto& drop : retired_targets_) {
		slot.garbage.push_back(std::move(drop));
	}
	retired_targets_.clear();
	vk::check(context_.api.ResetDescriptorPool(context_.device,
	                                           slot.descriptors, 0),
	          "vkResetDescriptorPool");
	vk::check(context_.api.ResetCommandPool(context_.device, slot.pool, 0),
	          "vkResetCommandPool");
	slot.uniform_offset = 0;
	slot.instance_state = vk::BufferState{};
	slot.immediate_state = vk::BufferState{};
	slot.immediate_uv_state = vk::BufferState{};
	slot.uniform_state = vk::BufferState{};
	return slot;
}

FrameSlot& GpuRendererVulkan::acquire_slot()
{
	FrameSlot& slot = slots_[slot_index_];
	wait_slot(slot);
	return recycle_slot(slot);
}

void GpuRendererVulkan::begin_frame_common()
{
	// Before anything else, so occlusion for this frame uses the previous
	// frame's pyramid. Polls, never waits.
	poll_pyramid();
	FrameSlot& slot = acquire_slot();
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
}

void GpuRendererVulkan::begin_frame_offscreen()
{
	if (!offscreen_ || offscreen_color_.handle == VK_NULL_HANDLE) {
		return;
	}
	begin_frame_common();
	ensure_depth_target(offscreen_width_, offscreen_height_);
	ensure_pyramid(offscreen_width_, offscreen_height_);
}

std::vector<uint8_t> GpuRendererVulkan::capture_rgba(uint32_t& width,
                                                    uint32_t& height)
{
	width  = 0;
	height = 0;
	if (!offscreen_ || offscreen_color_.handle == VK_NULL_HANDLE) {
		return {};
	}
	width  = offscreen_width_;
	height = offscreen_height_;
	return download_image_rgba(offscreen_color_, offscreen_color_state_,
	                           width, height);
}


void GpuRendererVulkan::end_frame_submit(VkSemaphore wait_semaphore,
                                         VkSemaphore signal_semaphore)
{
	vk::check(context_.api.EndCommandBuffer(cmd_), "vkEndCommandBuffer");
	VkCommandBufferSubmitInfo cmd_info{};
	cmd_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmd_info.commandBuffer = cmd_;
	// Windowed frames chain the swapchain through the submit: wait for the
	// acquire before touching the image, signal the present when done. The
	// offscreen path passes null for both and submits bare.
	VkSemaphoreSubmitInfo wait{};
	wait.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	wait.semaphore = wait_semaphore;
	wait.stageMask = kAcquireWaitStage;
	VkSemaphoreSubmitInfo signal{};
	signal.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signal.semaphore = signal_semaphore;
	signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkSubmitInfo2 submit{};
	submit.sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos    = &cmd_info;
	if (wait_semaphore != VK_NULL_HANDLE) {
		submit.waitSemaphoreInfoCount = 1;
		submit.pWaitSemaphoreInfos    = &wait;
	}
	if (signal_semaphore != VK_NULL_HANDLE) {
		submit.signalSemaphoreInfoCount = 1;
		submit.pSignalSemaphoreInfos    = &signal;
	}
	vk::check(context_.api.QueueSubmit2(context_.queue, 1, &submit,
	                                    slot_->fence),
	          "vkQueueSubmit2");
	++frames_rendered_;
	// Frame fences own the ordering the pyramid poll reads: the download
	// recorded above belongs to this serial once submitted.
	slot_->serial = ++submit_serial_;
	if (pyramid_recorded_) {
		pyramid_serial_    = submit_serial_;
		pyramid_in_flight_ = true;
	}
	pyramid_recorded_ = false;
	slot_index_ = (slot_index_ + 1) % GpuRenderer::kFramesInFlight;
	slot_       = nullptr;
	cmd_        = VK_NULL_HANDLE;
}

void GpuRendererVulkan::end_frame()
{
	if (scene_pass_open_) {
		end_scene_pass();
	}
	if (cmd_ == VK_NULL_HANDLE || slot_ == nullptr) {
		// Skipped frame (no swapchain image): safe, nothing was recorded.
		return;
	}
	if (windowed_) {
		end_frame_windowed();
		return;
	}
	end_frame_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void GpuRendererVulkan::wait_idle()
{
	if (!device_valid_) {
		return;
	}
	context_.api.DeviceWaitIdle(context_.device);
	completed_serial_ = submit_serial_;
}

const char* GpuRendererVulkan::device_name() const
{
	return device_valid_ ? device_name_.c_str() : "";
}

const char* GpuRendererVulkan::device_driver() const
{
	return device_valid_ ? device_driver_.c_str() : "";
}

const char* GpuRendererVulkan::driver_name() const
{
	return device_valid_ ? "vulkan" : "";
}

void GpuRendererVulkan::ensure_depth_target(uint32_t width, uint32_t height)
{
	if (depth_.handle != VK_NULL_HANDLE && depth_width_ == width &&
	    depth_height_ == height) {
		return;
	}
	if (depth_.handle != VK_NULL_HANDLE) {
		wait_idle();
		memory_.destroy_image(depth_);
		depth_ = vk::Image{};
	}
	depth_width_  = 0;
	depth_height_ = 0;
	if (width == 0 || height == 0) {
		return;
	}
	depth_ = create_target_image("depth", kDepthFormat, width, height, 1,
	                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	depth_state_  = vk::ImageState{};
	depth_width_  = width;
	depth_height_ = height;
}

void GpuRendererVulkan::ensure_pyramid_stand_in(uint32_t width, uint32_t height)
{
	if (pyramid_.handle != VK_NULL_HANDLE && pyramid_width_ == width &&
	    pyramid_height_ == height && pyramid_levels_ == 1) {
		return;
	}
	if (pyramid_.handle != VK_NULL_HANDLE) {
		wait_idle();
		memory_.destroy_image(pyramid_);
		pyramid_ = vk::Image{};
	}
	for (vk::ImageState& state : pyramid_states_) {
		state = vk::ImageState{};
	}
	pyramid_width_  = 0;
	pyramid_height_ = 0;
	pyramid_levels_ = 0;
	if (width == 0 || height == 0) {
		return;
	}
	// Colour target 1 when the reduction is unavailable: one R32F mip,
	// no mip chain and no compute usage. lit.frag writes location 1
	// unconditionally, so the scene pass cannot simply drop the
	// attachment. Writing the depth into a texture nothing ever reads
	// keeps the two in agreement and costs one mip.
	pyramid_ = create_target_image("pyramid", kPyramidFormat, width, height, 1,
	                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	                                       VK_IMAGE_USAGE_SAMPLED_BIT |
	                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	VkImageViewCreateInfo view_info{};
	view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image    = pyramid_.handle;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format   = kPyramidFormat;
	view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.baseMipLevel   = 0;
	view_info.subresourceRange.levelCount     = 1;
	view_info.subresourceRange.baseArrayLayer = 0;
	view_info.subresourceRange.layerCount     = 1;
	vk::check(context_.api.CreateImageView(context_.device, &view_info,
	                                       nullptr, &pyramid_views_[0]),
	          "vkCreateImageView");
	pyramid_width_  = width;
	pyramid_height_ = height;
	pyramid_levels_ = 1; // nothing to reduce and nothing to read back
}

void GpuRendererVulkan::destroy_targets()
{
	clear_descriptor_caches();
	if (offscreen_color_.handle != VK_NULL_HANDLE) {
		memory_.destroy_image(offscreen_color_);
		offscreen_color_ = vk::Image{};
	}
	offscreen_color_state_ = vk::ImageState{};
	offscreen_width_       = 0;
	offscreen_height_      = 0;
	if (depth_.handle != VK_NULL_HANDLE) {
		memory_.destroy_image(depth_);
		depth_ = vk::Image{};
	}
	depth_state_  = vk::ImageState{};
	depth_width_  = 0;
	depth_height_ = 0;
	destroy_pyramid();
}

bool GpuRendererVulkan::resize_offscreen(uint32_t width, uint32_t height)
{
		if (!offscreen_ || offscreen_color_.handle == VK_NULL_HANDLE ||
		    width == 0 || height == 0) {
			return false;
		}
		if (width == offscreen_width_ && height == offscreen_height_) {
			return true;
		}
		// Created before the old one is retired, so a failed resize leaves the
		// renderer on its old target rather than on none.
		vk::Image replacement;
		try {
			replacement = create_target_image(
			        "offscreen", kColorFormat, width, height, 1,
			        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			                VK_IMAGE_USAGE_SAMPLED_BIT |
			                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		} catch (const std::exception&) {
			return false;
		}
		// The old targets stay alive until a frame-ring slot is reused (a full
		// turn later), so frames still in flight keep reading valid images.
		retire_image(offscreen_color_);
		offscreen_color_       = replacement;
		offscreen_color_state_ = vk::ImageState{};
		offscreen_width_       = width;
		offscreen_height_      = height;
		retire_image(depth_);
		depth_          = vk::Image{};
		depth_state_    = vk::ImageState{};
		depth_width_    = 0;
		depth_height_   = 0;
		// Both sized from the target: the depth buffer because it is now the
		// wrong size, the pyramid because its extent is. Retiring the pyramid
		// clears the CPU-side chain and occlusion_ready_, so the frame after
		// a resize treats every entity as visible rather than querying a
		// stale chain against the new camera.
		retire_pyramid_gpu();
		ensure_depth_target(width, height);
		ensure_pyramid(width, height);
		// FXAA/SSAO intermediates are sized from the target too, but lazily:
		// the next ensure_* recreates (and retires) them on mismatch.
		return true;
	}


bool GpuRendererVulkan::write_ppm(const std::string& path, std::string* error)
{
	uint32_t width  = 0;
	uint32_t height = 0;
	const std::vector<uint8_t> pixels = capture_rgba(width, height);
	if (pixels.empty()) {
		if (error) {
			*error = "no offscreen target to capture";
		}
		return false;
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot write " + path;
		}
		return false;
	}
	out << "P6\n" << width << ' ' << height << "\n255\n";
	for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
		out.put(static_cast<char>(pixels[i]));
		out.put(static_cast<char>(pixels[i + 1]));
		out.put(static_cast<char>(pixels[i + 2]));
	}
	return static_cast<bool>(out);
}

void GpuRendererVulkan::set_viewport(VkCommandBuffer cmd, uint32_t width,
                                     uint32_t height, float max_depth)
{
	// Y is flipped: a negative height over the target, so NDC +Y is
	// framebuffer row 0 (the shaders and the CPU depth pyramid assume it).
	VkViewport viewport{};
	viewport.x         = 0.0f;
	viewport.y         = static_cast<float>(height);
	viewport.width     = static_cast<float>(width);
	viewport.height    = -static_cast<float>(height);
	viewport.minDepth  = 0.0f;
	viewport.maxDepth  = max_depth;
	context_.api.CmdSetViewport(cmd, 0, 1, &viewport);
}

void GpuRendererVulkan::set_scissor(VkCommandBuffer cmd, uint32_t width,
                                    uint32_t height)
{
	VkRect2D scissor{};
	scissor.offset = VkOffset2D{0, 0};
	scissor.extent = VkExtent2D{width, height};
	context_.api.CmdSetScissor(cmd, 0, 1, &scissor);
}

void GpuRendererVulkan::set_blob_casters(const glm::vec4* spheres, uint32_t count)
{
	blob_count_ = 0;
	if (spheres == nullptr || count == 0) {
		return;
	}
	blob_count_ = std::min(count, pbr::kMaxBlobCasters);
	for (uint32_t i = 0; i < blob_count_; ++i) {
		blob_spheres_[i] = spheres[i];
	}
}

void GpuRendererVulkan::set_point_lights(const glm::vec4* positions,
                                         const glm::vec4* colors,
                                         uint32_t count)
{
	point_light_count_ = 0;
	if (positions == nullptr || colors == nullptr || count == 0) {
		return;
	}
	point_light_count_ = std::min(count, pbr::kMaxPointLights);
	for (uint32_t i = 0; i < point_light_count_; ++i) {
		point_light_positions_[i] = positions[i];
		point_light_colors_[i]    = colors[i];
	}
}

void GpuRendererVulkan::destroy_pipelines()
{
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	for (VkPipeline* pipeline :
	     {&lit_pipeline_, &shadow_pipeline_, &sky_pipeline_,
	      &fxaa_pipeline_, &ssao_pipeline_}) {
		if (*pipeline != VK_NULL_HANDLE) {
			api.DestroyPipeline(device, *pipeline, nullptr);
			*pipeline = VK_NULL_HANDLE;
		}
	}
	for (VkSampler* sampler :
	     {&fxaa_sampler_, &ssao_sampler_, &ssao_depth_sampler_}) {
		if (*sampler != VK_NULL_HANDLE) {
			api.DestroySampler(device, *sampler, nullptr);
			*sampler = VK_NULL_HANDLE;
		}
	}
	for (VkPipelineLayout* layout : {&lit_layout_, &shadow_layout_, &sky_layout_}) {
		if (*layout != VK_NULL_HANDLE) {
			api.DestroyPipelineLayout(device, *layout, nullptr);
			*layout = VK_NULL_HANDLE;
		}
	}
	sky_unavailable_ = false;
	sky_format_      = VK_FORMAT_UNDEFINED;
}

} // namespace lotus

namespace lotus {

/// Test hook, declared by the tests that use it: the validation-layer
/// messages the renderer's context has counted so far.
uint32_t test_vulkan_validation_messages(const GpuRenderer& gpu)
{
	const auto* vulkan = dynamic_cast<const GpuRendererVulkan*>(&gpu);
	return vulkan != nullptr ? vulkan->validation_messages() : 0u;
}

/// Test hook, declared by the tests that use it: content-keyed descriptor
/// sets (sampler, storage) allocated from the cache pool and not yet
/// returned to it.
uint32_t test_vulkan_keyed_descriptor_sets(const GpuRenderer& gpu)
{
	const auto* vulkan = dynamic_cast<const GpuRendererVulkan*>(&gpu);
	return vulkan != nullptr ? vulkan->keyed_descriptor_sets() : 0u;
}

} // namespace lotus
