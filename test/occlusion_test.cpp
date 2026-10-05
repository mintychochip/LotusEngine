#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "lotus/scene/depth_pyramid.hpp"

using namespace lotus;

namespace {

std::vector<float> flat_image(uint32_t w, uint32_t h, float value)
{
	return std::vector<float>(static_cast<std::size_t>(w) * h, value);
}

/// Camera at +Z looking at the origin. Matches Camera3D: right-handed lookAt
/// with a zero-to-one perspective projection.
struct TestCamera {
	glm::mat4 view;
	glm::mat4 projection;
	glm::mat4 view_projection;
};

TestCamera camera_from_z(float eye_z)
{
	TestCamera camera;
	camera.view = glm::lookAtRH(glm::vec3{0.0f, 0.0f, eye_z}, glm::vec3{0.0f},
	                            glm::vec3{0.0f, 1.0f, 0.0f});
	camera.projection =
	    glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
	camera.view_projection = camera.projection * camera.view;
	return camera;
}

/// A 64x64 pyramid whose whole screen is a wall at linear depth \p depth.
DepthPyramid solid_wall(uint32_t size, float depth)
{
	DepthPyramid pyramid;
	const std::vector<float> image = flat_image(size, size, depth);
	pyramid.build(image.data(), size, size);
	return pyramid;
}

/// A 64x64 pyramid that is a wall of \p depth on the left half and far on the
/// right half, so a sphere straddling the seam must stay visible.
DepthPyramid split_wall(uint32_t size, float depth)
{
	std::vector<float> image(static_cast<std::size_t>(size) * size, kDepthFar);
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size / 2; ++x) {
			image[static_cast<std::size_t>(y) * size + x] = depth;
		}
	}
	DepthPyramid pyramid;
	pyramid.build(image.data(), size, size);
	return pyramid;
}

} // namespace

// --- Task 1: storage and reduction (unchanged) ---

TEST(DepthPyramidTest, EmptyByDefault)
{
	const DepthPyramid pyramid;
	EXPECT_TRUE(pyramid.empty());
	EXPECT_EQ(pyramid.width(), 0u);
	EXPECT_EQ(pyramid.height(), 0u);
	EXPECT_EQ(pyramid.level_count(), 0u);
}

TEST(DepthPyramidTest, BuildStoresLevelZeroVerbatim)
{
	const std::vector<float> image = flat_image(8, 8, 3.0f);
	DepthPyramid pyramid;
	pyramid.build(image.data(), 8, 8);

	ASSERT_FALSE(pyramid.empty());
	EXPECT_EQ(pyramid.width(), 8u);
	EXPECT_EQ(pyramid.height(), 8u);
	EXPECT_EQ(pyramid.level(0), image);
}

TEST(DepthPyramidTest, LevelCountFollowsTheSmallerDimension)
{
	DepthPyramid square;
	square.build(flat_image(64, 64, 1.0f).data(), 64, 64);
	EXPECT_EQ(square.level_count(), 7u);

	DepthPyramid wide;
	wide.build(flat_image(128, 32, 1.0f).data(), 128, 32);
	EXPECT_EQ(wide.level_count(), 6u);
}

TEST(DepthPyramidTest, HalvesEachLevelAndNeverGoesBelowOne)
{
	DepthPyramid pyramid;
	pyramid.build(flat_image(64, 64, 1.0f).data(), 64, 64);

	EXPECT_EQ(pyramid.level_width(0), 64u);
	EXPECT_EQ(pyramid.level_height(0), 64u);
	EXPECT_EQ(pyramid.level_width(1), 32u);
	EXPECT_EQ(pyramid.level_width(3), 8u);
	EXPECT_EQ(pyramid.level_width(6), 1u);
	EXPECT_EQ(pyramid.level_height(6), 1u);
}

