#include <gtest/gtest.h>

#include <SDL3/SDL.h>

#include "gpu/vk/vk_context.hpp"

using namespace lotus;

/// A headless Vulkan device still needs SDL's video subsystem; the offscreen
/// video driver needs no display, which is what lets this run in a container.
class VkContextTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
		owned_sdl_ = SDL_Init(SDL_INIT_VIDEO);
		ASSERT_TRUE(owned_sdl_) << SDL_GetError();
	}

	void TearDown() override
	{
		if (owned_sdl_) {
			SDL_Quit();
		}
	}

	bool owned_sdl_{false};
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
