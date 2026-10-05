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