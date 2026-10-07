#include "lotus/scene/glb.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <nlohmann/json.hpp>

namespace lotus {
namespace {

constexpr std::uint32_t kGlbMagic      = 0x46546C67; // "glTF"
constexpr std::uint32_t kChunkJson     = 0x4E4F534A; // "JSON"
constexpr std::uint32_t kChunkBin      = 0x004E4942; // "BIN\0"

// glTF component types.
constexpr int kComponentByte   = 5120;
constexpr int kComponentUByte  = 5121;
constexpr int kComponentShort  = 5122;
constexpr int kComponentUShort = 5123;
constexpr int kComponentUInt   = 5125;
constexpr int kComponentFloat  = 5126;

struct Glb {
	nlohmann::json json;
	std::vector<std::uint8_t> bin;
};

std::size_t component_size(int component_type)
{
	switch (component_type) {
	case kComponentByte:
	case kComponentUByte:
		return 1;
	case kComponentShort:
	case kComponentUShort:
		return 2;
	case kComponentUInt:
	case kComponentFloat:
		return 4;
	default:
		return 0;
	}
}

int type_components(const std::string& type)
{
	if (type == "SCALAR") {
		return 1;
	}
	if (type == "VEC2") {
		return 2;
	}
	if (type == "VEC3") {
		return 3;
	}
	if (type == "VEC4") {
		return 4;
	}
	if (type == "MAT4") {
		return 16;
	}
	return 0;
}

bool read_glb(const std::string& path, Glb& out, std::string* error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (error) {
			*error = "cannot open " + path;
		}
		return false;
	}
	std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
	                                std::istreambuf_iterator<char>()};
	if (bytes.size() < 12) {
		if (error) {
			*error = "not a glb: too small";
		}
		return false;
	}

	std::uint32_t magic = 0;
	std::uint32_t version = 0;
	std::memcpy(&magic, bytes.data(), 4);
	std::memcpy(&version, bytes.data() + 4, 4);
	if (magic != kGlbMagic) {
		if (error) {
			*error = "not a glb: bad magic";
		}
		return false;
	}
	if (version != 2) {
		if (error) {
			*error = "unsupported glb version: " + std::to_string(version);
		}
		return false;
	}

	bool has_json = false;
	std::size_t offset = 12;
	while (offset + 8 <= bytes.size()) {
		std::uint32_t chunk_length = 0;
		std::uint32_t chunk_type   = 0;
		std::memcpy(&chunk_length, bytes.data() + offset, 4);
		std::memcpy(&chunk_type, bytes.data() + offset + 4, 4);
		offset += 8;
		if (offset + chunk_length > bytes.size()) {
			break;
		}
		if (chunk_type == kChunkJson) {
			try {
				out.json = nlohmann::json::parse(bytes.begin() + offset,
				                                 bytes.begin() + offset + chunk_length);
				has_json = true;
			} catch (const std::exception& ex) {
				if (error) {
					*error = std::string{"glb JSON error: "} + ex.what();
				}
				return false;
			}
		} else if (chunk_type == kChunkBin) {
			out.bin.assign(bytes.begin() + offset,
			               bytes.begin() + offset + chunk_length);
		}
		offset += chunk_length;
		offset = (offset + 3) & ~static_cast<std::size_t>(3); // 4-byte aligned
	}

	if (!has_json) {
		if (error) {
			*error = "glb has no JSON chunk";
		}
		return false;
	}
	return true;
}

struct AccessorView {
	const std::uint8_t* data{nullptr};
	std::size_t count{0};
	std::size_t stride{0};
	int component_type{0};
	int components{0};
};

bool resolve_accessor(const Glb& glb, int index, AccessorView& out,
                      std::string* error)
{
	const auto fail = [error](const std::string& message) {
		if (error) {
			*error = message;
		}
		return false;
	};

	const auto& accessors = glb.json.at("accessors");
	if (index < 0 || static_cast<std::size_t>(index) >= accessors.size()) {
		return fail("accessor index out of range");
	}
	const auto& accessor = accessors.at(static_cast<std::size_t>(index));
	if (!accessor.contains("bufferView")) {
		return fail("accessor has no bufferView (sparse accessors unsupported)");
	}

	const int buffer_view_index = accessor.at("bufferView").get<int>();
	const auto& buffer_views     = glb.json.at("bufferViews");
	if (buffer_view_index < 0 ||
	    static_cast<std::size_t>(buffer_view_index) >= buffer_views.size()) {
		return fail("bufferView index out of range");
	}
	const auto& buffer_view = buffer_views.at(static_cast<std::size_t>(buffer_view_index));

	if (buffer_view.value("buffer", 0) != 0) {
		return fail("only the embedded glb buffer is supported");
	}

	out.component_type = accessor.at("componentType").get<int>();
	out.components = type_components(accessor.at("type").get<std::string>());
	out.count      = accessor.at("count").get<std::size_t>();
	if (out.components == 0 || component_size(out.component_type) == 0) {
		return fail("unsupported accessor type");
	}

	const std::size_t packed = static_cast<std::size_t>(out.components) *
	                           component_size(out.component_type);
	out.stride = buffer_view.contains("byteStride")
	                     ? buffer_view.at("byteStride").get<std::size_t>()
	                     : packed;
	if (out.stride < packed) {
		out.stride = packed;
	}

	const std::size_t base = buffer_view.value("byteOffset", std::size_t{0}) +
	                         accessor.value("byteOffset", std::size_t{0});
	if (base + out.stride * (out.count ? out.count - 1 : 0) + packed >
	    glb.bin.size()) {
		return fail("accessor reads past the end of the buffer");
	}
	out.data = glb.bin.data() + base;
	return true;
}

