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

TEST(SceneSerialization, Version1DocumentMigratesToCurrent)
{
	// A document written under format version 1 must load under the current
	// version through the migration hook rather than being rejected.
	const std::string text = R"({
	  "version": 1,
	  "name": "old",
	  "entities": [
	    {"name": "cube", "components": {"Transform": {"position": [1.0, 2.0, 3.0]}}}
	  ]
	})";

	Scene scene{"s"};
	std::string error;
	ASSERT_TRUE(scene_from_json(scene, text, &error)) << error;
	EXPECT_EQ(scene.name(), "old");
	EXPECT_NE(find_by_name(scene, "cube"), NullEntity);
}

TEST(SceneSerialization, MigrationHookRewritesTheVersionInPlace)
{
	// The hook is what makes the version bump non-breaking. Calling it directly
	// pins that behaviour: without it a version-1 document is only accepted
	// because the guard loosened, which would stop being true at the next bump.
	nlohmann::json document = nlohmann::json::parse(
	        R"({"version":1,"name":"old","entities":[]})");

	ASSERT_TRUE(migrate_scene_json(document, 1, kSceneFormatVersion));
	EXPECT_EQ(document.at("version").get<int>(), kSceneFormatVersion);

	// No downgrade path, and nothing below version 1.
	EXPECT_FALSE(migrate_scene_json(document, kSceneFormatVersion, 1));
	EXPECT_FALSE(migrate_scene_json(document, 0, kSceneFormatVersion));

	// Future versions have no upgrade path either.
	EXPECT_FALSE(migrate_scene_json(document, kSceneFormatVersion, 99));
}

TEST(SceneSerialization, MissingVersionIsRejected)
{
	Scene scene{"live"};
	scene.create("survivor");
	std::string error;
	EXPECT_FALSE(scene_from_json(scene, R"({"entities":[]})", &error));
	EXPECT_FALSE(error.empty());
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

TEST(SceneSerialization, GuidRoundTripsThroughSaveLoad)
{
	Scene source{"s"};
	const Entity a = source.create("a");
	const Entity b = source.create("b");
	source.registry().emplace<EntityRef>(a, EntityRef{source.guid_of(b)});

	const std::string text = to_json_string(source);
	// Derived state must not be written: `resolved` is rebuilt on load. The
	// target must be, or the reference is lost across a save.
	EXPECT_EQ(text.find("resolved"), std::string::npos) << text;
	EXPECT_NE(text.find("\"target\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));

	const Entity la = find_by_name(loaded, "a");
	const Entity lb = find_by_name(loaded, "b");
	ASSERT_NE(la, NullEntity);
	ASSERT_NE(lb, NullEntity);
	ASSERT_TRUE(loaded.registry().all_of<EntityRef>(la));

	// Same GUID, and it resolves to this scene's "b", not some other handle.
	EXPECT_EQ(loaded.registry().get<EntityRef>(la).target.value,
	          loaded.guid_of(lb).value);
	EXPECT_EQ(loaded.registry().get<EntityRef>(la).resolved, lb);
	EXPECT_TRUE(loaded.reference_errors().empty());
}

TEST(SceneSerialization, UnresolvedReferenceIsReportedNotFatal)
{
	Scene source{"s"};
	const Entity a = source.create("a");
	source.registry().emplace<EntityRef>(a, EntityRef{SceneGuid{0xDEADBEEF}});

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, to_json_string(source), nullptr));

	// The reference is reported, and the scene is still usable.
	ASSERT_EQ(loaded.reference_errors().size(), 1u);
	EXPECT_NE(loaded.reference_errors()[0].find("a"), std::string::npos);
	EXPECT_NE(find_by_name(loaded, "a"), NullEntity);
	EXPECT_EQ(loaded.registry().get<EntityRef>(find_by_name(loaded, "a")).resolved,
	          NullEntity);
}

TEST(SceneSerialization, LoadedGuidsDoNotCollideWithNewOnes)
{
	// Parsing calls Scene::create per entity, so the counter is already at 3
	// when the document's own GUIDs are applied. Entity "b" claims GUID 3 --
	// exactly the value a fresh create() would hand out next. Without the
	// advance-past-loaded-GUIDs guard the new entity reuses 3 and two entities
	// answer to the same identity. Entity "a" claims 7 to make the guard's
	// max-scan, not just an off-by-one, the thing under test.
	const std::string text = R"({
	  "version": 2,
	  "name": "loaded-scene",
	  "entities": [
	    {"name": "a", "components": {"SceneGuid": "7"}},
	    {"name": "b", "components": {"SceneGuid": "3"}}
	  ]
	})";

	Scene scene{"s"};
	ASSERT_TRUE(scene_from_json(scene, text, nullptr));
	const Entity a = find_by_name(scene, "a");
	const Entity b = find_by_name(scene, "b");
	ASSERT_NE(a, NullEntity);
	ASSERT_NE(b, NullEntity);
	EXPECT_EQ(scene.guid_of(a).value, 7u);
	EXPECT_EQ(scene.guid_of(b).value, 3u);

	const Entity fresh = scene.create("fresh");
	EXPECT_NE(scene.guid_of(fresh).value, 0u);
	EXPECT_NE(scene.guid_of(fresh).value, scene.guid_of(a).value);
	EXPECT_NE(scene.guid_of(fresh).value, scene.guid_of(b).value)
	    << "a new entity reused a loaded entity's GUID";
	// The property that matters: every GUID maps back to its own entity.
	EXPECT_EQ(scene.find_by_guid(scene.guid_of(a)), a);
	EXPECT_EQ(scene.find_by_guid(scene.guid_of(b)), b);
	EXPECT_EQ(scene.find_by_guid(scene.guid_of(fresh)), fresh);
}

