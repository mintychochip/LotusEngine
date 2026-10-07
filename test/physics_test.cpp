#include <gtest/gtest.h>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(PhysicsTest, DynamicSphereFallsOntoStaticFloor)
{
	Scene scene{"physics"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{50.0f, 0.5f, 50.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

	const Entity ball = scene.create("ball");
	scene.registry().emplace<RigidBody>(
	        ball, RigidBody{PhysicsMotionType::Dynamic, 1.0f});
	scene.registry().emplace<SphereCollider>(ball, SphereCollider{0.5f});
	scene.registry().get<Transform>(ball).position = {0.0f, 3.0f, 0.0f};

	scene.update(0.0f);
	const float start_y = scene.registry().get<Transform>(ball).position.y;

	for (int i = 0; i < 120; ++i) {
		world.step(scene, 1.0f / 60.0f);
	}

	const float end_y = scene.registry().get<Transform>(ball).position.y;
	EXPECT_LT(end_y, start_y);
	EXPECT_GT(end_y, 0.0f);
}

TEST(PhysicsTest, RaycastHitsFloor)
{
	Scene scene{"ray"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{10.0f, 0.5f, 10.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};
	scene.update(0.0f);
	world.step(scene, 0.0f);

	PhysicsRaycastHit hit;
	EXPECT_TRUE(world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f,
	                          hit));
	EXPECT_EQ(hit.entity, floor);
	EXPECT_NEAR(hit.point.y, 0.0f, 0.15f);
}
