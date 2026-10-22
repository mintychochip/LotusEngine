#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"
#include "vulkan_device.hpp"

using namespace lotus;

namespace {

/// Skips (or fails, under LOTUS_REQUIRE_VULKAN) when the machine has no Vulkan
/// device; the offscreen renderer needs no window system.
class SkyOrderGpuTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		LOTUS_SKIP_WITHOUT_VULKAN_DEVICE();
	}
};

// Not square, so a viewport that swaps or mis-sizes the two axes shows up. The
// editor viewport is a different size again, so the pass that renders into it
// cannot borrow its extent from the offscreen target.
constexpr uint32_t kWidth          = 192;
constexpr uint32_t kHeight         = 128;
constexpr uint32_t kViewportWidth  = 160;
constexpr uint32_t kViewportHeight = 96;

/// A camera at eye height looking straight at the horizon, over a ground plane
/// 400 units wide, with a cube a few units ahead.
///
/// The far plane is 4 units, so nearly all the ground lies beyond it. Depth
/// clamp is on, so that ground is not clipped: it is drawn, with its depth
/// clamped to exactly 1.0, the value an untouched pixel holds. That is the
/// case that tells a correct sky from a sky that merely passes the benchmark
/// scene, whose ground ends inside its far plane. The cube is wholly inside
/// the far plane and covers the ordinary case.
Scene make_scene(bool with_geometry)
{
	Scene scene{"s"};
	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera,
	                                   Camera3D{{0.0f, 1.5f, 0.0f},
	                                            {0.0f, 1.5f, -10.0f},
	                                            {0.0f, 1.0f, 0.0f},
	                                            50.0f,
	                                            0.1f,
	                                            4.0f});
	if (with_geometry) {
		const Entity ground = scene.create("ground");
		scene.registry().emplace<MeshSource>(ground,
		                                     MeshSource{"builtin:plane", ""});
		scene.registry().get<Transform>(ground).scale = {400.0f, 1.0f, 400.0f};
		scene.registry().emplace<Material>(
		        ground, Material{{0.55f, 0.55f, 0.57f}, "", 0.9f});

		const Entity cube = scene.create("cube");
		scene.registry().emplace<MeshSource>(cube,
		                                     MeshSource{"builtin:cube", ""});
		scene.registry().get<Transform>(cube).position = {0.0f, 1.5f, -3.0f};
		scene.registry().emplace<Material>(
		        cube, Material{{0.78f, 0.32f, 0.22f}, "", 0.5f});
	}
	scene.update(0.0f);
	return scene;
}

/// Everything that depends on the previous frame (SSAO) or on a pixel's
/// neighbours (FXAA) is off, so that a pixel's value depends on nothing but the
/// geometry that covers it and, for a pixel nothing covers, on the sky.
void configure(GpuRenderer& gpu, bool sky)
{
	LightingSettings& lighting = gpu.lighting();
	lighting.sky_enabled       = sky;
	lighting.fxaa_enabled      = false;
	lighting.ssao_enabled      = false;
	lighting.sky_color         = {0.32f, 0.45f, 0.68f};
}

/// Three frames, the last of which is returned as RGBA: the scene pass into the
/// offscreen target, which is what the headless renderer and the benchmark use.
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

/// Three frames, the last of which is returned as RGBA: the viewport pass into
/// the editor's viewport texture, which has a pass of its own.
std::vector<uint8_t> render_viewport_pass(GpuRenderer& gpu, Scene& scene,
                                          bool sky)
{
	configure(gpu, sky);
	for (int frame = 0; frame < 3; ++frame) {
		gpu.begin_frame_offscreen();
		scene.upload(gpu, static_cast<float>(kViewportWidth) / kViewportHeight);
		EXPECT_TRUE(gpu.begin_viewport_pass());
		scene.draw(gpu);
		gpu.end_viewport_pass();
		gpu.end_frame();
		gpu.wait_idle();
	}
	uint32_t width  = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels = gpu.capture_viewport_rgba(width, height);
	EXPECT_EQ(width, kViewportWidth);
	EXPECT_EQ(height, kViewportHeight);
	return pixels;
}

bool same_pixel(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                std::size_t pixel)
{
	for (std::size_t channel = 0; channel < 4; ++channel) {
		if (a[pixel * 4 + channel] != b[pixel * 4 + channel]) {
			return false;
		}
	}
	return true;
}

