// Hi-Z occlusion: the compute reduction of the linear-depth target, the mip K
// readback and its adoption as the CPU depth pyramid the next frame culls
// against.
//
// Behavioural reference: src/gpu/sdlgpu/renderer_sdlgpu.cpp (record_pyramid,
// poll_pyramid_fence, ensure_pyramid_texture, create_occlusion_pipeline).
// Per-frame identity with the reference requires identical culling, so this
// runs every frame the scene pass ran: the one-frame lag matches the
// reference exactly (frame N culls against frame N-1's pyramid).

#include "gpu/vulkan/renderer_vulkan.hpp"

#include "lotus/gpu/depth.comp.spv.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace lotus {
namespace {

constexpr VkFormat kPyramidFormat = VK_FORMAT_R32_SFLOAT;

/// Row pitch of the mip K download, matching the reference: D3D12 asks 256,
/// so every backend pads to it.
constexpr uint32_t kTransferRowAlignment = 256;

uint32_t pyramid_levels()
{
	return kDepthPyramidMinLevel + 1;
}

} // namespace

bool GpuRendererVulkan::create_occlusion_pipeline()
{
	if (hiz_pipeline_ != VK_NULL_HANDLE) {
		return true;
	}
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	try {
		VkShaderModule module = vk::create_shader_module(
		        api, device, depth_comp_spv, sizeof(depth_comp_spv));
		const VkDescriptorSetLayout sets[2] = {layouts_.empty,
		                                       layouts_.storage};
		hiz_layout_ = vk::create_pipeline_layout(api, device, sets, 2);
		hiz_pipeline_ =
		        vk::create_compute_pipeline(api, device, hiz_layout_,
		                                    module, "hiz");
		api.DestroyShaderModule(device, module, nullptr);
	} catch (const std::exception&) {
		return false;
	}
	return true;
}

void GpuRendererVulkan::destroy_occlusion()
{
	destroy_pyramid();
	const vk::Api& api    = context_.api;
	const VkDevice device = context_.device;
	if (hiz_pipeline_ != VK_NULL_HANDLE) {
		api.DestroyPipeline(device, hiz_pipeline_, nullptr);
		hiz_pipeline_ = VK_NULL_HANDLE;
	}
	if (hiz_layout_ != VK_NULL_HANDLE) {
		api.DestroyPipelineLayout(device, hiz_layout_, nullptr);
		hiz_layout_ = VK_NULL_HANDLE;
	}
	occlusion_enabled_ = false;
}

void GpuRendererVulkan::destroy_pyramid()
{
	// A pyramid download still in flight is simply forgotten: its commands
	// belong to a submitted frame whose slot fence still guards the readback
	// buffer, which dies with the slot teardown or the next ensure.
	if (pyramid_readback_.handle != VK_NULL_HANDLE) {
		memory_.destroy_buffer(pyramid_readback_);
		pyramid_readback_ = vk::Buffer{};
	}
	if (pyramid_.handle != VK_NULL_HANDLE) {
		for (VkImageView& view : pyramid_views_) {
			if (view != VK_NULL_HANDLE) {
				context_.api.DestroyImageView(context_.device, view,
				                              nullptr);
				view = VK_NULL_HANDLE;
			}
		}
		memory_.destroy_image(pyramid_);
		pyramid_ = vk::Image{};
	}
	for (vk::ImageState& state : pyramid_states_) {
		state = vk::ImageState{};
	}
	pyramid_levels_      = 0;
	pyramid_width_       = 0;
	pyramid_height_      = 0;
	pyramid_pitch_       = 0;
	pyramid_in_flight_   = false;
	pyramid_recorded_    = false;
	occlusion_pyramid_.clear();
	occlusion_ready_ = false;
}

