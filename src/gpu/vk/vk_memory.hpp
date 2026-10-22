#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/vk/vk_context.hpp"

namespace lotus::vk {

/// What a resource is for. Decides which memory types are acceptable and how
/// the allocator treats it.
enum class MemoryClass {
	/// Device-local only: images, render targets, static mesh data.
	DeviceLocal,
	/// Device-local and host-visible where the device offers it (ReBAR, UMA
	/// integrated), else host memory. Per-frame data lives here: the CPU writes
	/// instance records, immediate vertices and uniforms straight into what the
	/// GPU reads, with no staging copy and no copy pass.
	Dynamic,
	/// Host-visible, written by the CPU and copied by the GPU: uploads.
	Staging,
	/// Host-visible and host-cached where possible: almost always read by the
	/// CPU (frame capture, Hi-Z readback), so plain loads stay cheap. Reading
	/// uncached (write-combined) memory instead costs 0.85 ms a frame on the
	/// Intel iGPU.
	Readback,
};

/// A sub-allocation inside a device memory block.
struct Allocation {
	VkDeviceMemory memory{VK_NULL_HANDLE};
	uint64_t offset{0};
	uint64_t size{0};
	uint32_t type_index{0};
	uint32_t block{0}; ///< index into Memory's block list
	bool host_visible{false};
	/// False when the memory needs flush/invalidate around host access (the
	/// cached-but-coherent-less type on Intel, which readback wants).
	bool coherent{true};
};

/// A VkBuffer bound to its memory.
struct Buffer {
	VkBuffer handle{VK_NULL_HANDLE};
	Allocation allocation;
	uint64_t size{0};
	/// Host pointer: writable for Dynamic and Staging, readable for Readback,
	/// null for DeviceLocal.
	void* mapped{nullptr};
};

/// A 2D VkImage (one sample, one array layer) with a view and its memory.
struct Image {
	VkImage handle{VK_NULL_HANDLE};
	VkImageView view{VK_NULL_HANDLE};
	Allocation allocation;
	VkFormat format{VK_FORMAT_UNDEFINED};
	uint32_t width{0};
	uint32_t height{0};
	uint32_t mip_levels{1};
};

/// Block sub-allocator over the device's memory types, in place of a dependency
/// on VulkanMemoryAllocator: one pool of blocks per (memory type, buffers or
/// images), first-fit free lists that coalesce on release, a dedicated
/// allocation for anything larger than a block. Buffers and optimal-tiling
/// images never share a block, because bufferImageGranularity allows aliasing
/// between them only at some alignment this code does not try to reason about.
class Memory {
public:
	Memory() = default;
	~Memory();
	Memory(const Memory&)            = delete;
	Memory& operator=(const Memory&) = delete;

	void init(Context& context);
	void shutdown();

	/// Buffer offsets are aligned to 256 bytes, which satisfies
	/// minUniformBufferOffsetAlignment, minStorageBufferOffsetAlignment and the
	/// vertex/index alignment of every device the engine runs on.
	Buffer create_buffer(MemoryClass klass, VkBufferUsageFlags usage,
	                     uint64_t size, const char* name = nullptr);
	void destroy_buffer(Buffer& buffer);

	Image create_image(MemoryClass klass, VkImageUsageFlags usage, VkFormat format,
	                   uint32_t width, uint32_t height, uint32_t mip_levels = 1,
	                   const char* name = nullptr);
	void destroy_image(Image& image);

	/// Host writes visible to the GPU, GPU writes visible to the CPU. No-ops on
	/// coherent memory, which every class prefers.
	void flush(const Allocation& allocation, uint64_t offset, uint64_t size) const;
	void invalidate(const Allocation& allocation, uint64_t offset,
	                uint64_t size) const;

	/// Named allocations, for diagnostics (device names come from the caller).
	uint64_t bytes_in_use() const { return bytes_in_use_; }
	uint32_t block_count() const { return static_cast<uint32_t>(blocks_.size()); }

private:
	/// A free or live range inside a block.
	struct Range {
		uint64_t offset{0};
		uint64_t size{0};
		bool live{false};
	};
	struct Block {
		VkDeviceMemory memory{VK_NULL_HANDLE};
		uint64_t size{0};
		uint32_t type_index{0};
		bool host_visible{false};
		bool coherent{true};
		bool is_image{false};
		void* mapped{nullptr};
		uint64_t used{0};
		std::vector<Range> ranges;
	};

	/// The best memory type among \p type_bits: \p required must hold,
	/// \p preferred wins when several qualify.
	uint32_t find_type(VkMemoryPropertyFlags required,
	                   VkMemoryPropertyFlags preferred,
	                   uint32_t type_bits) const;
	/// Index of a block with room for \p size at \p alignment, creating one when
	/// needed. Indices, never references: create_block may grow blocks_.
	uint32_t block_for(uint32_t type_index, bool is_image, uint64_t size,
	                   uint64_t alignment);
	Allocation allocate(uint32_t type_index, bool is_image, uint64_t size,
	                    uint64_t alignment);
	void release(const Allocation& allocation);

	Context* context_{nullptr};
	std::vector<Block> blocks_;
	uint64_t bytes_in_use_{0};
};

} // namespace lotus::vk