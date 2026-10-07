#include "lotus/scene/player_controller.hpp"

#include <SDL3/SDL.h>

#include <cmath>

#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus {
namespace {

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

} // namespace

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
		physics.set_character_velocity(scene, player, glm::vec3{0.0f});
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

	glm::vec3 direction{right, 0.0f, -forward};
	if (glm::length(direction) > 0.0f) {
		direction = glm::normalize(direction);
	}

	float speed = move_speed;
	if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) {
		speed *= sprint_multiplier;
	}

	physics.set_character_velocity(scene, player, direction * speed);
}

void update_follow_camera(Scene& scene, const FollowCameraSettings& settings)
{
	const Entity player = find_named(scene, "player");
	Camera3D* camera    = find_follow_camera(scene);
	if (player == NullEntity || !camera) {
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
	camera->position = focus - settings.offset;
}

} // namespace lotus