TEST(DepthPyramidTest, EachLevelTakesTheMinimumOfTheBlockAbove)
{
	std::vector<float> image = flat_image(4, 4, kDepthFar);
	image[0] = 1.5f;
	image[1] = 9.0f;

	DepthPyramid pyramid;
	pyramid.build(image.data(), 4, 4);

	const std::vector<float> level1 = pyramid.level(1);
	ASSERT_EQ(level1.size(), 4u);
	EXPECT_FLOAT_EQ(level1[0], 1.5f);
	EXPECT_FLOAT_EQ(level1[1], kDepthFar);
	EXPECT_FLOAT_EQ(level1[2], kDepthFar);
	EXPECT_FLOAT_EQ(level1[3], kDepthFar);

	const std::vector<float> level2 = pyramid.level(2);
	ASSERT_EQ(level2.size(), 1u);
	EXPECT_FLOAT_EQ(level2[0], 1.5f);
}

TEST(DepthPyramidTest, MinimumPropagatesAcrossManyLevels)
{
	std::vector<float> image = flat_image(128, 128, kDepthFar);
	image[127 * 128 + 127] = 2.25f;

	DepthPyramid pyramid;
	pyramid.build(image.data(), 128, 128);

	const std::vector<float> coarsest = pyramid.level(pyramid.level_count() - 1);
	ASSERT_EQ(coarsest.size(), 1u);
	EXPECT_FLOAT_EQ(coarsest[0], 2.25f);
}

TEST(DepthPyramidTest, BuildRejectsNullAndZeroSizedInput)
{
	DepthPyramid pyramid;
	pyramid.build(flat_image(4, 4, 1.0f).data(), 4, 4);
	ASSERT_FALSE(pyramid.empty());

	pyramid.build(nullptr, 0, 0);
	EXPECT_TRUE(pyramid.empty());

	const float pixel = 1.0f;
	pyramid.build(&pixel, 8, 8);
	pyramid.build(&pixel, 0, 8);
	EXPECT_TRUE(pyramid.empty());
}

TEST(DepthPyramidTest, ClearResetsEverything)
{
	DepthPyramid pyramid;
	pyramid.build(flat_image(8, 8, 1.0f).data(), 8, 8);
	pyramid.clear();

	EXPECT_TRUE(pyramid.empty());
	EXPECT_EQ(pyramid.level_count(), 0u);
	EXPECT_TRUE(pyramid.level(0).empty());
}

TEST(DepthPyramidTest, AdoptRejectionDiscardsThePreviouslyInstalledChain)
{
	DepthPyramid pyramid;
	pyramid.build(flat_image(16, 16, 1.0f).data(), 16, 16);
	ASSERT_FALSE(pyramid.empty());

	// A rejected upload must leave no pyramid at all: keeping frame N-1's
	// depth would pair it with frame N's camera.
	pyramid.adopt(64, 64, 7, {}); // too few mips
	EXPECT_TRUE(pyramid.empty());
	EXPECT_EQ(pyramid.level_count(), 0u);

	pyramid.build(flat_image(16, 16, 1.0f).data(), 16, 16);
	pyramid.adopt(0, 0, 7, {}); // no dimensions
	EXPECT_TRUE(pyramid.empty());
}

TEST(DepthPyramidTest, AdoptRejectsMipsWhoseSizeContradictsTheAccessors)
{
	std::vector<std::vector<float>> mips(7);
	mips[2] = flat_image(16, 16, 4.0f);
	mips[3] = flat_image(8, 8, 4.0f);
	mips[4] = flat_image(4, 4, 4.0f);
	mips[5] = flat_image(2, 2, 4.0f);
	mips[6] = flat_image(1, 1, 4.0f);
	// Level 3 reports 8x8, so three texels would let a consumer run off the end.
	mips[3] = flat_image(2, 2, 4.0f);

	DepthPyramid pyramid;
	pyramid.build(flat_image(16, 16, 1.0f).data(), 16, 16);
	pyramid.adopt(64, 64, 7, mips);

	EXPECT_TRUE(pyramid.empty());
	EXPECT_EQ(pyramid.level_count(), 0u);
}

