#include "gpu/vk/vk_memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace lotus::vk {
namespace {

/// Block sizes, one pool of blocks per (memory type, buffers or images). A
/// request larger than its block size gets a dedicated VkDeviceMemory.
constexpr uint64_t kDeviceLocalBlockSize = 64u * 1024u * 1024u;
constexpr uint64_t kDynamicBlockSize     = 4u * 1024u * 1024u;
constexpr uint64_t kStagingBlockSize     = 8u * 1024u * 1024u;
constexpr uint64_t kReadbackBlockSize    = 4u * 1024u * 1024u;

/// Buffer offsets are aligned to 256 bytes, which satisfies
/// minUniformBufferOffsetAlignment, minStorageBufferOffsetAlignment and the
/// vertex/index alignment of every device the engine runs on.
constexpr uint64_t kBufferAlignment = 256;

uint64_t align_up(uint64_t value, uint64_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}

/// The block size for a buffer class. Images always use the device-local size.
uint64_t block_size_for(MemoryClass klass)
{
	switch (klass) {
	case MemoryClass::DeviceLocal: return kDeviceLocalBlockSize;
	case MemoryClass::Dynamic: return kDynamicBlockSize;
	case MemoryClass::Staging: return kStagingBlockSize;
	case MemoryClass::Readback: return kReadbackBlockSize;
	}
	return kDeviceLocalBlockSize;
}

/// The default size of a new shared block when the class is not known (the
/// member block_for path): images and non-host-visible types use the
/// device-local size, ReBAR/UMA device-local host-visible the dynamic size,
/// other host-visible the staging size. The exact per-class sizes above still
/// decide the dedicated threshold in create_buffer/create_image.
uint64_t default_block_size(VkMemoryPropertyFlags flags, bool is_image)
{
	if (is_image) {
		return kDeviceLocalBlockSize;
	}
	if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
		return kDeviceLocalBlockSize;
	}
	if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
		return kDynamicBlockSize;
	}
	return kStagingBlockSize;
}

/// The best readback type: require host-visible, prefer cached (cheap CPU
/// loads are the whole point), then coherent. Lowest index wins within a rank.
uint32_t readback_type(const VkPhysicalDeviceMemoryProperties& props,
                       uint32_t type_bits)
{
	uint32_t best      = UINT32_MAX;
	int best_rank      = INT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		if (((type_bits >> i) & 1u) == 0) {
			continue;
		}
		const auto flags = props.memoryTypes[i].propertyFlags;
		if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
			continue;
		}
		const int rank =
		        (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0
		        ? 0
		        : ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0 ? 1 : 2);
		if (rank < best_rank) {
			best_rank = rank;
			best      = i;
		}
	}
	if (best == UINT32_MAX) {
		throw std::runtime_error("no suitable Vulkan memory type");
	}
	return best;
}

/// True when the format has a depth component (colour otherwise).
bool format_has_depth(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
	default: break;
	}
	return false;
}

bool format_has_stencil(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return true;
	default: break;
	}
	return false;
}

