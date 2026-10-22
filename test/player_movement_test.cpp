#include <gtest/gtest.h>

#include <cmath>

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

TEST(PlayerMovementTest, ThirdPersonToggleCyclesChaseEyeChase)
{
	Scene scene{"toggle"};
	const Entity player = scene.create("player");
	scene.registry().get<Transform>(player).position = {1.0f, 0.0f, 2.0f};
	const Entity camera_entity = scene.create("camera");
	auto& authored             = scene.registry().emplace<Camera3D>(camera_entity);
	authored.position          = {7.5f, 4.5f, 10.5f};
	authored.target            = {0.0f, 1.0f, 0.0f};

	EXPECT_EQ(camera_view(scene), CameraView::Chase);
	update_follow_camera(scene);
	const Camera3D& chase = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(chase.position.z, 7.0f, 1e-4f);

	// F5 leaves the chase writer for the eye view, not the fixed pose.
	EXPECT_FALSE(toggle_camera_third_person(scene));
	EXPECT_EQ(camera_view(scene), CameraView::Eye);
	const Camera3D& eye = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_FALSE(camera_third_person(scene));
	EXPECT_TRUE(eye.follow);
	EXPECT_NEAR(eye.position.x, 1.0f, 1e-4f);
	EXPECT_NEAR(eye.position.y, kFirstPersonEyeHeight, 1e-4f);
	EXPECT_NEAR(eye.position.z, 2.0f, 1e-4f);

	// Follow frames while first person keep the eye glued to the player.
	scene.registry().get<Transform>(player).position = {4.0f, 0.0f, -1.0f};
	update_follow_camera(scene);
	const Camera3D& tracked = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_EQ(camera_view(scene), CameraView::Eye);
	EXPECT_NEAR(tracked.position.x, 4.0f, 1e-4f);
	EXPECT_NEAR(tracked.position.y, kFirstPersonEyeHeight, 1e-4f);
	EXPECT_NEAR(tracked.position.z, -1.0f, 1e-4f);

	// F5 again returns to the chase view exactly.
	EXPECT_TRUE(toggle_camera_third_person(scene));
	EXPECT_EQ(camera_view(scene), CameraView::Chase);
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

	EXPECT_FALSE(toggle_camera_third_person(scene));
	EXPECT_EQ(camera_view(scene), CameraView::Eye);
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

TEST(PlayerMovementTest, FixedPoseRestoresAuthoredShot)
{
	Scene scene{"fixed"};
	const Entity player = scene.create("player");
	scene.registry().get<Transform>(player).position = {1.0f, 0.0f, 2.0f};
	const Entity camera_entity = scene.create("camera");
	auto& authored             = scene.registry().emplace<Camera3D>(camera_entity);
	authored.position          = {7.5f, 4.5f, 10.5f};
	authored.target            = {0.0f, 1.0f, 0.0f};

	update_follow_camera(scene);
	EXPECT_EQ(camera_view(scene), CameraView::Chase);

	set_camera_third_person(scene, false);
	EXPECT_EQ(camera_view(scene), CameraView::Fixed);
	const Camera3D& fixed = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_FALSE(camera_third_person(scene));
	EXPECT_FALSE(fixed.follow);
	EXPECT_NEAR(fixed.position.x, 7.5f, 1e-4f);
	EXPECT_NEAR(fixed.position.y, 4.5f, 1e-4f);
	EXPECT_NEAR(fixed.position.z, 10.5f, 1e-4f);
	EXPECT_NEAR(fixed.target.x, 0.0f, 1e-4f);
	EXPECT_NEAR(fixed.target.y, 1.0f, 1e-4f);
	EXPECT_NEAR(fixed.target.z, 0.0f, 1e-4f);

	// Follow frames while fixed leave the wide shot alone.
	update_follow_camera(scene);
	const Camera3D& still = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(still.position.x, 7.5f, 1e-4f);
	EXPECT_EQ(camera_view(scene), CameraView::Fixed);
}

TEST(PlayerMovementTest, ToggleFromFixedReturnsToChase)
{
	Scene scene{"unfixed"};
	const Entity player = scene.create("player");
	scene.registry().get<Transform>(player).position = {1.0f, 0.0f, 2.0f};
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);
	update_follow_camera(scene);

	// A manual orbit (follow off, pose moved) is the fixed state too.
	auto& camera = scene.registry().get<Camera3D>(camera_entity);
	camera.follow   = false;
	camera.position = {20.0f, 10.0f, 20.0f};
	camera.target   = {0.0f, 0.0f, 0.0f};
	EXPECT_EQ(camera_view(scene), CameraView::Fixed);

	EXPECT_TRUE(toggle_camera_third_person(scene));
	EXPECT_EQ(camera_view(scene), CameraView::Chase);
	const Camera3D& resumed = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_TRUE(resumed.follow);
	EXPECT_NEAR(resumed.position.x, 1.0f, 1e-4f);
	EXPECT_NEAR(resumed.position.y, 2.2f, 1e-4f);
	EXPECT_NEAR(resumed.position.z, 7.0f, 1e-4f);
}

