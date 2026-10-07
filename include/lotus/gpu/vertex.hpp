#pragma once

#include <glm/glm.hpp>

namespace lotus {

/// One vertex of the lit pipeline: world-space position, normal, colour.
/// Geometry is uploaded pre-transformed, so the scene stays in one draw call.
///
/// Kept free of GPU headers so the scene layer can build vertices (and be
/// tested) without a renderer.
struct GpuVertex {
	glm::vec3 position{0.0f};
	glm::vec3 normal{0.0f, 0.0f, 1.0f};
	glm::vec3 color{1.0f};
};

} // namespace lotus
