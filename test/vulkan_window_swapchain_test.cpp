// Windowed-path test for the Vulkan backend: swapchain acquire, scene
// rendering in the swapchain format, present and validation silence.
//
// Backend-agnostic: on other backends it skips. The surface comes from SDL's
// offscreen video driver, which creates a VK_EXT_headless_surface one: the
// lavapipe and Intel drivers present to it, NVIDIA does not, so init throws
// "no suitable Vulkan device" there and the test skips cleanly.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include <imgui.h>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

namespace lotus {

/// True when \p gpu is the Vulkan backend. Declared here, defined weakly by
/// the smoke test (fallback) and strongly by the Vulkan backend.
bool test_is_vulkan_backend(const GpuRenderer& gpu);

/// Validation-layer messages reported so far. Same linkage as above.
uint32_t test_vulkan_validation_messages(const GpuRenderer& gpu);

} // namespace lotus

namespace {

/// SDL video for a test that owns a window; the offscreen driver needs no
/// display, which is what lets this run in a container.
class VulkanWindowSwapchain : public ::testing::Test {
protected:
	void SetUp() override
	{
		// This test's point is a silent validation layer, so it asks for
		// it, synchronization validation included (the acquire/present
		// hazards only show there), unless the caller already chose a mode.
		// Honoured only where the layer is installed; undone in TearDown so
		// later tests keep their own environment.
		if (std::getenv("LOTUS_VK_VALIDATION") == nullptr) {
			setenv("LOTUS_VK_VALIDATION", "sync", 1);
			set_validation_env_ = true;
		}
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
		owned_sdl_ = SDL_Init(SDL_INIT_VIDEO);
		ASSERT_TRUE(owned_sdl_) << SDL_GetError();
	}

	void TearDown() override
	{
		if (owned_sdl_) {
			SDL_Quit();
		}
		if (set_validation_env_) {
			unsetenv("LOTUS_VK_VALIDATION");
		}
	}

	bool owned_sdl_{false};
	bool set_validation_env_{false};
};

/// A camera looking at a cube over a ground plane: enough geometry to force
/// the lit pipeline (built for the swapchain format) to record.
Scene make_window_scene()
{
	Scene scene{"window"};
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

/// One windowed frame through the full path, skipped frames included.
void render_window_frame(GpuRenderer& gpu, SDL_Window* window, Scene& scene,
                         float aspect)
{
	gpu.begin_frame(window, 0.05f, 0.05f, 0.08f);
	if (!gpu.has_frame()) {
		return;
	}
	scene.upload(gpu, aspect);
	gpu.begin_scene_pass();
	scene.draw(gpu);
	gpu.end_scene_pass();
	gpu.end_frame();
}

} // namespace

/// Frames render through the swapchain path and present, a resize rebuilds,
/// and validation stays silent throughout.
TEST_F(VulkanWindowSwapchain, AcquireRenderPresentStaysValidationClean)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	if (!test_is_vulkan_backend(gpu)) {
		GTEST_SKIP() << "not the Vulkan backend";
	}
	SDL_Window* window = SDL_CreateWindow("lotus-vk-window-test", 320, 240,
	                                      SDL_WINDOW_VULKAN |
	                                              SDL_WINDOW_HIDDEN);
	ASSERT_NE(window, nullptr) << SDL_GetError();
	try {
		gpu.init(window);
	} catch (const std::runtime_error& e) {
		SDL_DestroyWindow(window);
		GTEST_SKIP() << "no present-capable device: " << e.what();
	}
	// Mesa's headless WSI always builds dma-buf (DRM) images, while its
	// common code leaves vkGetMemoryFdKHR unloaded on software devices
	// (wsi_common_headless.c / wsi_common.c, Mesa 26.0.3): creating a
	// headless swapchain on lavapipe calls a null pointer inside the driver.
	// Real surfaces (Wayland, X11) take the software path instead, so
	// lavapipe's swapchain is covered by running the windowed application.
	if (std::string_view{gpu.device_driver()} == "llvmpipe") {
		gpu.shutdown();
		SDL_DestroyWindow(window);
		GTEST_SKIP() << "Mesa headless WSI cannot create a swapchain on "
		                "software devices";
	}
	// The UI backend over the (still unacquired) swapchain: created now,
	// its pipeline follows once the first frame lands the format. Like the
	// editor, the caller owns the ImGui context the backend attaches to.
	ImGui::CreateContext();
	EXPECT_TRUE(gpu.init_ui_backend());
	Scene scene            = make_window_scene();
	const uint64_t before  = gpu.frames_rendered();
	constexpr int kFrames  = 5;
	for (int frame = 0; frame < kFrames; ++frame) {
		render_window_frame(gpu, window, scene, 320.0f / 240.0f);
	}
	// A resize rebuilds the swapchain; frames keep presenting after.
	SDL_SetWindowSize(window, 160, 120);
	for (int frame = 0; frame < 3; ++frame) {
		render_window_frame(gpu, window, scene, 160.0f / 120.0f);
	}
	gpu.wait_idle();
	EXPECT_GE(gpu.frames_rendered(), before + kFrames + 3);
	EXPECT_EQ(test_vulkan_validation_messages(gpu), 0u)
	        << "validation layer reported errors";
	gpu.shutdown_ui_backend();
	ImGui::DestroyContext();
	gpu.shutdown();
	SDL_DestroyWindow(window);
}
