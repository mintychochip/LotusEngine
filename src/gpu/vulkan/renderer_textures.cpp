// Samplers, targets, the 1x1 fallback textures and the albedo/normal caches.

#include "gpu/vulkan/renderer_vulkan.hpp"

#include "lotus/scene/assets.hpp"
#include "stb_image.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace lotus {

void GpuRendererVulkan::create_samplers()
{
	if (!device_valid_) {
		return;
	}
	const auto make = [&](VkFilter min, VkFilter mag, VkSamplerMipmapMode mips,
	                      VkSamplerAddressMode wrap, float max_lod) {
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
		vk::check(context_.api.CreateSampler(context_.device, &info, nullptr,
		                                     &sampler),
		          "vkCreateSampler");
		return sampler;
	};
	if (default_sampler_ == VK_NULL_HANDLE) {
		// LINEAR/LINEAR/LINEAR-mip/REPEAT, no anisotropy/compare/LOD bias.
		// maxLod is 0, so every lookup samples mip 0: the generated chain is
		// never read. That is how the engine has always rendered, and the
		// bench's golden image and exact hashes depend on it; sampling the
		// chain would be a visual change, not a refactor.
		default_sampler_ = make(VK_FILTER_LINEAR, VK_FILTER_LINEAR,
		                        VK_SAMPLER_MIPMAP_MODE_LINEAR,
		                        VK_SAMPLER_ADDRESS_MODE_REPEAT, 0.0f);
	}
	if (shadow_sampler_ == VK_NULL_HANDLE) {
		// NEAREST/CLAMP: lit.frag does its own 5x5 PCF, so no depth
		// filtering is required beyond plain depth sampling.
		shadow_sampler_ = make(VK_FILTER_NEAREST, VK_FILTER_NEAREST,
		                       VK_SAMPLER_MIPMAP_MODE_NEAREST,
		                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		                       VK_LOD_CLAMP_NONE);
	}
}

void GpuRendererVulkan::destroy_samplers()
{
	if (default_sampler_ != VK_NULL_HANDLE) {
		context_.api.DestroySampler(context_.device, default_sampler_, nullptr);
		default_sampler_ = VK_NULL_HANDLE;
	}
	if (shadow_sampler_ != VK_NULL_HANDLE) {
		context_.api.DestroySampler(context_.device, shadow_sampler_, nullptr);
		shadow_sampler_ = VK_NULL_HANDLE;
	}
}

vk::Image GpuRendererVulkan::create_target_image(const char* name, VkFormat format,
                                                 uint32_t width, uint32_t height,
                                                 uint32_t mips,
                                                 VkImageUsageFlags usage)
{
	return memory_.create_image(vk::MemoryClass::DeviceLocal, usage, format,
	                            width, height, mips, name);
}

void GpuRendererVulkan::create_solid_texture(const char* name,
                                             const uint8_t (&texel)[4],
                                             vk::Image& image,
                                             vk::ImageState& state)
{
	image = memory_.create_image(vk::MemoryClass::DeviceLocal,
	                             VK_IMAGE_USAGE_SAMPLED_BIT |
	                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                             VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, name);
	state = vk::ImageState{};
	vk::Buffer staging = memory_.create_buffer(vk::MemoryClass::Staging,
	                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                           sizeof(texel), "texel.upload");
	std::memcpy(staging.mapped, texel, sizeof(texel));
	memory_.flush(staging.allocation, 0, sizeof(texel));
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		vk::BarrierBatch batch(context_.api);
		vk::BufferState staging_state{};
		buffer_transfer_src(batch, cmd, staging, staging_state);
		image_transfer_dst(batch, cmd, image, 0, 1, state);
		batch.flush(cmd);
		VkBufferImageCopy region{};
		region.bufferOffset      = 0;
		region.bufferRowLength   = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel       = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount     = 1;
		region.imageOffset = VkOffset3D{0, 0, 0};
		region.imageExtent = VkExtent3D{1, 1, 1};
		context_.api.CmdCopyBufferToImage(cmd, staging.handle, image.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                                  1, &region);
		image_shader_read(batch, cmd, image, 0, 1, state);
		batch.flush(cmd);
	});
	memory_.destroy_buffer(staging);
}

