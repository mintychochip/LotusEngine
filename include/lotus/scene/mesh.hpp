#pragma once

#include "lotus/scene/components.hpp"

namespace lotus::mesh {

/// Axis-aligned cube centred on the origin, one normal per face.
Mesh cube(float size = 1.0f);

/// UV sphere centred on the origin.
Mesh sphere(float radius = 0.75f, int segments = 24, int rings = 16);

/// Cylinder centred on the origin, axis along Y, height 1, radius 0.5.
Mesh cylinder(float radius = 0.5f, float height = 1.0f, int segments = 24);

/// Cone centred on the origin, apex at +Y, base at -Y, height 1, radius 0.5.
Mesh cone(float radius = 0.5f, float height = 1.0f, int segments = 24);

/// Flat plane on XZ, centred on origin, size 1x1.
Mesh plane(float size = 1.0f);

/// Square pyramid centred on origin, base at -Y, apex at +Y, height 1.
Mesh pyramid(float base_size = 1.0f, float height = 1.0f);

/// Torus centred on origin, lying on XZ plane, major radius 0.5, minor 0.2.
Mesh torus(float major_radius = 0.5f, float minor_radius = 0.2f,
           int major_segments = 24, int minor_segments = 12);

/// Heightfield terrain with mountainous displacement.
/// \p size is the world-space width/depth, \p segments is the grid resolution,
/// \p height is the maximum peak height, \p seed is the noise seed.
Mesh terrain(float size = 50.0f, int segments = 64, float height = 8.0f,
             int seed = 42);

/// Query terrain height at world-space (x, z) using the same noise as terrain().
float terrain_height_at(float x, float z, float size = 50.0f,
                        float height = 8.0f, int seed = 42);

/// Local-space bounding sphere of \p mesh (centre of the AABB plus the
/// furthest vertex distance). Returns a zero radius for empty geometry.
BoundingSphere compute_mesh_bounds(const Mesh& mesh);

/// Resolves a MeshSource path into geometry:
///   "builtin:cube"   -> cube()
///   "builtin:sphere" -> sphere()
///   "builtin:cylinder" -> cylinder()
///   "builtin:cone" -> cone()
///   "builtin:plane" -> plane()
///   "builtin:pyramid" -> pyramid()
///   "builtin:torus" -> torus()
///   anything else    -> a .glb file loaded from disk
/// Returns false and fills \p error when the source cannot be resolved.
bool resolve_mesh(const std::string& source, Mesh& out,
                  std::string* error = nullptr);

} // namespace lotus::mesh
