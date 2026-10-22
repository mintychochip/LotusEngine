// FXAA, SSAO and the editor viewport pass.
//
// Behavioural reference: src/gpu/sdlgpu/renderer_sdlgpu.cpp (ensure_fxaa_*,
// fxaa_resolve, ensure_ssao_*, run_ssao_pass, resize_viewport,
// begin/end_viewport_pass, capture_viewport_rgba, download_rgba,
// resize_offscreen). Pass order, uniform contents, clear values and every
// degradation message match it; the comments there explain why.

#include "gpu/vulkan/renderer_vulkan.hpp"

#include "lotus/gpu/fxaa.frag.spv.h"
#include "lotus/gpu/fxaa.vert.spv.h"
#include "lotus/gpu/ssao.frag.spv.h"
#include "lotus/gpu/ssao.vert.spv.h"

	#include "lotus/scene/depth_pyramid.hpp"

	#include <algorithm>
	#include <cstring>
	#include <iostream>

	namespace lotus {
	namespace {

	constexpr VkFormat kColorFormat   = VK_FORMAT_R8G8B8A8_UNORM;
	constexpr VkFormat kDepthFormat   = VK_FORMAT_D32_SFLOAT;
	constexpr VkFormat kPyramidFormat = VK_FORMAT_R32_SFLOAT;

	uint64_t align_up(uint64_t value, uint64_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

VkSampler make_sampler(const vk::Api& api, VkDevice device, VkFilter min,
                       VkFilter mag, VkSamplerMipmapMode mips,
                       VkSamplerAddressMode wrap, float max_lod)
{
	VkSamplerCreateInfo info{};
	info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.magFilter               = mag;
	info.minFilter               = min;
	info.mipmapMode              = mips;
	info.addressModeU            = wrap;
	info.addressModeV            = wrap;
	info.addressModeW            = wrap;
	info.mipLodBias              = 0.0f;
	info.anisotropyEnable        = VK_FALSE;
	info.compareEnable           = VK_FALSE;
	info.minLod                  = 0.0f;
	info.maxLod                  = max_lod;
	info.borderColor             = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	info.unnormalizedCoordinates = VK_FALSE;
	VkSampler sampler{VK_NULL_HANDLE};
	vk::check(api.CreateSampler(device, &info, nullptr, &sampler),
	          "vkCreateSampler");
	return sampler;
}

} // namespace

void GpuRendererVulkan::retire_image(vk::Image image)
{
	// The retired view may be recreated under a recycled handle; drop the
	// content-keyed entries so a later frame cannot hit on a dead image.
	// Unconditional (even when null): recreates that keep the old image
	// still replace samplers elsewhere, and clearing is nearly free.
	clear_descriptor_caches();
	if (image.handle == VK_NULL_HANDLE) {
		return;
	}
	retired_targets_.push_back([this, image]() mutable {
		memory_.destroy_image(image);
	});
}

void GpuRendererVulkan::retire_pyramid_gpu()
{
	clear_descriptor_caches();
	if (pyramid_.handle != VK_NULL_HANDLE) {
		vk::Image image = pyramid_;
		pyramid_        = vk::Image{};
		VkImageView views[3]{pyramid_views_[0], pyramid_views_[1],
		                     pyramid_views_[2]};
		pyramid_views_[0] = pyramid_views_[1] = pyramid_views_[2] =
		        VK_NULL_HANDLE;
		retired_targets_.push_back(
		        [this, image, views]() mutable {
			        const vk::Api& api    = context_.api;
			        const VkDevice device = context_.device;
			        for (VkImageView view : views) {
				        if (view != VK_NULL_HANDLE) {
					        api.DestroyImageView(device, view,
					                             nullptr);
				        }
			        }
			        memory_.destroy_image(image);
		        });
	}
	if (pyramid_readback_.handle != VK_NULL_HANDLE) {
		vk::Buffer readback = pyramid_readback_;
		pyramid_readback_   = vk::Buffer{};
		retired_targets_.push_back([this, readback]() mutable {
			memory_.destroy_buffer(readback);
		});
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

std::vector<uint8_t> GpuRendererVulkan::download_image_rgba(
        vk::Image& image, vk::ImageState& state, uint32_t width,
        uint32_t height)
{
	// Like the reference: the GPU goes idle first, so no frame is still
	// reading the target while it is copied out.
	wait_idle();
	const uint64_t pitch_alignment =
	        context_.properties.limits.optimalBufferCopyRowPitchAlignment;
	const uint32_t row_texels =
	        static_cast<uint32_t>(align_up(static_cast<uint64_t>(width) * 4,
	                                       std::max<uint64_t>(pitch_alignment,
	                                                          1)) /
	                              4);
	const uint64_t buffer_size =
	        static_cast<uint64_t>(row_texels) * height * 4;
	vk::Buffer readback = memory_.create_buffer(vk::MemoryClass::Readback,
	                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                            buffer_size, "capture");
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		image_transfer_src(*barriers_, cmd, image, 0, 1, state);
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
		context_.api.CmdCopyImageToBuffer(cmd, image.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                                  readback.handle, 1, &region);
		image_as_color_attachment(*barriers_, cmd, image, 0, 1, state);
		barriers_->flush(cmd);
	});
	if (!readback.allocation.coherent) {
		memory_.invalidate(readback.allocation, 0, buffer_size);
	}
	std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
	const auto* src         = static_cast<const uint8_t*>(readback.mapped);
	const uint64_t row_bytes = static_cast<uint64_t>(width) * 4;
	const uint64_t pitch     = static_cast<uint64_t>(row_texels) * 4;
	for (uint32_t y = 0; y < height; ++y) {
		std::memcpy(pixels.data() + static_cast<std::size_t>(y) * row_bytes,
		            src + y * pitch, row_bytes);
	}
	// The API promises RGBA, but a windowed session renders in the swapchain's
	// format, which is BGRA on the drivers that can present here.
	if (image.format == VK_FORMAT_B8G8R8A8_UNORM) {
		for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
			const uint8_t blue = pixels[i];
			pixels[i]          = pixels[i + 2];
			pixels[i + 2]      = blue;
		}
	}
	memory_.destroy_buffer(readback);
	return pixels;
}

bool GpuRendererVulkan::ensure_fxaa_source(uint32_t width, uint32_t height)
{
	if (!device_valid_ || width == 0 || height == 0) {
		return false;
	}
	// The intermediate shares the session's colour format (swapchain in
	// windowed sessions), so every pass that redirects into it agrees with
	// the lit pipeline and the resolve below.
	if (fxaa_source_.handle != VK_NULL_HANDLE && fxaa_width_ == width &&
	    fxaa_height_ == height && fxaa_format_ == pipeline_color_format_) {
		return true;
	}
	vk::Image replacement;
	try {
		replacement = create_target_image(
		        "fxaa", pipeline_color_format_, width, height, 1,
		        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                VK_IMAGE_USAGE_SAMPLED_BIT);
	} catch (const std::exception& e) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			std::cerr << "fxaa: intermediate target unavailable, "
			             "rendering directly: "
			          << e.what() << '\n';
		}
		return false;
	}
	// Created before the old one is retired, so a failed recreate leaves the
	// renderer on its old source rather than on none.
	retire_image(fxaa_source_);
	fxaa_source_       = replacement;
	fxaa_source_state_ = vk::ImageState{};
	fxaa_width_        = width;
	fxaa_height_       = height;
	fxaa_format_       = pipeline_color_format_;
	return true;
}

