#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include "lotus/scene/scene.hpp"
#include "lotus/scene/scene_manager.hpp"

using namespace lotus;

namespace {

glm::vec3 world_position(const Scene& scene, Entity entity)
{
	return glm::vec3{scene.registry().get<Transform>(entity).world[3]};
}

} // namespace

TEST(SceneHierarchy, NewEntityHasCoreComponents)
{
	Scene scene{"test"};
	const Entity e = scene.create("e");
	EXPECT_TRUE(scene.registry().all_of<Transform>(e));
	EXPECT_TRUE(scene.registry().all_of<Hierarchy>(e));
	EXPECT_TRUE(scene.registry().all_of<Name>(e));
	EXPECT_EQ(scene.registry().get<Name>(e).value, "e");
}

TEST(SceneHierarchy, RootsExcludeChildren)
{
	Scene scene{"test"};
	const Entity parent = scene.create("parent");
	const Entity child  = scene.create("child");
	scene.set_parent(child, parent);

	const auto roots = scene.roots();
	ASSERT_EQ(roots.size(), 1u);
	EXPECT_EQ(roots[0], parent);
}

TEST(SceneHierarchy, ChildWorldFollowsParent)
{
	Scene scene{"test"};
	const Entity parent = scene.create("parent");
	const Entity child  = scene.create("child");

	scene.registry().get<Transform>(parent).position = {2.0f, 0.0f, 0.0f};
	scene.registry().get<Transform>(child).position  = {1.0f, 0.0f, 0.0f};
	scene.set_parent(child, parent);

	scene.update(0.0f);

	EXPECT_FLOAT_EQ(world_position(scene, parent).x, 2.0f);
	EXPECT_FLOAT_EQ(world_position(scene, child).x, 3.0f);
}

