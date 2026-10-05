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
struct MeshSource {
	std::string path;
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

/// Surface colour, multiplied into the lighting result.
struct Material {
	glm::vec3 base_color{0.82f, 0.82f, 0.86f};
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

} // namespace lotus
