#pragma once

#include <string>

#include "lotus/scene/components.hpp"

namespace lotus {

/// Loads a binary glTF (.glb) file into mesh geometry.
///
/// All primitives of all meshes are merged into a single vertex/index set, and
/// node transforms are ignored (place the entity with a Transform instead).
/// Only the embedded buffer is supported; external .bin buffers are rejected.
/// When a primitive has no NORMAL attribute, smooth normals are computed.
bool load_glb(const std::string& path, Mesh& out, std::string* error = nullptr);

} // namespace lotus
