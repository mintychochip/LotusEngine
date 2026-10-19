#include "lotus/scene/player_controller.hpp"

#include <SDL3/SDL.h>

#include <cmath>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus {
namespace {

/// Upward speed of a jump. With the physics world's gravity this clears a
/// little under a metre, which is over the character's own height (a 0.85 m
/// half-height capsule on top of a 0.35 m radius).
constexpr float kJumpSpeed = 4.5f;

Entity find_named(Scene& scene, const char* name)
{
	const Entity entity = scene.find(name);
	if (entity == NullEntity) {
		return NullEntity;
	}
	return entity;
}

Camera3D* find_follow_camera(Scene& scene)
{
	if (const Entity named = scene.find("camera");
	    named != NullEntity) {
		return scene.registry().try_get<Camera3D>(named);
	}
	for (const Entity entity : scene.registry().view<Camera3D>()) {
		return scene.registry().try_get<Camera3D>(entity);
	}
	return nullptr;
}

glm::vec3 movement_direction_on_xz(glm::vec3 horizontal)
{
	horizontal.y = 0.0f;
	const float len = glm::length(horizontal);
	if (len <= 0.0001f) {
		return glm::vec3{0.0f};
	}
	return horizontal / len;
}

} // namespace

glm::vec3 movement_direction_from_camera(const Camera3D& camera,
                                         float forward_input, float right_input)
{
	glm::vec3 forward = movement_direction_on_xz(camera.target - camera.position);
	if (glm::length(forward) <= 0.0001f) {
		forward = glm::vec3{0.0f, 0.0f, -1.0f};
	}
	const glm::vec3 right =
	        movement_direction_on_xz(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}));
	glm::vec3 move = forward * forward_input + right * right_input;
	return movement_direction_on_xz(move);
}

void apply_character_movement_input(Scene& scene, PhysicsWorld& physics,
                                    bool enable_input, float move_speed,
                                    float sprint_multiplier)
{
	const Entity player = find_named(scene, "player");
	if (player == NullEntity) {
		return;
	}
	CharacterController* controller =
	        scene.registry().try_get<CharacterController>(player);
	if (!controller) {
		return;
	}

	if (!enable_input) {
		return;
	}

	const bool* keys = SDL_GetKeyboardState(nullptr);
	float forward    = 0.0f;
	float right      = 0.0f;
	if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) {
		++forward;
	}
	if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) {
		--forward;
	}
	if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) {
		++right;
	}
	if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) {
		--right;
	}

	glm::vec3 direction{0.0f};
	if (Camera3D* camera = find_follow_camera(scene)) {
		direction = movement_direction_from_camera(*camera, forward, right);
	} else {
		direction = movement_direction_on_xz(glm::vec3{right, 0.0f, -forward});
	}

	float speed = move_speed;
	if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) {
		speed *= sprint_multiplier;
	}

	glm::vec3 velocity = direction * speed;
	// Jump on Space: an upward request, made only while the character stands on
	// something, so it cannot be stacked in mid-air. The physics world keeps the
	// climb once the character has left the ground.
	if (keys[SDL_SCANCODE_SPACE] && controller->grounded) {
		velocity.y = kJumpSpeed;
	}
	physics.set_character_velocity(scene, player, velocity);
}

void update_follow_camera(Scene& scene, const FollowCameraSettings& settings)
{
	const Entity player = find_named(scene, "player");
	Camera3D* camera    = find_follow_camera(scene);
	if (player == NullEntity || !camera) {
		return;
	}
	if (!camera->follow) {
		return;
	}
	const Transform* transform = scene.registry().try_get<Transform>(player);
	if (!transform) {
		return;
	}

	const glm::vec3 feet = transform->position;
	const glm::vec3 focus =
	        feet + glm::vec3{0.0f, settings.look_at_height, 0.0f};
	camera->target   = focus;
	camera->position = feet + settings.offset;
}

} // namespace lotus
