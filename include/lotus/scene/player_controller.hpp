#pragma once

#include <glm/glm.hpp>

#include "lotus/scene/components.hpp"

namespace lotus {

class PhysicsWorld;
class Scene;

/// Tunables for the third-person chase camera on entity \c "camera".
struct FollowCameraSettings {
	glm::vec3 offset{0.0f, 2.2f, 5.0f};
	float look_at_height{1.4f};
};

/// Height of the first-person eye above the character's feet
/// (\c Transform::position.y). The capsule is 0.35 m radius with 0.85 m
/// half-height, so 1.5 m sits a little below the top of the head.
inline constexpr float kFirstPersonEyeHeight{1.5f};

/// World-space walk direction on the XZ plane from camera forward and strafe
/// input in [-1, 1]. Returns zero when input is neutral.
glm::vec3 movement_direction_from_camera(const Camera3D& camera,
                                         float forward_input, float right_input);

/// Reads WASD (+ optional sprint with Shift) and drives \c CharacterController
/// on entity \c "player". No-op when \p enable_input is false.
void apply_character_movement_input(Scene& scene, PhysicsWorld& physics,
                                    bool enable_input, float move_speed = 4.0f,
                                    float sprint_multiplier = 1.75f);

/// Keeps the gameplay camera glued to \c "player": the third-person chase
/// view while third person is on, the first-person eye view otherwise.
/// Runs every frame, so both views track a moving character.
void update_follow_camera(Scene& scene,
                          const FollowCameraSettings& settings = {});

/// True when the gameplay camera shows the third-person chase view. False
/// when it shows the character's own first-person eye view, or when there
/// is no camera at all.
bool camera_third_person(Scene& scene);

/// Switches the gameplay camera between the third-person chase view and the
/// first-person eye view at \c kFirstPersonEyeHeight looking along the
/// character's facing (\c Transform::rotation.y). Snaps immediately, and the
/// per-frame follow keeps the pose glued to the player afterwards. No-op
/// without a camera.
void set_camera_third_person(Scene& scene, bool enabled);

/// Flips between the chase view and the first-person view and reports the
/// new state (true = third person). No-op (false) without a camera.
bool toggle_camera_third_person(Scene& scene);

} // namespace lotus
