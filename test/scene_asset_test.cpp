#include <gtest/gtest.h>

#include "lotus/scene/assets.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(SceneAssetTest, MeshResolvesThroughRegistryKey)
{
	AssetRegistry registry;
	registry.set("hero_mesh", "builtin:cube");

	Scene scene{"test"};
	scene.set_assets(&registry);
	const Entity entity = scene.create("mesh");
	scene.registry().emplace<MeshSource>(
	        entity, MeshSource{"builtin:sphere", "hero_mesh"});

	scene.update(0.0f);

	ASSERT_TRUE(scene.registry().all_of<Mesh>(entity));
	const Mesh& mesh = scene.registry().get<Mesh>(entity);
	EXPECT_EQ(mesh.source, "builtin:cube");
	EXPECT_EQ(mesh.indices.size(), 36u);
	EXPECT_TRUE(scene.last_mesh_error().empty());
}

TEST(SceneAssetTest, StalePathStillLoadsWhenKeyKnown)
{
	AssetRegistry registry;
	registry.set("cube_key", "builtin:cube");

	Scene scene{"test"};
	scene.set_assets(&registry);
	const Entity entity = scene.create("mesh");
	scene.registry().emplace<MeshSource>(
	        entity, MeshSource{"assets/moved-away.glb", "cube_key"});

	scene.update(0.0f);

	ASSERT_TRUE(scene.registry().all_of<Mesh>(entity));
	EXPECT_EQ(scene.registry().get<Mesh>(entity).source, "builtin:cube");
}
