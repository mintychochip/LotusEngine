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

/// One offscreen context and allocator per test.
class VkBarrierTest : public ::testing::Test {
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

TEST_F(VkBarrierTest, UnchangedStateQueuesNothing)
{
	vk::Image image = memory_.create_image(
	        vk::MemoryClass::DeviceLocal,
	        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	        VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, "barrier-image");
	vk::BarrierBatch batch(context_.api);
	vk::ImageState state{};
	vk::image_shader_read(batch, nullptr, image, 0, 1, state);
	vk::image_shader_read(batch, nullptr, image, 0, 1, state);
	EXPECT_EQ(batch.pending(), 1u);

	context_.submit_and_wait(
	        [&](VkCommandBuffer cmd) { batch.flush(cmd); });
	EXPECT_EQ(batch.pending(), 0u);
	EXPECT_EQ(state.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	memory_.destroy_image(image);
}

TEST_F(VkBarrierTest, LayoutChangeChains)
{
	vk::Image image = memory_.create_image(
	        vk::MemoryClass::DeviceLocal,
	        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	        VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, "chain-image");
	vk::BarrierBatch batch(context_.api);
	vk::ImageState state{};
	// The second call's source is the first call's destination: the redundant
	// check compares against the updated state, so both transitions queue.
	vk::image_as_color_attachment(batch, nullptr, image, 0, 1, state);
	EXPECT_EQ(state.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vk::image_shader_read(batch, nullptr, image, 0, 1, state);
	EXPECT_EQ(batch.pending(), 2u);
	EXPECT_EQ(state.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	context_.submit_and_wait(
	        [&](VkCommandBuffer cmd) { batch.flush(cmd); });
	EXPECT_EQ(batch.pending(), 0u);

	memory_.destroy_image(image);
}

TEST_F(VkBarrierTest, SubmittedBarriersAreValidationClean)
{
	constexpr uint32_t kWidth  = 8;
	constexpr uint32_t kHeight = 8;
	constexpr uint64_t kBytes  = kWidth * kHeight * 4;
	vk::Image image = memory_.create_image(
	        vk::MemoryClass::DeviceLocal,
	        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	        VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, "validated-image");
	vk::Buffer staging = memory_.create_buffer(
	        vk::MemoryClass::Staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kBytes,
	        "validated-staging");
	ASSERT_NE(staging.mapped, nullptr);

	std::vector<uint8_t> pattern(static_cast<std::size_t>(kBytes));
	for (std::size_t i = 0; i < pattern.size(); ++i) {
		pattern[i] = static_cast<uint8_t>(i * 31 + 7);
	}
	std::memcpy(staging.mapped, pattern.data(), pattern.size());
	memory_.flush(staging.allocation, 0, kBytes);

	vk::BarrierBatch batch(context_.api);
	vk::ImageState image_state{};
	vk::BufferState staging_state{};
	context_.submit_and_wait([&](VkCommandBuffer cmd) {
		vk::image_transfer_dst(batch, cmd, image, 0, 1, image_state);
		vk::buffer_transfer_src(batch, cmd, staging, staging_state);
		batch.flush(cmd);
		VkBufferImageCopy copy{};
		copy.bufferOffset      = 0;
		copy.bufferRowLength   = 0;
		copy.bufferImageHeight = 0;
		copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel       = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount     = 1;
		copy.imageOffset.x = 0;
		copy.imageOffset.y = 0;
		copy.imageOffset.z = 0;
		copy.imageExtent.width  = kWidth;
		copy.imageExtent.height = kHeight;
		copy.imageExtent.depth  = 1;
		context_.api.CmdCopyBufferToImage(cmd, staging.handle, image.handle,
		                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                                  1, &copy);
		vk::image_shader_read(batch, cmd, image, 0, 1, image_state);
		batch.flush(cmd);
	});

	EXPECT_EQ(context_.validation_messages(), 0u);
	memory_.destroy_buffer(staging);
	memory_.destroy_image(image);
}

} // namespace
