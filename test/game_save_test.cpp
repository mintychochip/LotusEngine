#include <gtest/gtest.h>

#include <filesystem>

#include "lotus/scene/game_save.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;
namespace fs = std::filesystem;

TEST(GameSaveTest, RoundTripPlayerPose)
{
	Scene scene{"main"};
	const Entity player = scene.create("player");
	scene.registry().get<Transform>(player).position = {3.0f, 0.0f, -2.0f};

	GameSaveData data = capture_game_state(scene, "main", {"hub"});
	const fs::path path = fs::temp_directory_path() / "lotus_game_save.json";
	std::string error;
	ASSERT_TRUE(save_game_to_file(data, path.string(), &error)) << error;

	GameSaveData loaded;
	ASSERT_TRUE(load_game_from_file(path.string(), loaded, &error)) << error;
	EXPECT_EQ(loaded.scene_name, "main");
	EXPECT_EQ(loaded.resident_regions.size(), 1u);
	EXPECT_NEAR(loaded.player_position.x, 3.0f, 1e-4f);
	EXPECT_NEAR(loaded.player_position.z, -2.0f, 1e-4f);

	Scene restored{"main"};
	(void)restored.create("player");
	apply_game_state(restored, loaded);
	const Entity restored_player = restored.find("player");
	EXPECT_NEAR(restored.registry().get<Transform>(restored_player).position.x,
	            3.0f, 1e-4f);

	fs::remove(path);
}
