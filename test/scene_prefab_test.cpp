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

/// A fragment with a parent, a child, and a reference from the child to the
/// parent, so hierarchy and internal-reference rewriting are both exercised.
const char* kTwoEntityPrefab = R"({
  "version": 2,
  "name": "rig",
  "entities": [
    {"name": "body", "components": {
      "SceneGuid": "1",
      "Material": {"base_color": [1.0, 0.0, 0.0]},
      "Transform": {"position": [0.0, 0.0, 0.0]}},
     "children": [
       {"name": "head", "components": {
         "SceneGuid": "2",
         "EntityRef": {"target": "1"},
         "Transform": {"position": [0.0, 1.0, 0.0]}}}
     ]}
  ]
})";

} // namespace

TEST(PrefabTest, ParsesAFragmentAndKeepsHierarchy)
{
	Prefab prefab;
	std::string error;
	ASSERT_TRUE(prefab_from_json(prefab, kTwoEntityPrefab, &error)) << error;
	EXPECT_EQ(prefab.name, "rig");
	ASSERT_TRUE(prefab.document.contains("entities"));
	EXPECT_EQ(prefab.document.at("entities").size(), 1u);
	// The nested child must survive parsing, or instancing loses the tree.
	EXPECT_TRUE(prefab.document.at("entities").at(0).contains("children"));
}

TEST(PrefabTest, RejectsAFutureVersion)
{
	Prefab prefab;
	std::string error;
	EXPECT_FALSE(prefab_from_json(
	        prefab, R"({"version":99,"name":"x","entities":[]})", &error));
	EXPECT_FALSE(error.empty());
}

TEST(PrefabTest, InstantiatesHierarchyAndComponents)
{
	Prefab prefab;
	ASSERT_TRUE(prefab_from_json(prefab, kTwoEntityPrefab, nullptr));

	Scene scene{"level"};
	const Entity root = instantiate(prefab, scene);
	ASSERT_NE(root, NullEntity);

	const Entity body = find_by_name(scene, "body");
	const Entity head = find_by_name(scene, "head");
	ASSERT_NE(body, NullEntity);
	ASSERT_NE(head, NullEntity);

	// Hierarchy survived: head is a child of body, not a sibling.
	EXPECT_EQ(scene.registry().get<Hierarchy>(head).parent, body);
	EXPECT_EQ(scene.registry().get<Hierarchy>(body).parent, NullEntity);
	EXPECT_EQ(root, body);

	// Components were copied through the codec, including ones the milestone
	// does not name explicitly.
	ASSERT_TRUE(scene.registry().all_of<Material>(body));
	EXPECT_FLOAT_EQ(scene.registry().get<Material>(body).base_color.x, 1.0f);
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(head).position.y, 1.0f);
}

TEST(PrefabTest, TwoInstancesAreIndependent)
{
	Prefab prefab;
	ASSERT_TRUE(prefab_from_json(prefab, kTwoEntityPrefab, nullptr));

	Scene scene{"level"};
	ASSERT_NE(instantiate(prefab, scene), NullEntity);
	ASSERT_NE(instantiate(prefab, scene), NullEntity);

	// Two bodies, two heads, four distinct GUIDs: the second instance must not
	// reuse the first's identity.
	std::vector<Entity> bodies;
	std::vector<Entity> heads;
	for (const Entity e : scene.registry().view<Name>()) {
		if (scene.registry().get<Name>(e).value == "body") {
			bodies.push_back(e);
		} else if (scene.registry().get<Name>(e).value == "head") {
			heads.push_back(e);
		}
	}
	ASSERT_EQ(bodies.size(), 2u);
	ASSERT_EQ(heads.size(), 2u);
	EXPECT_NE(scene.guid_of(bodies[0]).value, scene.guid_of(bodies[1]).value);
	EXPECT_NE(scene.guid_of(heads[0]).value, scene.guid_of(heads[1]).value);
	EXPECT_EQ(scene.find_by_guid(scene.guid_of(bodies[0])), bodies[0]);
	EXPECT_EQ(scene.find_by_guid(scene.guid_of(bodies[1])), bodies[1]);

	// Mutating one instance must not touch the other.
	scene.registry().get<Transform>(bodies[0]).position.x = 5.0f;
	EXPECT_FLOAT_EQ(scene.registry().get<Transform>(bodies[1]).position.x, 0.0f);
}

TEST(PrefabTest, InternalReferencesPointWithinTheirOwnInstance)
{
	Prefab prefab;
	ASSERT_TRUE(prefab_from_json(prefab, kTwoEntityPrefab, nullptr));

	Scene scene{"level"};
	ASSERT_NE(instantiate(prefab, scene), NullEntity);
	ASSERT_NE(instantiate(prefab, scene), NullEntity);

	// Each head references the body of its own instance, not the first one.
	std::vector<Entity> bodies;
	for (const Entity e : scene.registry().view<Name>()) {
		if (scene.registry().get<Name>(e).value == "body") {
			bodies.push_back(e);
		}
	}
	ASSERT_EQ(bodies.size(), 2u);

	for (const Entity head : scene.registry().view<EntityRef>()) {
		const EntityRef& ref = scene.registry().get<EntityRef>(head);
		const Entity owner_parent = scene.registry().get<Hierarchy>(head).parent;
		EXPECT_EQ(ref.resolved, owner_parent)
		    << "a head references a body outside its own instance";
		EXPECT_NE(ref.resolved, NullEntity);
	}
	EXPECT_TRUE(scene.reference_errors().empty());
}

