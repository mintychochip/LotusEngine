#pragma once

#include <cstdint>
#include <string>

namespace lotus {

/// Handle to geometry resident in the renderer's mesh cache
/// (GpuRenderer::acquire_mesh). Valid until the renderer evicts the mesh for
/// being idle, which only happens between frames.
using GpuMeshId = uint32_t;

/// "No mesh": a batch with this id draws from the immediate vertex stream.
inline constexpr GpuMeshId kNoGpuMesh = 0xFFFFFFFFu;

/// One lit draw sharing an albedo texture (keyed by \p albedo_id), in one of
/// two forms:
///
///  * immediate (mesh == kNoGpuMesh): \p vertex_count world-space vertices
///    starting at \p first_vertex of the stream written through
///    GpuRenderer::begin_geometry_upload;
///  * instanced (mesh != kNoGpuMesh): the cached mesh drawn \p instance_count
///    times, one per record starting at \p first_instance of the instance
///    array written through the same call. \p vertex_count is then the total
///    number of vertices the batch submits (indices x instances), kept for
///    statistics; the draw itself takes its index count from the mesh.
struct GpuDrawBatch {
	std::string albedo_id;
	uint32_t first_vertex{0};
	uint32_t vertex_count{0};
	GpuMeshId mesh{kNoGpuMesh};
	uint32_t first_instance{0};
	uint32_t instance_count{0};
	/// Optional tangent-space normal map key; empty binds the flat normal.
	std::string normal_id;
};

} // namespace lotus
