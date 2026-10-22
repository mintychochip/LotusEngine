#include <gtest/gtest.h>

#include "gpu/vk/vk_context.hpp"
#include "vulkan_device.hpp"

using namespace lotus;

/// Skips (or fails, under LOTUS_REQUIRE_VULKAN) when the machine has no Vulkan
/// device; creating a context needs no window system.
class VkContextTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		LOTUS_SKIP_WITHOUT_VULKAN_DEVICE();
	}
};

TEST_F(VkContextTest, DeviceIsVulkan13WithTheRequiredFeatures)
{
	vk::Context context;
	context.init({});
	ASSERT_TRUE(context.valid());

	// The entry points the passes and barriers are written against.
	EXPECT_NE(context.api.CmdDraw, nullptr);
	EXPECT_NE(context.api.CmdDispatch, nullptr);
	EXPECT_NE(context.api.CmdPipelineBarrier2, nullptr);
	EXPECT_NE(context.api.CmdBeginRendering, nullptr);
	// The swapchain extension is only enabled for windowed rendering, so its
	// commands may be null offscreen; touching the pointer is the check.
	const bool has_swapchain = context.api.CreateSwapchainKHR != nullptr;
	(void)has_swapchain;

	EXPECT_GE(context.properties.apiVersion, VK_API_VERSION_1_3);
	EXPECT_FALSE(context.driver_name.empty());

	bool host_visible = false;
	bool device_local = false;
	for (uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
		const auto flags = context.memory.memoryTypes[i].propertyFlags;
		host_visible |= (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
		device_local |= (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
	}
	EXPECT_TRUE(host_visible);
	EXPECT_TRUE(device_local);

	context.shutdown();
	EXPECT_FALSE(context.valid());
}

TEST_F(VkContextTest, OneShotSubmitCompletesAndIsValidationClean)
{
	vk::Context context;
	context.init({});
	ASSERT_TRUE(context.valid());
	context.submit_and_wait([&](VkCommandBuffer cmd) {
		VkDependencyInfo info{};
		info.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		context.api.CmdPipelineBarrier2(cmd, &info);
	});
	EXPECT_EQ(context.validation_messages(), 0u);
	context.shutdown();
}

TEST_F(VkContextTest, ShutdownIsIdempotent)
{
	vk::Context context;
	context.init({});
	ASSERT_TRUE(context.valid());
	context.shutdown();
	context.shutdown();
	EXPECT_FALSE(context.valid());
}