bool GpuRendererVulkan::ensure_fxaa_pipeline()
{
	if (!device_valid_) {
		return false;
	}
	if (fxaa_unavailable_) {
		return false;
	}
	if (fxaa_pipeline_ != VK_NULL_HANDLE &&
	    fxaa_sampler_ != VK_NULL_HANDLE &&
	    fxaa_format_ == pipeline_color_format_) {
		return true;
	}
	destroy_fxaa();
	// The resolve reads the same sets as the lit pass (samplers at set 2,
	// fragment UBO at set 3); sharing the layout keeps the indices lined up
	// with the committed SPIR-V.
	try {
		VkShaderModule vert = vk::create_shader_module(
		        context_.api, context_.device, fxaa_vert_spv,
		        sizeof(fxaa_vert_spv));
		VkShaderModule frag = vk::create_shader_module(
		        context_.api, context_.device, fxaa_frag_spv,
		        sizeof(fxaa_frag_spv));
		vk::GraphicsPipelineInfo info{};
		info.layout             = lit_layout_;
		info.vertex             = vert;
		info.fragment           = frag;
		info.color_target_count = 1;
		info.color_formats[0]   = pipeline_color_format_;
		info.depth_format       = VK_FORMAT_UNDEFINED;
		info.depth_test         = false;
		info.depth_write        = false;
		info.vertex_input       = false;
		info.depth_clamp        = false;
		fxaa_pipeline_ = vk::create_graphics_pipeline(
		        context_.api, context_.device, info, "fxaa");
		context_.api.DestroyShaderModule(context_.device, vert, nullptr);
		context_.api.DestroyShaderModule(context_.device, frag, nullptr);
		fxaa_sampler_ = make_sampler(context_.api, context_.device,
		                             VK_FILTER_LINEAR, VK_FILTER_LINEAR,
		                             VK_SAMPLER_MIPMAP_MODE_LINEAR,
		                             VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                             0.0f);
	} catch (const std::exception& e) {
		std::cerr << "fxaa: post pipeline unavailable, "
		             "rendering directly: "
		          << e.what() << '\n';
		destroy_fxaa();
		fxaa_unavailable_ = true;
		return false;
	}
	fxaa_format_ = pipeline_color_format_;
	return true;
}