/// \p off is the scene rendered without the sky and \p on with it. The top rows
/// look over the horizon: with the sky off they are the clear colour, which is
/// what "no geometry here" looks like, so everything that is not that colour is
/// geometry.
void expect_sky_fills_only_where_nothing_is_drawn(
        const std::vector<uint8_t>& off, const std::vector<uint8_t>& on,
        uint32_t width, uint32_t height)
{
	const std::size_t pixels = static_cast<std::size_t>(width) * height;
	ASSERT_EQ(off.size(), pixels * 4);
	ASSERT_EQ(on.size(), off.size());

	std::vector<bool> geometry(pixels, false);
	std::size_t geometry_count = 0;
	for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
		geometry[pixel] = !(off[pixel * 4] == off[0] &&
		                    off[pixel * 4 + 1] == off[1] &&
		                    off[pixel * 4 + 2] == off[2]);
		geometry_count += geometry[pixel] ? 1u : 0u;
	}

	// Oracles that do not come from the renderer being tested. The corners
	// say which half is which, and the horizon -- the camera is level, so it
	// is the middle row, give or take the sliver of ground that reaches it --
	// pins the scale: a viewport the wrong size for its target does not cut
	// the picture off, it stretches it, and moves the horizon.
	const auto is_geometry = [&](uint32_t x, uint32_t y) {
		return geometry[static_cast<std::size_t>(y) * width + x];
	};
	EXPECT_FALSE(is_geometry(0, 0)) << "top-left should be background";
	EXPECT_FALSE(is_geometry(width - 1, 0)) << "top-right should be background";
	EXPECT_TRUE(is_geometry(0, height - 1)) << "bottom-left should be ground";
	EXPECT_TRUE(is_geometry(width - 1, height - 1))
	    << "bottom-right should be ground";
	const uint32_t column = width / 8; // clear of the cube in the middle
	uint32_t horizon      = 0;
	while (horizon < height && !is_geometry(column, horizon)) {
		++horizon;
	}
	EXPECT_NEAR(static_cast<double>(horizon), height / 2.0, 2.0)
	    << "the horizon is not where a level camera puts it";
	EXPECT_GT(geometry_count, pixels * 35 / 100);
	EXPECT_LT(geometry_count, pixels * 65 / 100);

	std::size_t overpainted = 0; // geometry the sky changed
	std::size_t skied       = 0; // background the sky changed
	std::size_t background  = 0;
	for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
		const bool changed = !same_pixel(on, off, pixel);
		if (geometry[pixel]) {
			overpainted += changed ? 1u : 0u;
		} else {
			++background;
			skied += changed ? 1u : 0u;
		}
	}
	EXPECT_EQ(overpainted, 0u)
	    << overpainted << " of " << geometry_count
	    << " geometry pixels were painted over by the sky (geometry past the "
	       "far plane has depth 1.0, the depth of an untouched pixel)";
	EXPECT_GT(background, 0u);
	EXPECT_EQ(skied, background)
	    << "the sky must fill every pixel nothing else covers";
}

} // namespace

/// The sky fills what nothing else draws, and only that.
///
/// The sky is drawn after the geometry so that it costs nothing where geometry
/// already stands. That is only the same picture if it can tell a pixel nothing
/// touched from one geometry did, and depth alone cannot: geometry beyond the
/// far plane is clamped to depth 1.0, which is what an untouched pixel holds. A
/// sky that tested against depth 1.0 would paint over that geometry and cut
/// every ground plane off at the far plane.
///
/// So: render with the sky off, which shows exactly where the geometry is, and
/// with it on. Every pixel that is geometry must come out byte for byte the
/// same, and the sky must be there on the rest.
TEST_F(SkyOrderGpuTest, TheSkyFillsOnlyWhereNothingIsDrawn)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	ASSERT_NO_THROW(gpu.init_offscreen(kWidth, kHeight));
	Scene scene = make_scene(true);

	const std::vector<uint8_t> off = render_scene_pass(gpu, scene, false);
	const std::vector<uint8_t> on  = render_scene_pass(gpu, scene, true);
	expect_sky_fills_only_where_nothing_is_drawn(off, on, kWidth, kHeight);
}

/// The same, through the editor's viewport pass: a pass of its own, with its own
/// target, whose extent the sky's depth range depends on.
TEST_F(SkyOrderGpuTest, TheViewportPassDrawsTheSkyTheSameWay)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	ASSERT_NO_THROW(gpu.init_offscreen(kWidth, kHeight));
	ASSERT_TRUE(gpu.resize_viewport(kViewportWidth, kViewportHeight));
	Scene scene = make_scene(true);

	const std::vector<uint8_t> off = render_viewport_pass(gpu, scene, false);
	const std::vector<uint8_t> on  = render_viewport_pass(gpu, scene, true);
	expect_sky_fills_only_where_nothing_is_drawn(off, on, kViewportWidth,
	                                             kViewportHeight);
}

/// A pass in which nothing was drawn keeps its clear colour: there is no
/// geometry to anchor a sky to, and the editor's clear-only window pass relies
/// on that. The sky is drawn as part of drawing geometry, so a frame with none
/// must not draw it.
TEST_F(SkyOrderGpuTest, AFrameWithNothingToDrawKeepsTheClearColor)
{
	const std::unique_ptr<GpuRenderer> owned = create_gpu_renderer();
	GpuRenderer& gpu = *owned;
	ASSERT_NO_THROW(gpu.init_offscreen(kWidth, kHeight));
	Scene scene = make_scene(false);

	const std::vector<uint8_t> off = render_scene_pass(gpu, scene, false);
	const std::vector<uint8_t> on  = render_scene_pass(gpu, scene, true);
	ASSERT_EQ(on.size(), off.size());
	EXPECT_EQ(on, off) << "the sky was drawn into a pass that drew nothing";
}
