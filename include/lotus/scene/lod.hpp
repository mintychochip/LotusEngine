#pragma once

#include <vector>

namespace lotus {

/// Picks a level of detail for \p distance.
///
/// \p max_distances is ordered from the most detailed level to the least, each
/// entry being the largest distance at which that level is still used. Returns
/// the level index, or -1 when the distance exceeds every level (the object is
/// past its last LOD and should not be drawn).
///
/// Empty input returns -1.
int select_lod_level(const std::vector<float>& max_distances, float distance);

} // namespace lotus
