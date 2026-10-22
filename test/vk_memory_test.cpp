#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/vk/vk_barrier.hpp"
#include "gpu/vk/vk_context.hpp"
#include "gpu/vk/vk_memory.hpp"
#include "vulkan_device.hpp"

using namespace lotus;

namespace {

/// One offscreen context and allocator per test; the submit helper below is
/// the only command-buffer source.
class VkMemoryTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		LOTUS_SKIP_WITHOUT_VULKAN_DEVICE();
		context_.init({});
		ASSERT_TRUE(context_.valid());
		memory_.init(context_);
	}

	void TearDown() override
	{
		memory_.shutdown();
		EXPECT_EQ(context_.validation_messages(), 0u);
		context_.shutdown();
	}

	vk::Context context_;
	vk::Memory memory_;
};

constexpr uint64_t kPage = 4096;

/// staging pattern -> device-local -> readback, compared byte for byte.
TEST_F(VkMemoryTest, DeviceLocalBufferRoundTrip)
{
	vk::Buffer staging = memory_.create_buffer(
	        vk::MemoryClass::Staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kPage,
	        "roundtrip-staging");
	vk::Buffer device = memory_.create_buffer(
	        vk::MemoryClass::DeviceLocal,
	        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	        kPage, "roundtrip-device");
	vk::Buffer readback = memory_.create_buffer(
	        vk::MemoryClass::Readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kPage,
	        "roundtrip-readback");
	ASSERT_NE(staging.mapped, nullptr);
	// On UMA (lavapipe, Intel iGPU) the device-local type itself is
	// host-visible, so mapped follows the memory type, not the class.
	EXPECT_EQ(device.mapped != nullptr, device.allocation.host_visible);
	ASSERT_NE(readback.mapped, nullptr);

	std::vector<uint8_t> pattern(kPage);
	for (uint64_t i = 0; i < kPage; ++i) {
		pattern[static_cast<std::size_t>(i)] =
		        static_cast<uint8_t>((i * 2654435761u) >> 13);
	}
	std::memcpy(staging.mapped, pattern.data(), pattern.size());
	memory_.flush(staging.allocation, 0, kPage);

	vk::BarrierBatch batch(context_.api);
	vk::BufferState staging_state{};
	vk::BufferState device_state{};
	vk::BufferState readback_state{};
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		vk::buffer_transfer_src(batch, cmd, staging, staging_state);
		vk::buffer_transfer_dst(batch, cmd, device, device_state);
		batch.flush(cmd);
		VkBufferCopy first{};
		first.srcOffset = 0;
		first.dstOffset = 0;
		first.size      = kPage;
		context_.api.CmdCopyBuffer(cmd, staging.handle, device.handle, 1,
		                           &first);
		vk::buffer_transfer_src(batch, cmd, device, device_state);
		vk::buffer_transfer_dst(batch, cmd, readback, readback_state);
		batch.flush(cmd);
		VkBufferCopy second{};
		second.srcOffset = 0;
		second.dstOffset = 0;
		second.size      = kPage;
		context_.api.CmdCopyBuffer(cmd, device.handle, readback.handle, 1,
		                           &second);
	});

	memory_.invalidate(readback.allocation, 0, kPage);
	EXPECT_EQ(std::memcmp(readback.mapped, pattern.data(), pattern.size()), 0);

	memory_.destroy_buffer(staging);
	memory_.destroy_buffer(device);
	memory_.destroy_buffer(readback);
}

/// A dynamic buffer is written straight by the CPU and read by the GPU, with
/// no staging copy in between.
TEST_F(VkMemoryTest, DynamicBufferIsWritableAndVisibleToTheGpu)
{
	vk::Buffer dynamic = memory_.create_buffer(
	        vk::MemoryClass::Dynamic, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kPage,
	        "dynamic");
	vk::Buffer readback = memory_.create_buffer(
	        vk::MemoryClass::Readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kPage,
	        "dynamic-readback");
	ASSERT_NE(dynamic.mapped, nullptr);
	ASSERT_NE(readback.mapped, nullptr);

	std::vector<uint8_t> pattern(kPage);
	for (uint64_t i = 0; i < kPage; ++i) {
		pattern[static_cast<std::size_t>(i)] =
		        static_cast<uint8_t>(i ^ (i >> 4));
	}
	std::memcpy(dynamic.mapped, pattern.data(), pattern.size());
	memory_.flush(dynamic.allocation, 0, kPage);

	vk::BarrierBatch batch(context_.api);
	vk::BufferState dynamic_state{};
	vk::BufferState readback_state{};
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		vk::buffer_transfer_src(batch, cmd, dynamic, dynamic_state);
		vk::buffer_transfer_dst(batch, cmd, readback, readback_state);
		batch.flush(cmd);
		VkBufferCopy copy{};
		copy.srcOffset = 0;
		copy.dstOffset = 0;
		copy.size      = kPage;
		context_.api.CmdCopyBuffer(cmd, dynamic.handle, readback.handle, 1,
		                           &copy);
	});

	memory_.invalidate(readback.allocation, 0, kPage);
	EXPECT_EQ(std::memcmp(readback.mapped, pattern.data(), pattern.size()), 0);

	memory_.destroy_buffer(dynamic);
	memory_.destroy_buffer(readback);
}