VkImageAspectFlags aspect_for(VkFormat format)
{
	if (!format_has_depth(format)) {
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
	if (format_has_stencil(format)) {
		aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	return aspect;
}

/// A mapped range rounded out to the non-coherent atom size, as Vulkan
/// requires for flush/invalidate on non-coherent memory.
VkMappedMemoryRange mapped_range(const Context& context,
                                 const Allocation& allocation, uint64_t offset,
                                 uint64_t size)
{
	VkMappedMemoryRange range{};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = allocation.memory;
	const uint64_t atom = std::max<uint64_t>(
	        context.properties.limits.nonCoherentAtomSize, 1);
	const uint64_t begin = allocation.offset + offset;
	range.offset         = begin / atom * atom;
	range.size           = align_up(begin + size, atom) - range.offset;
	return range;
}

void name_object(const Api& api, VkDevice device, VkObjectType type,
                 uint64_t handle, const char* name)
{
	if (name == nullptr || handle == 0 ||
	    api.SetDebugUtilsObjectNameEXT == nullptr) {
		return;
	}
	VkDebugUtilsObjectNameInfoEXT info{};
	info.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	info.objectType   = type;
	info.objectHandle = handle;
	info.pObjectName  = name;
	check(api.SetDebugUtilsObjectNameEXT(device, &info),
	      "vkSetDebugUtilsObjectNameEXT");
}

/// A freshly allocated device memory block, mapped when host-visible.
struct RawBlock {
	VkDeviceMemory memory{VK_NULL_HANDLE};
	void* mapped{nullptr};
};

RawBlock allocate_block_memory(const Api& api, VkDevice device,
                               uint32_t type_index, uint64_t size,
                               bool host_visible)
{
	VkMemoryAllocateInfo info{};
	info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	info.allocationSize  = size;
	info.memoryTypeIndex = type_index;
	VkDeviceMemory memory{VK_NULL_HANDLE};
	check(api.AllocateMemory(device, &info, nullptr, &memory),
	      "vkAllocateMemory");
	if (!host_visible) {
		return {memory, nullptr};
	}
	void* mapped{nullptr};
	const VkResult mapped_result =
	        api.MapMemory(device, memory, 0, size, 0, &mapped);
	if (mapped_result != VK_SUCCESS) {
		api.FreeMemory(device, memory, nullptr);
		fail("vkMapMemory", mapped_result);
	}
	return {memory, mapped};
}

} // namespace

Memory::~Memory()
{
	// The context must outlive the memory: blocks are freed explicitly by
	// shutdown() while the device is alive. A late destructor only releases
	// the bookkeeping so a member-order slip cannot crash on a dead device.
	if (context_ != nullptr && context_->device != VK_NULL_HANDLE &&
	    !blocks_.empty()) {
		shutdown();
	} else {
		blocks_.clear();
		bytes_in_use_ = 0;
	}
}

void Memory::init(Context& context)
{
	context_ = &context;
}

/// The best memory type among \p type_bits: \p required must hold,
/// \p preferred wins when several qualify; lowest index wins overall.
uint32_t Memory::find_type(VkMemoryPropertyFlags required,
                           VkMemoryPropertyFlags preferred,
                           uint32_t type_bits) const
{
	const auto& props = context_->memory;
	uint32_t fallback = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		if (((type_bits >> i) & 1u) == 0) {
			continue;
		}
		const auto flags = props.memoryTypes[i].propertyFlags;
		if ((flags & required) != required) {
			continue;
		}
		if (preferred == 0 || (flags & preferred) == preferred) {
			return i;
		}
		if (fallback == UINT32_MAX) {
			fallback = i;
		}
	}
	if (fallback != UINT32_MAX) {
		return fallback;
	}
	throw std::runtime_error("no suitable Vulkan memory type");
}

/// Index of a block with room for \p size at \p alignment, creating one when
/// needed. New shared blocks are at least the default size for their type.
uint32_t Memory::block_for(uint32_t type_index, bool is_image, uint64_t size,
                           uint64_t alignment)
{
	for (uint32_t b = 0; b < blocks_.size(); ++b) {
		const Block& block = blocks_[b];
		if (block.type_index != type_index || block.is_image != is_image) {
			continue;
		}
		for (const Range& range : block.ranges) {
			if (range.live) {
				continue;
			}
			if (align_up(range.offset, alignment) + size <=
			    range.offset + range.size) {
				return b;
			}
		}
	}
	const auto& api    = context_->api;
	const auto device  = context_->device;
	const auto flags   = context_->memory.memoryTypes[type_index].propertyFlags;
	const bool host_visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
	const uint64_t block_size =
	        std::max(default_block_size(flags, is_image),
	                 align_up(size, alignment));
	RawBlock raw =
	        allocate_block_memory(api, device, type_index, block_size,
	                              host_visible);
	Block block;
	block.memory       = raw.memory;
	block.size         = block_size;
	block.type_index   = type_index;
	block.host_visible = host_visible;
	block.coherent =
	        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
	block.is_image = is_image;
	block.mapped   = raw.mapped;
	block.ranges.push_back(Range{0, block_size, false});
	blocks_.push_back(block);
	return static_cast<uint32_t>(blocks_.size() - 1);
}

