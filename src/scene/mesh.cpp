#include "lotus/scene/mesh.hpp"

#include <cmath>
#include <cstdint>

#include "lotus/scene/glb.hpp"

namespace lotus::mesh {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Shared deterministic value-noise helpers for terrain() and
// terrain_height_at(). Single code path so both return bit-identical heights.
inline float terrain_hash(int x, int z, int s)
{
	uint32_t h = static_cast<uint32_t>(x) * 374761393u +
	             static_cast<uint32_t>(z) * 668265263u +
	             static_cast<uint32_t>(s) * 2147483647u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h = h ^ (h >> 16);
	return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

// Quintic smoother (Perlin fade): 6f^5 - 15f^4 + 10f^3.
inline float terrain_smooth(float t)
{
	return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// 2D value noise with quintic interpolation between grid points.
inline float terrain_noise(float x, float z, int seed)
{
	const int ix = static_cast<int>(std::floor(x));
	const int iz = static_cast<int>(std::floor(z));
	const float fx = x - static_cast<float>(ix);
	const float fz = z - static_cast<float>(iz);
	const float a = terrain_hash(ix, iz, seed);
	const float b = terrain_hash(ix + 1, iz, seed);
	const float c = terrain_hash(ix, iz + 1, seed);
	const float d = terrain_hash(ix + 1, iz + 1, seed);
	const float u = terrain_smooth(fx);
	const float v = terrain_smooth(fz);
	return a * (1.0f - u) * (1.0f - v) + b * u * (1.0f - v) +
	       c * (1.0f - u) * v + d * u * v;
}

// 4-octave fbm, gain 0.5, lacunarity 2.02 with per-octave domain rotation
// [0.8 -0.6; 0.6 0.8], normalized by total amplitude.
inline float terrain_fbm(float nx, float nz, int seed)
{
	float value = 0.0f;
	float amplitude = 1.0f;
	float total = 0.0f;
	float px = nx;
	float pz = nz;
	for (int octave = 0; octave < 4; ++octave) {
		value += terrain_noise(px, pz, seed) * amplitude;
		total += amplitude;
		amplitude *= 0.5f;
		const float rx = 0.8f * px - 0.6f * pz;
		const float rz = 0.6f * px + 0.8f * pz;
		px = rx * 2.02f;
		pz = rz * 2.02f;
	}
	return value / total;
}
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

	const glm::vec2 face_uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f},
	                               {0.0f, 1.0f}};

	Mesh result;
	result.positions.reserve(24);
	result.normals.reserve(24);
	result.uvs.reserve(24);
	result.indices.reserve(36);

