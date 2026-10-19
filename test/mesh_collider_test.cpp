#include <gtest/gtest.h>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/mesh.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(MeshColliderTest, StaticMeshGetsPhysicsBody)
{
	Scene scene{"physics"};
	PhysicsWorld world;

	const Entity prop = scene.create("crate");
	scene.registry().emplace<MeshSource>(prop, MeshSource{"builtin:cube", ""});
	scene.registry().emplace<RigidBody>(
	        prop, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<MeshCollider>(prop);

	scene.update(0.0f);
	world.step(scene, 0.0f);
	world.step(scene, 1.0f / 60.0f);

	PhysicsRaycastHit hit;
	EXPECT_TRUE(world.raycast({0.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 10.0f,
	                          hit));
	EXPECT_EQ(hit.entity, prop);
}