Allocation Memory::allocate(uint32_t type_index, bool is_image, uint64_t size,
                            uint64_t alignment)
{
	const uint32_t index = block_for(type_index, is_image, size, alignment);
	Block& block         = blocks_[index];
	for (std::size_t i = 0; i < block.ranges.size(); ++i) {
		Range& range = block.ranges[i];
		if (range.live) {
			continue;
		}
		const uint64_t aligned = align_up(range.offset, alignment);
		if (aligned + size > range.offset + range.size) {
			continue;
		}
		const uint64_t prefix = aligned - range.offset;
		const uint64_t suffix =
		        (range.offset + range.size) - (aligned + size);
		// Replace the free range with its live middle and free edges.
		std::vector<Range> replacement;
		if (prefix > 0) {
			replacement.push_back(Range{range.offset, prefix, false});
		}
		replacement.push_back(Range{aligned, size, true});
		if (suffix > 0) {
			replacement.push_back(
			        Range{aligned + size, suffix, false});
		}
		block.ranges.erase(block.ranges.begin() +
		                   static_cast<std::ptrdiff_t>(i));
		block.ranges.insert(block.ranges.begin() +
		                            static_cast<std::ptrdiff_t>(i),
		                    replacement.begin(), replacement.end());
		block.used += size;
		bytes_in_use_ += size;
		return Allocation{block.memory, aligned,      size,
		                  type_index,   index,        block.host_visible,
		                  block.coherent};
	}
	throw std::runtime_error("Vulkan block allocator found no room");
}

void Memory::release(const Allocation& allocation)
{
	if (allocation.memory == VK_NULL_HANDLE || allocation.size == 0) {
		return;
	}
	Block& block = blocks_[allocation.block];
	for (std::size_t i = 0; i < block.ranges.size(); ++i) {
		Range& range = block.ranges[i];
		if (!range.live || range.offset != allocation.offset ||
		    range.size != allocation.size) {
			continue;
		}
		range.live = false;
		// Coalesce with the free neighbours on both sides.
		if (i > 0 && !block.ranges[i - 1].live) {
			block.ranges[i - 1].size += range.size;
			block.ranges.erase(block.ranges.begin() +
			                   static_cast<std::ptrdiff_t>(i));
			--i;
		}
		if (i + 1 < block.ranges.size() && !block.ranges[i + 1].live) {
			block.ranges[i].size += block.ranges[i + 1].size;
			block.ranges.erase(block.ranges.begin() +
			                   static_cast<std::ptrdiff_t>(i + 1));
		}
		block.used -= allocation.size;
		bytes_in_use_ -= allocation.size;
		return;
	}
}