void GpuRendererVulkan::fxaa_resolve(vk::Image& destination,
                                     vk::ImageState& destination_state,
                                     uint32_t width, uint32_t height)
{
	if (cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
	    fxaa_source_.handle == VK_NULL_HANDLE || width == 0 ||
	    height == 0 || !ensure_fxaa_pipeline() ||
	    fxaa_sampler_ == VK_NULL_HANDLE) {
		return;
	}
	image_shader_read(*barriers_, cmd_, fxaa_source_, 0, 1,
	                  fxaa_source_state_);
	image_as_color_attachment(*barriers_, cmd_, destination, 0, 1,
	                          destination_state);
	barriers_->flush(cmd_);

	// Fullscreen triangle over a DONT_CARE target: every pixel is covered,
	// so there is nothing to preserve.
	VkRenderingAttachmentInfo target{};
	target.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	target.imageView   = destination.view;
	target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	target.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	target.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rendering{};
	rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea            = VkRect2D{{0, 0}, {width, height}};
	rendering.layerCount            = 1;
	rendering.colorAttachmentCount  = 1;
	rendering.pColorAttachments     = &target;
	rendering.pDepthAttachment      = nullptr;
	context_.api.CmdBeginRendering(cmd_, &rendering);

	set_viewport(cmd_, width, height, 1.0f);
	set_scissor(cmd_, width, height);
	FxaaUniforms uniforms{};
	uniforms.texel = glm::vec4{1.0f / static_cast<float>(width),
	                           1.0f / static_cast<float>(height), 0.0f,
	                           0.0f};
	const uint64_t offset    = push_uniforms(&uniforms, sizeof(uniforms));
	VkDescriptorSet frag_set = alloc_fragment_set(offset);
	VkDescriptorSet sampler_set =
	        alloc_sampler_set(fxaa_source_.view, fxaa_sampler_,
	                          white_texture_.view, default_sampler_,
	                          white_texture_.view, default_sampler_);
	context_.api.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             fxaa_pipeline_);
	const uint32_t dynamic = static_cast<uint32_t>(offset);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 2, 1, &sampler_set, 0,
	                                   nullptr);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 3, 1, &frag_set, 1,
	                                   &dynamic);
	// Fullscreen triangle from fxaa.vert; covers the DONT_CARE target fully.
	context_.api.CmdDraw(cmd_, 3, 1, 0, 0);
	context_.api.CmdEndRendering(cmd_);
}

void GpuRendererVulkan::destroy_fxaa()
{
	if (fxaa_pipeline_ != VK_NULL_HANDLE) {
		context_.api.DestroyPipeline(context_.device, fxaa_pipeline_,
		                             nullptr);
		fxaa_pipeline_ = VK_NULL_HANDLE;
	}
	if (fxaa_sampler_ != VK_NULL_HANDLE) {
		context_.api.DestroySampler(context_.device, fxaa_sampler_,
		                            nullptr);
		fxaa_sampler_ = VK_NULL_HANDLE;
	}
	retire_image(fxaa_source_);
	fxaa_source_       = vk::Image{};
	fxaa_source_state_ = vk::ImageState{};
	fxaa_width_        = 0;
	fxaa_height_       = 0;
	fxaa_format_       = VK_FORMAT_UNDEFINED;
	pass_redirected_   = false;
}

