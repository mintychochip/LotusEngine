#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <string>

#include "lotus/ecs.hpp"

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

/// Axis-aligned colored quad drawn in world space.
struct Sprite {
	glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
	glm::vec2 size{1.0f, 1.0f};
};

/// 2D camera. Half-height is 1/zoom in world units.
struct Camera2D {
	glm::vec2 position{0.0f};
	float zoom{1.0f};

	glm::mat4 view_projection(float aspect) const
	{
		const float half_h = 1.0f / zoom;
		const float half_w = half_h * aspect;
		const glm::mat4 proj =
		        glm::ortho(-half_w, half_w, -half_h, half_h, -1.0f, 1.0f);
		const glm::mat4 view =
		        glm::translate(glm::mat4{1.0f}, glm::vec3{-position, 0.0f});
		return proj * view;
	}
};

} // namespace lotus
