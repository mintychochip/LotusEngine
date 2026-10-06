#include "lotus/scene/streaming.hpp"

#include <algorithm>

namespace lotus {
namespace {

bool is_resident(const std::vector<std::string>& resident, const std::string& name)
{
	return std::find(resident.begin(), resident.end(), name) != resident.end();
}

} // namespace

std::vector<std::string> ResidencyPolicy::to_load(
        const std::vector<StreamRegion>& regions, const glm::vec3& viewer,
        const std::vector<std::string>& resident, float load_margin) const
{
	std::vector<std::string> out;
	for (const StreamRegion& region : regions) {
		if (is_resident(resident, region.name)) {
			continue; // already in the scene; loading again would duplicate it
		}
		// Inside the region, or within the load margin of its edge.
		if (glm::distance(viewer, region.center) <= region.radius + load_margin) {
			out.push_back(region.name);
		}
	}
	return out;
}

std::vector<std::string> ResidencyPolicy::to_unload(
        const std::vector<StreamRegion>& regions, const glm::vec3& viewer,
        const std::vector<std::string>& resident, float unload_margin) const
{
	std::vector<std::string> out;
	for (const StreamRegion& region : regions) {
		if (!is_resident(resident, region.name)) {
			continue;
		}
		// Kept until the viewer is strictly beyond radius + margin, which is
		// what prevents load/unload thrashing at a boundary.
		if (glm::distance(viewer, region.center) >
		    region.radius + unload_margin) {
			out.push_back(region.name);
		}
	}
	return out;
}

} // namespace lotus
