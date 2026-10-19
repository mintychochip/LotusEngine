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

TEST(PlayerMovementTest, ThirdPersonIsOnByDefault)
{
	Scene scene{"default"};
	const Entity player = scene.create("player");
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);
	(void)player;
	EXPECT_TRUE(camera_third_person(scene));
}

TEST(PlayerMovementTest, ThirdPersonToggleSwitchesToFirstPerson)
{
	Scene scene{"toggle"};
	const Entity player = scene.create("player");
	scene.registry().get<Transform>(player).position = {1.0f, 0.0f, 2.0f};
	const Entity camera_entity = scene.create("camera");
	auto& authored             = scene.registry().emplace<Camera3D>(camera_entity);
	authored.position          = {7.5f, 4.5f, 10.5f};
	authored.target            = {0.0f, 1.0f, 0.0f};

	update_follow_camera(scene);
	const Camera3D& chase = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(chase.position.z, 7.0f, 1e-4f);

	EXPECT_FALSE(toggle_camera_third_person(scene));
	const Camera3D& eye = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_FALSE(camera_third_person(scene));
	EXPECT_NEAR(eye.position.x, 1.0f, 1e-4f);
	EXPECT_NEAR(eye.position.y, kFirstPersonEyeHeight, 1e-4f);
	EXPECT_NEAR(eye.position.z, 2.0f, 1e-4f);

	// Follow frames while first person keep the eye glued to the player.
	scene.registry().get<Transform>(player).position = {4.0f, 0.0f, -1.0f};
	update_follow_camera(scene);
	const Camera3D& tracked = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(tracked.position.x, 4.0f, 1e-4f);
	EXPECT_NEAR(tracked.position.y, kFirstPersonEyeHeight, 1e-4f);
	EXPECT_NEAR(tracked.position.z, -1.0f, 1e-4f);

	EXPECT_TRUE(toggle_camera_third_person(scene));
	update_follow_camera(scene);
	const Camera3D& resumed = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(resumed.position.x, 4.0f, 1e-4f);
	EXPECT_NEAR(resumed.position.y, 2.2f, 1e-4f);
	EXPECT_NEAR(resumed.position.z, 4.0f, 1e-4f);
	EXPECT_NEAR(resumed.target.y, 1.4f, 1e-4f);
}

TEST(PlayerMovementTest, FirstPersonPoseLooksAlongFacing)
{
	Scene scene{"facing"};
	const Entity player = scene.create("player");
	auto& transform     = scene.registry().get<Transform>(player);
	transform.position = {2.0f, 0.0f, 3.0f};
	// Face +X: yaw = atan2(1, 0) = pi/2.
	transform.rotation.y = 1.57079632679f;
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	set_camera_third_person(scene, false);
	const Camera3D& camera = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_FALSE(camera_third_person(scene));
	EXPECT_NEAR(camera.position.x, 2.0f, 1e-4f);
	EXPECT_NEAR(camera.position.y, kFirstPersonEyeHeight, 1e-4f);
	EXPECT_NEAR(camera.position.z, 3.0f, 1e-4f);
	glm::vec3 view = camera.target - camera.position;
	EXPECT_GT(glm::length(view), 0.5f);
	view = glm::normalize(view);
	EXPECT_NEAR(view.x, 1.0f, 1e-4f);
	EXPECT_NEAR(view.y, 0.0f, 1e-4f);
	EXPECT_NEAR(view.z, 0.0f, 1e-4f);
}

TEST(PlayerMovementTest, ThirdPersonToggleWithoutCameraIsNoOp)
{
	Scene scene{"nocamera"};
	scene.create("player");
	EXPECT_FALSE(camera_third_person(scene));
	EXPECT_FALSE(toggle_camera_third_person(scene));
}
