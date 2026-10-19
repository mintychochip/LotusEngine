#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lotus/scene/residency_system.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;
namespace fs = std::filesystem;

TEST(ResidencySystemTest, LoadsAndUnloadsByOriginName)
{
	const fs::path dir = fs::temp_directory_path() / "lotus_residency";
	fs::create_directories(dir);

	const fs::path region_scene = dir / "tile.json";
	{
		std::ofstream out(region_scene);
		out << R"({"version":3,"entities":[{"name":"prop","components":{"SceneGuid":"a1"}}]})";
	}

	const fs::path catalog = dir / "regions.json";
	{
		std::ofstream out(catalog);
		out << R"({"regions":[{"name":"tile_a","path":"tile.json","center":[0,0,0],"radius":50}]})";
	}

	ResidencySystem system;
	system.set_scene_directory(dir.string());
	std::string error;
	ASSERT_TRUE(system.load_catalog(catalog.string(), &error)) << error;

	Scene scene{"world"};
	system.update(scene, glm::vec3{0.0f, 0.0f, 0.0f});
	EXPECT_EQ(scene.registry().view<Name>().size(), 1u);
	EXPECT_EQ(system.resident().size(), 1u);

	system.clear_resident();
	EXPECT_TRUE(system.resident().empty());
	system.update(scene, glm::vec3{0.0f, 0.0f, 0.0f});
	EXPECT_EQ(system.resident().size(), 1u);

	system.update(scene, glm::vec3{200.0f, 0.0f, 0.0f});
	EXPECT_EQ(scene.registry().view<Name>().size(), 0u);
	EXPECT_TRUE(system.resident().empty());

	fs::remove_all(dir);
}

TEST(ResidencySystemTest, LimitsLoadsPerFrame)
{
	const fs::path dir = fs::temp_directory_path() / "lotus_residency_budget";
	fs::create_directories(dir);

	for (int i = 0; i < 3; ++i) {
		const fs::path region_scene = dir / ("tile" + std::to_string(i) + ".json");
		std::ofstream out(region_scene);
		out << R"({"version":3,"entities":[{"name":"prop","components":{"SceneGuid":")"
		    << i << R"("}}]})";
	}

	const fs::path catalog = dir / "regions.json";
	{
		std::ofstream out(catalog);
		out << R"({"regions":[)"
		    << R"({"name":"a","path":"tile0.json","center":[0,0,0],"radius":50},)"
		    << R"({"name":"b","path":"tile1.json","center":[0,0,0],"radius":50},)"
		    << R"({"name":"c","path":"tile2.json","center":[0,0,0],"radius":50}]})";
	}

	ResidencySystem system;
	system.set_scene_directory(dir.string());
	system.set_max_loads_per_frame(1);
	std::string error;
	ASSERT_TRUE(system.load_catalog(catalog.string(), &error)) << error;

	Scene scene{"world"};
	system.update(scene, glm::vec3{0.0f, 0.0f, 0.0f});
	EXPECT_EQ(system.resident().size(), 1u);
	EXPECT_EQ(scene.registry().view<Name>().size(), 1u);

	system.update(scene, glm::vec3{0.0f, 0.0f, 0.0f});
	EXPECT_EQ(system.resident().size(), 2u);

	system.update(scene, glm::vec3{0.0f, 0.0f, 0.0f});
	EXPECT_EQ(system.resident().size(), 3u);

	fs::remove_all(dir);
}