TEST(PlayerMovementTest, ThirdPersonToggleWithoutCameraIsNoOp)
{
	Scene scene{"nocamera"};
	scene.create("player");
	EXPECT_FALSE(camera_third_person(scene));
	EXPECT_FALSE(toggle_camera_third_person(scene));
}

TEST(PlayerMovementTest, LookDeltaYawsAndPitches)
{
	Scene scene{"look"};
	scene.create("player");
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	// Rightward drag turns the view right, upward drag looks up (SDL
	// motion y grows downward).
	const CameraLookState look = apply_look_delta(scene, 100.0f, -50.0f);
	EXPECT_NEAR(look.yaw_radians, 100.0f * kLookSensitivity, 1e-5f);
	EXPECT_NEAR(look.pitch_radians, 50.0f * kLookSensitivity, 1e-5f);
	EXPECT_NEAR(camera_look(scene).yaw_radians, look.yaw_radians, 1e-6f);
	EXPECT_NEAR(camera_look(scene).pitch_radians, look.pitch_radians,
	            1e-6f);
}

TEST(PlayerMovementTest, LookPitchClampsAtEightyDegrees)
{
	Scene scene{"pitch"};
	scene.create("player");
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	const CameraLookState up = apply_look_delta(scene, 0.0f, -100000.0f);
	EXPECT_NEAR(up.pitch_radians, kMaxLookPitch, 1e-4f);
	const CameraLookState down = apply_look_delta(scene, 0.0f, 200000.0f);
	EXPECT_NEAR(down.pitch_radians, -kMaxLookPitch, 1e-4f);
}

TEST(PlayerMovementTest, LookYawWraps)
{
	Scene scene{"wrap"};
	scene.create("player");
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	// Ten full turns plus a quarter: the state keeps the quarter only.
	constexpr float kTau = 6.283185307179586f;
	const CameraLookState spun =
	        apply_look_delta(scene, 10.0f * kTau / kLookSensitivity,
	                         0.0f);
	EXPECT_NEAR(spun.yaw_radians, 0.0f, 1e-3f);
	const CameraLookState quarter =
	        apply_look_delta(scene, 0.25f * kTau / kLookSensitivity, 0.0f);
	EXPECT_NEAR(quarter.yaw_radians, 0.25f * kTau, 1e-3f);
	// A setter beyond pi wraps into (-pi, pi] too.
	set_camera_look(scene, 3.0f * kTau, 0.0f);
	EXPECT_LE(std::abs(camera_look(scene).yaw_radians), 3.141592653589793f);
}

TEST(PlayerMovementTest, ChaseOrbitTurnsWithLook)
{
	Scene scene{"orbit"};
	scene.registry().get<Transform>(scene.create("player")).position = {
	        1.0f, 0.0f, 2.0f};
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	update_follow_camera(scene);
	const Camera3D& neutral = scene.registry().get<Camera3D>(camera_entity);
	const float neutral_distance =
	        glm::length(neutral.position - neutral.target);

	// A quarter turn right swings the camera to the -X side at the same
	// orbit distance, keeping the chest height.
	set_camera_look(scene, 1.57079632679f, 0.0f);
	update_follow_camera(scene);
	const Camera3D& turned = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(glm::length(turned.position - turned.target),
	            neutral_distance, 1e-3f);
	EXPECT_NEAR(turned.position.x, 1.0f - 5.0f, 1e-2f);
	EXPECT_NEAR(turned.position.z, 2.0f, 1e-2f);
	EXPECT_NEAR(turned.position.y, 2.2f, 1e-2f);
	EXPECT_NEAR(turned.target.y, 1.4f, 1e-4f);
}

TEST(PlayerMovementTest, EyeLookTurnsBodyAndPitches)
{
	Scene scene{"eyelook"};
	auto& transform = scene.registry().get<Transform>(scene.create("player"));
	transform.position   = {2.0f, 0.0f, 3.0f};
	transform.rotation.y = 0.0f;
	const Entity camera_entity = scene.create("camera");
	scene.registry().emplace<Camera3D>(camera_entity);

	EXPECT_FALSE(toggle_camera_third_person(scene));
	// Look right and up: the body turns with the view, the eye pitches.
	apply_look_delta(scene, 1.57079632679f / kLookSensitivity,
	                 -0.5f / kLookSensitivity);
	update_follow_camera(scene);
	EXPECT_NEAR(transform.rotation.y, 1.57079632679f, 1e-3f);
	const Camera3D& camera = scene.registry().get<Camera3D>(camera_entity);
	EXPECT_NEAR(camera.position.x, 2.0f, 1e-4f);
	EXPECT_NEAR(camera.position.y, kFirstPersonEyeHeight, 1e-4f);
	EXPECT_NEAR(camera.position.z, 3.0f, 1e-4f);
	glm::vec3 view = camera.target - camera.position;
	EXPECT_GT(glm::length(view), 0.5f);
	view = glm::normalize(view);
	EXPECT_NEAR(view.x, std::cos(0.5f), 1e-3f);
	EXPECT_NEAR(view.y, std::sin(0.5f), 1e-3f);
	EXPECT_NEAR(view.z, 0.0f, 1e-3f);
}