Buffer Memory::create_buffer(MemoryClass klass, VkBufferUsageFlags usage,
                             uint64_t size, const char* name)
{
	const auto& api = context_->api;
	const auto device = context_->device;
	VkBufferCreateInfo info{};
	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size        = size;
	info.usage       = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer handle{VK_NULL_HANDLE};
	check(api.CreateBuffer(device, &info, nullptr, &handle), "vkCreateBuffer");
	try {
		VkMemoryRequirements requirements{};
		api.GetBufferMemoryRequirements(device, handle, &requirements);
		const uint64_t alloc_size = std::max(size, requirements.size);
		const uint64_t alignment = std::lcm(kBufferAlignment,
		                                    requirements.alignment);
		uint32_t type_index      = UINT32_MAX;
		switch (klass) {
		case MemoryClass::DeviceLocal:
			type_index = find_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			                       0, requirements.memoryTypeBits);
			break;
		case MemoryClass::Dynamic:
			type_index = find_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			                       requirements.memoryTypeBits);
			break;
		case MemoryClass::Staging:
			type_index = find_type(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			                       0, requirements.memoryTypeBits);
			break;
		case MemoryClass::Readback:
			type_index = readback_type(context_->memory,
			                           requirements.memoryTypeBits);
			break;
		}
		Buffer buffer;
		buffer.size = size;
		if (alloc_size > block_size_for(klass)) {
			// A dedicated block holding exactly this one range.
			const auto flags = context_->memory.memoryTypes[type_index]
			                           .propertyFlags;
			const bool host_visible =
			        (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
			RawBlock raw = allocate_block_memory(api, device, type_index,
			                                     alloc_size, host_visible);
			name_object(api, device, VK_OBJECT_TYPE_DEVICE_MEMORY,
			            reinterpret_cast<uint64_t>(raw.memory), name);
			Block block;
			block.memory       = raw.memory;
			block.size         = alloc_size;
			block.type_index   = type_index;
			block.host_visible = host_visible;
			block.coherent =
			        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
			block.is_image = false;
			block.mapped   = raw.mapped;
			block.used     = alloc_size;
			block.ranges.push_back(Range{0, alloc_size, true});
			blocks_.push_back(block);
			const uint32_t index =
			        static_cast<uint32_t>(blocks_.size() - 1);
			bytes_in_use_ += alloc_size;
			buffer.allocation = Allocation{raw.memory, 0, alloc_size,
			                               type_index, index, host_visible,
			                               block.coherent};
		} else {
			buffer.allocation =
			        allocate(type_index, false, alloc_size, alignment);
			if (name != nullptr &&
			    blocks_[buffer.allocation.block].ranges.size() == 1) {
				// Newly created block (one live range): name it once.
				name_object(api, device, VK_OBJECT_TYPE_DEVICE_MEMORY,
				            reinterpret_cast<uint64_t>(
				                    buffer.allocation.memory),
				            name);
			}
		}
		check(api.BindBufferMemory(device, handle,
		                           buffer.allocation.memory,
		                           buffer.allocation.offset),
		      "vkBindBufferMemory");
		name_object(api, device, VK_OBJECT_TYPE_BUFFER,
		            reinterpret_cast<uint64_t>(handle), name);
		buffer.handle = handle;
		if (buffer.allocation.host_visible) {
			buffer.mapped = static_cast<char*>(
			                        blocks_[buffer.allocation.block].mapped) +
			                buffer.allocation.offset;
		}
		return buffer;
	} catch (...) {
		api.DestroyBuffer(device, handle, nullptr);
		throw;
	}
}

void Memory::destroy_buffer(Buffer& buffer)
{
	if (buffer.handle == VK_NULL_HANDLE) {
		return;
	}
	release(buffer.allocation);
	context_->api.DestroyBuffer(context_->device, buffer.handle, nullptr);
	buffer = Buffer{};
}