	for (int face = 0; face < 6; ++face) {
		const std::uint32_t base = static_cast<std::uint32_t>(result.positions.size());
		for (int corner = 0; corner < 4; ++corner) {
			result.positions.push_back(face_corners[face][corner]);
			result.normals.push_back(face_normals[face]);
			result.uvs.push_back(face_uvs[corner]);
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
	result.uvs.reserve(result.positions.capacity());

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
			result.uvs.push_back(glm::vec2{u, v});
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

Mesh cylinder(float radius, float height, int segments)
{
	if (segments < 3) {
		segments = 3;
	}
	const float half_h = height * 0.5f;

	Mesh result;
	result.positions.reserve(static_cast<std::size_t>((segments + 1) * 2));
	result.normals.reserve(result.positions.capacity());
	result.uvs.reserve(result.positions.capacity());
	result.indices.reserve(static_cast<std::size_t>(segments) * 6);

	// Side vertices: top ring and bottom ring
	for (int i = 0; i <= segments; ++i) {
		const float u     = static_cast<float>(i) / static_cast<float>(segments);
		const float theta = u * 2.0f * kPi;
		const float c     = std::cos(theta);
		const float s     = std::sin(theta);
		// Top ring
		result.positions.push_back({radius * c, half_h, radius * s});
		result.normals.push_back({c, 0.0f, s});
		result.uvs.push_back({u, 0.0f});
		// Bottom ring
		result.positions.push_back({radius * c, -half_h, radius * s});
		result.normals.push_back({c, 0.0f, s});
		result.uvs.push_back({u, 1.0f});
	}
	// Side indices
	for (int i = 0; i < segments; ++i) {
		const std::uint32_t a = static_cast<std::uint32_t>(i * 2);
		const std::uint32_t b = a + 2;
		result.indices.push_back(a);
		result.indices.push_back(b);
		result.indices.push_back(a + 1);
		result.indices.push_back(a + 1);
		result.indices.push_back(b);
		result.indices.push_back(b + 1);
	}
	// Top cap
	const std::uint32_t top_center = static_cast<std::uint32_t>(result.positions.size());
	result.positions.push_back({0.0f, half_h, 0.0f});
	result.normals.push_back({0.0f, 1.0f, 0.0f});
	result.uvs.push_back({0.5f, 0.5f});
	for (int i = 0; i <= segments; ++i) {
		const float theta = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
		result.positions.push_back({radius * std::cos(theta), half_h, radius * std::sin(theta)});
		result.normals.push_back({0.0f, 1.0f, 0.0f});
		result.uvs.push_back({0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)});
	}
	for (int i = 0; i < segments; ++i) {
		result.indices.push_back(top_center);
		result.indices.push_back(top_center + 1 + static_cast<std::uint32_t>(i));
		result.indices.push_back(top_center + 2 + static_cast<std::uint32_t>(i));
	}
	// Bottom cap
	const std::uint32_t bot_center = static_cast<std::uint32_t>(result.positions.size());
	result.positions.push_back({0.0f, -half_h, 0.0f});
	result.normals.push_back({0.0f, -1.0f, 0.0f});
	result.uvs.push_back({0.5f, 0.5f});
	for (int i = 0; i <= segments; ++i) {
		const float theta = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
		result.positions.push_back({radius * std::cos(theta), -half_h, radius * std::sin(theta)});
		result.normals.push_back({0.0f, -1.0f, 0.0f});
	result.uvs.push_back({0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)});
	}
	for (int i = 0; i < segments; ++i) {
		result.indices.push_back(bot_center);
		result.indices.push_back(bot_center + 2 + static_cast<std::uint32_t>(i));
		result.indices.push_back(bot_center + 1 + static_cast<std::uint32_t>(i));
	}
	return result;
}

Mesh cone(float radius, float height, int segments)
{
	if (segments < 3) {
		segments = 3;
	}
	const float half_h = height * 0.5f;

	Mesh result;
	result.positions.reserve(static_cast<std::size_t>(segments + 2));
	result.normals.reserve(result.positions.capacity());
	result.uvs.reserve(result.positions.capacity());
	result.indices.reserve(static_cast<std::size_t>(segments) * 6);

	// Apex
	result.positions.push_back({0.0f, half_h, 0.0f});
	result.normals.push_back({0.0f, 1.0f, 0.0f});
	result.uvs.push_back({0.5f, 0.0f});
	// Base ring
	for (int i = 0; i <= segments; ++i) {
		const float u     = static_cast<float>(i) / static_cast<float>(segments);
		const float theta = u * 2.0f * kPi;
		const float c     = std::cos(theta);
		const float s     = std::sin(theta);
		result.positions.push_back({radius * c, -half_h, radius * s});
		// Normal for cone side: tilted
		const float slope = radius / height;
		const float inv   = 1.0f / std::sqrt(1.0f + slope * slope);
		result.normals.push_back({c * inv, slope * inv, s * inv});
		result.uvs.push_back({u, 1.0f});
	}
	// Side indices
	for (int i = 0; i < segments; ++i) {
		result.indices.push_back(0);
		result.indices.push_back(1 + static_cast<std::uint32_t>(i));
		result.indices.push_back(2 + static_cast<std::uint32_t>(i));
	}
	// Base cap
	const std::uint32_t bot_center = static_cast<std::uint32_t>(result.positions.size());
	result.positions.push_back({0.0f, -half_h, 0.0f});
	result.normals.push_back({0.0f, -1.0f, 0.0f});
	result.uvs.push_back({0.5f, 0.5f});
	for (int i = 0; i <= segments; ++i) {
		const float theta = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * kPi;
		result.positions.push_back({radius * std::cos(theta), -half_h, radius * std::sin(theta)});
		result.normals.push_back({0.0f, -1.0f, 0.0f});
		result.uvs.push_back({0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)});
	}
	for (int i = 0; i < segments; ++i) {
		result.indices.push_back(bot_center);
		result.indices.push_back(bot_center + 2 + static_cast<std::uint32_t>(i));
		result.indices.push_back(bot_center + 1 + static_cast<std::uint32_t>(i));
	}
	return result;
}