TEST(SceneHierarchy, GrandchildAccumulates)
{
	Scene scene{"test"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	const Entity c = scene.create("c");

	scene.registry().get<Transform>(a).position = {1.0f, 0.0f, 0.0f};
	scene.registry().get<Transform>(b).position = {1.0f, 0.0f, 0.0f};
	scene.registry().get<Transform>(c).position = {1.0f, 0.0f, 0.0f};
	scene.set_parent(b, a);
	scene.set_parent(c, b);

	scene.update(0.0f);

	EXPECT_FLOAT_EQ(world_position(scene, c).x, 3.0f);
}

TEST(SceneHierarchy, ReparentRecomputesWorld)
{
	Scene scene{"test"};
	const Entity parent_a = scene.create("a");
	const Entity parent_b = scene.create("b");
	const Entity child    = scene.create("child");

	scene.registry().get<Transform>(parent_a).position = {10.0f, 0.0f, 0.0f};
	scene.registry().get<Transform>(parent_b).position = {-5.0f, 0.0f, 0.0f};
	scene.set_parent(child, parent_a);
	scene.update(0.0f);
	EXPECT_FLOAT_EQ(world_position(scene, child).x, 10.0f);

	scene.set_parent(child, parent_b);
	scene.update(0.0f);
	EXPECT_FLOAT_EQ(world_position(scene, child).x, -5.0f);
}

TEST(SceneHierarchy, DetachReturnsEntityToRoot)
{
	Scene scene{"test"};
	const Entity parent = scene.create("parent");
	const Entity child  = scene.create("child");
	scene.set_parent(child, parent);

	scene.detach(child);
	scene.registry().get<Transform>(parent).position = {7.0f, 0.0f, 0.0f};
	scene.update(0.0f);

	EXPECT_EQ(scene.roots().size(), 2u);
	EXPECT_FLOAT_EQ(world_position(scene, child).x, 0.0f);
}

TEST(SceneHierarchy, CyclesAreRejected)
{
	Scene scene{"test"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	scene.set_parent(b, a);

	// a cannot become a child of its own descendant.
	scene.set_parent(a, b);

	EXPECT_EQ(scene.roots().size(), 1u);
	EXPECT_EQ(scene.registry().get<Hierarchy>(a).parent, NullEntity);
}

TEST(SceneHierarchy, DestroyIsRecursive)
{
	Scene scene{"test"};
	const Entity root  = scene.create("root");
	const Entity child = scene.create("child");
	const Entity grand = scene.create("grand");
	scene.set_parent(child, root);
	scene.set_parent(grand, child);

	scene.destroy(root);

	EXPECT_FALSE(scene.registry().valid(root));
	EXPECT_FALSE(scene.registry().valid(child));
	EXPECT_FALSE(scene.registry().valid(grand));
	EXPECT_TRUE(scene.roots().empty());
}

TEST(SceneHierarchy, DestroyChildKeepsParent)
{
	Scene scene{"test"};
	const Entity root  = scene.create("root");
	const Entity child = scene.create("child");
	scene.set_parent(child, root);

	scene.destroy(child);

	EXPECT_TRUE(scene.registry().valid(root));
	EXPECT_EQ(scene.registry().get<Hierarchy>(root).first_child, NullEntity);
}

TEST(SceneManagerTest, CreateGetActivate)
{
	SceneManager manager;
	Scene& a = manager.create("a");
	Scene& b = manager.create("b");
	EXPECT_EQ(manager.count(), 2u);
	EXPECT_EQ(manager.get("a"), &a);
	EXPECT_EQ(manager.get("b"), &b);

	EXPECT_EQ(manager.active(), &a); // first created becomes active
	manager.activate("b");
	EXPECT_EQ(manager.active(), &b);
	EXPECT_EQ(manager.active_name(), "b");
}

TEST(SceneManagerTest, DestroyActiveFallsBack)
{
	SceneManager manager;
	manager.create("a");
	manager.create("b");
	manager.activate("b");

	EXPECT_TRUE(manager.destroy("b"));
	EXPECT_EQ(manager.count(), 1u);
	ASSERT_NE(manager.active(), nullptr);
	EXPECT_EQ(manager.active_name(), "a");
}

TEST(SceneManagerTest, ActivateCreatesWhenMissing)
{
	SceneManager manager;
	Scene& scene = manager.activate("fresh");
	EXPECT_EQ(manager.count(), 1u);
	EXPECT_EQ(manager.active(), &scene);
}

TEST(SceneManagerTest, NamesAreSortedAndStable)
{
	SceneManager manager;
	manager.create("zulu");
	manager.create("alpha");
	manager.create("mike");

	// The header promises sorted order; unordered_map iteration is arbitrary,
	// so without the sort this is whatever bucket order the map produced.
	const std::vector<std::string> names = manager.names();
	ASSERT_EQ(names.size(), 3u);
	EXPECT_EQ(names[0], "alpha");
	EXPECT_EQ(names[1], "mike");
	EXPECT_EQ(names[2], "zulu");
}

TEST(SceneIdentity, ClearRestartsGuidAllocation)
{
	Scene scene{"s"};
	const Entity first = scene.create("first");
	const uint64_t before = scene.guid_of(first).value;
	ASSERT_NE(before, 0u);

	scene.clear();
	EXPECT_EQ(scene.registry().view<Name>().size(), 0u);

	// A cleared scene is a fresh one: the counter restarts, so GUIDs do not
	// climb forever across a reset. Deleting the reset in Scene::clear leaves
	// this failing, which is the point.
	const Entity after = scene.create("after");
	EXPECT_EQ(scene.guid_of(after).value, 1u);
}

TEST(SceneIdentity, GuidSurvivesReordering)
{
	Scene scene{"s"};
	const Entity a = scene.create("a");
	const Entity b = scene.create("b");
	const SceneGuid guid_b = scene.guid_of(b);
	EXPECT_NE(guid_b.value, scene.guid_of(a).value);
	EXPECT_EQ(scene.find_by_guid(guid_b), b);

	// Destroying an earlier entity must not disturb the identity of a later one.
	scene.destroy(a);
	EXPECT_EQ(scene.find_by_guid(guid_b), b);
	EXPECT_EQ(scene.guid_of(b).value, guid_b.value);
}
