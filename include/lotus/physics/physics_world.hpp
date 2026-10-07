#pragma once

#include <cstdint>
#include <unordered_map>

#include <glm/glm.hpp>

#include "lotus/ecs.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus {

struct PhysicsRaycastHit {
	Entity entity{NullEntity};
	glm::vec3 point{0.0f};
	glm::vec3 normal{0.0f, 1.0f, 0.0f};
	float fraction{1.0f};
};

/// Single-threaded Jolt backend. Syncs static/kinematic bodies from
/// \c Transform, steps with a fixed timestep, then writes dynamic results back
/// and calls \c update_world_transforms.
class PhysicsWorld {
public:
	PhysicsWorld();
	~PhysicsWorld();

	PhysicsWorld(const PhysicsWorld&)            = delete;
	PhysicsWorld& operator=(const PhysicsWorld&) = delete;

	void step(Scene& scene, float dt);

	/// Casts a ray in world space. Returns false when nothing is hit.
	bool raycast(const glm::vec3& origin, const glm::vec3& direction,
	             float max_distance, PhysicsRaycastHit& hit) const;

private:
	struct Impl;
	Impl* impl_{nullptr};
};

} // namespace lotus
