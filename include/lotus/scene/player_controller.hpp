#pragma once

#include <glm/glm.hpp>

namespace lotus {

class PhysicsWorld;
class Scene;

/// Tunables for the default third-person follow camera on entity \c "camera".
struct FollowCameraSettings {
	glm::vec3 offset{0.0f, 2.2f, 5.0f};
	float look_at_height{1.4f};
};

/// Reads WASD (+ optional sprint with Shift) and drives \c CharacterController
/// on entity \c "player". No-op when \p enable_input is false.
void apply_character_movement_input(Scene& scene, PhysicsWorld& physics,
                                    bool enable_input, float move_speed = 4.0f,
                                    float sprint_multiplier = 1.75f);

/// Places the first \c Camera3D (or entity \c "camera") behind \c "player".
void update_follow_camera(Scene& scene,
                          const FollowCameraSettings& settings = {});

} // namespace lotus
