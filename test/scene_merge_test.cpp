#include <gtest/gtest.h>

#include <string>

#include "lotus/scene/prefab.hpp"
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

std::size_t count_named(Scene& scene, const std::string& name)
{
	std::size_t n = 0;
	for (const Entity e : scene.registry().view<Name>()) {
		if (scene.registry().get<Name>(e).value == name) {
			++n;
		}
	}
	return n;
}

/// A document with two entities and a reference from the second to the first.
const char* kTwoEntityScene = R"({
  "version": 2,
  "name": "incoming",
  "entities": [
    {"name": "alpha", "components": {
      "SceneGuid": "1",
      "Material": {"base_color": [0.1, 0.2, 0.3]}}},
    {"name": "beta", "components": {
      "SceneGuid": "2",
      "EntityRef": {"target": "1"}}}
  ]
})";

} // namespace

TEST(SceneMergeTest, MergesTheUnionOfEntities)
{
	Scene target{"live"};
	const Entity existing = target.create("existing");
	const SceneGuid existing_guid = target.guid_of(existing);

	std::string error;
	ASSERT_TRUE(load_scene_additive(target, kTwoEntityScene, &error)) << error;

	// Union, not replacement: the pre-existing entity survives untouched.
	EXPECT_EQ(count_named(target, "existing"), 1u);
	EXPECT_EQ(count_named(target, "alpha"), 1u);
	EXPECT_EQ(count_named(target, "beta"), 1u);
	EXPECT_EQ(target.guid_of(find_by_name(target, "existing")).value,
	          existing_guid.value);

	// Merged components came across.
	const Entity alpha = find_by_name(target, "alpha");
	ASSERT_NE(alpha, NullEntity);
	ASSERT_TRUE(target.registry().all_of<Material>(alpha));
	EXPECT_FLOAT_EQ(target.registry().get<Material>(alpha).base_color.y, 0.2f);
}

TEST(SceneMergeTest, MergedEntitiesGetFreshGuids)
{
	Scene target{"live"};
	ASSERT_TRUE(load_scene_additive(target, kTwoEntityScene, nullptr));

	const Entity alpha = find_by_name(target, "alpha");
	const Entity beta  = find_by_name(target, "beta");
	ASSERT_NE(alpha, NullEntity);
	ASSERT_NE(beta, NullEntity);

	// Not the document's GUIDs: those were 1 and 2, and the target's own
	// counter already passed them.
	EXPECT_NE(target.guid_of(alpha).value, 0u);
	EXPECT_NE(target.guid_of(beta).value, 0u);
	EXPECT_NE(target.guid_of(alpha).value, target.guid_of(beta).value);
	EXPECT_EQ(target.find_by_guid(target.guid_of(alpha)), alpha);
	EXPECT_EQ(target.find_by_guid(target.guid_of(beta)), beta);
}

TEST(SceneMergeTest, InternalReferencesPointWithinTheMergedCopies)
{
	Scene target{"live"};
	ASSERT_TRUE(load_scene_additive(target, kTwoEntityScene, nullptr));

	const Entity beta = find_by_name(target, "beta");
	ASSERT_NE(beta, NullEntity);
	ASSERT_TRUE(target.registry().all_of<EntityRef>(beta));
	EXPECT_EQ(target.registry().get<EntityRef>(beta).resolved,
	          find_by_name(target, "alpha"));
	EXPECT_TRUE(target.reference_errors().empty());
}

TEST(SceneMergeTest, MergingTheSameDocumentTwiceIsIndependent)
{
	Scene target{"live"};
	ASSERT_TRUE(load_scene_additive(target, kTwoEntityScene, nullptr));
	ASSERT_TRUE(load_scene_additive(target, kTwoEntityScene, nullptr));

	ASSERT_EQ(count_named(target, "alpha"), 2u);
	ASSERT_EQ(count_named(target, "beta"), 2u);

	// Each beta references an alpha, and the two alphas are distinct entities.
	std::vector<Entity> resolved;
	for (const Entity e : target.registry().view<EntityRef>()) {
		resolved.push_back(target.registry().get<EntityRef>(e).resolved);
	}
	ASSERT_EQ(resolved.size(), 2u);
	EXPECT_NE(resolved[0], resolved[1])
	    << "both merged copies resolved to the same entity";
	EXPECT_TRUE(target.reference_errors().empty());

	// And the two alphas have different identities.
	std::vector<uint64_t> guids;
	for (const Entity e : target.registry().view<Name>()) {
		if (target.registry().get<Name>(e).value == "alpha") {
			guids.push_back(target.guid_of(e).value);
		}
	}
	ASSERT_EQ(guids.size(), 2u);
	EXPECT_NE(guids[0], guids[1]);
}

TEST(SceneMergeTest, MalformedDocumentMergesNothing)
{
	Scene target{"live"};
	const Entity survivor = target.create("survivor");
	const SceneGuid survivor_guid = target.guid_of(survivor);
	const std::size_t before = target.registry().view<Name>().size();

	std::string error;
	// entities must be an array: rejected during validation, before any merge.
	EXPECT_FALSE(load_scene_additive(
	        target, R"({"version":2,"name":"bad","entities":42})", &error));
	EXPECT_FALSE(error.empty());

	EXPECT_EQ(target.registry().view<Name>().size(), before);
	EXPECT_EQ(find_by_name(target, "survivor"), survivor);
	EXPECT_EQ(target.guid_of(survivor).value, survivor_guid.value);
}

TEST(SceneMergeTest, MergedHierarchyIsPreserved)
{
	const char* text = R"({
	  "version": 2,
	  "name": "tree",
	  "entities": [
	    {"name": "root", "components": {"SceneGuid": "1"},
	     "children": [
	       {"name": "leaf", "components": {"SceneGuid": "2"}}
	     ]}
	  ]
	})";

	Scene target{"live"};
	ASSERT_TRUE(load_scene_additive(target, text, nullptr));

	const Entity root = find_by_name(target, "root");
	const Entity leaf = find_by_name(target, "leaf");
	ASSERT_NE(root, NullEntity);
	ASSERT_NE(leaf, NullEntity);
	EXPECT_EQ(target.registry().get<Hierarchy>(leaf).parent, root);
	EXPECT_EQ(target.registry().get<Hierarchy>(root).parent, NullEntity);
}

TEST(SceneMergeTest, FileRoundTripsThroughAdditiveLoad)
{
	const std::string path = "scene_merge_test.json";
	Scene source{"incoming"};
	const Entity e = source.create("from_file");
	source.registry().get<Transform>(e).position = {1.0f, 2.0f, 3.0f};

	std::string error;
	ASSERT_TRUE(save_scene(source, path, &error)) << error;

	Scene target{"live"};
	target.create("pre_existing");
	ASSERT_TRUE(load_scene_additive_from_file(target, path, &error)) << error;

	EXPECT_NE(find_by_name(target, "pre_existing"), NullEntity);
	const Entity loaded = find_by_name(target, "from_file");
	ASSERT_NE(loaded, NullEntity);
	EXPECT_FLOAT_EQ(target.registry().get<Transform>(loaded).position.z, 3.0f);

	std::remove(path.c_str());
}