bool GpuRendererVulkan::ensure_ssao_resources(uint32_t width, uint32_t height)
{
	if (!device_valid_ || width == 0 || height == 0) {
		return false;
	}
	// Quarter-res AO: resolution-independent UVs keep it valid at any size.
	const uint32_t w = std::max(1u, width / 4u);
	const uint32_t h = std::max(1u, height / 4u);
	if (ssao_texture_.handle != VK_NULL_HANDLE && ssao_width_ == w &&
	    ssao_height_ == h) {
		return true;
	}
	vk::Image replacement;
	try {
		replacement = create_target_image(
		        "ssao", kColorFormat, w, h, 1,
		        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                VK_IMAGE_USAGE_SAMPLED_BIT);
	} catch (const std::exception& e) {
		if (!ssao_unavailable_) {
			ssao_unavailable_ = true;
			std::cerr << "ssao: AO target unavailable, "
			             "continuing without AO: "
			          << e.what() << '\n';
		}
		return false;
	}
	retire_image(ssao_texture_);
	ssao_texture_ = replacement;
	ssao_state_   = vk::ImageState{};
	ssao_width_   = w;
	ssao_height_  = h;
	return true;
}

bool GpuRendererVulkan::ensure_ssao_pipeline()
{
	if (!device_valid_) {
		return false;
	}
	if (ssao_unavailable_) {
		return false;
	}
	if (ssao_pipeline_ != VK_NULL_HANDLE &&
	    ssao_sampler_ != VK_NULL_HANDLE &&
	    ssao_depth_sampler_ != VK_NULL_HANDLE) {
		return true;
	}
	destroy_ssao();
	ssao_unavailable_ = false;
	try {
		VkShaderModule vert = vk::create_shader_module(
		        context_.api, context_.device, ssao_vert_spv,
		        sizeof(ssao_vert_spv));
		VkShaderModule frag = vk::create_shader_module(
		        context_.api, context_.device, ssao_frag_spv,
		        sizeof(ssao_frag_spv));
		vk::GraphicsPipelineInfo info{};
		info.layout             = lit_layout_;
		info.vertex             = vert;
		info.fragment           = frag;
		info.color_target_count = 1;
		info.color_formats[0]   = kColorFormat;
		info.depth_format       = VK_FORMAT_UNDEFINED;
		info.depth_test         = false;
		info.depth_write        = false;
		info.vertex_input       = false;
		info.depth_clamp        = false;
		ssao_pipeline_ = vk::create_graphics_pipeline(
		        context_.api, context_.device, info, "ssao");
		context_.api.DestroyShaderModule(context_.device, vert, nullptr);
		context_.api.DestroyShaderModule(context_.device, frag, nullptr);
		ssao_sampler_ = make_sampler(
		        context_.api, context_.device, VK_FILTER_LINEAR,
		        VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR,
		        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f);
		// Nearest: the depth source is R32_FLOAT, and float-linear
		// filtering is not universally supported (lavapipe included).
		// Taps are exact texel reads; the 12-tap average smooths.
		ssao_depth_sampler_ = make_sampler(
		        context_.api, context_.device, VK_FILTER_NEAREST,
		        VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
		        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		        VK_LOD_CLAMP_NONE);
	} catch (const std::exception& e) {
		std::cerr << "ssao: post pipeline unavailable, "
		             "continuing without AO: "
		          << e.what() << '\n';
		destroy_ssao();
		ssao_unavailable_ = true;
		return false;
	}
	return true;
}

