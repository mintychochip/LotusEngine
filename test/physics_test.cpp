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

TEST(PhysicsTest, ClearDropsDynamicBodySoReloadUsesTheNewTransform)
{
	auto make_ball = [](Scene& scene, float y) {
		const Entity floor = scene.create("floor");
		scene.registry().emplace<RigidBody>(
		        floor, RigidBody{PhysicsMotionType::Static});
		scene.registry().emplace<BoxCollider>(
		        floor, BoxCollider{{20.0f, 0.5f, 20.0f}});
		scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

		const Entity ball = scene.create("ball");
		scene.registry().emplace<RigidBody>(
		        ball, RigidBody{PhysicsMotionType::Dynamic, 1.0f});
		scene.registry().emplace<SphereCollider>(ball, SphereCollider{0.5f});
		scene.registry().get<Transform>(ball).position = {0.0f, y, 0.0f};
		return ball;
	};

	Scene first{"first"};
	PhysicsWorld world;
	make_ball(first, 3.0f);
	first.update(0.0f);
	for (int i = 0; i < 90; ++i) {
		world.step(first, 1.0f / 60.0f);
	}

	Scene reloaded{"reloaded"};
	const Entity ball = make_ball(reloaded, 10.0f);
	reloaded.update(0.0f);
	world.clear();
	world.step(reloaded, 1.0f / 60.0f);

	const float y = reloaded.registry().get<Transform>(ball).position.y;
	EXPECT_GT(y, 9.0f);
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

TEST(PhysicsTest, CharacterControllerStaysOnFloor)
{
	Scene scene{"character"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{20.0f, 0.5f, 20.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

	const Entity player = scene.create("player");
	scene.registry().emplace<CharacterController>(player);
	scene.registry().get<Transform>(player).position = {0.0f, 0.0f, 0.0f};

	scene.update(0.0f);
	for (int i = 0; i < 120; ++i) {
		world.set_character_velocity(scene, player, {1.0f, 0.0f, 0.0f});
		world.step(scene, 1.0f / 60.0f);
	}

	const float y = scene.registry().get<Transform>(player).position.y;
	EXPECT_NEAR(y, 0.0f, 0.05f);
	EXPECT_TRUE(scene.registry().get<CharacterController>(player).grounded);
	EXPECT_GT(scene.registry().get<Transform>(player).position.x, 0.5f);
}

TEST(PhysicsTest, CharacterControllerLandsAfterDrop)
{
	Scene scene{"drop"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{20.0f, 0.5f, 20.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

	const Entity player = scene.create("player");
	scene.registry().emplace<CharacterController>(player);
	scene.registry().get<Transform>(player).position = {0.0f, 4.0f, 0.0f};

	scene.update(0.0f);
	for (int i = 0; i < 180; ++i) {
		world.step(scene, 1.0f / 60.0f);
	}

	const float y = scene.registry().get<Transform>(player).position.y;
	EXPECT_NEAR(y, 0.0f, 0.08f);
	EXPECT_TRUE(scene.registry().get<CharacterController>(player).grounded);
}

TEST(PhysicsTest, RebuildsColliderWhenBoxExtentsChange)
{
	Scene scene{"resize"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	auto& box = scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{5.0f, 0.5f, 5.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

	scene.update(0.0f);
	world.step(scene, 0.0f);

	PhysicsRaycastHit hit;
	EXPECT_TRUE(world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f,
	                          hit));
	const float top_before = hit.point.y;

	box.half_extents.y = 2.0f;
	world.step(scene, 0.0f);

	EXPECT_TRUE(world.raycast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f,
	                          hit));
	EXPECT_GT(hit.point.y, top_before + 1.0f);
}

TEST(PhysicsTest, RebuildsCharacterCapsuleWhenResized)
{
	Scene scene{"capsule"};
	PhysicsWorld world;

	const Entity floor = scene.create("floor");
	scene.registry().emplace<RigidBody>(
	        floor, RigidBody{PhysicsMotionType::Static});
	scene.registry().emplace<BoxCollider>(
	        floor, BoxCollider{{20.0f, 0.5f, 20.0f}});
	scene.registry().get<Transform>(floor).position = {0.0f, -0.5f, 0.0f};

	const Entity player = scene.create("player");
	auto& controller =
	        scene.registry().emplace<CharacterController>(player);
	scene.registry().get<Transform>(player).position = {0.0f, 0.0f, 0.0f};

	scene.update(0.0f);
	for (int i = 0; i < 30; ++i) {
		world.step(scene, 1.0f / 60.0f);
	}
	EXPECT_TRUE(controller.grounded);

	controller.half_height = 0.3f;
	world.step(scene, 1.0f / 60.0f);
	EXPECT_TRUE(controller.grounded);
	EXPECT_NEAR(scene.registry().get<Transform>(player).position.y, 0.0f,
	            0.1f);
}
