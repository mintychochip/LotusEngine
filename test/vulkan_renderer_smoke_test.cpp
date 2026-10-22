// Smoke test for the Vulkan backend's offscreen path: core, geometry, textures
// and the shadow/lit/sky passes.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"
#include "vulkan_device.hpp"

using namespace lotus;

namespace lotus {

/// Validation-layer messages the renderer's context has counted so far; defined
/// by the Vulkan backend as a test hook.
uint32_t test_vulkan_validation_messages(const GpuRenderer& gpu);

/// Content-keyed descriptor sets (sampler, storage) the renderer has allocated
/// from its cache pool and not yet returned to it; also a test hook.
uint32_t test_vulkan_keyed_descriptor_sets(const GpuRenderer& gpu);

} // namespace lotus

namespace {

/// Skips (or fails, under LOTUS_REQUIRE_VULKAN) when the machine has no Vulkan
/// device; the offscreen renderer needs no window system.
class VulkanRendererSmoke : public ::testing::Test {
protected:
	void SetUp() override
	{
		LOTUS_SKIP_WITHOUT_VULKAN_DEVICE();
	}
};

constexpr uint32_t kWidth  = 192;
constexpr uint32_t kHeight = 128;

/// A camera at eye height looking at the horizon, over a wide ground plane
/// with a cube a few units ahead: background above, geometry below.
Scene make_scene()
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera,
	                                   Camera3D{{0.0f, 1.5f, 0.0f},
	                                            {0.0f, 1.5f, -10.0f},
	                                            {0.0f, 1.0f, 0.0f},
	                                            50.0f,
	                                            0.1f,
	                                            40.0f});
	const Entity ground = scene.create("ground");
	scene.registry().emplace<MeshSource>(ground,
	                                     MeshSource{"builtin:plane", ""});
	scene.registry().get<Transform>(ground).scale = {40.0f, 1.0f, 40.0f};
	scene.registry().emplace<Material>(
	        ground, Material{{0.55f, 0.55f, 0.57f}, "", 0.9f});
	const Entity cube = scene.create("cube");
	scene.registry().emplace<MeshSource>(cube,
	                                     MeshSource{"builtin:cube", ""});
	scene.registry().get<Transform>(cube).position = {0.0f, 1.5f, -3.0f};
	scene.registry().emplace<Material>(
	        cube, Material{{0.78f, 0.32f, 0.22f}, "", 0.5f});
	scene.update(0.0f);
	return scene;
}

void configure(GpuRenderer& gpu, bool sky)
{
	LightingSettings& lighting = gpu.lighting();
	lighting.sky_enabled       = sky;
	lighting.fxaa_enabled      = false;
	lighting.ssao_enabled      = false;
	lighting.sky_color         = {0.32f, 0.45f, 0.68f};
}

/// Three frames, the last returned as RGBA: history-free enough for a smoke
/// assertion, the same shape the GPU tests use.
std::vector<uint8_t> render_scene_pass(GpuRenderer& gpu, Scene& scene, bool sky)
{
	configure(gpu, sky);
	for (int frame = 0; frame < 3; ++frame) {
		gpu.begin_frame_offscreen();
		scene.upload(gpu, static_cast<float>(kWidth) / kHeight);
		gpu.begin_scene_pass();
		scene.draw(gpu);
		gpu.end_scene_pass();
		gpu.end_frame();
		gpu.wait_idle();
	}
	uint32_t width  = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels = gpu.capture_rgba(width, height);
	EXPECT_EQ(width, kWidth);
	EXPECT_EQ(height, kHeight);
	return pixels;
}

} // namespace

