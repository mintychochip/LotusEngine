#pragma once

#include <glm/glm.hpp>

namespace lotus {

/// A world-space bounding sphere.
struct BoundingSphere {
	glm::vec3 center{0.0f};
	float radius{0.0f};
};

/// View frustum extracted from a view-projection matrix, used for
/// bounding-sphere culling.
///
/// Assumes a **zero-to-one** depth range (Vulkan / D3D), not
/// OpenGL's minus-one-to-one: the near plane comes from row 2 alone. Feeding
/// this an OpenGL-style projection would produce a wrong near plane.
struct Frustum {
	/// left, right, bottom, top, near, far - normalised.
	glm::vec4 planes[6];

	static Frustum from_view_projection(const glm::mat4& view_projection);

	/// False when the sphere is entirely outside any plane.
	bool intersects_sphere(const glm::vec3& center, float radius) const;

	/// Convenience overload for a BoundingSphere.
	bool intersects(const BoundingSphere& sphere) const
	{
		return intersects_sphere(sphere.center, sphere.radius);
	}
};

} // namespace lotus
