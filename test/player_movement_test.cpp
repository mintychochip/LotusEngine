#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/player_controller.hpp"
#include "lotus/scene/scene.hpp"

using namespace lotus;

TEST(PlayerMovementTest, CameraRelativeForwardUsesViewAxis)
{
	Camera3D camera;
	camera.position = {0.0f, 2.0f, 5.0f};
	camera.target   = {5.0f, 0.0f, 0.0f};

	const glm::vec3 dir =
	        movement_direction_from_camera(camera, 1.0f, 0.0f);
	glm::vec3 expected = camera.target - camera.position;
	expected.y         = 0.0f;
	expected           = glm::normalize(expected);
	EXPECT_NEAR(glm::dot(dir, expected), 1.0f, 0.02f);
}

TEST(PlayerMovementTest, FollowCameraSitsAtFeetPlusOffset)
{
	Scene scene{"follow"};
	const Entity player = scene.create("player");
	scene.registry().get<Transform>(player).position = {1.0f, 0.0f, 2.0f};
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	update_follow_camera(scene);

	const Camera3D& camera = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(camera.position.x, 1.0f, 1e-4f);
	EXPECT_NEAR(camera.position.y, 2.2f, 1e-4f);
	EXPECT_NEAR(camera.position.z, 7.0f, 1e-4f);
	EXPECT_NEAR(camera.target.x, 1.0f, 1e-4f);
	EXPECT_NEAR(camera.target.y, 1.4f, 1e-4f);
	EXPECT_NEAR(camera.target.z, 2.0f, 1e-4f);
}

TEST(PlayerMovementTest, DisabledInputLeavesDesiredVelocity)
{
	Scene scene{"input"};
	const Entity player = scene.create("player");
	auto& controller    = scene.registry().emplace<CharacterController>(player);
	controller.desired_velocity = {3.0f, 0.0f, 1.0f};
	PhysicsWorld physics;

	apply_character_movement_input(scene, physics, false);

	EXPECT_NEAR(controller.desired_velocity.x, 3.0f, 1e-4f);
	EXPECT_NEAR(controller.desired_velocity.z, 1.0f, 1e-4f);
}

TEST(PlayerMovementTest, NeutralInputYieldsZero)
{
	Camera3D camera;
	camera.position = {0.0f, 2.0f, 5.0f};
	camera.target   = {0.0f, 0.0f, 0.0f};
	const glm::vec3 dir =
	        movement_direction_from_camera(camera, 0.0f, 0.0f);
	EXPECT_NEAR(glm::length(dir), 0.0f, 1e-4f);
}