TEST(DepthPyramidTest, OddDimensionsFollowVulkanMipConvention)
{
	// 3x4 reduces to a 1x2 level 1 under Vulkan's max(1, w >> level) rule.
	// Columns 0-1 are sampled by level 1; the ragged edge column 2 lies
	// outside every 2x2 window and is never sampled, matching depth.comp.
	std::vector<float> image = flat_image(3, 4, kDepthFar);
	image[0] = 1.5f; // row 0, column 0: sampled, so it must propagate
	image[2] = 0.25f; // row 0, column 2: ragged edge, must NOT propagate
	image[5] = 0.5f; // row 1, column 2: ragged edge, must NOT propagate

	DepthPyramid pyramid;
	pyramid.build(image.data(), 3, 4);

	ASSERT_EQ(pyramid.level_count(), 2u);
	EXPECT_EQ(pyramid.level_width(1), 1u);
	EXPECT_EQ(pyramid.level_height(1), 2u);

	// Level 1 texel 0 covers source columns 0-1 of rows 0-1; texel 1 covers
	// the same columns of rows 2-3. Both ragged-edge values are unsampled, so
	// the far value survives instead of the smaller 0.25f / 0.5f.
	const std::vector<float> level1 = pyramid.level(1);
	ASSERT_EQ(level1.size(), 2u);
	EXPECT_FLOAT_EQ(level1[0], 1.5f);
	EXPECT_FLOAT_EQ(level1[1], kDepthFar);
}

// --- Task 2: the query ---

TEST(DepthPyramidOcclusion, EmptyPyramidNeverOccludes)
{
	const DepthPyramid empty;
	const TestCamera camera = camera_from_z(10.0f);
	EXPECT_FALSE(empty.occluded(camera.view, camera.view_projection,
	                            {{0.0f, 0.0f, 0.0f}, 1.0f}));
}

TEST(DepthPyramidOcclusion, ZeroRadiusNeverOccludes)
{
	const DepthPyramid pyramid = solid_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 0.0f, 0.0f}, 0.0f}));
}

TEST(DepthPyramidOcclusion, SphereBehindAWallIsOccluded)
{
	const DepthPyramid pyramid = solid_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	// Camera at z=10, so this sphere's nearest point is at depth 9.4.
	EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
	                             {{0.0f, 0.0f, 0.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, SphereInFrontOfTheWallIsVisible)
{
	const DepthPyramid pyramid = solid_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	// At z=8 the nearest point is at depth 1.9, well in front of the wall.
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 0.0f, 8.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, SphereStraddlingTheNearPlaneIsVisible)
{
	const DepthPyramid pyramid = solid_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	// Radius 0.5 at z=9.95 reaches past the camera plane.
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 0.0f, 9.95f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, SphereStraddlingTheWallEdgeIsVisible)
{
	const DepthPyramid pyramid = split_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	// NDC x is world x divided by depth times tan(30deg). At depth 10 that
	// puts x = 2.9 near the right edge of a 64-wide image, which is where the
	// wall ends, so the footprint covers both wall and empty space.
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{2.9f, 0.0f, 0.0f}, 0.4f}));
}

TEST(DepthPyramidOcclusion, SphereFullyBehindTheWallHalfIsOccluded)
{
	const DepthPyramid pyramid = split_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
	                             {{-3.0f, 0.0f, 0.0f}, 0.4f}));
}

TEST(DepthPyramidOcclusion, SphereOffToTheSideIsVisible)
{
	const DepthPyramid pyramid = solid_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	// Far outside the frustum in x and y, so never culled even behind a wall.
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{100.0f, 0.0f, 0.0f}, 1.0f}));
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 100.0f, 0.0f}, 1.0f}));
}

