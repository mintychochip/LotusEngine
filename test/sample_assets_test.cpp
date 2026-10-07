#include <gtest/gtest.h>

#include "lotus/scene/animation.hpp"
#include "lotus/scene/assets.hpp"

using namespace lotus;

TEST(SampleAssetsTest, RegistryLoadsSampleBobClip)
{
	AssetRegistry assets;
	std::string error;
	ASSERT_TRUE(assets.load("assets/assets.json", &error)) << error;
	const std::string* path = assets.find("sample_bob");
	ASSERT_NE(path, nullptr);
	const std::string clip_path = "assets/" + *path;
	AnimationClip clip;
	ASSERT_TRUE(load_animation_clip_from_file(clip_path, clip, &error)) << error;
	EXPECT_EQ(clip.tracks.size(), 1u);
	EXPECT_EQ(clip.tracks.front().keyframes.size(), 3u);
}
