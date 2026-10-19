#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "lotus/ecs.hpp"
#include "lotus/scene/culling.hpp"

namespace lotus {

/// Human-readable label, shown in the editor.
struct Name {
	std::string value;
};

/// Local TRS plus the cached world matrix produced by propagation.
struct Transform {
	glm::vec3 position{0.0f};
	glm::vec3 rotation{0.0f}; // Euler, radians
	glm::vec3 scale{1.0f};
	glm::mat4 world{1.0f};

	glm::mat4 local_matrix() const
	{
		glm::mat4 m = glm::translate(glm::mat4{1.0f}, position);
		m           = glm::rotate(m, rotation.z, glm::vec3{0.0f, 0.0f, 1.0f});
		m           = glm::rotate(m, rotation.y, glm::vec3{0.0f, 1.0f, 0.0f});
		m           = glm::rotate(m, rotation.x, glm::vec3{1.0f, 0.0f, 0.0f});
		m           = glm::scale(m, scale);
		return m;
	}
};

/// Persistent identity that survives save/load and reordering. Zero means
/// unassigned; Scene::create assigns one, and a document without one gets a
/// fresh value on load.
struct SceneGuid {
	uint64_t value{0};
};

/// Names another entity by GUID rather than by handle, so a reference survives
/// a reload. \p resolved is derived state filled by Scene::resolve_refs() and
/// is never serialized.
struct EntityRef {
	SceneGuid target;
	Entity resolved{NullEntity};
};

/// Intrusive linked-list hierarchy: O(1) reparent, no allocation.
struct Hierarchy {
	Entity parent{NullEntity};
	Entity first_child{NullEntity};
	Entity next_sibling{NullEntity};
	Entity prev_sibling{NullEntity};
};

/// Axis-aligned coloured quad drawn in world space (2D path).
struct Sprite {
	glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
	glm::vec2 size{1.0f, 1.0f};
};

/// 2D camera. Half-height is 1/zoom in world units.
struct Camera2D {
	glm::vec2 position{0.0f};
	float zoom{1.0f};

	glm::mat4 view() const
	{
		return glm::translate(glm::mat4{1.0f}, glm::vec3{-position, 0.0f});
	}

	glm::mat4 view_projection(float aspect) const
	{
		const float half_h = 1.0f / zoom;
		const float half_w = half_h * aspect;
		const glm::mat4 proj =
		        glm::ortho(-half_w, half_w, -half_h, half_h, -1.0f, 1.0f);
		return proj * view();
	}
};

/// Indexed triangle mesh. Geometry is transient (resolved from a MeshSource by
/// lotus::mesh::resolve_mesh); only the source path is serialized.
struct Mesh {
	std::vector<glm::vec3> positions;
	std::vector<glm::vec3> normals;
	std::vector<glm::vec2> uvs;
	std::vector<std::uint32_t> indices;
	/// Source this geometry was resolved from, used to detect changes.
	std::string source;
	/// Local-space bounds, filled in when the mesh is resolved.
	BoundingSphere bounds;

