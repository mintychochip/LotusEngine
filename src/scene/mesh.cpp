#include "lotus/scene/mesh.hpp"

#include <cmath>

namespace lotus::mesh {
namespace {

constexpr float kPi = 3.14159265358979323846f;

} // namespace

Mesh cube(float size)
{
	const float h = size * 0.5f;

	// Each face gets its own four vertices so normals stay flat.
	const glm::vec3 face_normals[6] = {
	        {0.0f, 0.0f, 1.0f},  // +Z
	        {0.0f, 0.0f, -1.0f}, // -Z
	        {1.0f, 0.0f, 0.0f},  // +X
	        {-1.0f, 0.0f, 0.0f}, // -X
	        {0.0f, 1.0f, 0.0f},  // +Y
	        {0.0f, -1.0f, 0.0f}, // -Y
	};
	// Counter-clockwise when viewed from outside, per face.
	const glm::vec3 face_corners[6][4] = {
	        {{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},
	        {{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}},
	        {{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}},
	        {{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}},
	        {{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}},
	        {{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}},
	};

	Mesh result;
	result.positions.reserve(24);
	result.normals.reserve(24);
	result.indices.reserve(36);

	for (int face = 0; face < 6; ++face) {
		const std::uint32_t base = static_cast<std::uint32_t>(result.positions.size());
		for (int corner = 0; corner < 4; ++corner) {
			result.positions.push_back(face_corners[face][corner]);
			result.normals.push_back(face_normals[face]);
		}
		result.indices.push_back(base + 0);
		result.indices.push_back(base + 1);
		result.indices.push_back(base + 2);
		result.indices.push_back(base + 0);
		result.indices.push_back(base + 2);
		result.indices.push_back(base + 3);
	}
	return result;
}

Mesh sphere(float radius, int segments, int rings)
{
	if (segments < 3) {
		segments = 3;
	}
	if (rings < 2) {
		rings = 2;
	}

	Mesh result;
	result.positions.reserve(static_cast<std::size_t>((segments + 1) * (rings + 1)));
	result.normals.reserve(result.positions.capacity());

	for (int ring = 0; ring <= rings; ++ring) {
		const float v     = static_cast<float>(ring) / static_cast<float>(rings);
		const float phi   = v * kPi;                 // 0..pi
		const float y     = std::cos(phi);
		const float ring_r = std::sin(phi);
		for (int segment = 0; segment <= segments; ++segment) {
			const float u     = static_cast<float>(segment) /
			                    static_cast<float>(segments);
			const float theta = u * 2.0f * kPi;
			const glm::vec3 normal{ring_r * std::cos(theta), y,
			                       ring_r * std::sin(theta)};
			result.normals.push_back(normal);
			result.positions.push_back(normal * radius);
		}
	}

	const std::uint32_t stride = static_cast<std::uint32_t>(segments + 1);
	for (int ring = 0; ring < rings; ++ring) {
		for (int segment = 0; segment < segments; ++segment) {
			const std::uint32_t a = static_cast<std::uint32_t>(ring) * stride +
			                        static_cast<std::uint32_t>(segment);
			const std::uint32_t b = a + stride;
			result.indices.push_back(a);
			result.indices.push_back(b);
			result.indices.push_back(a + 1);
			result.indices.push_back(a + 1);
			result.indices.push_back(b);
			result.indices.push_back(b + 1);
		}
	}
	return result;
}

bool resolve_mesh(const std::string& source, Mesh& out, std::string* error)
{
	if (source == "builtin:cube") {
		out = cube();
		return true;
	}
	if (source == "builtin:sphere") {
		out = sphere();
		return true;
	}
	if (source.rfind("builtin:", 0) == 0) {
		if (error) {
			*error = "unknown builtin mesh: " + source;
		}
		return false;
	}
	if (error) {
		*error = "mesh loading is not implemented yet: " + source;
	}
	return false;
}

} // namespace lotus::mesh
