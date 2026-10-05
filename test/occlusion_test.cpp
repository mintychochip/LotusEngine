#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/depth_pyramid.hpp"

using namespace lotus;

namespace {

/// Fills a w*h image with one value.
std::vector<float> flat_image(uint32_t w, uint32_t h, float value)
{
	return std::vector<float>(static_cast<std::size_t>(w) * h, value);
}

} // namespace

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
	// 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1 is seven levels.
	EXPECT_EQ(square.level_count(), 7u);

	DepthPyramid wide;
	wide.build(flat_image(128, 32, 1.0f).data(), 128, 32);
	// 32 -> 16 -> 8 -> 4 -> 2 -> 1 bounds the chain.
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
	// 4x4, with the only small depth at the top-left texel.
	std::vector<float> image = flat_image(4, 4, kDepthFar);
	image[0] = 1.5f;
	image[1] = 9.0f;

	DepthPyramid pyramid;
	pyramid.build(image.data(), 4, 4);

	// Level 1 is 2x2; its (0,0) texel covers source texels (0,0),(1,0),(0,1),(1,1).
	const std::vector<float> level1 = pyramid.level(1);
	ASSERT_EQ(level1.size(), 4u);
	EXPECT_FLOAT_EQ(level1[0], 1.5f);
	// The remaining three 2x2 blocks see nothing but far values.
	EXPECT_FLOAT_EQ(level1[1], kDepthFar);
	EXPECT_FLOAT_EQ(level1[2], kDepthFar);
	EXPECT_FLOAT_EQ(level1[3], kDepthFar);

	// Level 2 is 1x1 and sees every texel of level 1, so it takes the minimum.
	const std::vector<float> level2 = pyramid.level(2);
	ASSERT_EQ(level2.size(), 1u);
	EXPECT_FLOAT_EQ(level2[0], 1.5f);
}

TEST(DepthPyramidTest, MinimumPropagatesAcrossManyLevels)
{
	std::vector<float> image = flat_image(128, 128, kDepthFar);
	image[127 * 128 + 127] = 2.25f; // bottom-right corner only

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

	// A non-null pointer with a zero dimension must still be refused.
	const float pixel = 1.0f;
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
