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

/// Entity carrying the gameplay camera: \c "camera" when present, else the
/// first \c Camera3D. May lack a \c Camera3D; callers check.
Entity find_follow_camera_entity(Scene& scene)
{
	if (const Entity named = scene.find("camera");
	    named != NullEntity) {
		return named;
	}
	for (const Entity entity : scene.registry().view<Camera3D>()) {
		return entity;
	}
	return NullEntity;
}

Camera3D* find_follow_camera(Scene& scene)
{
	const Entity entity = find_follow_camera_entity(scene);
	if (entity == NullEntity) {
		return nullptr;
	}
	return scene.registry().try_get<Camera3D>(entity);
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

/// Facing of a character whose yaw is \p yaw_radians about Y: +Z at zero,
/// matching the \c face_toward helper (\c rotation.y = atan2(dx, dz)).
glm::vec3 facing_from_yaw(float yaw_radians)
{
	return {std::sin(yaw_radians), 0.0f, std::cos(yaw_radians)};
}

/// Parks the camera at the character's eye, looking along its facing.
void apply_first_person_pose(Camera3D& camera, const Transform& transform)
{
	const glm::vec3 eye =
	        transform.position + glm::vec3{0.0f, kFirstPersonEyeHeight, 0.0f};
	camera.position = eye;
	camera.target   = eye + facing_from_yaw(transform.rotation.y);
}

/// Parks the camera behind the character, looking at its chest.
void apply_chase_pose(Camera3D& camera, const Transform& transform,
                      const FollowCameraSettings& settings)
{
	const glm::vec3 feet = transform.position;
	camera.target        = feet + glm::vec3{0.0f, settings.look_at_height, 0.0f};
	camera.position      = feet + settings.offset;
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

/// Records the authored pose once, before either moving view first
/// overwrites it, so leaving the follow views can restore the fixed wide
/// shot exactly. Later frames must not move the stash.
void stash_authored_pose_once(Scene& scene, Entity entity, const Camera3D& camera)
{
	if (entity != NullEntity && !scene.registry().all_of<CameraHome>(entity)) {
		scene.registry().emplace<CameraHome>(entity, camera.position,
		                                     camera.target);
	}
}

/// The controller-owned follow mode: \c Eye when the camera entity carries
/// an eye state, \c Chase otherwise (absent state means chase).
CameraFollowMode follow_mode(Scene& scene, Entity entity)
{
	if (entity != NullEntity) {
		if (const auto* state =
		            scene.registry().try_get<CameraFollowModeState>(entity)) {
			return state->mode;
		}
	}
	return CameraFollowMode::Chase;
}

/// Parks the camera per the controller-owned mode with default chase
/// tuning. The authored pose is stashed first so the fixed wide shot
/// survives either moving view.
void snap_to_follow_mode(Scene& scene, Entity entity, Camera3D& camera,
                         const Transform& transform)
{
	stash_authored_pose_once(scene, entity, camera);
	if (follow_mode(scene, entity) == CameraFollowMode::Eye) {
		apply_first_person_pose(camera, transform);
		return;
	}
	apply_chase_pose(camera, transform, FollowCameraSettings{});
}

CameraView camera_view(Scene& scene)
{
	const Entity entity = find_follow_camera_entity(scene);
	const Camera3D* camera =
	        entity == NullEntity
	                ? nullptr
	                : scene.registry().try_get<Camera3D>(entity);
	if (camera == nullptr || !camera->follow) {
		return CameraView::Fixed;
	}
	return follow_mode(scene, entity) == CameraFollowMode::Eye
	        ? CameraView::Eye
	        : CameraView::Chase;
}

void update_follow_camera(Scene& scene, const FollowCameraSettings& settings)
{
	const Entity player = find_named(scene, "player");
	const Entity entity = find_follow_camera_entity(scene);
	Camera3D* camera =
	        entity == NullEntity
	                ? nullptr
	                : scene.registry().try_get<Camera3D>(entity);
	if (player == NullEntity || !camera) {
		return;
	}
	// Fixed pose (the authored wide shot or a manual orbit): hold it.
	// Only the follow modes rewrite the camera, consulting the
	// controller-owned mode every frame so F5 never fights the writer.
	if (!camera->follow) {
		return;
	}
	const Transform* transform = scene.registry().try_get<Transform>(player);
	if (!transform) {
		return;
	}
	stash_authored_pose_once(scene, entity, *camera);
	if (follow_mode(scene, entity) == CameraFollowMode::Eye) {
		apply_first_person_pose(*camera, *transform);
		return;
	}
	apply_chase_pose(*camera, *transform, settings);
}

bool camera_third_person(Scene& scene)
{
	return camera_view(scene) == CameraView::Chase;
}

void set_camera_third_person(Scene& scene, bool enabled)
{
	const Entity entity = find_follow_camera_entity(scene);
	if (entity == NullEntity) {
		return;
	}
	Camera3D* camera = scene.registry().try_get<Camera3D>(entity);
	if (!camera) {
		return;
	}
	if (enabled) {
		camera->follow = true;
		scene.registry().emplace_or_replace<CameraFollowModeState>(
		        entity, CameraFollowModeState{CameraFollowMode::Chase});
		if (const Entity player = find_named(scene, "player");
		    player != NullEntity) {
			if (const Transform* transform =
			            scene.registry().try_get<Transform>(player)) {
				snap_to_follow_mode(scene, entity, *camera, *transform);
			}
		}
		return;
	}
	stash_authored_pose_once(scene, entity, *camera);
	// A direct re-follow (for example the RPG orbit reset) resumes the
	// chase view, so leave no stale eye mode behind.
	scene.registry().emplace_or_replace<CameraFollowModeState>(
	        entity, CameraFollowModeState{CameraFollowMode::Chase});
	camera->follow   = false;
	const CameraHome& home = scene.registry().get<CameraHome>(entity);
	camera->position = home.position;
	camera->target   = home.target;
}

bool toggle_camera_third_person(Scene& scene)
{
	const Entity entity = find_follow_camera_entity(scene);
	if (entity == NullEntity) {
		return false;
	}
	Camera3D* camera = scene.registry().try_get<Camera3D>(entity);
	if (!camera) {
		return false;
	}
	// From the fixed pose F5 returns to the chase view; while following
	// it flips the controller-owned mode between chase and eye. Either
	// way the writer below applies the same mode every frame.
	if (!camera->follow) {
		set_camera_third_person(scene, true);
		return true;
	}
	const CameraFollowMode flipped =
	        follow_mode(scene, entity) == CameraFollowMode::Chase
	                ? CameraFollowMode::Eye
	                : CameraFollowMode::Chase;
	scene.registry().emplace_or_replace<CameraFollowModeState>(
	        entity, CameraFollowModeState{flipped});
	if (const Entity player = find_named(scene, "player");
	    player != NullEntity) {
		if (const Transform* transform =
		            scene.registry().try_get<Transform>(player)) {
			snap_to_follow_mode(scene, entity, *camera, *transform);
		}
	}
	return flipped == CameraFollowMode::Chase;
}

} // namespace lotus