Mesh plane(float size)
{
	const float h = size * 0.5f;
	Mesh result;
	result.positions = {
	        {-h, 0.0f, -h}, {h, 0.0f, -h}, {h, 0.0f, h}, {-h, 0.0f, h}};
	result.normals = {
	        {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
	        {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
	result.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
	result.indices = {0, 1, 2, 0, 2, 3};
	return result;
}

Mesh pyramid(float base_size, float height)
{
	const float hb = base_size * 0.5f;
	const float hh = height * 0.5f;
	Mesh result;
	// 4 base corners + 1 apex
	result.positions = {
	        {-hb, -hh, -hb}, {hb, -hh, -hb}, {hb, -hh, hb}, {-hb, -hh, hb},
	        {0.0f, hh, 0.0f}};
	// Base normal
	result.normals = {
	        {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
	        {0.0f, -1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
	        {0.0f, 1.0f, 0.0f}};
	result.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0.5f}};
	// Base (two triangles, facing down)
	result.indices = {0, 2, 1, 0, 3, 2};
	// Sides
	result.indices.insert(result.indices.end(), {0, 1, 4}); // front
	result.indices.insert(result.indices.end(), {1, 2, 4}); // right
	result.indices.insert(result.indices.end(), {2, 3, 4}); // back
	result.indices.insert(result.indices.end(), {3, 0, 4}); // left
	return result;
}

Mesh torus(float major_radius, float minor_radius, int major_segments, int minor_segments)
{
	if (major_segments < 3) {
		major_segments = 3;
	}
	if (minor_segments < 3) {
		minor_segments = 3;
	}
	Mesh result;
	result.positions.reserve(static_cast<std::size_t>((major_segments + 1) * (minor_segments + 1)));
	result.normals.reserve(result.positions.capacity());
	result.uvs.reserve(result.positions.capacity());
	result.indices.reserve(static_cast<std::size_t>(major_segments) * static_cast<std::size_t>(minor_segments) * 6);

	for (int i = 0; i <= major_segments; ++i) {
		const float u     = static_cast<float>(i) / static_cast<float>(major_segments);
		const float theta = u * 2.0f * kPi;
		const float ct    = std::cos(theta);
		const float st    = std::sin(theta);
		for (int j = 0; j <= minor_segments; ++j) {
			const float v     = static_cast<float>(j) / static_cast<float>(minor_segments);
			const float phi   = v * 2.0f * kPi;
			const float cp    = std::cos(phi);
			const float sp    = std::sin(phi);
			const glm::vec3 normal{cp * ct, sp, cp * st};
			result.normals.push_back(normal);
			result.positions.push_back({(major_radius + minor_radius * cp) * ct,
			                            minor_radius * sp,
			                            (major_radius + minor_radius * cp) * st});
			result.uvs.push_back({u, v});
		}
	}
	const std::uint32_t stride = static_cast<std::uint32_t>(minor_segments + 1);
	for (int i = 0; i < major_segments; ++i) {
		for (int j = 0; j < minor_segments; ++j) {
			const std::uint32_t a = static_cast<std::uint32_t>(i) * stride + static_cast<std::uint32_t>(j);
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

Mesh terrain(float size, int segments, float height, int seed)
{
	if (segments < 2) {
		segments = 2;
	}
	if (height < 0.0f) {
		height = 0.0f;
	}

	// Heights come from the shared file-local terrain_fbm() so terrain() and
	// terrain_height_at() stay bit-identical for identical inputs.

	Mesh result;
	const int verts_per_side = segments + 1;
	result.positions.reserve(static_cast<std::size_t>(verts_per_side) * verts_per_side);
	result.normals.reserve(result.positions.capacity());
	result.uvs.reserve(result.positions.capacity());
	result.indices.reserve(static_cast<std::size_t>(segments) * segments * 6);

	const float half_size = size * 0.5f;
	const float step = size / static_cast<float>(segments);

	// Generate vertices
	for (int z = 0; z <= segments; ++z) {
		for (int x = 0; x <= segments; ++x) {
			const float wx = -half_size + static_cast<float>(x) * step;
			const float wz = -half_size + static_cast<float>(z) * step;
			// Scale noise to get mountains
			const float nx = static_cast<float>(x) / static_cast<float>(segments) * 4.0f;
			const float nz = static_cast<float>(z) / static_cast<float>(segments) * 4.0f;
			const float h = terrain_fbm(nx, nz, seed) * height;
			result.positions.push_back({wx, h, wz});
			result.uvs.push_back({
			        static_cast<float>(x) / static_cast<float>(segments),
			        static_cast<float>(z) / static_cast<float>(segments)});
		}
	}

	// Compute normals from heightfield
	for (int z = 0; z <= segments; ++z) {
		for (int x = 0; x <= segments; ++x) {
			const int idx = z * verts_per_side + x;
			const float hL = (x > 0) ? result.positions[idx - 1].y : result.positions[idx].y;
			const float hR = (x < segments) ? result.positions[idx + 1].y : result.positions[idx].y;
			const float hD = (z > 0) ? result.positions[idx - verts_per_side].y : result.positions[idx].y;
			const float hU = (z < segments) ? result.positions[idx + verts_per_side].y : result.positions[idx].y;
			const glm::vec3 normal = glm::normalize(glm::vec3{hL - hR, 2.0f * step, hD - hU});
			result.normals.push_back(normal);
		}
	}

	// Generate indices
	for (int z = 0; z < segments; ++z) {
		for (int x = 0; x < segments; ++x) {
			const int a = z * verts_per_side + x;
			const int b = a + 1;
			const int c = a + verts_per_side;
			const int d = c + 1;
			result.indices.push_back(a);
			result.indices.push_back(c);
			result.indices.push_back(b);
			result.indices.push_back(b);
			result.indices.push_back(c);
			result.indices.push_back(d);
		}
	}
	return result;
}

float terrain_height_at(float x, float z, float size, float height, int seed)
{
	const float half_size = size * 0.5f;
	const float nx = (x + half_size) / size * 4.0f;
	const float nz = (z + half_size) / size * 4.0f;
	return terrain_fbm(nx, nz, seed) * height;
}

BoundingSphere compute_mesh_bounds(const Mesh& mesh)
{
	if (mesh.positions.empty()) {
		return BoundingSphere{};
	}
	glm::vec3 minimum = mesh.positions.front();
	glm::vec3 maximum = mesh.positions.front();
	for (const glm::vec3& position : mesh.positions) {
		minimum = glm::min(minimum, position);
		maximum = glm::max(maximum, position);
	}
	BoundingSphere bounds;
	bounds.center = (minimum + maximum) * 0.5f;
	for (const glm::vec3& position : mesh.positions) {
		bounds.radius = std::max(bounds.radius, glm::length(position - bounds.center));
	}
	return bounds;
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
	if (source == "builtin:cylinder") {
		out = cylinder();
		return true;
	}
	if (source == "builtin:cone") {
		out = cone();
		return true;
	}
	if (source == "builtin:plane") {
		out = plane();
		return true;
	}
	if (source == "builtin:pyramid") {
		out = pyramid();
		return true;
	}
	if (source == "builtin:torus") {
		out = torus();
		return true;
	}
	if (source == "builtin:terrain") {
		out = terrain();
		return true;
	}
	// "builtin:sphere:<segments>:<rings>" lets LOD levels differ in tessellation.
	constexpr const char* kSpherePrefix = "builtin:sphere:";
	if (source.rfind(kSpherePrefix, 0) == 0) {
		const std::string rest  = source.substr(std::char_traits<char>::length(kSpherePrefix));
		const std::size_t colon = rest.find(':');
		if (colon == std::string::npos) {
			if (error) {
				*error = "expected builtin:sphere:<segments>:<rings>, got " + source;
			}
			return false;
		}
		try {
			const int segments = std::stoi(rest.substr(0, colon));
			const int rings    = std::stoi(rest.substr(colon + 1));
			if (segments < 3 || rings < 2) {
				if (error) {
					*error = "sphere segments must be >= 3 and rings >= 2: " + source;
				}
				return false;
			}
			out = sphere(0.75f, segments, rings);
			return true;
		} catch (const std::exception&) {
			if (error) {
				*error = "bad builtin:sphere parameters: " + source;
			}
			return false;
		}
	}
	if (source.rfind("builtin:", 0) == 0) {
		if (error) {
			*error = "unknown builtin mesh: " + source;
		}
		return false;
	}
	return load_glb(source, out, error);
}

} // namespace lotus::mesh
