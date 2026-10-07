#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include "lotus/scene/components.hpp"
#include "lotus/scene/player_controller.hpp"

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

TEST(PlayerMovementTest, NeutralInputYieldsZero)
{
	Camera3D camera;
	camera.position = {0.0f, 2.0f, 5.0f};
	camera.target   = {0.0f, 0.0f, 0.0f};
	const glm::vec3 dir =
	        movement_direction_from_camera(camera, 0.0f, 0.0f);
	EXPECT_NEAR(glm::length(dir), 0.0f, 1e-4f);
}
