#pragma once

#include "lotus/scene/components.hpp"

namespace lotus::mesh {

/// Axis-aligned cube centred on the origin, one normal per face.
Mesh cube(float size = 1.0f);

/// UV sphere centred on the origin.
Mesh sphere(float radius = 0.75f, int segments = 24, int rings = 16);

/// Local-space bounding sphere of \p mesh (centre of the AABB plus the
/// furthest vertex distance). Returns a zero radius for empty geometry.
BoundingSphere compute_mesh_bounds(const Mesh& mesh);

/// Resolves a MeshSource path into geometry:
///   "builtin:cube"   -> cube()
///   "builtin:sphere" -> sphere()
///   anything else    -> a .glb file loaded from disk
/// Returns false and fills \p error when the source cannot be resolved.
bool resolve_mesh(const std::string& source, Mesh& out,
                  std::string* error = nullptr);

} // namespace lotus::mesh
