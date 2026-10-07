#pragma once

#include <glm/glm.hpp>

#include "lotus/scene/components.hpp"

namespace lotus {

class PhysicsWorld;
class Scene;

/// Tunables for the default third-person follow camera on entity \c "camera".
struct FollowCameraSettings {
	glm::vec3 offset{0.0f, 2.2f, 5.0f};
	float look_at_height{1.4f};
};

/// World-space walk direction on the XZ plane from camera forward and strafe
/// input in [-1, 1]. Returns zero when input is neutral.
glm::vec3 movement_direction_from_camera(const Camera3D& camera,
                                         float forward_input, float right_input);

/// Reads WASD (+ optional sprint with Shift) and drives \c CharacterController
/// on entity \c "player". No-op when \p enable_input is false.
void apply_character_movement_input(Scene& scene, PhysicsWorld& physics,
                                    bool enable_input, float move_speed = 4.0f,
                                    float sprint_multiplier = 1.75f);

/// Places the first \c Camera3D (or entity \c "camera") behind \c "player".
void update_follow_camera(Scene& scene,
                          const FollowCameraSettings& settings = {});

} // namespace lotus