	bool empty() const { return positions.empty() || indices.empty(); }
	std::size_t triangle_count() const { return indices.size() / 3; }
};

/// Serialized reference to mesh geometry: "builtin:cube", "builtin:sphere",
/// "builtin:sphere:<segments>:<rings>", or a path to a .glb file.
///
/// \p id is an optional stable asset key. When set, an AssetRegistry can
/// re-point it at a new path without editing any scene, so moving an asset does
/// not require touching every document that referenced it. Empty means "use
/// \p path directly", which is what a pre-v3 document migrates to.
struct MeshSource {
	std::string path;
	std::string id;
};

/// One level of detail: geometry source plus the furthest distance at which it
/// is still used.
struct LodLevel {
	std::string source;
	float max_distance{20.0f};
};

/// Distance-based level of detail. Levels are ordered most detailed first;
/// past the last level's distance the entity is culled entirely.
struct Lod {
	std::vector<LodLevel> levels;
};

/// Resolved geometry for a Lod, parallel to Lod::levels. Transient.
struct LodGeometry {
	std::vector<Mesh> levels;
};

/// Surface colour and optional albedo texture (asset registry key).
struct Material {
	glm::vec3 base_color{0.82f, 0.82f, 0.86f};
	std::string albedo_id;
	float roughness{0.5f};
};

/// Perspective camera.
///
/// SDL_GPU uses a zero-to-one depth range (D3D/Metal convention), so this uses
/// glm's RH zero-to-one projection rather than the OpenGL default.
struct Camera3D {
	glm::vec3 position{3.0f, 2.5f, 4.0f};
	glm::vec3 target{0.0f, 0.0f, 0.0f};
	glm::vec3 up{0.0f, 1.0f, 0.0f};
	float fov_degrees{55.0f};
	float near_plane{0.1f};
	float far_plane{100.0f};
	bool follow{true};

	glm::mat4 view() const
	{
		return glm::lookAtRH(position, target, up);
	}

	glm::mat4 view_projection(float aspect) const
	{
		const glm::mat4 projection = glm::perspectiveRH_ZO(
		        glm::radians(fov_degrees), aspect > 0.0f ? aspect : 1.0f,
		        near_plane, far_plane);
		return projection * view();
	}
};

/// Which scene file an entity came from. Assigned when a document is loaded or
/// merged, so merged entity sets can be told apart and unloaded by origin.
/// Empty for entities created directly.
struct SceneOrigin {
	std::string scene;
};

/// How a rigid body interacts with the simulation.
enum class PhysicsMotionType : std::uint8_t {
	Static,
	Kinematic,
	Dynamic,
};

/// Simulation properties for an entity with a collider shape.
struct RigidBody {
	PhysicsMotionType motion_type{PhysicsMotionType::Dynamic};
	float mass{1.0f};
	float friction{0.5f};
	float restitution{0.0f};
};

struct BoxCollider {
	glm::vec3 half_extents{0.5f};
};

struct SphereCollider {
	float radius{0.5f};
};

/// Convex hull collider built from the entity's \c Mesh positions (local
/// space, scaled by \c Transform::scale). Requires \c Mesh + \c RigidBody.
struct MeshCollider {};

/// Height field collider built from the entity's \c Mesh when it is a regular
/// \f$(n \times n)\f$ grid (row-major X then Z, as \c mesh::terrain). Heights
/// are taken from vertex Y in local space, scaled by \c Transform::scale, then
/// clamped to \c min_height and \c max_height. Requires \c Mesh + \c RigidBody.
struct HeightFieldCollider {
	float min_height{0.0f};
	float max_height{1.0f};
};

/// Capsule character driven by Jolt CharacterVirtual. \p Transform::position
/// is the feet; \p desired_velocity is in world space (m/s). Gravity is applied
/// when not grounded. Updated during \c PhysicsWorld::step after rigid bodies.
struct CharacterController {
	float radius{0.4f};
	float half_height{0.9f};
	glm::vec3 desired_velocity{0.0f};
	bool grounded{false};
};

/// Plays a transform animation clip from the asset registry.
struct Animator {
	std::string clip_id;
	float time{0.0f};
	float speed{1.0f};
	bool loop{true};
	bool playing{true};
};

/// Per-scene environment. Replaces the renderer's global lighting when a scene
/// specifies one; a scene without settings leaves the globals alone.
/// `exposure` scales linear HDR before ACES tone mapping (1.0 = neutral).
struct SceneSettings {
	glm::vec3 light_direction{0.5f, 0.9f, 0.4f};
	glm::vec3 light_color{1.0f, 1.0f, 1.0f};
	float light_intensity{1.0f};
	glm::vec3 ambient{0.12f, 0.13f, 0.17f};
	float exposure{1.0f};
};

} // namespace lotus