TEST(PrefabTest, CapturesASubtreeFromAScene)
{
	Scene scene{"level"};
	const Entity root  = scene.create("root");
	const Entity child = scene.create("child");
	scene.set_parent(child, root);
	scene.registry().emplace<Material>(root, Material{{0.25f, 0.5f, 0.75f}});

	const Prefab prefab = prefab_from_scene(scene, root);
	ASSERT_TRUE(prefab.document.contains("entities"));
	EXPECT_EQ(prefab.document.at("entities").size(), 1u);
	EXPECT_EQ(prefab.name, "root");

	// Round-trip: instantiate the captured prefab into a fresh scene.
	Scene other{"other"};
	const Entity clone = instantiate(prefab, other);
	ASSERT_NE(clone, NullEntity);
	const Entity cloned_child = find_by_name(other, "child");
	ASSERT_NE(cloned_child, NullEntity);
	EXPECT_EQ(other.registry().get<Hierarchy>(cloned_child).parent, clone);
	EXPECT_FLOAT_EQ(other.registry().get<Material>(clone).base_color.z, 0.75f);
}

TEST(PrefabTest, EmptyPrefabInstantiatesNothing)
{
	Prefab prefab;
	ASSERT_TRUE(prefab_from_json(
	        prefab, R"({"version":2,"name":"empty","entities":[]})", nullptr));
	Scene scene{"level"};
	EXPECT_EQ(instantiate(prefab, scene), NullEntity);
	EXPECT_EQ(scene.registry().view<Name>().size(), 0u);
}

TEST(PrefabTest, MalformedPrefabLeavesTheSceneUntouched)
{
	Prefab prefab;
	// entities must be an array; parsing must reject it rather than half-load.
	std::string error;
	EXPECT_FALSE(prefab_from_json(
	        prefab, R"({"version":2,"name":"bad","entities":42})", &error));
	EXPECT_FALSE(error.empty());

	Scene scene{"level"};
	const Entity survivor = scene.create("survivor");
	const std::size_t before = scene.registry().view<Name>().size();
	EXPECT_EQ(instantiate(prefab, scene), NullEntity);
	EXPECT_EQ(scene.registry().view<Name>().size(), before);
	EXPECT_EQ(find_by_name(scene, "survivor"), survivor);
}

TEST(PrefabTest, InternalReferenceToANonExistentGuidIsReported)
{
	// Pins the semantics: an EntityRef whose target is not in the fragment and
	// not in the host scene is kept as-is and reported, never silently dropped.
	Prefab prefab;
	ASSERT_TRUE(prefab_from_json(
	        prefab,
	        R"({"version":2,"name":"dangling","entities":[
	             {"name":"solo","components":{
	               "SceneGuid":"1","EntityRef":{"target":"ffff"}}}]})",
	        nullptr));

	Scene scene{"level"};
	const Entity solo = instantiate(prefab, scene);
	ASSERT_NE(solo, NullEntity);
	ASSERT_TRUE(scene.registry().all_of<EntityRef>(solo));
	EXPECT_EQ(scene.registry().get<EntityRef>(solo).resolved, NullEntity);
	ASSERT_EQ(scene.reference_errors().size(), 1u);
}

TEST(PrefabTest, NestedPrefabContentInstantiatesAsOneTree)
{
	// "Nesting" today means a prefab whose document contains a subtree that was
	// itself captured from another prefab: it is flattened into one document at
	// capture time, so instantiation is a single clone. Pinned so the shape
	// cannot change silently.
	Scene source{"level"};
	const Entity inner = source.create("inner");
	const Entity leaf  = source.create("leaf");
	source.set_parent(leaf, inner);
	source.registry().emplace<EntityRef>(leaf, EntityRef{source.guid_of(inner)});

	// Capture the inner subtree, then wrap it under a new outer root.
	const Prefab inner_prefab = prefab_from_scene(source, inner);
	Scene host{"host"};
	const Entity outer = host.create("outer");
	ASSERT_NE(instantiate(inner_prefab, host, outer), NullEntity);

	// Re-capture the whole thing: outer -> inner -> leaf, reference intact.
	const Prefab outer_prefab = prefab_from_scene(host, outer);
	Scene fresh{"fresh"};
	const Entity clone = instantiate(outer_prefab, fresh);
	ASSERT_NE(clone, NullEntity);

	const Entity c_outer = find_by_name(fresh, "outer");
	const Entity c_inner = find_by_name(fresh, "inner");
	const Entity c_leaf  = find_by_name(fresh, "leaf");
	ASSERT_NE(c_outer, NullEntity);
	ASSERT_NE(c_inner, NullEntity);
	ASSERT_NE(c_leaf, NullEntity);

	// The whole chain survived two capture/instantiate round trips.
	EXPECT_EQ(fresh.registry().get<Hierarchy>(c_inner).parent, c_outer);
	EXPECT_EQ(fresh.registry().get<Hierarchy>(c_leaf).parent, c_inner);
	EXPECT_EQ(fresh.registry().get<EntityRef>(c_leaf).resolved, c_inner);
	EXPECT_TRUE(fresh.reference_errors().empty());
}