/// Offscreen frames render: non-empty pixels, geometry that differs from the
/// clear colour, the sky only where nothing was drawn, and zero validation
/// messages.
TEST_F(VulkanRendererSmoke, OffscreenFrameRendersGeometryAndSky)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	ASSERT_NO_THROW(gpu.init_offscreen(kWidth, kHeight));
	EXPECT_NE(std::string(gpu.device_name()), "");
	EXPECT_NE(std::string(gpu.driver_name()), "");
	EXPECT_GT(gpu.vertex_capacity(), 0u);
	EXPECT_TRUE(gpu.occlusion_enabled());
	EXPECT_FALSE(gpu.occlusion_ready());
	EXPECT_FALSE(gpu.begin_viewport_pass());
	Scene scene                  = make_scene();
	const uint64_t before        = gpu.frames_rendered();
	const std::vector<uint8_t> off = render_scene_pass(gpu, scene, false);
	const std::vector<uint8_t> on  = render_scene_pass(gpu, scene, true);
	EXPECT_GT(gpu.frames_rendered(), before);

	const std::size_t pixels = static_cast<std::size_t>(kWidth) * kHeight;
	ASSERT_EQ(off.size(), pixels * 4);
	ASSERT_EQ(on.size(), off.size());

	// Geometry is whatever differs from the clear colour (pixel 0, top-left
	// background); the sky-off frame must contain some, and the sky must
	// leave every one of those pixels byte-identical.
	std::size_t geometry    = 0;
	std::size_t overpainted = 0;
	std::size_t skied       = 0;
	for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
		const bool is_geometry =
		        !(off[pixel * 4] == off[0] && off[pixel * 4 + 1] == off[1] &&
		          off[pixel * 4 + 2] == off[2]);
		bool same = true;
		for (std::size_t channel = 0; channel < 4; ++channel) {
			same &= on[pixel * 4 + channel] == off[pixel * 4 + channel];
		}
		geometry += is_geometry ? 1u : 0u;
		overpainted += (is_geometry && !same) ? 1u : 0u;
		skied += (!is_geometry && !same) ? 1u : 0u;
	}
	EXPECT_GT(geometry, pixels * 10 / 100);
	EXPECT_EQ(overpainted, 0u);
	EXPECT_GT(skied, 0u);
	EXPECT_EQ(test_vulkan_validation_messages(gpu), 0u)
	        << "validation layer reported errors";
	EXPECT_GT(gpu.cached_mesh_count(), 0u);
}

/// Resize keeps rendering at the new size.
TEST_F(VulkanRendererSmoke, ResizeOffscreenKeepsRendering)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	ASSERT_NO_THROW(gpu.init_offscreen(kWidth, kHeight));
	EXPECT_TRUE(gpu.resize_offscreen(kWidth / 2, kHeight / 2));
	Scene scene = make_scene();
	gpu.begin_frame_offscreen();
	scene.upload(gpu, static_cast<float>(kWidth) / kHeight);
	gpu.begin_scene_pass();
	scene.draw(gpu);
	gpu.end_scene_pass();
	gpu.end_frame();
	gpu.wait_idle();
	uint32_t width  = 0;
	uint32_t height = 0;
	const std::vector<uint8_t> pixels = gpu.capture_rgba(width, height);
	EXPECT_EQ(width, kWidth / 2);
	EXPECT_EQ(height, kHeight / 2);
	EXPECT_EQ(pixels.size(), static_cast<std::size_t>(width) * height * 4);
	EXPECT_EQ(test_vulkan_validation_messages(gpu), 0u);
}

/// The editor resizes its viewport on every pixel of a drag, and each resize
/// replaces the images the cached descriptor sets name. Those sets have to go
/// back to the pool: a few of them leaked per resize used to exhaust it after
/// about 700 resizes (2048 sets) and abort the next frame on NVIDIA and Intel.
/// Lavapipe never refuses, so the count is checked as well.
TEST_F(VulkanRendererSmoke, ViewportResizesReturnDescriptorSetsToThePool)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	ASSERT_NO_THROW(gpu.init_offscreen(kWidth, kHeight));
	configure(gpu, true);
	Scene scene = make_scene();

	constexpr uint32_t kResizes     = 1200;
	constexpr uint32_t kViewportMin = 96;
	constexpr uint32_t kViewportHeight = 64;
	// A few sets per generation, and the frames in flight keep the last few
	// generations alive: far below the pool, far above a leak's growth.
	constexpr uint32_t kBound = 64;
	uint32_t peak = 0;
	for (uint32_t i = 0; i < kResizes; ++i) {
		const uint32_t width = kViewportMin + i % 61;
		ASSERT_TRUE(gpu.resize_viewport(width, kViewportHeight));
		gpu.begin_frame_offscreen();
		scene.upload(gpu, static_cast<float>(width) / kViewportHeight);
		ASSERT_TRUE(gpu.begin_viewport_pass());
		scene.draw(gpu);
		gpu.end_viewport_pass();
		gpu.end_frame();
		peak = std::max(peak, test_vulkan_keyed_descriptor_sets(gpu));
	}
	gpu.wait_idle();
	EXPECT_LE(peak, kBound);
	EXPECT_EQ(test_vulkan_validation_messages(gpu), 0u);
}
