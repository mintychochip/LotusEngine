#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

using namespace lotus;

namespace {

Entity find_by_name(Scene& scene, const std::string& name)
{
	for (const Entity e : scene.registry().view<Name>()) {
		if (scene.registry().get<Name>(e).value == name) {
			return e;
		}
	}
	return NullEntity;
}

} // namespace

TEST(SceneSerialization, RoundTripPreservesHierarchyAndComponents)
{
	Scene source{"level"};
	const Entity root  = source.create("root");
	const Entity child = source.create("child");

	source.registry().get<Transform>(root).position = {1.0f, 2.0f, 0.0f};
	source.registry().emplace<Sprite>(
	        root, Sprite{{0.1f, 0.2f, 0.3f, 1.0f}, {0.5f, 0.25f}});
	source.registry().get<Transform>(child).position = {3.0f, 0.0f, 0.0f};
	source.set_parent(child, root);

	const std::string text = to_json_string(source);

	Scene restored{"empty"};
	std::string error;
	ASSERT_TRUE(scene_from_json(restored, text, &error)) << error;

	EXPECT_EQ(restored.name(), "level");
	const Entity restored_root = find_by_name(restored, "root");
	const Entity restored_child = find_by_name(restored, "child");
	ASSERT_NE(restored_root, NullEntity);
	ASSERT_NE(restored_child, NullEntity);

	EXPECT_FLOAT_EQ(restored.registry().get<Transform>(restored_root).position.x,
	                1.0f);
	const auto& sprite = restored.registry().get<Sprite>(restored_root);
	EXPECT_FLOAT_EQ(sprite.color.g, 0.2f);
	EXPECT_FLOAT_EQ(sprite.size.y, 0.25f);

	// Hierarchy survives, and the parent link is rebuilt.
	EXPECT_EQ(restored.registry().get<Hierarchy>(restored_child).parent,
	          restored_root);
	ASSERT_EQ(restored.roots().size(), 1u);
	EXPECT_EQ(restored.roots()[0], restored_root);
}

TEST(SceneSerialization, DerivedFieldsAreNotWritten)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Sprite>(e, Sprite{});
	const std::string text = to_json_string(scene);
	const auto root        = nlohmann::json::parse(text);

	EXPECT_EQ(root.at("version").get<int>(), kSceneFormatVersion);
	const auto& components = root.at("entities").at(0).at("components");
	EXPECT_FALSE(components.contains("Hierarchy"));
	EXPECT_FALSE(components.contains("Name"));
	EXPECT_FALSE(components.at("Transform").contains("world"));
}

TEST(SceneSerialization, OmittedFieldsKeepDefaults)
{
	Scene scene{"s"};
	scene_from_json(scene,
	                R"({"version":1,"name":"s","entities":[
	                     {"name":"e","components":{"Transform":{"position":[5,0,0]}}}]})");
	const Entity e = find_by_name(scene, "e");
	ASSERT_NE(e, NullEntity);
	const auto& t = scene.registry().get<Transform>(e);
	EXPECT_FLOAT_EQ(t.position.x, 5.0f);
	EXPECT_FLOAT_EQ(t.scale.y, 1.0f); // default preserved
}

TEST(SceneSerialization, UnknownComponentIsSkipped)
{
	Scene scene{"s"};
	std::string error;
	EXPECT_TRUE(scene_from_json(
	        scene,
	        R"({"version":1,"entities":[{"name":"e","components":{
	             "TotallyUnknown":{"x":1},"Transform":{"position":[1,0,0]}}}]})",
	        &error)) << error;
	const Entity e = find_by_name(scene, "e");
	ASSERT_NE(e, NullEntity);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(e).position.x, 1.0f);
}

