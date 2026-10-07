#include "lotus/scene/lod.hpp"

namespace lotus {

int select_lod_level(const std::vector<float>& max_distances, float distance)
{
	for (std::size_t level = 0; level < max_distances.size(); ++level) {
		if (distance <= max_distances[level]) {
			return static_cast<int>(level);
		}
	}
	return -1;
}

} // namespace lotus
