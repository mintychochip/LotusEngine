#include "lotus/scene/culling.hpp"

namespace lotus {

namespace {

/// glm stores matrices column-major, so a row is m[0..3][row].
glm::vec4 matrix_row(const glm::mat4& matrix, int row)
{
	return glm::vec4{matrix[0][row], matrix[1][row], matrix[2][row],
	                 matrix[3][row]};
}

} // namespace

Frustum Frustum::from_view_projection(const glm::mat4& view_projection)
{
	const glm::vec4 row0 = matrix_row(view_projection, 0);
	const glm::vec4 row1 = matrix_row(view_projection, 1);
	const glm::vec4 row2 = matrix_row(view_projection, 2);
	const glm::vec4 row3 = matrix_row(view_projection, 3);

	Frustum frustum;
	frustum.planes[0] = row3 + row0; // left
	frustum.planes[1] = row3 - row0; // right
	frustum.planes[2] = row3 + row1; // bottom
	frustum.planes[3] = row3 - row1; // top
	// Zero-to-one depth: the near plane is row 2, not row3 + row2.
	frustum.planes[4] = row2;        // near
	frustum.planes[5] = row3 - row2; // far

	for (glm::vec4& plane : frustum.planes) {
		const float length = glm::length(glm::vec3{plane});
		if (length > 0.0f) {
			plane /= length;
		}
	}
	return frustum;
}

bool Frustum::intersects_sphere(const glm::vec3& center, float radius) const
{
	for (const glm::vec4& plane : planes) {
		const float distance = glm::dot(plane, glm::vec4{center, 1.0f});
		if (distance < -radius) {
			return false; // fully behind this plane
		}
	}
	return true;
}

} // namespace lotus