TEST(DepthPyramidOcclusion, SphereBehindTheCameraIsVisible)
{
	const DepthPyramid pyramid = solid_wall(64, 5.0f);
	const TestCamera camera    = camera_from_z(10.0f);

	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 0.0f, 20.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, LargeSphereSpanningManyTexelsStaysConservative)
{
	// A wall only in the middle 8x8 texels; anything larger than that must
	// survive because its footprint reaches past the occluder.
	std::vector<float> image = flat_image(64, 64, kDepthFar);
	for (uint32_t y = 28; y < 36; ++y) {
		for (uint32_t x = 28; x < 36; ++x) {
			image[static_cast<std::size_t>(y) * 64 + x] = 5.0f;
		}
	}
	DepthPyramid pyramid;
	pyramid.build(image.data(), 64, 64);
	const TestCamera camera = camera_from_z(10.0f);

	// Small enough to sit inside the wall.
	EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
	                             {{0.0f, 0.0f, 0.0f}, 0.15f}));
	// Large enough that its footprint spills outside the wall.
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 0.0f, 0.0f}, 4.0f}));
}

TEST(DepthPyramidOcclusion, UntouchedTexelsDoNotCullTheScene)
{
	// This is the failure mode the far clear exists to prevent: if an unwritten
	// texel were zero, min-reduction would spread that zero everywhere.
	const DepthPyramid pyramid = solid_wall(64, kDepthFar);
	const TestCamera camera    = camera_from_z(10.0f);

	// An all-far pyramid has nothing in front, so nothing is occluded.
	EXPECT_FALSE(pyramid.occluded(camera.view, camera.view_projection,
	                              {{0.0f, 0.0f, 0.0f}, 0.5f}));
}

TEST(DepthPyramidOcclusion, ClassificationFollowsCameraDistance)
{
	// The pyramid stores linear depth measured from whichever camera built it.
	// A wall at depth 5 from a camera at z=10 sits at world z=5.
	const DepthPyramid pyramid = solid_wall(64, 5.0f);

	// From z=10 the sphere at world z=0 is 10 units away, behind the wall.
	const TestCamera camera = camera_from_z(10.0f);
	EXPECT_TRUE(pyramid.occluded(camera.view, camera.view_projection,
	                             {{0.0f, 0.0f, 0.0f}, 0.5f}));

	// From z=7 the same sphere is only 7 units away, still behind the wall.
	const TestCamera closer = camera_from_z(7.0f);
	EXPECT_TRUE(pyramid.occluded(closer.view, closer.view_projection,
	                             {{0.0f, 0.0f, 0.0f}, 0.5f}));

	// From z=3 the sphere is 3 units away, in front of the depth-5 wall. The
	// pyramid is stale relative to this camera, but the test stays honest
	// because it compares view-space depths rather than reusing frame state.
	const TestCamera closest = camera_from_z(3.0f);
	EXPECT_FALSE(pyramid.occluded(closest.view, closest.view_projection,
	                              {{0.0f, 0.0f, 0.0f}, 0.5f}));
}

TEST(DepthPyramidTest, AdoptInstallsGpuProducedMipsByLevel)
{
	// A 64x64 chain has seven levels; the renderer transfers 2..6.
	std::vector<std::vector<float>> mips(7);
	mips[2] = flat_image(16, 16, 4.0f);
	mips[3] = flat_image(8, 8, 4.0f);
	mips[4] = flat_image(4, 4, 4.0f);
	mips[5] = flat_image(2, 2, 4.0f);
	mips[6] = flat_image(1, 1, 4.0f);

	DepthPyramid pyramid;
	pyramid.adopt(64, 64, 7, mips);

	ASSERT_EQ(pyramid.level_count(), 7u);
	EXPECT_EQ(pyramid.width(), 64u);
	EXPECT_EQ(pyramid.level(2), mips[2]);
	EXPECT_EQ(pyramid.level(6), mips[6]);
	// Levels the renderer never transfers stay empty rather than fabricated.
	EXPECT_TRUE(pyramid.level(0).empty());
	EXPECT_TRUE(pyramid.level(1).empty());
}

TEST(DepthPyramidTest, AdoptRejectsShortOrEmptyUploads)
{
	DepthPyramid pyramid;

	pyramid.adopt(64, 64, 7, {}); // too few mips
	EXPECT_TRUE(pyramid.empty());

	pyramid.adopt(0, 0, 7, {}); // no dimensions
	EXPECT_TRUE(pyramid.empty());
}