TEST(SceneSerialization, ReferencesResolveFromDiskNotJustInMemory)
{
	// The path that matters: references must be resolved on the scene that was
	// actually committed. Resolving before the move would leave the committed
	// scene's `resolved` fields at their default.
	const std::string text = R"({
	  "version": 2,
	  "name": "refs",
	  "entities": [
	    {"name": "a", "components": {"EntityRef": {"target": "2"}}},
	    {"name": "b", "components": {"SceneGuid": "2"}}
	  ]
	})";

	Scene scene{"s"};
	ASSERT_TRUE(scene_from_json(scene, text, nullptr));
	const Entity a = find_by_name(scene, "a");
	const Entity b = find_by_name(scene, "b");
	ASSERT_NE(a, NullEntity);
	ASSERT_NE(b, NullEntity);

	ASSERT_TRUE(scene.registry().all_of<EntityRef>(a));
	EXPECT_EQ(scene.registry().get<EntityRef>(a).resolved, b)
	    << "the committed scene's reference was never resolved";
	EXPECT_TRUE(scene.reference_errors().empty());
}

TEST(SceneSerialization, LegacyDocumentGetsGuids)
{
	// A version-1 document has no SceneGuid at all; every entity must still be
	// addressable afterwards.
	Scene scene{"s"};
	ASSERT_TRUE(scene_from_json(
	        scene,
	        R"({"version":1,"name":"old","entities":[{"name":"e"}]})",
	        nullptr));

	const Entity e = find_by_name(scene, "e");
	ASSERT_NE(e, NullEntity);
	EXPECT_NE(scene.guid_of(e).value, 0u);
}

namespace {

/// A component the engine knows nothing about, to prove registration is what
/// makes a custom type serializable rather than anything built in.
struct TestTag {
	int value{0};
};

} // namespace

TEST(SceneSerialization, RegisteredComponentRoundTrips)
{
	// Without registration the codec registry has no reader for TestTag, so the
	// component is skipped on load and the round trip silently loses it.
	const std::size_t before = default_component_codecs().names().size();
	EXPECT_EQ(default_component_codecs().reader("TestTag"), nullptr);

	register_component_codec(
	        "TestTag",
	        [](const Registry& r, Entity e, nlohmann::json& j) {
		        const auto* tag = r.try_get<TestTag>(e);
		        if (tag == nullptr) {
			        return false;
		        }
		        j = nlohmann::json{{"value", tag->value}};
		        return true;
	        },
	        [](Registry& r, Entity e, const nlohmann::json& j) {
		        r.emplace_or_replace<TestTag>(
		                e, TestTag{j.value("value", 0)});
	        });

	Scene source{"s"};
	const Entity e = source.create("tagged");
	source.registry().emplace<TestTag>(e, TestTag{42});

	const std::string text = to_json_string(source);
	EXPECT_NE(text.find("\"TestTag\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	const Entity le = find_by_name(loaded, "tagged");
	ASSERT_NE(le, NullEntity);
	ASSERT_TRUE(loaded.registry().all_of<TestTag>(le));
	EXPECT_EQ(loaded.registry().get<TestTag>(le).value, 42);

	// Registration adds one name and disturbs nothing already registered.
	EXPECT_EQ(default_component_codecs().names().size(), before + 1);
	EXPECT_NE(default_component_codecs().reader("Transform"), nullptr);
	EXPECT_NE(default_component_codecs().reader("SceneGuid"), nullptr);
}

TEST(SceneSerialization, Version2DocumentMigratesToCurrent)
{
	// A v2 document has no asset ids and no settings block. It must load under
	// v3 with the pre-v3 meaning: resolve by path, keep the globals.
	const std::string text = R"({
	  "version": 2,
	  "name": "old",
	  "entities": [
	    {"name": "mesh", "components": {
	      "MeshSource": {"path": "builtin:cube"}}}
	  ]
	})";

	Scene scene{"s"};
	std::string error;
	ASSERT_TRUE(scene_from_json(scene, text, &error)) << error;
	const Entity e = find_by_name(scene, "mesh");
	ASSERT_NE(e, NullEntity);
	ASSERT_TRUE(scene.registry().all_of<MeshSource>(e));
	EXPECT_EQ(scene.registry().get<MeshSource>(e).path, "builtin:cube");
	EXPECT_TRUE(scene.registry().get<MeshSource>(e).id.empty());
	EXPECT_FALSE(scene.has_settings());
}