void GpuRendererVulkan::create_white_texture()
{
	if (white_texture_.handle != VK_NULL_HANDLE || !device_valid_) {
		return;
	}
	const uint8_t white[4] = {255, 255, 255, 255};
	create_solid_texture("white", white, white_texture_, white_state_);
}

void GpuRendererVulkan::create_flat_normal_texture()
{
	if (flat_normal_texture_.handle != VK_NULL_HANDLE || !device_valid_) {
		return;
	}
	// (0.5, 0.5, 1) is the tangent-space "straight up" normal. The zero alpha
	// is the sentinel lit.frag tests: no normal map, exact legacy path.
	const uint8_t flat[4] = {128, 128, 255, 0};
	create_solid_texture("flat_normal", flat, flat_normal_texture_,
	                     flat_normal_state_);
}

void GpuRendererVulkan::destroy_textures()
{
	for (TextureCache* cache : {&albedo_textures_, &normal_textures_}) {
		for (auto& entry : *cache) {
			if (entry.second.image.handle != VK_NULL_HANDLE) {
				memory_.destroy_image(entry.second.image);
				entry.second.image = vk::Image{};
			}
		}
		cache->clear();
	}
	if (flat_normal_texture_.handle != VK_NULL_HANDLE) {
		memory_.destroy_image(flat_normal_texture_);
		flat_normal_texture_ = vk::Image{};
	}
	flat_normal_state_ = vk::ImageState{};
	if (white_texture_.handle != VK_NULL_HANDLE) {
		memory_.destroy_image(white_texture_);
		white_texture_ = vk::Image{};
	}
	white_state_ = vk::ImageState{};
}

