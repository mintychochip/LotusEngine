#pragma once

#include <cstdint>
#include <string>

namespace lotus {

/// One lit draw range sharing the same albedo texture (keyed by \p albedo_id).
struct GpuDrawBatch {
	std::string albedo_id;
	uint32_t first_vertex{0};
	uint32_t vertex_count{0};
};

} // namespace lotus
