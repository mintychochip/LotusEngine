#include "gpu/vk/vk_barrier.hpp"

namespace lotus::vk {
namespace {

/// Sampled/attachment aspect: depth when the format has a depth component,
/// colour otherwise.
VkImageAspectFlags aspect_for(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT;
	default: break;
	}
	return VK_IMAGE_ASPECT_COLOR_BIT;
}

} // namespace

void BarrierBatch::image(VkCommandBuffer cmd, const Image& image,
                         uint32_t base_mip, uint32_t mip_count,
                         ImageState& state, VkImageLayout layout,
                         VkPipelineStageFlags2 stage, VkAccessFlags2 access)
{
	(void)cmd; // Batching only; the barrier goes out on flush().
	if (state.layout == layout && state.stage == stage &&
	    state.access == access) {
		return;
	}
	VkImageMemoryBarrier2 barrier{};
	barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.srcStageMask     = state.stage;
	barrier.srcAccessMask    = state.access;
	barrier.dstStageMask     = stage;
	barrier.dstAccessMask    = access;
	barrier.oldLayout        = state.layout;
	barrier.newLayout        = layout;
	barrier.image            = image.handle;
	barrier.subresourceRange.aspectMask     = aspect_for(image.format);
	barrier.subresourceRange.baseMipLevel   = base_mip;
	barrier.subresourceRange.levelCount     = mip_count;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = 1;
	images_.push_back(barrier);
	state.layout = layout;
	state.stage  = stage;
	state.access = access;
}

void BarrierBatch::buffer(VkCommandBuffer cmd, const Buffer& buffer,
                          BufferState& state, VkPipelineStageFlags2 stage,
                          VkAccessFlags2 access, uint64_t offset, uint64_t size)
{
	(void)cmd; // Batching only; the barrier goes out on flush().
	if (state.stage == stage && state.access == access) {
		return;
	}
	VkBufferMemoryBarrier2 barrier{};
	barrier.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	barrier.srcStageMask  = state.stage;
	barrier.srcAccessMask = state.access;
	barrier.dstStageMask  = stage;
	barrier.dstAccessMask = access;
	barrier.buffer        = buffer.handle;
	barrier.offset        = offset;
	barrier.size          = size;
	buffers_.push_back(barrier);
	state.stage  = stage;
	state.access = access;
}

/// Records everything queued, as one barrier call, and clears the queue. An
/// empty queue records nothing and never calls into Vulkan.
void BarrierBatch::flush(VkCommandBuffer cmd)
{
	if (images_.empty() && buffers_.empty()) {
		return;
	}
	VkDependencyInfo info{};
	info.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	info.imageMemoryBarrierCount  = static_cast<uint32_t>(images_.size());
	info.pImageMemoryBarriers     = images_.data();
	info.bufferMemoryBarrierCount = static_cast<uint32_t>(buffers_.size());
	info.pBufferMemoryBarriers    = buffers_.data();
	api_.CmdPipelineBarrier2(cmd, &info);
	images_.clear();
	buffers_.clear();
}

void image_as_color_attachment(BarrierBatch& batch, VkCommandBuffer cmd,
                               const Image& image, uint32_t base_mip,
                               uint32_t mip_count, ImageState& state)
{
	batch.image(cmd, image, base_mip, mip_count, state,
	            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void image_as_depth_attachment(BarrierBatch& batch, VkCommandBuffer cmd,
                               const Image& image, ImageState& state)
{
	batch.image(cmd, image, 0, image.mip_levels, state,
	            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
	            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
	                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
	            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

void image_as_depth_read(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Image& image, ImageState& state)
{
	batch.image(cmd, image, 0, image.mip_levels, state,
	            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
	            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
	                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
	            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
}

void image_shader_read(BarrierBatch& batch, VkCommandBuffer cmd,
                       const Image& image, uint32_t base_mip,
                       uint32_t mip_count, ImageState& state)
{
	batch.image(cmd, image, base_mip, mip_count, state,
	            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
	                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void image_storage_read_write(BarrierBatch& batch, VkCommandBuffer cmd,
                              const Image& image, uint32_t base_mip,
                              uint32_t mip_count, ImageState& state)
{
	batch.image(cmd, image, base_mip, mip_count, state, VK_IMAGE_LAYOUT_GENERAL,
	            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	            VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
	                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

void image_transfer_src(BarrierBatch& batch, VkCommandBuffer cmd,
                        const Image& image, uint32_t base_mip,
                        uint32_t mip_count, ImageState& state)
{
	batch.image(cmd, image, base_mip, mip_count, state,
	            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	            VK_ACCESS_2_TRANSFER_READ_BIT);
}

void image_transfer_dst(BarrierBatch& batch, VkCommandBuffer cmd,
                        const Image& image, uint32_t base_mip,
                        uint32_t mip_count, ImageState& state)
{
	batch.image(cmd, image, base_mip, mip_count, state,
	            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	            VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

void image_present_src(BarrierBatch& batch, VkCommandBuffer cmd,
                       const Image& image, ImageState& state)
{
	batch.image(cmd, image, 0, image.mip_levels, state,
	            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	            VK_PIPELINE_STAGE_2_NONE, 0);
}

void buffer_vertex_read(BarrierBatch& batch, VkCommandBuffer cmd,
                        const Buffer& buffer, BufferState& state)
{
	batch.buffer(cmd, buffer, state, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
	             VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
}

void buffer_index_read(BarrierBatch& batch, VkCommandBuffer cmd,
                       const Buffer& buffer, BufferState& state)
{
	batch.buffer(cmd, buffer, state, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
	             VK_ACCESS_2_INDEX_READ_BIT);
}

void buffer_uniform_read(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state)
{
	batch.buffer(cmd, buffer, state,
	             VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
	                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
	                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	             VK_ACCESS_2_UNIFORM_READ_BIT);
}

void buffer_transfer_dst(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state)
{
	batch.buffer(cmd, buffer, state, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	             VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

void buffer_transfer_src(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state)
{
	batch.buffer(cmd, buffer, state, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	             VK_ACCESS_2_TRANSFER_READ_BIT);
}

void buffer_storage_read(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state)
{
	batch.buffer(cmd, buffer, state, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	             VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

} // namespace lotus::vk
