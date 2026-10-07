#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/components.hpp"

namespace lotus {

class AssetRegistry;

/// One glTF material slot after import (registry key + base colour).
struct GlbMaterialSlot {
	glm::vec3 base_color{1.0f, 1.0f, 1.0f};
	std::string albedo_id;
};

/// Loads a binary glTF (.glb) file into mesh geometry.
///
/// All primitives of all meshes are merged into a single vertex/index set, and
/// node transforms are ignored (place the entity with a Transform instead).
/// Only the embedded buffer is supported; external .bin buffers are rejected.
/// When a primitive has no NORMAL attribute, smooth normals are computed.
bool load_glb(const std::string& path, Mesh& out, std::string* error = nullptr);

/// Reads materials from a .glb. Embedded images are written as PNG sidecars
/// under \p assets_dir and registered as \c "<stem>#<material index>".
/// \p first_primitive_material is the material index on the first primitive
/// that references one, or -1.
bool import_glb_materials(const std::string& path, const std::string& stem,
                          const std::string& assets_dir,
                          AssetRegistry& registry,
                          std::vector<GlbMaterialSlot>& materials,
                          int& first_primitive_material,
                          std::string* error = nullptr);

} // namespace lotus