void GpuRendererVulkan::ensure_pyramid(uint32_t width, uint32_t height)
{
	if (pyramid_reduction_unavailable_) {
		// The reduction is permanently off. A resize must still be able to
		// refresh the stand-in texture the scene pass writes as colour
		// target 1, but the mip chain is never attempted again.
		if (pyramid_.handle != VK_NULL_HANDLE &&
		    pyramid_width_ == width && pyramid_height_ == height) {
			return;
		}
		destroy_pyramid();
		if (width != 0 && height != 0) {
			ensure_pyramid_stand_in(width, height);
		}
		return;
	}
	if (pyramid_.handle != VK_NULL_HANDLE && pyramid_width_ == width &&
	    pyramid_height_ == height && pyramid_levels_ == pyramid_levels()) {
		return;
	}

	// Anything referencing the old texture is dropped rather than waited on.
	destroy_pyramid();

	// The texture is created whether or not the compute pipeline exists: mip
	// 0 is written by the scene pass as colour target 1, which the pipeline
	// declares unconditionally. Occlusion-disabled therefore means "no
	// reduction, no culling", not "a different pipeline".
	if (width == 0 || height == 0) {
		return;
	}
	try {
		pyramid_ = create_target_image(
		        "pyramid", kPyramidFormat, width, height,
		        pyramid_levels(),
		        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                VK_IMAGE_USAGE_SAMPLED_BIT |
		                VK_IMAGE_USAGE_STORAGE_BIT |
		                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	} catch (const std::exception& e) {
		// Degrade rather than leave pyramid_ null: the scene pass binds mip
		// 0 as colour target 1 unconditionally. Permanent: the cause is a
		// missing usage flag or an unsupported format, not the size.
		pyramid_reduction_unavailable_ = true;
		occlusion_enabled_             = false;
		std::cerr << "occlusion: depth pyramid unavailable, continuing "
		             "without occlusion culling: "
		          << e.what() << '\n';
		ensure_pyramid_stand_in(width, height);
		return;
	}
	for (uint32_t level = 0; level < pyramid_levels(); ++level) {
		VkImageViewCreateInfo info{};
		info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.image    = pyramid_.handle;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format   = kPyramidFormat;
		info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		info.subresourceRange.baseMipLevel   = level;
		info.subresourceRange.levelCount     = 1;
		info.subresourceRange.baseArrayLayer = 0;
		info.subresourceRange.layerCount     = 1;
		vk::check(context_.api.CreateImageView(context_.device, &info,
		                                       nullptr,
		                                       &pyramid_views_[level]),
		          "vkCreateImageView");
	}
	const uint32_t w = std::max(1u, width >> kDepthPyramidMinLevel);
	const uint32_t h = std::max(1u, height >> kDepthPyramidMinLevel);
	const uint32_t pitch =
	        ((w * sizeof(float) + kTransferRowAlignment - 1) /
	         kTransferRowAlignment) *
	        kTransferRowAlignment;
	pyramid_readback_ = memory_.create_buffer(vk::MemoryClass::Readback,
	                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                          static_cast<uint64_t>(pitch) *
	                                                  h,
	                                          "pyramid.readback");
	pyramid_width_  = width;
	pyramid_height_ = height;
	pyramid_levels_ = pyramid_levels();
	pyramid_pitch_  = pitch / sizeof(float);
}

VkDescriptorSet GpuRendererVulkan::alloc_storage_set(VkImageView src,
                                                     VkImageView dst)
{
	VkDescriptorSetAllocateInfo alloc{};
	alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool     = slot_->descriptors;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts        = &layouts_.storage;
	VkDescriptorSet set{VK_NULL_HANDLE};
	vk::check(context_.api.AllocateDescriptorSets(context_.device, &alloc,
	                                              &set),
	          "vkAllocateDescriptorSets");
	const VkDescriptorImageInfo images[2] = {
	        {VK_NULL_HANDLE, src, VK_IMAGE_LAYOUT_GENERAL},
	        {VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL},
	};
	VkWriteDescriptorSet writes[2]{};
	for (uint32_t i = 0; i < 2; ++i) {
		writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet          = set;
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].pImageInfo      = &images[i];
	}
	context_.api.UpdateDescriptorSets(context_.device, 2, writes, 0, nullptr);
	return set;
}