TEST(SceneSerialization, MalformedJsonLeavesSceneUntouched)
{
	Scene scene{"live"};
	const Entity survivor = scene.create("survivor");
	scene.registry().get<Transform>(survivor).position = {7.0f, 8.0f, 0.0f};
	scene.registry().emplace<Sprite>(survivor,
	                                 Sprite{{0.25f, 0.5f, 0.75f, 1.0f}, {2.0f, 3.0f}});

	std::string error;
	EXPECT_FALSE(scene_from_json(scene, "{ this is not json", &error));
	EXPECT_FALSE(error.empty());

	// The live scene must be byte-for-byte intact: entity count, names,
	// and component values.
	EXPECT_TRUE(scene.registry().valid(survivor));
	EXPECT_EQ(scene.registry().view<Name>().size(), 1u);
	EXPECT_NE(find_by_name(scene, "survivor"), NullEntity);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(survivor).position.x, 7.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(survivor).position.y, 8.0f);
	const auto& sprite = scene.registry().get<Sprite>(survivor);
	EXPECT_FLOAT_EQ(sprite.color.b, 0.75f);
	EXPECT_FLOAT_EQ(sprite.size.y, 3.0f);
}

TEST(SceneSerialization, VectorsAreInline)
{
	Scene scene{"s"};
	const Entity e = scene.create("e");
	scene.registry().emplace<Sprite>(e, Sprite{});
	const std::string text = to_json_string(scene);

	// Readability contract: a Transform is one line, not twelve.
	EXPECT_NE(text.find("\"position\": [0.0, 0.0, 0.0]"), std::string::npos)
	        << text;
	EXPECT_NE(text.find("\"scale\": [1.0, 1.0, 1.0]"), std::string::npos) << text;
	// And floats are rounded, not float32->double noise.
	EXPECT_EQ(text.find("000000"), std::string::npos) << text;
}

TEST(SceneSerialization, UnsupportedVersionIsRejected)
{
	Scene scene{"live"};
	scene.create("survivor");
	std::string error;
	EXPECT_FALSE(scene_from_json(scene, R"({"version":999,"entities":[]})", &error));
	EXPECT_NE(find_by_name(scene, "survivor"), NullEntity);
}

TEST(SceneSerialization, BadShapeLeavesSceneUntouched)
{
	Scene scene{"live"};
	scene.create("survivor");
	std::string error;
	// entities must be an array
	EXPECT_FALSE(scene_from_json(scene, R"({"version":1,"entities":42})", &error));
	EXPECT_NE(find_by_name(scene, "survivor"), NullEntity);
}

TEST(SceneSerialization, CameraComponentRoundTrips)
{
	Scene source{"s"};
	const Entity cam = source.create("camera");
	auto& camera     = source.registry().emplace<Camera2D>(cam);
	camera.zoom      = 2.5f;
	camera.position  = {1.5f, -2.0f};

	Scene restored{"empty"};
	ASSERT_TRUE(scene_from_json(restored, to_json_string(source)));

	const Camera2D* restored_camera = restored.active_camera();
	ASSERT_NE(restored_camera, nullptr);
	EXPECT_FLOAT_EQ(restored_camera->zoom, 2.5f);
	EXPECT_FLOAT_EQ(restored_camera->position.x, 1.5f);
}

TEST(SceneSerialization, FileRoundTrip)
{
	const std::string path = "scene_roundtrip_test.json";
	Scene source{"disk"};
	const Entity e = source.create("entity");
	source.registry().get<Transform>(e).position = {4.0f, 5.0f, 0.0f};

	std::string error;
	ASSERT_TRUE(save_scene(source, path, &error)) << error;

	Scene loaded{"empty"};
	ASSERT_TRUE(load_scene(loaded, path, &error)) << error;
	const Entity loaded_entity = find_by_name(loaded, "entity");
	ASSERT_NE(loaded_entity, NullEntity);
	EXPECT_FLOAT_EQ(loaded.registry().get<Transform>(loaded_entity).position.y,
	                5.0f);

	std::remove(path.c_str());
}

TEST(SceneSerialization, MissingFileReportsError)
{
	Scene scene{"s"};
	std::string error;
	EXPECT_FALSE(load_scene(scene, "definitely_missing_scene.json", &error));
	EXPECT_FALSE(error.empty());
}