Image Memory::create_image(MemoryClass klass, VkImageUsageFlags usage,
                           VkFormat format, uint32_t width, uint32_t height,
                           uint32_t mip_levels, const char* name)
{
	(void)klass; // Images are always device-local, whatever was asked.
	const auto& api = context_->api;
	const auto device = context_->device;
	VkImageCreateInfo info{};
	info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType     = VK_IMAGE_TYPE_2D;
	info.format        = format;
	info.extent.width  = width;
	info.extent.height = height;
	info.extent.depth  = 1;
	info.mipLevels     = mip_levels;
	info.arrayLayers   = 1;
	info.samples       = VK_SAMPLE_COUNT_1_BIT;
	info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	info.usage         = usage;
	info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage handle{VK_NULL_HANDLE};
	check(api.CreateImage(device, &info, nullptr, &handle), "vkCreateImage");
	try {
		VkMemoryRequirements requirements{};
		api.GetImageMemoryRequirements(device, handle, &requirements);
		const uint64_t alloc_size = std::max<uint64_t>(1, requirements.size);
		const uint32_t type_index =
		        find_type(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
		                  requirements.memoryTypeBits);
		Image image;
		image.format     = format;
		image.width      = width;
		image.height     = height;
		image.mip_levels = mip_levels;
		if (alloc_size > kDeviceLocalBlockSize) {
			const auto flags = context_->memory.memoryTypes[type_index]
			                           .propertyFlags;
			const bool host_visible =
			        (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
			RawBlock raw = allocate_block_memory(api, device, type_index,
			                                     alloc_size, host_visible);
			name_object(api, device, VK_OBJECT_TYPE_DEVICE_MEMORY,
			            reinterpret_cast<uint64_t>(raw.memory), name);
			Block block;
			block.memory       = raw.memory;
			block.size         = alloc_size;
			block.type_index   = type_index;
			block.host_visible = host_visible;
			block.coherent =
			        (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
			block.is_image = true;
			block.mapped   = raw.mapped;
			block.used     = alloc_size;
			block.ranges.push_back(Range{0, alloc_size, true});
			blocks_.push_back(block);
			const uint32_t index =
			        static_cast<uint32_t>(blocks_.size() - 1);
			bytes_in_use_ += alloc_size;
			image.allocation = Allocation{raw.memory, 0, alloc_size,
			                              type_index, index, host_visible,
			                              block.coherent};
		} else {
			image.allocation = allocate(type_index, true, alloc_size,
			                            requirements.alignment);
			if (name != nullptr &&
			    blocks_[image.allocation.block].ranges.size() == 1) {
				name_object(api, device, VK_OBJECT_TYPE_DEVICE_MEMORY,
				            reinterpret_cast<uint64_t>(
				                    image.allocation.memory),
				            name);
			}
		}
		check(api.BindImageMemory(device, handle, image.allocation.memory,
		                          image.allocation.offset),
		      "vkBindImageMemory");
		name_object(api, device, VK_OBJECT_TYPE_IMAGE,
		            reinterpret_cast<uint64_t>(handle), name);
		VkImageViewCreateInfo view_info{};
		view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view_info.image    = handle;
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format   = format;
		view_info.subresourceRange.aspectMask     = aspect_for(format);
		view_info.subresourceRange.baseMipLevel   = 0;
		view_info.subresourceRange.levelCount     = mip_levels;
		view_info.subresourceRange.baseArrayLayer = 0;
		view_info.subresourceRange.layerCount     = 1;
		check(api.CreateImageView(device, &view_info, nullptr, &image.view),
		      "vkCreateImageView");
		name_object(api, device, VK_OBJECT_TYPE_IMAGE_VIEW,
		            reinterpret_cast<uint64_t>(image.view), name);
		image.handle = handle;
		return image;
	} catch (...) {
		api.DestroyImage(device, handle, nullptr);
		throw;
	}
}

void Memory::destroy_image(Image& image)
{
	if (image.handle == VK_NULL_HANDLE) {
		return;
	}
	const auto& api = context_->api;
	const auto device = context_->device;
	if (image.view != VK_NULL_HANDLE) {
		api.DestroyImageView(device, image.view, nullptr);
	}
	release(image.allocation);
	api.DestroyImage(device, image.handle, nullptr);
	image = Image{};
}

void Memory::flush(const Allocation& allocation, uint64_t offset,
                   uint64_t size) const
{
	if (allocation.coherent || size == 0 ||
	    allocation.memory == VK_NULL_HANDLE) {
		return;
	}
	const VkMappedMemoryRange range =
	        mapped_range(*context_, allocation, offset, size);
	check(context_->api.FlushMappedMemoryRanges(context_->device, 1, &range),
	      "vkFlushMappedMemoryRanges");
}

void Memory::invalidate(const Allocation& allocation, uint64_t offset,
                        uint64_t size) const
{
	if (allocation.coherent || size == 0 ||
	    allocation.memory == VK_NULL_HANDLE) {
		return;
	}
	const VkMappedMemoryRange range =
	        mapped_range(*context_, allocation, offset, size);
	check(context_->api.InvalidateMappedMemoryRanges(context_->device, 1,
	                                                 &range),
	      "vkInvalidateMappedMemoryRanges");
}

void Memory::shutdown()
{
	if (context_ == nullptr) {
		return;
	}
	if (context_->device != VK_NULL_HANDLE) {
		const auto& api = context_->api;
		for (Block& block : blocks_) {
			if (block.mapped != nullptr) {
				api.UnmapMemory(context_->device, block.memory);
				block.mapped = nullptr;
			}
			if (block.memory != VK_NULL_HANDLE) {
				api.FreeMemory(context_->device, block.memory,
				               nullptr);
				block.memory = VK_NULL_HANDLE;
			}
		}
	}
	blocks_.clear();
	bytes_in_use_ = 0;
}

} // namespace lotus::vk