bool GpuRendererVulkan::load_texture_from_file(const std::string& path,
                                               TextureCache& cache,
                                               const std::string& id)
{
	if (!device_valid_ || path.empty()) {
		return false;
	}
	const auto existing = cache.find(id);
	if (existing != cache.end() &&
	    existing->second.image.handle != VK_NULL_HANDLE) {
		return true;
	}
	int width    = 0;
	int height   = 0;
	int channels = 0;
	uint8_t* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
	if (pixels == nullptr || width <= 0 || height <= 0) {
		if (pixels != nullptr) {
			stbi_image_free(pixels);
		}
		return false;
	}

	// Full mip chain so texture() auto-LODs: the sampler already requests
	// LINEAR mip filtering, but a num_levels=1 texture has no mips to pick.
	uint32_t levels = 1;
	for (uint32_t dim = static_cast<uint32_t>(std::max(width, height)) >> 1;
	     dim > 0; dim >>= 1) {
		++levels;
	}
	vk::Image image = memory_.create_image(
	        vk::MemoryClass::DeviceLocal,
	        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	        VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(width),
	        static_cast<uint32_t>(height), levels, "texture");
	vk::ImageState state;

	const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
	const std::size_t total     = row_bytes * static_cast<std::size_t>(height);
	vk::Buffer staging = memory_.create_buffer(vk::MemoryClass::Staging,
	                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                           total, "texture.upload");
	std::memcpy(staging.mapped, pixels, total);
	stbi_image_free(pixels);
	memory_.flush(staging.allocation, 0, total);

	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		vk::BarrierBatch batch(context_.api);
		vk::BufferState staging_state{};
		buffer_transfer_src(batch, cmd, staging, staging_state);
		image_transfer_dst(batch, cmd, image, 0, levels, state);
		batch.flush(cmd);
		VkBufferImageCopy region{};
		region.bufferOffset      = 0;
		region.bufferRowLength   = 0;
		region.bufferImageHeight = 0;
		region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel       = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount     = 1;
		region.imageOffset = VkOffset3D{0, 0, 0};
		region.imageExtent = VkExtent3D{static_cast<uint32_t>(width),
		                                static_cast<uint32_t>(height), 1};
		context_.api.CmdCopyBufferToImage(cmd, staging.handle, image.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                                  1, &region);
		// Level-by-level LINEAR blits down the chain, so minified fragments
		// sample filtered mips instead of shimmering on full-res texels.
		// One state per mip: level K-1 is a blit source while level K is
		// still a blit destination, so a single whole-image state would
		// record the wrong oldLayout for every level after the first.
		std::vector<vk::ImageState> mip_states(
		        levels, vk::ImageState{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                               VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		                               VK_ACCESS_2_TRANSFER_WRITE_BIT});
		uint32_t mip_width  = static_cast<uint32_t>(width);
		uint32_t mip_height = static_cast<uint32_t>(height);
		for (uint32_t level = 1; level < levels; ++level) {
			const uint32_t src_width  = mip_width;
			const uint32_t src_height = mip_height;
			mip_width  = std::max(1u, mip_width / 2);
			mip_height = std::max(1u, mip_height / 2);
			image_transfer_src(batch, cmd, image, level - 1, 1,
			                   mip_states[level - 1]);
			batch.flush(cmd);
			VkImageBlit blit{};
			blit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.mipLevel       = level - 1;
			blit.srcSubresource.baseArrayLayer = 0;
			blit.srcSubresource.layerCount     = 1;
			blit.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(src_width),
			                                static_cast<int32_t>(src_height),
			                                1};
			blit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.mipLevel       = level;
			blit.dstSubresource.baseArrayLayer = 0;
			blit.dstSubresource.layerCount     = 1;
			blit.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(mip_width),
			                                static_cast<int32_t>(mip_height),
			                                1};
			context_.api.CmdBlitImage(cmd, image.handle,
			                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			                          image.handle,
			                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			                          &blit, VK_FILTER_LINEAR);
		}
		for (uint32_t level = 0; level < levels; ++level) {
			image_shader_read(batch, cmd, image, level, 1,
			                  mip_states[level]);
		}
		state = mip_states[0];
		batch.flush(cmd);
	});
	memory_.destroy_buffer(staging);
	cache[id] = CachedTexture{image, state};
	return true;
}

VkImageView GpuRendererVulkan::cached_texture(TextureCache& cache,
                                              const std::string& id,
                                              VkImageView fallback,
                                              const AssetRegistry* assets)
{
	const auto it = cache.find(id);
	if (it != cache.end() && it->second.image.handle != VK_NULL_HANDLE) {
		return it->second.image.view;
	}
	std::string path;
	if (assets != nullptr) {
		const std::string* registered = assets->find(id);
		if (registered != nullptr && !registered->empty()) {
			path = *registered;
		}
	}
	if (path.empty() || !load_texture_from_file(path, cache, id)) {
		return fallback;
	}
	return cache[id].image.view;
}

VkImageView GpuRendererVulkan::texture_for_albedo(const std::string& albedo_id,
                                                 const AssetRegistry* assets)
{
	if (white_texture_.handle == VK_NULL_HANDLE) {
		create_white_texture();
	}
	if (albedo_id.empty()) {
		return white_texture_.view;
	}
	return cached_texture(albedo_textures_, albedo_id, white_texture_.view,
	                      assets);
}

VkImageView GpuRendererVulkan::texture_for_normal(const std::string& normal_id,
                                                 const AssetRegistry* assets)
{
	if (flat_normal_texture_.handle == VK_NULL_HANDLE) {
		create_flat_normal_texture();
	}
	if (normal_id.empty()) {
		return flat_normal_texture_.view;
	}
	return cached_texture(normal_textures_, normal_id,
	                      flat_normal_texture_.view, assets);
}

} // namespace lotus