TEST(SceneSerialization, AssetIdRoundTripsAndIsOptional)
{
	Scene source{"s"};
	const Entity a = source.create("keyed");
	source.registry().emplace<MeshSource>(a, MeshSource{"assets/moved.glb", "hero"});
	const Entity b = source.create("bare");
	source.registry().emplace<MeshSource>(b, MeshSource{"builtin:cube", ""});

	const std::string text = to_json_string(source);
	// The id is written when set and omitted when not, so a v2-shaped document
	// stays v2-shaped apart from the version number.
	EXPECT_NE(text.find("\"id\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	EXPECT_EQ(loaded.registry().get<MeshSource>(find_by_name(loaded, "keyed")).id,
	          "hero");
	EXPECT_TRUE(loaded.registry().get<MeshSource>(find_by_name(loaded, "bare"))
	                    .id.empty());
}

TEST(SceneSerialization, SceneSettingsRoundTripAndAreOptional)
{
	Scene with{"s"};
	with.set_settings(SceneSettings{{0.0f, -1.0f, 0.0f},
	                                {1.0f, 0.5f, 0.25f},
	                                2.5f,
	                                {0.4f, 0.3f, 0.2f},
	                                0.75f});

	const std::string text = to_json_string(with);
	EXPECT_NE(text.find("\"settings\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"exposure\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	ASSERT_TRUE(loaded.has_settings());
	EXPECT_FLOAT_EQ(loaded.settings().light_intensity, 2.5f);
	EXPECT_FLOAT_EQ(loaded.settings().light_direction.y, -1.0f);
	EXPECT_FLOAT_EQ(loaded.settings().ambient.z, 0.2f);
	EXPECT_FLOAT_EQ(loaded.settings().exposure, 0.75f);

	// A document without settings must not claim to have them, or loading it
	// would silently override the renderer's globals.
	Scene plain{"s"};
	ASSERT_TRUE(scene_from_json(
	        plain, R"({"version":3,"name":"p","entities":[]})", nullptr));
	EXPECT_FALSE(plain.has_settings());
	EXPECT_EQ(to_json_string(plain).find("\"settings\""), std::string::npos);
}

TEST(SceneSerialization, OldV3SettingsWithoutExposureLoadNeutral)
{
	// Documents written before exposure existed carry no "exposure" key.
	// They must still load, with a neutral 1.0 exposure.
	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded,
	                            R"({"version":3,"name":"p","settings":{)"
	                            R"("light_direction":[0.0,-1.0,0.0],)"
	                            R"("light_color":[1.0,1.0,1.0],)"
	                            R"("light_intensity":2.0,)"
	                            R"("ambient":[0.1,0.1,0.1])"
	                            R"(},"entities":[]})",
	                            nullptr));
	ASSERT_TRUE(loaded.has_settings());
	EXPECT_FLOAT_EQ(loaded.settings().exposure, 1.0f);
	EXPECT_FLOAT_EQ(loaded.settings().light_intensity, 2.0f);
	EXPECT_FLOAT_EQ(loaded.settings().light_direction.y, -1.0f);
}

TEST(SceneSerialization, NewSettingsKeysRoundTrip)
{
	// Non-default hemisphere colours and toggles survive save/load.
	Scene with{"s"};
	SceneSettings settings;
	settings.sky_color       = {0.25f, 0.35f, 0.5f};
	settings.ground_color    = {0.1f, 0.08f, 0.06f};
	settings.shadows_enabled = false;
	settings.fxaa_enabled    = false;
	with.set_settings(settings);

	const std::string text = to_json_string(with);
	EXPECT_NE(text.find("\"sky_color\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"ground_color\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"shadows_enabled\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"fxaa_enabled\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	ASSERT_TRUE(loaded.has_settings());
	EXPECT_FLOAT_EQ(loaded.settings().sky_color.x, 0.25f);
	EXPECT_FLOAT_EQ(loaded.settings().sky_color.y, 0.35f);
	EXPECT_FLOAT_EQ(loaded.settings().sky_color.z, 0.5f);
	EXPECT_FLOAT_EQ(loaded.settings().ground_color.x, 0.1f);
	EXPECT_FALSE(loaded.settings().shadows_enabled);
	EXPECT_FALSE(loaded.settings().fxaa_enabled);
}

TEST(SceneSerialization, OldV3SettingsWithoutRealismKeysKeepDefaults)
{
	// Documents written before the realism slice carry none of the new keys.
	// They must still load: hemisphere == legacy floor, both features on.
	// No version bump -- the keys are optional within v3.
	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded,
	                            R"({"version":3,"name":"p","settings":{)"
	                            R"("light_direction":[0.0,-1.0,0.0],)"
	                            R"("light_color":[1.0,1.0,1.0],)"
	                            R"("light_intensity":2.0,)"
	                            R"("ambient":[0.1,0.1,0.1],)"
	                            R"("exposure":1.0)"
	                            R"(},"entities":[]})",
	                            nullptr));
	ASSERT_TRUE(loaded.has_settings());
	const SceneSettings defaults;
	EXPECT_FLOAT_EQ(loaded.settings().sky_color.x, defaults.sky_color.x);
	EXPECT_FLOAT_EQ(loaded.settings().sky_color.y, defaults.sky_color.y);
	EXPECT_FLOAT_EQ(loaded.settings().sky_color.z, defaults.sky_color.z);
	EXPECT_FLOAT_EQ(loaded.settings().ground_color.x,
	                defaults.ground_color.x);
	EXPECT_TRUE(loaded.settings().shadows_enabled);
	EXPECT_TRUE(loaded.settings().fxaa_enabled);
	// And the saved form still declares version 3.
	EXPECT_NE(to_json_string(loaded).find("\"version\": 3"),
	          std::string::npos);
}

TEST(SceneSerialization, SkySettingsRoundTrip)
{
	// The procedural-sky slice survives save/load, including off.
	Scene with{"s"};
	SceneSettings settings;
	settings.sky_enabled   = false;
	settings.horizon_warmth = 0.5f;
	with.set_settings(settings);

	const std::string text = to_json_string(with);
	EXPECT_NE(text.find("\"sky_enabled\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"horizon_warmth\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	ASSERT_TRUE(loaded.has_settings());
	EXPECT_FALSE(loaded.settings().sky_enabled);
	EXPECT_FLOAT_EQ(loaded.settings().horizon_warmth, 0.5f);
}

TEST(SceneSerialization, OldV3SettingsWithoutSkyKeysKeepDefaults)
{
	// Documents written before the sky slice carry neither new key. They
	// must still load with the sky enabled at neutral warmth (no black
	// void), and the saved form still declares version 3: the keys are
	// optional within v3, never a version bump.
	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded,
	                            R"({"version":3,"name":"p","settings":{)"
	                            R"("light_direction":[0.0,-1.0,0.0],)"
	                            R"("light_color":[1.0,1.0,1.0],)"
	                            R"("light_intensity":2.0,)"
	                            R"("ambient":[0.1,0.1,0.1],)"
	                            R"("exposure":1.0,)"
	                            R"("sky_color":[0.25,0.35,0.5],)"
	                            R"("ground_color":[0.1,0.08,0.06],)"
	                            R"("shadows_enabled":true,)"
	                            R"("fxaa_enabled":true)"
	                            R"(},"entities":[]})",
	                            nullptr));
	ASSERT_TRUE(loaded.has_settings());
	const SceneSettings defaults;
	EXPECT_TRUE(loaded.settings().sky_enabled);
	EXPECT_FLOAT_EQ(loaded.settings().horizon_warmth,
	                defaults.horizon_warmth);
	EXPECT_NE(to_json_string(loaded).find("\"version\": 3"),
	          std::string::npos);
}

TEST(SceneSerialization, RenderBundleSettingsRoundTrip)
{
	// Fog, bounce fill, and SSAO survive save/load, including off.
	Scene with{"s"};
	SceneSettings settings;
	settings.fog_enabled      = false;
	settings.fog_density      = 0.05f;
	settings.bounce_enabled   = false;
	settings.bounce_color     = {0.5f, 0.3f, 0.2f};
	settings.bounce_intensity = 0.7f;
	settings.ssao_enabled     = false;
	with.set_settings(settings);

	const std::string text = to_json_string(with);
	EXPECT_NE(text.find("\"fog_enabled\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"fog_density\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"bounce_enabled\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"bounce_color\""), std::string::npos) << text;
	EXPECT_NE(text.find("\"bounce_intensity\""), std::string::npos)
	        << text;
	EXPECT_NE(text.find("\"ssao_enabled\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	ASSERT_TRUE(loaded.has_settings());
	EXPECT_FALSE(loaded.settings().fog_enabled);
	EXPECT_FLOAT_EQ(loaded.settings().fog_density, 0.05f);
	EXPECT_FALSE(loaded.settings().bounce_enabled);
	EXPECT_FLOAT_EQ(loaded.settings().bounce_color.x, 0.5f);
	EXPECT_FLOAT_EQ(loaded.settings().bounce_intensity, 0.7f);
	EXPECT_FALSE(loaded.settings().ssao_enabled);
}

TEST(SceneSerialization, OldV3SettingsWithoutBundleKeysKeepDefaults)
{
	// Documents written before the render-upgrade bundle carry none of the
	// new keys. They must still load with fog/bounce/SSAO on (sane, not
	// black), and the saved form still declares version 3: the keys are
	// optional within v3, never a version bump.
	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded,
	                            R"({"version":3,"name":"p","settings":{)"
	                            R"("light_direction":[0.0,-1.0,0.0],)"
	                            R"("light_color":[1.0,1.0,1.0],)"
	                            R"("light_intensity":2.0,)"
	                            R"("ambient":[0.1,0.1,0.1],)"
	                            R"("exposure":1.0,)"
	                            R"("sky_color":[0.25,0.35,0.5],)"
	                            R"("ground_color":[0.1,0.08,0.06],)"
	                            R"("shadows_enabled":true,)"
	                            R"("fxaa_enabled":true,)"
	                            R"("sky_enabled":true,)"
	                            R"("horizon_warmth":1.0)"
	                            R"(},"entities":[]})",
	                            nullptr));
	ASSERT_TRUE(loaded.has_settings());
	const SceneSettings defaults;
	EXPECT_TRUE(loaded.settings().fog_enabled);
	EXPECT_FLOAT_EQ(loaded.settings().fog_density, defaults.fog_density);
	EXPECT_TRUE(loaded.settings().bounce_enabled);
	EXPECT_FLOAT_EQ(loaded.settings().bounce_color.x,
	                defaults.bounce_color.x);
	EXPECT_FLOAT_EQ(loaded.settings().bounce_intensity,
	                defaults.bounce_intensity);
	EXPECT_TRUE(loaded.settings().ssao_enabled);
	EXPECT_NE(to_json_string(loaded).find("\"version\": 3"),
	          std::string::npos);
}

TEST(SceneSerialization, BlobCasterRoundTrips)
{
	Scene with{"s"};
	const Entity e = with.create("blob");
	with.registry().emplace<BlobCaster>(e, BlobCaster{0.75f});

	const std::string text = to_json_string(with);
	EXPECT_NE(text.find("\"BlobCaster\""), std::string::npos) << text;

	Scene loaded{"s"};
	ASSERT_TRUE(scene_from_json(loaded, text, nullptr));
	const Entity found = loaded.find("blob");
	ASSERT_NE(found, NullEntity);
	ASSERT_TRUE(loaded.registry().all_of<BlobCaster>(found));
	EXPECT_FLOAT_EQ(loaded.registry().get<BlobCaster>(found).radius,
	                0.75f);
	// Center comes from the Transform, gathered per frame (up to 4).
	const std::vector<glm::vec4> spheres = loaded.blob_spheres();
	ASSERT_EQ(spheres.size(), 1u);
	EXPECT_FLOAT_EQ(spheres[0].w, 0.75f);
}
