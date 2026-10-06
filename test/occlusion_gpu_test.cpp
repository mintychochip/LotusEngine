#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <SDL3/SDL.h>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/depth_pyramid.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

namespace {
/// A headless Vulkan device still needs SDL's video subsystem: there is no
/// window, but SDL_CreateGPUDevice refuses to run uninitialised. The offscreen
/// video driver needs no display, which is what lets this run in a container.
class OcclusionGpuTest : public ::testing::Test {
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


/// A wall straight ahead and a cube behind it, seen by a headless camera at
/// +Z. Exercises the whole GPU path end to end: MRT linear depth out of the
/// lit pass, the compute reduction into the Hi-Z pyramid, the transfer
/// readback, and the CPU query in Scene::build_vertices.
///
/// `bin/lotus` cannot run in a headless container (no window, so no
/// presentation), which is why this drives init_offscreen instead.
TEST_F(OcclusionGpuTest, PyramidReachesTheCpuWithoutBlocking)
{
	GpuRenderer gpu;
	ASSERT_NO_THROW(gpu.init_offscreen(256, 256));
	ASSERT_TRUE(gpu.occlusion_enabled()) << "compute pipeline unavailable";

	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera, Camera3D{{0.0f, 0.0f, 10.0f},
	                                                   {0.0f, 0.0f, 0.0f},
	                                                   {0.0f, 1.0f, 0.0f},
	                                                   60.0f, 0.1f, 100.0f});

	// A wall across the view, and a cube behind it.
	const Entity wall = scene.create("wall");
	scene.registry().emplace<MeshSource>(wall, MeshSource{"builtin:cube"});
	scene.registry().get<Transform>(wall).scale    = {40.0f, 40.0f, 0.25f};
	scene.registry().get<Transform>(wall).position = {0.0f, 0.0f, 5.0f};

	const Entity hidden = scene.create("hidden");
	scene.registry().emplace<MeshSource>(hidden, MeshSource{"builtin:cube"});
	scene.registry().get<Transform>(hidden).position = {0.0f, 0.0f, 0.0f};

	scene.update(0.0f);

	// Enough frames for the readback fence of an early frame to land. Bounded
	// rather than fixed so the test is not timing-fragile: it stops as soon as
	// the pyramid arrives.
	bool ready        = false;
	uint32_t occluded  = 0;
	int frames         = 0;
	for (int frame = 0; frame < 16 && !ready; ++frame) {
		gpu.begin_frame_offscreen();
		scene.upload(gpu, 1.0f);
		gpu.begin_scene_pass();
		scene.draw(gpu);
		gpu.end_scene_pass();
		gpu.end_frame();
		SDL_WaitForGPUIdle(gpu.device());
		ready    = gpu.occlusion_ready();
		occluded = scene.occluded_entities();
		frames   = frame + 1;
	}

	EXPECT_TRUE(ready) << "pyramid never reached the CPU";
	EXPECT_GT(occluded, 0u) << "nothing behind the wall was culled";

	// Readiness alone is NOT sufficient evidence. `occlusion_ready()` goes
	// true on an all-sky pyramid, and a descriptor-set mismatch produced a
	// valid pipeline with an empty SDL_GetError() and an all-zero pyramid --
	// which reads as "geometry zero units away" and culls nothing. Assert that
	// real depth actually arrived, at a level the query reads.
	ASSERT_GE(gpu.occlusion_pyramid().level_count(), 3u);
	const std::vector<float> mip2 = gpu.occlusion_pyramid().level(2);
	ASSERT_FALSE(mip2.empty());
	const float nearest = *std::min_element(mip2.begin(), mip2.end());
	EXPECT_LT(nearest, kDepthFar)
	    << "mip 2 is all far-values: nothing was ever written to it";

	// Evidence, printed rather than asserted: the numbers a reviewer needs to
	// confirm this test is exercising real depth, not an empty pyramid.
	double sum        = 0.0;
	uint32_t covered  = 0;
	for (const float value : mip2) {
		sum += value;
		covered += (value < kDepthFar) ? 1u : 0u;
	}
	std::printf(
	    "occlusion: frames=%d ready=%d occluded=%u drawn=%u culled=%u "
	    "levels=%u mip2=%ux%u min=%g max=%g mean=%g covered=%u/%zu\n",
	    frames, static_cast<int>(ready), occluded, scene.drawn_entities(),
	    scene.culled_entities(), gpu.occlusion_pyramid().level_count(),
	    gpu.occlusion_pyramid().level_width(2),
	    gpu.occlusion_pyramid().level_height(2), static_cast<double>(nearest),
	    static_cast<double>(*std::max_element(mip2.begin(), mip2.end())),
	    sum / static_cast<double>(mip2.size()), covered, mip2.size());
}

} // namespace