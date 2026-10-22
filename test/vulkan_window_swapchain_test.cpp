// Windowed-path tests for the Vulkan backend: swapchain acquire, scene
// rendering in the swapchain format, present, capture and validation silence.
//
// Backend-agnostic: on other backends they skip. The surface comes from SDL's
// offscreen video driver, which creates a VK_EXT_headless_surface one: Intel's
// driver presents to it. NVIDIA does not, so init throws "no suitable Vulkan
// device" there, and lavapipe cannot either (see SetUp). Both skip cleanly;
// their swapchains are covered by running the windowed application.

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

/// A windowed Vulkan session over a hidden window on SDL's offscreen video
/// driver, which needs no display (that is what lets this run in a
/// container). Skips when no device can present to such a surface.
class VulkanWindowSwapchain : public ::testing::Test {
protected:
	void SetUp() override
	{
		// These tests' point is a silent validation layer, so they ask for
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

		gpu_ = create_gpu_renderer();
		if (!test_is_vulkan_backend(*gpu_)) {
			GTEST_SKIP() << "not the Vulkan backend";
		}
		window_ = SDL_CreateWindow("lotus-vk-window-test", 320, 240,
		                           SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
		ASSERT_NE(window_, nullptr) << SDL_GetError();
		try {
			gpu_->init(window_);
		} catch (const std::runtime_error& e) {
			GTEST_SKIP() << "no present-capable device: " << e.what();
		}
		// Mesa's headless WSI always builds dma-buf (DRM) images, while its
		// common code leaves vkGetMemoryFdKHR unloaded on software devices
		// (wsi_common_headless.c / wsi_common.c, Mesa 26.0.3): creating a
		// headless swapchain on lavapipe calls a null pointer inside the
		// driver. Real surfaces (Wayland, X11) take the software path
		// instead, so lavapipe's swapchain is covered by running the
		// windowed application.
		if (std::string_view{gpu_->device_driver()} == "llvmpipe") {
			GTEST_SKIP() << "Mesa headless WSI cannot create a swapchain on "
			                "software devices";
		}
	}

	void TearDown() override
	{
		// The renderer goes first: it owns the surface made from the window.
		if (gpu_) {
			gpu_->shutdown();
			gpu_.reset();
		}
		if (window_ != nullptr) {
			SDL_DestroyWindow(window_);
		}
		if (owned_sdl_) {
			SDL_Quit();
		}
		if (set_validation_env_) {
			unsetenv("LOTUS_VK_VALIDATION");
		}
	}

	GpuRenderer& gpu() { return *gpu_; }

	std::unique_ptr<GpuRenderer> gpu_;
	SDL_Window* window_{nullptr};
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
	GpuRenderer& gpu = this->gpu();
	// The UI backend over the (still unacquired) swapchain: created now,
	// its pipeline follows once the first frame lands the format. Like the
	// editor, the caller owns the ImGui context the backend attaches to.
	ImGui::CreateContext();
	EXPECT_TRUE(gpu.init_ui_backend());
	Scene scene            = make_window_scene();
	const uint64_t before  = gpu.frames_rendered();
	constexpr int kFrames  = 5;
	for (int frame = 0; frame < kFrames; ++frame) {
		render_window_frame(gpu, window_, scene, 320.0f / 240.0f);
	}
	// A resize rebuilds the swapchain; frames keep presenting after.
	SDL_SetWindowSize(window_, 160, 120);
	for (int frame = 0; frame < 3; ++frame) {
		render_window_frame(gpu, window_, scene, 160.0f / 120.0f);
	}
	gpu.wait_idle();
	EXPECT_GE(gpu.frames_rendered(), before + kFrames + 3);
	EXPECT_EQ(test_vulkan_validation_messages(gpu), 0u)
	        << "validation layer reported errors";
	gpu.shutdown_ui_backend();
	ImGui::DestroyContext();
}

/// A windowed session renders in the swapchain's format, which is B8G8R8A8 on
/// the drivers that can present here, yet the viewport capture promises RGBA8.
/// A clear colour with three different channels, each exact in UNORM8, must
/// come back in R, G, B order whatever the format.
TEST_F(VulkanWindowSwapchain, ViewportCaptureIsRgbaInTheSwapchainFormat)
{
	GpuRenderer& gpu = this->gpu();
	// Nothing but the clear colour reaches the pixels: no geometry, no FXAA.
	gpu.lighting().fxaa_enabled = false;
	gpu.begin_frame(window_, 0.8f, 0.4f, 0.2f);
	ASSERT_TRUE(gpu.has_frame());
	// After the first acquire, which is when the session learns the swapchain
	// format: the viewport is created in it.
	ASSERT_TRUE(gpu.resize_viewport(16, 16));
	ASSERT_TRUE(gpu.begin_viewport_pass());
	gpu.end_viewport_pass();
	gpu.end_frame();

	uint32_t width  = 0;
	uint32_t height = 0;
	const std::vector<uint8_t> pixels = gpu.capture_viewport_rgba(width, height);
	ASSERT_EQ(width, 16u);
	ASSERT_EQ(height, 16u);
	ASSERT_EQ(pixels.size(), 16u * 16u * 4u);
	for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
		ASSERT_EQ(pixels[offset + 0], 204) << "red of pixel " << offset / 4;
		ASSERT_EQ(pixels[offset + 1], 102) << "green of pixel " << offset / 4;
		ASSERT_EQ(pixels[offset + 2], 51) << "blue of pixel " << offset / 4;
		ASSERT_EQ(pixels[offset + 3], 255) << "alpha of pixel " << offset / 4;
	}
	EXPECT_EQ(test_vulkan_validation_messages(gpu), 0u)
	        << "validation layer reported errors";
}
