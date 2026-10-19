#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gpu/vk/vk_api.hpp"
#include "gpu/vk/vk_memory.hpp"

namespace lotus::vk {

/// Where an image subresource was last used. One per subresource that is
/// tracked separately: the renderer keeps one per depth-pyramid mip, because
/// mip 0 is a colour attachment while later mips are storage images.
struct ImageState {
	VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
	VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
	VkAccessFlags2 access{0};
};

struct BufferState {
	VkPipelineStageFlags2 stage{VK_PIPELINE_STAGE_2_NONE};
	VkAccessFlags2 access{0};
};

/// Records the barriers a frame needs, and only those: a resource is
/// transitioned when the requested layout or the stage/access pair differs from
/// what it was last used with, and everything queued goes out as one
/// vkCmdPipelineBarrier2. Synchronization scope two covers every later command
/// in submission order, so one instance of a shared image can serve both frames
/// in flight without per-frame copies.
class BarrierBatch {
public:
	explicit BarrierBatch(const Api& api) : api_{api} {}

	void image(VkCommandBuffer cmd, const Image& image, uint32_t base_mip,
	           uint32_t mip_count, ImageState& state, VkImageLayout layout,
	           VkPipelineStageFlags2 stage, VkAccessFlags2 access);
	void buffer(VkCommandBuffer cmd, const Buffer& buffer, BufferState& state,
	            VkPipelineStageFlags2 stage, VkAccessFlags2 access,
	            uint64_t offset = 0, uint64_t size = VK_WHOLE_SIZE);
	/// Records everything queued, as one barrier call, and clears the queue.
	void flush(VkCommandBuffer cmd);

	/// Queued but not yet flushed (tests assert that redundant calls add
	/// nothing).
	std::size_t pending() const { return images_.size() + buffers_.size(); }

private:
	const Api& api_;
	std::vector<VkImageMemoryBarrier2> images_;
	std::vector<VkBufferMemoryBarrier2> buffers_;
};

// --- named transitions: the stage/access pair the resource is used with next ---

void image_as_color_attachment(BarrierBatch& batch, VkCommandBuffer cmd,
                               const Image& image, uint32_t base_mip,
                               uint32_t mip_count, ImageState& state);
/// Depth attachment that will be written (the shadow pass).
void image_as_depth_attachment(BarrierBatch& batch, VkCommandBuffer cmd,
                               const Image& image, ImageState& state);
/// Depth attachment read without writing: sampling depth in place is not done
/// by the engine, but the transition is needed for layouts that must change
/// before a read.
void image_as_depth_read(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Image& image, ImageState& state);
/// Sampled from a fragment or compute stage.
void image_shader_read(BarrierBatch& batch, VkCommandBuffer cmd,
                       const Image& image, uint32_t base_mip, uint32_t mip_count,
                       ImageState& state);
/// Read-write storage image (the Hi-Z reduction).
void image_storage_read_write(BarrierBatch& batch, VkCommandBuffer cmd,
                              const Image& image, uint32_t base_mip,
                              uint32_t mip_count, ImageState& state);
void image_transfer_src(BarrierBatch& batch, VkCommandBuffer cmd,
                        const Image& image, uint32_t base_mip,
                        uint32_t mip_count, ImageState& state);
void image_transfer_dst(BarrierBatch& batch, VkCommandBuffer cmd,
                        const Image& image, uint32_t base_mip,
                        uint32_t mip_count, ImageState& state);
void image_present_src(BarrierBatch& batch, VkCommandBuffer cmd,
                       const Image& image, ImageState& state);

void buffer_vertex_read(BarrierBatch& batch, VkCommandBuffer cmd,
                        const Buffer& buffer, BufferState& state);
void buffer_index_read(BarrierBatch& batch, VkCommandBuffer cmd,
                       const Buffer& buffer, BufferState& state);
void buffer_uniform_read(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state);
void buffer_transfer_dst(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state);
void buffer_transfer_src(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state);
void buffer_storage_read(BarrierBatch& batch, VkCommandBuffer cmd,
                         const Buffer& buffer, BufferState& state);

} // namespace lotus::vk