/// 200 small buffers share blocks, free them fully, and reuse them.
TEST_F(VkMemoryTest, SmallAllocationsShareBlocksAndAreReused)
{
	const uint64_t bytes_before  = memory_.bytes_in_use();
	const uint32_t blocks_before = memory_.block_count();

	std::vector<vk::Buffer> buffers;
	for (int i = 0; i < 200; ++i) {
		buffers.push_back(memory_.create_buffer(
		        vk::MemoryClass::DeviceLocal,
		        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1024, "small"));
	}
	EXPECT_LE(memory_.block_count() - blocks_before, 2u);

	for (vk::Buffer& buffer : buffers) {
		memory_.destroy_buffer(buffer);
	}
	buffers.clear();
	EXPECT_EQ(memory_.bytes_in_use(), bytes_before);

	const uint32_t blocks_after_free = memory_.block_count();
	for (int i = 0; i < 200; ++i) {
		buffers.push_back(memory_.create_buffer(
		        vk::MemoryClass::DeviceLocal,
		        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1024, "small"));
	}
	EXPECT_EQ(memory_.block_count(), blocks_after_free);
	for (vk::Buffer& buffer : buffers) {
		memory_.destroy_buffer(buffer);
	}
	EXPECT_EQ(memory_.bytes_in_use(), bytes_before);
}

/// Float pattern uploaded through staging, downloaded, compared exactly.
TEST_F(VkMemoryTest, ImageRoundTrip)
{
	constexpr uint32_t kWidth  = 8;
	constexpr uint32_t kHeight = 4;
	constexpr uint64_t kBytes  = kWidth * kHeight * sizeof(float);
	vk::Image image = memory_.create_image(
	        vk::MemoryClass::DeviceLocal,
	        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                VK_IMAGE_USAGE_SAMPLED_BIT |
	                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	        VK_FORMAT_R32_SFLOAT, kWidth, kHeight, 1, "roundtrip-image");
	vk::Buffer staging = memory_.create_buffer(
	        vk::MemoryClass::Staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kBytes,
	        "image-staging");
	vk::Buffer readback = memory_.create_buffer(
	        vk::MemoryClass::Readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kBytes,
	        "image-readback");
	ASSERT_NE(staging.mapped, nullptr);
	ASSERT_NE(readback.mapped, nullptr);

	std::vector<float> pattern(kWidth * kHeight);
	for (std::size_t i = 0; i < pattern.size(); ++i) {
		pattern[i] = static_cast<float>(i) * 0.5f + 1.0f;
	}
	std::memcpy(staging.mapped, pattern.data(), kBytes);
	memory_.flush(staging.allocation, 0, kBytes);

	vk::BarrierBatch batch(context_.api);
	vk::ImageState image_state{};
	vk::BufferState staging_state{};
	vk::BufferState readback_state{};
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		vk::image_transfer_dst(batch, cmd, image, 0, 1, image_state);
		vk::buffer_transfer_src(batch, cmd, staging, staging_state);
		batch.flush(cmd);
		VkBufferImageCopy upload{};
		upload.bufferOffset      = 0;
		upload.bufferRowLength   = 0;
		upload.bufferImageHeight = 0;
		upload.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		upload.imageSubresource.mipLevel       = 0;
		upload.imageSubresource.baseArrayLayer = 0;
		upload.imageSubresource.layerCount     = 1;
		upload.imageOffset.x = 0;
		upload.imageOffset.y = 0;
		upload.imageOffset.z = 0;
		upload.imageExtent.width  = kWidth;
		upload.imageExtent.height = kHeight;
		upload.imageExtent.depth  = 1;
		context_.api.CmdCopyBufferToImage(cmd, staging.handle, image.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                                  1, &upload);
		vk::image_transfer_src(batch, cmd, image, 0, 1, image_state);
		vk::buffer_transfer_dst(batch, cmd, readback, readback_state);
		batch.flush(cmd);
		VkBufferImageCopy download = upload;
		context_.api.CmdCopyImageToBuffer(cmd, image.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                                  readback.handle, 1, &download);
	});

	memory_.invalidate(readback.allocation, 0, kBytes);
	EXPECT_EQ(std::memcmp(readback.mapped, pattern.data(),
	                      static_cast<std::size_t>(kBytes)),
	          0);

	memory_.destroy_buffer(staging);
	memory_.destroy_buffer(readback);
	memory_.destroy_image(image);
}

TEST_F(VkMemoryTest, EverythingReleasedLeavesNoBytes)
{
	vk::Buffer buffer = memory_.create_buffer(
	        vk::MemoryClass::DeviceLocal, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	        1024, "leftover-buffer");
	vk::Image image = memory_.create_image(
	        vk::MemoryClass::DeviceLocal,
	        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	        VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, "leftover-image");
	memory_.destroy_buffer(buffer);
	memory_.destroy_image(image);
	memory_.shutdown();
	EXPECT_EQ(memory_.bytes_in_use(), 0u);
	EXPECT_EQ(memory_.block_count(), 0u);
}

} // namespace
