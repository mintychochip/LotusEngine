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

void GpuRendererVulkan::init(SDL_Window* window, bool debug_device)
{
	(void)debug_device;
	if (device_valid_) {
		return;
	}
	window_ = window;
	vk::ContextOptions options;
	options.window     = nullptr; // offscreen for now; the swapchain is stage 2
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
	layouts_.create(context_);
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
	// A frame may be open; its command buffer dies with the pool.
	cmd_          = nullptr;
	slot_         = nullptr;
	scene_pass_open_ = false;
	abandon_geometry_staging();
	geometry_draws_.clear();
	pending_meshes_.clear();
	destroy_meshes();
	destroy_textures();
	destroy_shadows();
	destroy_occlusion();
	destroy_pipelines();
	layouts_.destroy(context_);
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
	window_      = nullptr;
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

FrameSlot& GpuRendererVulkan::acquire_slot()
{
	FrameSlot& slot = slots_[slot_index_];
	// The slot's fence was signalled before its first use (created signalled)
	// and after every submit, so waiting here throttles to kFramesInFlight.
	vk::check(context_.api.WaitForFences(context_.device, 1, &slot.fence,
	                                     VK_TRUE, UINT64_MAX),
	          "vkWaitForFences");
	vk::check(context_.api.ResetFences(context_.device, 1, &slot.fence),
	          "vkResetFences");
	destroy_garbage(slot);
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

void GpuRendererVulkan::begin_frame(SDL_Window* window, float clear_r, float clear_g,
                                    float clear_b)
{
	(void)window;
	clear_r_ = clear_r;
	clear_g_ = clear_g;
	clear_b_ = clear_b;
	// Windowed rendering (swapchain, present) belongs to the second half:
	// record the clear colour and report no frame.
}
std::vector<uint8_t> GpuRendererVulkan::capture_viewport_rgba(uint32_t& width,
                                                              uint32_t& height)
{
	// The editor viewport texture belongs to the second half.
	width  = 0;
	height = 0;
	return {};
}


void GpuRendererVulkan::end_frame_submit()
{
	vk::check(context_.api.EndCommandBuffer(cmd_), "vkEndCommandBuffer");
	VkCommandBufferSubmitInfo cmd_info{};
	cmd_info.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmd_info.commandBuffer = cmd_;
	VkSubmitInfo2 submit{};
	submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submit.commandBufferInfoCount = 1;
	submit.pCommandBufferInfos  = &cmd_info;
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
		return;
	}
	end_frame_submit();
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
	if (!offscreen_ || offscreen_color_.handle == VK_NULL_HANDLE || width == 0 ||
	    height == 0) {
		return false;
	}
	if (width == offscreen_width_ && height == offscreen_height_) {
		return true;
	}
	wait_idle();
	// Created before the old one is released, so a failed resize leaves the
	// renderer on its old target rather than on none.
	vk::Image replacement = create_target_image(
	        "offscreen", kColorFormat, width, height, 1,
	        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	memory_.destroy_image(offscreen_color_);
	offscreen_color_       = replacement;
	offscreen_color_state_ = vk::ImageState{};
	offscreen_width_       = width;
	offscreen_height_      = height;
	// Both sized from the target: the depth buffer because it is now the
	// wrong size, the pyramid because its extent is.
	ensure_depth_target(width, height);
	ensure_pyramid(width, height);
	return true;
}

std::vector<uint8_t> GpuRendererVulkan::capture_rgba(uint32_t& width, uint32_t& height)
{
	width  = 0;
	height = 0;
	if (!offscreen_ || offscreen_color_.handle == VK_NULL_HANDLE) {
		return {};
	}
	width  = offscreen_width_;
	height = offscreen_height_;
	// Like the reference: the GPU goes idle first, so no frame is still
	// reading the target while it is copied out.
	wait_idle();
	const uint64_t pitch_alignment =
	        context_.properties.limits.optimalBufferCopyRowPitchAlignment;
	const uint32_t row_texels =
	        static_cast<uint32_t>(align_up(static_cast<uint64_t>(width) * 4,
	                                       std::max<uint64_t>(pitch_alignment, 1)) /
	                              4);
	const uint64_t buffer_size =
	        static_cast<uint64_t>(row_texels) * height * 4;
	vk::Buffer readback = memory_.create_buffer(vk::MemoryClass::Readback,
	                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                            buffer_size, "capture");
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		image_transfer_src(*barriers_, cmd, offscreen_color_, 0, 1,
		                   offscreen_color_state_);
		barriers_->flush(cmd);
		VkBufferImageCopy region{};
		region.bufferOffset      = 0;
		region.bufferRowLength   = row_texels;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel       = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount     = 1;
		region.imageOffset = VkOffset3D{0, 0, 0};
		region.imageExtent = VkExtent3D{width, height, 1};
		context_.api.CmdCopyImageToBuffer(cmd, offscreen_color_.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                                  readback.handle, 1, &region);
		image_as_color_attachment(*barriers_, cmd, offscreen_color_, 0, 1,
		                          offscreen_color_state_);
		barriers_->flush(cmd);
	});
	if (!readback.allocation.coherent) {
		memory_.invalidate(readback.allocation, 0, buffer_size);
	}
	std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	const auto* src = static_cast<const uint8_t*>(readback.mapped);
	const uint64_t row_bytes = static_cast<uint64_t>(width) * 4;
	const uint64_t pitch     = static_cast<uint64_t>(row_texels) * 4;
	for (uint32_t y = 0; y < height; ++y) {
		std::memcpy(pixels.data() + static_cast<std::size_t>(y) * row_bytes,
		            src + y * pitch, row_bytes);
	}
	memory_.destroy_buffer(readback);
	return pixels;
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
	// The flip SDL applied: a negative height over the target.
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

void GpuRendererVulkan::destroy_pipelines()
{
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	for (VkPipeline* pipeline : {&lit_pipeline_, &shadow_pipeline_, &sky_pipeline_}) {
		if (*pipeline != VK_NULL_HANDLE) {
			api.DestroyPipeline(device, *pipeline, nullptr);
			*pipeline = VK_NULL_HANDLE;
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

bool test_is_vulkan_backend(const GpuRenderer& gpu)
{
	return dynamic_cast<const GpuRendererVulkan*>(&gpu) != nullptr;
}

uint32_t test_vulkan_validation_messages(const GpuRenderer& gpu)
{
	const auto* vulkan = dynamic_cast<const GpuRendererVulkan*>(&gpu);
	return vulkan != nullptr ? vulkan->validation_messages() : 0u;
}

} // namespace lotus
