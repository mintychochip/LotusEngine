#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

namespace lotus {

/// Primary lit vertex stream: position, normal, colour (48-byte stride). For
/// an immediate (world-space) draw these are world-space values; for a cached
/// mesh they are local-space, with colour 1 and roughness 1 so the instance's
/// multipliers (GpuInstance) pass through unchanged.
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

/// Per-instance record of an instanced mesh draw: the model matrix and the
/// material the instance multiplies into every vertex's colour and roughness.
/// Read by lit.vert and shadow.vert as the vertex buffer in slot 2 with
/// per-instance addressing; the layout must match the attribute descriptions
/// in src/gpu/vk/vk_pipelines.cpp (lit) and src/gpu/vulkan/renderer_passes.cpp
/// (shadow) and the locations in those shaders.
struct GpuInstance {
	glm::mat4 model{1.0f};
	glm::vec3 color{1.0f};
	float roughness{1.0f};
	/// rgb: emissive radiance (sRGB-authored), a: metallic.
	glm::vec4 material{0.0f};
};

static_assert(sizeof(GpuInstance) == 96);

/// Read-only view of a mesh's geometry, handed to GpuRenderer::acquire_mesh.
/// Plain pointers, so the renderer needs nothing from the scene module.
/// \p normals may hold fewer entries than \p positions: a vertex without a
/// normal is stored as the zero vector, which lit.vert turns into +Z. \p uvs
/// is used only when it has one entry per vertex; otherwise every UV is zero.
struct GpuMeshData {
	const glm::vec3* positions{nullptr};
	const glm::vec3* normals{nullptr};
	const glm::vec2* uvs{nullptr};
	const uint32_t* indices{nullptr};
	std::size_t vertex_count{0};
	std::size_t normal_count{0};
	std::size_t uv_count{0};
	std::size_t index_count{0};
};

} // namespace lotus
