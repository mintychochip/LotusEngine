#include <gtest/gtest.h>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(ScenePickTest, PhysicsRaycastSelectsColliderWithoutMesh)
{
	Scene scene{"pick"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{10.0f, 0.5f, 10.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

	const Entity camera = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera);
	auto& cam = scene.registry().get<Camera3D>(camera);
	cam.position = {0.0f, 2.0f, 8.0f};
	cam.target   = {0.0f, 0.0f, 0.0f};

	scene.update(0.0f);
	world.step(scene, 0.0f);

	const float aspect = 1.0f;
	EXPECT_EQ(scene.pick(0.0f, 0.0f, aspect, nullptr, Scene::PickMode::BoundsOnly),
	          NullEntity);
	EXPECT_EQ(scene.pick(0.0f, 0.0f, aspect, &world, Scene::PickMode::PhysicsOnly),
	          floor);
	EXPECT_EQ(scene.pick(0.0f, 0.0f, aspect, &world, Scene::PickMode::Combined),
	          floor);
}
