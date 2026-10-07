#pragma once

#include <glm/glm.hpp>

namespace lotus {

/// Primary lit vertex stream: world position, normal, colour (64-byte stride).
struct GpuVertex {
	glm::vec3 position{0.0f};
	float position_pad{0.0f};
	glm::vec3 normal{0.0f, 0.0f, 1.0f};
	float normal_pad{0.0f};
	glm::vec3 color{1.0f};
	/// Per-vertex material roughness in [0, 1] (lit.frag specular width).
	float roughness{0.5f};
};

static_assert(sizeof(GpuVertex) == 48);

} // namespace lotus
