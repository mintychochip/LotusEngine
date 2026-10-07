#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lotus/scene/animation.hpp"
#include "lotus/scene/assets.hpp"
#include "lotus/scene/components.hpp"

using namespace lotus;
namespace fs = std::filesystem;

TEST(AnimationTest, ClipAdvancesTargetPosition)
{
	Scene scene{"anim"};
	const Entity target = scene.create("target");
	const SceneGuid guid = scene.guid_of(target);

	const fs::path dir = fs::temp_directory_path() / "lotus_anim_test";
	fs::create_directories(dir);
	const fs::path clip_path = dir / "move.json";
	{
		std::ofstream out(clip_path);
		out << R"({
      "tracks": [{
        "guid": ")" << std::hex << guid.value << R"(",
        "property": "position",
        "keyframes": [
          {"t": 0, "v": [0, 0, 0]},
          {"t": 1, "v": [2, 0, 0]}
        ]
      }]
    })";
	}

	AssetRegistry assets;
	assets.set("move_clip", clip_path.string());

	const Entity player = scene.create("player");
	scene.registry().emplace<Animator>(player, Animator{"move_clip", 0.0f, 1.0f,
	                                                     true, true});

	AnimationLibrary library;
	animation_system_update(scene, library, &assets, 0.5f);
	scene.update(0.0f);

	EXPECT_NEAR(scene.registry().get<Transform>(target).position.x, 1.0f, 1e-3f);

	fs::remove_all(dir);
}

TEST(AnimationTest, LoadsClipFromFile)
{
	const fs::path path = fs::temp_directory_path() / "lotus_clip.json";
	{
		std::ofstream out(path);
		out << R"({"tracks":[{"guid":"1","property":"scale","keyframes":[{"t":0,"v":[1,1,1]}]}]})";
	}
	AnimationClip clip;
	std::string error;
	EXPECT_TRUE(load_animation_clip_from_file(path.string(), clip, &error))
	        << error;
	EXPECT_EQ(clip.tracks.size(), 1u);
	fs::remove(path);
}