void GpuRendererVulkan::run_ssao_pass(uint32_t width, uint32_t height)
{
	if (cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
	    pyramid_.handle == VK_NULL_HANDLE || width == 0 || height == 0 ||
	    !ensure_ssao_pipeline() ||
	    !ensure_ssao_resources(width, height)) {
		return;
	}
	image_as_color_attachment(*barriers_, cmd_, ssao_texture_, 0, 1,
	                          ssao_state_);
	image_shader_read(*barriers_, cmd_, pyramid_, 0, 1,
	                  pyramid_states_[0]);
	barriers_->flush(cmd_);

	VkRenderingAttachmentInfo target{};
	target.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	target.imageView   = ssao_texture_.view;
	target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	target.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
	target.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	target.clearValue.color =
	        VkClearColorValue{{1.0f, 1.0f, 1.0f, 1.0f}};
	VkRenderingInfo rendering{};
	rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea            = VkRect2D{{0, 0},
                                           {ssao_width_, ssao_height_}};
	rendering.layerCount            = 1;
	rendering.colorAttachmentCount  = 1;
	rendering.pColorAttachments     = &target;
	rendering.pDepthAttachment      = nullptr;
	context_.api.CmdBeginRendering(cmd_, &rendering);

	set_viewport(cmd_, ssao_width_, ssao_height_, 1.0f);
	set_scissor(cmd_, ssao_width_, ssao_height_);
	SsaoUniforms uniforms{};
	uniforms.texel =
	        glm::vec4{1.0f / static_cast<float>(width),
	                  1.0f / static_cast<float>(height), 0.0f, 0.0f};
	uniforms.params = glm::vec4{2.5f, 0.02f, 1.0f, 0.0f};
	const uint64_t offset    = push_uniforms(&uniforms, sizeof(uniforms));
	VkDescriptorSet frag_set = alloc_fragment_set(offset);
	VkDescriptorSet sampler_set =
	        alloc_sampler_set(pyramid_views_[0], ssao_depth_sampler_,
	                          white_texture_.view, default_sampler_,
	                          white_texture_.view, default_sampler_);
	context_.api.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                             ssao_pipeline_);
	const uint32_t dynamic = static_cast<uint32_t>(offset);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 2, 1, &sampler_set, 0,
	                                   nullptr);
	context_.api.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
	                                   lit_layout_, 3, 1, &frag_set, 1,
	                                   &dynamic);
	context_.api.CmdDraw(cmd_, 3, 1, 0, 0);
	context_.api.CmdEndRendering(cmd_);
	image_shader_read(*barriers_, cmd_, ssao_texture_, 0, 1, ssao_state_);
	barriers_->flush(cmd_);
	ssao_target_width_  = width;
	ssao_target_height_ = height;
}

void GpuRendererVulkan::destroy_ssao()
{
	if (ssao_pipeline_ != VK_NULL_HANDLE) {
		context_.api.DestroyPipeline(context_.device, ssao_pipeline_,
		                             nullptr);
		ssao_pipeline_ = VK_NULL_HANDLE;
	}
	if (ssao_sampler_ != VK_NULL_HANDLE) {
		context_.api.DestroySampler(context_.device, ssao_sampler_,
		                            nullptr);
		ssao_sampler_ = VK_NULL_HANDLE;
	}
	if (ssao_depth_sampler_ != VK_NULL_HANDLE) {
		context_.api.DestroySampler(context_.device, ssao_depth_sampler_,
		                            nullptr);
		ssao_depth_sampler_ = VK_NULL_HANDLE;
	}
	retire_image(ssao_texture_);
	ssao_texture_        = vk::Image{};
	ssao_state_          = vk::ImageState{};
	ssao_width_          = 0;
	ssao_height_         = 0;
	ssao_target_width_   = 0;
	ssao_target_height_  = 0;
}

bool GpuRendererVulkan::ssao_on() const
{
	// Scales fill only, never the sun. Disabled (or no texture yet) binds
	// white so the factor reads 1.
	return lighting_.ssao_enabled && !ssao_unavailable_ &&
	       ssao_texture_.handle != VK_NULL_HANDLE &&
	       ssao_sampler_ != VK_NULL_HANDLE && ssao_target_width_ != 0 &&
	       ssao_target_height_ != 0;
}

VkDescriptorSet GpuRendererVulkan::alloc_lit_sampler_set(VkImageView albedo)
{
	const bool shadow_on = lighting_.shadows_enabled && shadows_supported_ &&
	                       shadow_rendered_ &&
	                       shadow_map_.handle != VK_NULL_HANDLE &&
	                       shadow_sampler_ != VK_NULL_HANDLE;
	VkImageView shadow_view =
	        shadow_on ? shadow_map_.view : white_texture_.view;
	VkSampler shadow_sampler =
	        shadow_on ? shadow_sampler_ : default_sampler_;
	// Binding 2 (SSAO) samples the previous frame's AO texture, or white
	// (AO=1) when off/unavailable.
	const bool on          = ssao_on();
	VkImageView ao_view    = on ? ssao_texture_.view : white_texture_.view;
	VkSampler ao_sampler   = on ? ssao_sampler_ : default_sampler_;
	return alloc_sampler_set(albedo, default_sampler_, shadow_view,
	                         shadow_sampler, ao_view, ao_sampler);
}