void GpuRendererVulkan::order_storage_write(VkCommandBuffer cmd, uint32_t mip)
{
	// Execution and memory order between the dispatch that wrote \p mip and
	// the one that reads it. Both use GENERAL with read-write access, so a
	// single same-state transition would be elided and the hazard would stay;
	// splitting write from read-write forces the two barriers out.
	barriers_->image(cmd, pyramid_, mip, 1, pyramid_states_[mip],
	                 VK_IMAGE_LAYOUT_GENERAL,
	                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	image_storage_read_write(*barriers_, cmd, pyramid_, mip, 1,
	                         pyramid_states_[mip]);
}

void GpuRendererVulkan::record_pyramid()
{
	// Gated on occlusion_enabled_, not merely on hiz_pipeline_: the pyramid
	// texture always exists because the scene pass writes it as color target
	// 1, so without this the reduction would still run pointlessly whenever
	// occlusion was disabled.
	//
	// pyramid_in_flight_ keeps this to at most one outstanding download.
	// Recording a second one into the same readback buffer while the first
	// submission is still pending would overwrite the bytes the poll is
	// going to map. Skipping the download costs one frame of occlusion,
	// which is the safe direction: the frame still renders, it just culls
	// nothing.
	if (!occlusion_enabled_ || pyramid_.handle == VK_NULL_HANDLE ||
	    hiz_pipeline_ == VK_NULL_HANDLE ||
	    pyramid_readback_.handle == VK_NULL_HANDLE || cmd_ == VK_NULL_HANDLE ||
	    slot_ == nullptr || pyramid_in_flight_ ||
	    pyramid_levels_ <= kDepthPyramidMinLevel) {
		return;
	}

	// One dispatch per level: the (src, dst) mip pair is fixed per set, so
	// the pair cannot change within one bind.
	for (uint32_t level = 1; level <= kDepthPyramidMinLevel; ++level) {
		image_storage_read_write(*barriers_, cmd_, pyramid_, level - 1, 1,
		                         pyramid_states_[level - 1]);
		image_storage_read_write(*barriers_, cmd_, pyramid_, level, 1,
		                         pyramid_states_[level]);
		barriers_->flush(cmd_);
		if (level > 1) {
			order_storage_write(cmd_, level - 1);
			barriers_->flush(cmd_);
		}
		VkDescriptorSet set =
		        alloc_storage_set(pyramid_views_[level - 1],
		                          pyramid_views_[level]);
		context_.api.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE,
		                             hiz_pipeline_);
		context_.api.CmdBindDescriptorSets(cmd_,
		                                   VK_PIPELINE_BIND_POINT_COMPUTE,
		                                   hiz_layout_, 1, 1, &set, 0,
		                                   nullptr);
		const uint32_t groups_x =
		        (std::max(1u, pyramid_width_ >> level) + 7) / 8;
		const uint32_t groups_y =
		        (std::max(1u, pyramid_height_ >> level) + 7) / 8;
		context_.api.CmdDispatch(cmd_, groups_x, groups_y, 1);
	}

	// Mip K only: coarser levels are never built or read.
	const uint32_t w = std::max(1u, pyramid_width_ >> kDepthPyramidMinLevel);
	const uint32_t h = std::max(1u, pyramid_height_ >> kDepthPyramidMinLevel);
	image_transfer_src(*barriers_, cmd_, pyramid_, kDepthPyramidMinLevel, 1,
	                   pyramid_states_[kDepthPyramidMinLevel]);
	barriers_->flush(cmd_);
	VkBufferImageCopy region{};
	region.bufferOffset      = 0;
	region.bufferRowLength   = pyramid_pitch_;
	region.bufferImageHeight = h;
	region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel       = kDepthPyramidMinLevel;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount     = 1;
	region.imageOffset = VkOffset3D{0, 0, 0};
	region.imageExtent = VkExtent3D{w, h, 1};
	context_.api.CmdCopyImageToBuffer(cmd_, pyramid_.handle,
	                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                                  pyramid_readback_.handle, 1, &region);
	image_storage_read_write(*barriers_, cmd_, pyramid_,
	                         kDepthPyramidMinLevel, 1,
	                         pyramid_states_[kDepthPyramidMinLevel]);
	barriers_->flush(cmd_);
	pyramid_recorded_ = true;
}

void GpuRendererVulkan::poll_pyramid()
{
	// Collects the frames that have finished. Polls, never waits: a frame
	// that has not finished keeps its serial, and occlusion is skipped.
	for (FrameSlot& slot : slots_) {
		if (slot.serial > completed_serial_ &&
		    context_.api.GetFenceStatus(context_.device, slot.fence) ==
		            VK_SUCCESS) {
			completed_serial_ = slot.serial;
		}
	}
	occlusion_ready_ = false;
	if (!pyramid_in_flight_) {
		return;
	}
	if (completed_serial_ < pyramid_serial_) {
		return; // still in flight: skip occlusion this frame, never block
	}
	pyramid_in_flight_ = false;

	if (pyramid_readback_.handle == VK_NULL_HANDLE ||
	    pyramid_levels_ <= kDepthPyramidMinLevel) {
		return;
	}
	if (!pyramid_readback_.allocation.coherent) {
		const uint32_t h =
		        std::max(1u, pyramid_height_ >> kDepthPyramidMinLevel);
		memory_.invalidate(pyramid_readback_.allocation, 0,
		                   static_cast<uint64_t>(pyramid_pitch_) *
		                           sizeof(float) * h);
	}

	// The GPU already reduced these levels, so install them as-is.
	std::vector<std::vector<float>> mips(pyramid_levels_);
	{ // exactly one iteration: only mip K is read
		const uint32_t w =
		        std::max(1u, pyramid_width_ >> kDepthPyramidMinLevel);
		const uint32_t h =
		        std::max(1u, pyramid_height_ >> kDepthPyramidMinLevel);
		std::vector<float>& texels = mips[kDepthPyramidMinLevel];
		texels.resize(static_cast<std::size_t>(w) * h);
		const auto* rows = static_cast<const float*>(
		        pyramid_readback_.mapped);
		for (uint32_t y = 0; y < h; ++y) {
			const float* row = rows + static_cast<std::size_t>(y) *
			                                  pyramid_pitch_;
			std::memcpy(texels.data() + static_cast<std::size_t>(y) * w,
			            row, static_cast<std::size_t>(w) * sizeof(float));
		}
	}

	// adopt() rejects a chain whose transferred mips disagree with the
	// accessors and leaves the pyramid empty, so readiness is read back off
	// the object rather than assumed.
	occlusion_pyramid_.adopt(pyramid_width_, pyramid_height_,
	                         pyramid_levels_, std::move(mips));
	occlusion_ready_ = !occlusion_pyramid_.empty();
	occlusion_aspect_ =
	        occlusion_ready_ && occlusion_pyramid_.height() > 0
	                ? static_cast<float>(occlusion_pyramid_.width()) /
	                          static_cast<float>(occlusion_pyramid_.height())
	                : 0.0f;
}

} // namespace lotus
