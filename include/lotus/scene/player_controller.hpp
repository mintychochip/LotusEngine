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

/// Sensitivity of mouse look: radians of yaw/pitch per pixel of right-drag.
inline constexpr float kLookSensitivity{0.005f};

/// Look pitch limit (radians, 80 degrees): the view never tilts past
/// ±80°, so it can never flip over the top.
inline constexpr float kMaxLookPitch{1.3962634f};

/// Transient per-camera look state (never serialized): the view direction
/// owned by the controller, fed by right-drag and consulted every frame by
/// \c update_follow_camera, the only writer of the camera pose. Absent on
/// the camera entity means the neutral view (zero yaw and pitch).
struct CameraLookState {
	float yaw_radians{0.0f};
	float pitch_radians{0.0f};
};

/// Reports the controller-owned look direction (neutral when there is no
/// camera or no drag has happened yet).
CameraLookState camera_look(Scene& scene);

/// Sets the look direction, wrapping yaw to (-pi, pi] and clamping pitch
/// to ±\c kMaxLookPitch. No-op without a camera. This is the
/// \c --exec-visible seam headless screenshots use (\c camera.look).
void set_camera_look(Scene& scene, float yaw_radians, float pitch_radians);

/// Applies a right-drag delta in pixels (SDL motion units: +x right, +y
/// down): horizontal motion yaws (unbounded, wrapping), vertical motion
/// pitches (clamped, never flipping). Returns the resulting look state.
/// No-op (neutral) without a camera.
CameraLookState apply_look_delta(Scene& scene, float dx_pixels,
                                 float dy_pixels);

/// World-space walk direction on the XZ plane from camera forward and strafe
/// input in [-1, 1]. Returns zero when input is neutral.
glm::vec3 movement_direction_from_camera(const Camera3D& camera,
                                         float forward_input, float right_input);

/// Reads WASD (+ optional sprint with Shift) and drives \c CharacterController
/// on entity \c "player". No-op when \p enable_input is false.
void apply_character_movement_input(Scene& scene, PhysicsWorld& physics,
                                    bool enable_input, float move_speed = 4.0f,
                                    float sprint_multiplier = 1.75f);

/// Moving views the follow writer produces while \c Camera3D::follow is on.
/// The mode is controller-owned transient state (never serialized): a
/// document only knows follow on/off. \c follow off means the fixed pose.
enum class CameraFollowMode { Chase, Eye };

/// Transient per-camera follow mode. Absent means \c Chase.
struct CameraFollowModeState {
	CameraFollowMode mode{CameraFollowMode::Chase};
};

/// Observed gameplay view: \c Chase or \c Eye while the camera follows
/// \c "player", \c Fixed when it holds its pose (\c follow off) or when
/// there is no camera at all.
enum class CameraView { Chase, Eye, Fixed };

/// Reports the observed gameplay view.
CameraView camera_view(Scene& scene);

/// Keeps the gameplay camera glued to \c "player" while \c follow is on:
/// the chase view or the first-person eye view according to the
/// controller-owned mode. Holds the pose untouched while \c follow is off
/// (the fixed wide shot or a manual orbit). Runs every frame, so both
/// moving views track a moving character.
void update_follow_camera(Scene& scene,
                          const FollowCameraSettings& settings = {});

/// True when the gameplay camera shows the third-person chase view: it
/// follows \c "player" in \c Chase mode. False for the first-person eye
/// view, for the fixed pose, and when there is no camera at all.
bool camera_third_person(Scene& scene);

/// Switches the gameplay camera between the third-person chase view and
/// the scene's fixed wide shot (restored exactly from the pose stashed
/// before the follow writer first overwrote it). Snaps immediately. No-op
/// without a camera.
void set_camera_third_person(Scene& scene, bool enabled);

/// Flips the gameplay camera between the chase view and the first-person
/// eye view at \c kFirstPersonEyeHeight looking along the character's
/// facing (\c Transform::rotation.y), and reports the new state (true =
/// third person). From the fixed pose it returns to the chase view.
/// Snaps immediately; the per-frame follow keeps the pose glued to the
/// player afterwards. No-op (false) without a camera.
bool toggle_camera_third_person(Scene& scene);

} // namespace lotus