bool GpuRendererVulkan::viewport_ready() const
{
	return viewport_color_.handle != VK_NULL_HANDLE;
}

uint32_t GpuRendererVulkan::viewport_width() const
{
	return viewport_width_;
}

uint32_t GpuRendererVulkan::viewport_height() const
{
	return viewport_height_;
}

bool GpuRendererVulkan::resize_viewport(uint32_t width, uint32_t height)
{
	if (!device_valid_ || width == 0 || height == 0) {
		return false;
	}
	if (width == viewport_width_ && height == viewport_height_ &&
	    viewport_color_.handle != VK_NULL_HANDLE) {
		return true;
	}
	// Create new images before retiring old ones, so a failure leaves the
	// renderer on its old viewport rather than on none.
	vk::Image new_color;
	vk::Image new_depth;
	vk::Image new_linear;
	try {
		new_color = create_target_image(
		        "viewport", pipeline_color_format_, width, height, 1,
		        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		                VK_IMAGE_USAGE_SAMPLED_BIT |
		                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		new_depth = create_target_image(
		        "viewport.depth", kDepthFormat, width, height, 1,
		        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
		// R32F stand-in for colour target 1 (lit.frag writes linear depth
		// there). Sized to the panel, not the offscreen target, so the
		// viewport pass never distorts the pyramid's aspect ratio.
		new_linear = create_target_image(
		        "viewport.linear", kPyramidFormat, width, height, 1,
		        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	} catch (const std::exception&) {
		if (new_color.handle != VK_NULL_HANDLE) {
			memory_.destroy_image(new_color);
		}
		if (new_depth.handle != VK_NULL_HANDLE) {
			memory_.destroy_image(new_depth);
		}
		if (new_linear.handle != VK_NULL_HANDLE) {
			memory_.destroy_image(new_linear);
		}
		return false;
	}
	retire_image(viewport_color_);
	retire_image(viewport_depth_);
	retire_image(viewport_linear_);
	viewport_color_        = new_color;
	viewport_color_state_  = vk::ImageState{};
	viewport_depth_        = new_depth;
	viewport_depth_state_  = vk::ImageState{};
	viewport_linear_       = new_linear;
	viewport_linear_state_ = vk::ImageState{};
	viewport_width_        = width;
	viewport_height_       = height;
	// The view changed: the ImGui texture id names the old one.
	refresh_viewport_ui_texture();
	return true;
}

void GpuRendererVulkan::destroy_viewport()
{
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
	viewport_pass_open_    = false;
}

bool GpuRendererVulkan::begin_viewport_pass()
{
	if (cmd_ == VK_NULL_HANDLE || slot_ == nullptr ||
	    viewport_color_.handle == VK_NULL_HANDLE ||
	    viewport_linear_.handle == VK_NULL_HANDLE ||
	    viewport_depth_.handle == VK_NULL_HANDLE) {
		return false;
	}
	// Same once-per-frame shadow map the scene pass uses; a no-op when the
	// scene pass already rendered it.
	render_shadow_map();
	vk::Image* color_image       = &viewport_color_;
	vk::ImageState* color_state  = &viewport_color_state_;
	pass_redirected_             = false;
	sky_drawn_                   = false;
	pass_color_view_             = viewport_color_.view;
	if (lighting_.fxaa_enabled && !fxaa_unavailable_ &&
	    viewport_width_ != 0 && viewport_height_ != 0 &&
	    ensure_fxaa_pipeline() &&
	    ensure_fxaa_source(viewport_width_, viewport_height_)) {
		color_image      = &fxaa_source_;
		color_state      = &fxaa_source_state_;
		pass_redirected_ = true;
		pass_color_view_ = fxaa_source_.view;
	}
	image_as_color_attachment(*barriers_, cmd_, *color_image, 0, 1,
	                          *color_state);
	image_as_color_attachment(*barriers_, cmd_, viewport_linear_, 0, 1,
	                          viewport_linear_state_);
	image_as_depth_attachment(*barriers_, cmd_, viewport_depth_,
	                          viewport_depth_state_);
	barriers_->flush(cmd_);
	// The pass's viewport: that of its first colour target, which is the
	// viewport texture or the FXAA source made from it.
	pass_width_         = viewport_width_;
	pass_height_        = viewport_height_;
	viewport_pass_open_ = true;
	// Rendering itself begins on the first geometry draw, as in
	// begin_scene_pass; a pass that drew nothing is begun with a full
	// clear in end_viewport_pass.
	pass_rendering_begun_ = false;
	return true;
}

void GpuRendererVulkan::end_viewport_pass()
{
	if (!viewport_pass_open_) {
		return;
	}
	if (cmd_ != VK_NULL_HANDLE) {
		// A pass that drew nothing never began rendering: begin it now
		// with the full clear, exactly as before.
		if (!pass_rendering_begun_) {
			begin_rendering(cmd_, false);
			pass_rendering_begun_ = true;
		}
		context_.api.CmdEndRendering(cmd_);
		// FXAA resolve onto the viewport texture the UI samples. Skipped
		// when the pass rendered directly.
		if (pass_redirected_) {
			fxaa_resolve(viewport_color_, viewport_color_state_,
			             fxaa_width_, fxaa_height_);
		}
		pass_redirected_ = false;
		// The viewport pass does NOT record the Hi-Z pyramid. The main pass
		// (begin_scene_pass / end_scene_pass) stays authoritative for
		// occlusion.
	}
	// Cleared last: begin_rendering above takes the viewport's own
	// linear-depth and depth images only while the pass is still open, so a
	// pass that drew nothing must not close first, or it clears the scene's.
	viewport_pass_open_ = false;
}

std::vector<uint8_t> GpuRendererVulkan::capture_viewport_rgba(
        uint32_t& width, uint32_t& height)
{
	width  = 0;
	height = 0;
	if (viewport_color_.handle == VK_NULL_HANDLE) {
		return {};
	}
	width  = viewport_width_;
	height = viewport_height_;
	return download_image_rgba(viewport_color_, viewport_color_state_,
	                           width, height);
}

void GpuRendererVulkan::begin_rendering(VkCommandBuffer cmd,
                                        bool skip_target1_clear)
{
	// The scene pass renders into the offscreen target and the pyramid; the
	// viewport pass into its own colour, linear-depth and depth images. Both
	// declare the same two colour targets (lit.frag writes location 1
	// unconditionally), so the pipelines agree with either.
	VkImageView color_view  = pass_color_view_ != VK_NULL_HANDLE
	                                ? pass_color_view_
	                                : offscreen_color_.view;
	VkImageView linear_view = pyramid_views_[0];
	VkImageView depth_view  = depth_.view;
	if (viewport_pass_open_) {
		linear_view = viewport_linear_.view;
		depth_view  = viewport_depth_.view;
	}
	VkRenderingAttachmentInfo colors[2]{};
	colors[0].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colors[0].imageView   = color_view;
	colors[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colors[0].loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colors[0].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	colors[0].clearValue.color = VkClearColorValue{
	        {clear_r_, clear_g_, clear_b_, 1.0f}};
	// Cleared unless the caller proved the sky covers it: the sky writes
	// DEPTH_FAR (== kDepthFar) to every pixel no geometry touched, and the
	// geometry writes its own linear depth, so together they define every
	// texel and the clear is redundant memory traffic. An unwritten texel
	// would read as ~0 and poison the min reduction, so the proof matters.
	colors[1].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	colors[1].imageView   = linear_view;
	colors[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colors[1].loadOp      = skip_target1_clear
	                                ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
	                                : VK_ATTACHMENT_LOAD_OP_CLEAR;
	colors[1].storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
	const float far       = kDepthFar;
	colors[1].clearValue.color = VkClearColorValue{{far, far, far, far}};

	VkRenderingAttachmentInfo depth{};
	depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	depth.imageView   = depth_view;
	depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
	depth.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depth.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth.clearValue.depthStencil = VkClearDepthStencilValue{1.0f, 0};

	VkRenderingInfo rendering{};
	rendering.sType               = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea           = VkRect2D{{0, 0},
	                                           {pass_width_, pass_height_}};
	rendering.layerCount           = 1;
	rendering.colorAttachmentCount = 2;
	rendering.pColorAttachments    = colors;
	rendering.pDepthAttachment     = &depth;
	context_.api.CmdBeginRendering(cmd, &rendering);
}

} // namespace lotus