bool read_vec2(const Glb& glb, int accessor_index, std::vector<glm::vec2>& out,
               std::string* error)
{
	AccessorView view;
	if (!resolve_accessor(glb, accessor_index, view, error)) {
		return false;
	}
	if (view.component_type != kComponentFloat || view.components != 2) {
		if (error) {
			*error = "TEXCOORD must be a FLOAT VEC2 accessor";
		}
		return false;
	}
	out.reserve(out.size() + view.count);
	for (std::size_t i = 0; i < view.count; ++i) {
		glm::vec2 value{0.0f};
		std::memcpy(&value, view.data + i * view.stride, sizeof(glm::vec2));
		out.push_back(value);
	}
	return true;
}

bool read_vec3(const Glb& glb, int accessor_index, std::vector<glm::vec3>& out,
               std::string* error)
{
	AccessorView view;
	if (!resolve_accessor(glb, accessor_index, view, error)) {
		return false;
	}
	if (view.component_type != kComponentFloat || view.components != 3) {
		if (error) {
			*error = "POSITION/NORMAL must be a FLOAT VEC3 accessor";
		}
		return false;
	}
	out.reserve(out.size() + view.count);
	for (std::size_t i = 0; i < view.count; ++i) {
		glm::vec3 value{0.0f};
		std::memcpy(&value, view.data + i * view.stride, sizeof(glm::vec3));
		out.push_back(value);
	}
	return true;
}

bool read_indices(const Glb& glb, int accessor_index,
                  std::vector<std::uint32_t>& out, std::string* error)
{
	AccessorView view;
	if (!resolve_accessor(glb, accessor_index, view, error)) {
		return false;
	}
	if (view.components != 1) {
		if (error) {
			*error = "indices must be a SCALAR accessor";
		}
		return false;
	}
	out.reserve(out.size() + view.count);
	for (std::size_t i = 0; i < view.count; ++i) {
		const std::uint8_t* element = view.data + i * view.stride;
		std::uint32_t value = 0;
		switch (view.component_type) {
		case kComponentUByte:
			value = *element;
			break;
		case kComponentUShort: {
			std::uint16_t short_value = 0;
			std::memcpy(&short_value, element, 2);
			value = short_value;
			break;
		}
		case kComponentUInt:
			std::memcpy(&value, element, 4);
			break;
		default:
			if (error) {
				*error = "unsupported index component type";
			}
			return false;
		}
		out.push_back(value);
	}
	return true;
}

void compute_normals(Mesh& mesh)
{
	mesh.normals.assign(mesh.positions.size(), glm::vec3{0.0f});
	for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
		const std::uint32_t a = mesh.indices[i];
		const std::uint32_t b = mesh.indices[i + 1];
		const std::uint32_t c = mesh.indices[i + 2];
		if (a >= mesh.positions.size() || b >= mesh.positions.size() ||
		    c >= mesh.positions.size()) {
			continue;
		}
		const glm::vec3 face = glm::cross(mesh.positions[b] - mesh.positions[a],
		                                  mesh.positions[c] - mesh.positions[a]);
		mesh.normals[a] += face;
		mesh.normals[b] += face;
		mesh.normals[c] += face;
	}
	for (glm::vec3& normal : mesh.normals) {
		const float length = glm::length(normal);
		normal = length > 0.0f ? normal / length : glm::vec3{0.0f, 1.0f, 0.0f};
	}
}

} // namespace

bool load_glb(const std::string& path, Mesh& out, std::string* error)
{
	Glb glb;
	if (!read_glb(path, glb, error)) {
		return false;
	}
	if (!glb.json.contains("meshes") || !glb.json.at("meshes").is_array()) {
		if (error) {
			*error = "glb has no meshes";
		}
		return false;
	}

	Mesh result;
	for (const auto& mesh : glb.json.at("meshes")) {
		if (!mesh.contains("primitives")) {
			continue;
		}
		for (const auto& primitive : mesh.at("primitives")) {
			if (!primitive.contains("attributes") ||
			    !primitive.at("attributes").contains("POSITION")) {
				continue;
			}
			const auto& attributes = primitive.at("attributes");

			const std::uint32_t base =
			        static_cast<std::uint32_t>(result.positions.size());
			const std::size_t position_count = result.positions.size();
			if (!read_vec3(glb, attributes.at("POSITION").get<int>(),
			               result.positions, error)) {
				return false;
			}
			const std::size_t added = result.positions.size() - position_count;

			if (attributes.contains("NORMAL")) {
				if (!read_vec3(glb, attributes.at("NORMAL").get<int>(),
				               result.normals, error)) {
					return false;
				}
			}
			if (attributes.contains("TEXCOORD_0")) {
				if (!read_vec2(glb, attributes.at("TEXCOORD_0").get<int>(),
				               result.uvs, error)) {
					return false;
				}
			}

			const std::size_t index_count = result.indices.size();
			if (primitive.contains("indices")) {
				if (!read_indices(glb, primitive.at("indices").get<int>(),
				                  result.indices, error)) {
					return false;
				}
				for (std::size_t i = index_count; i < result.indices.size(); ++i) {
					result.indices[i] += base;
				}
			} else {
				// Non-indexed primitive: synthesise indices.
				for (std::size_t i = 0; i < added; ++i) {
					result.indices.push_back(base + static_cast<std::uint32_t>(i));
				}
			}
		}
	}

	if (result.positions.empty() || result.indices.empty()) {
		if (error) {
			*error = "glb contained no usable geometry";
		}
		return false;
	}
	if (result.normals.size() != result.positions.size()) {
		compute_normals(result);
	}

	out = std::move(result);
	return true;
}

} // namespace lotus
