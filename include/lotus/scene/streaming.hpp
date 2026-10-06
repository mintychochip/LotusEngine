#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/scene.hpp"

namespace lotus {

/// One loadable region of the world: a scene file plus the world-space sphere
/// it occupies.
struct StreamRegion {
	std::string name;
	/// Scene file to merge when the region becomes resident.
	std::string path;
	glm::vec3 center{0.0f};
	float radius{1.0f};

	bool contains(const glm::vec3& point) const
	{
		return glm::distance(point, center) <= radius;
	}
};

/// Decides which regions should be resident for a viewer position, so a large
/// world can be merged in and unloaded by origin instead of held in one scene.
///
/// Deliberately a pure function of (regions, viewer, hysteresis): it owns no
/// scene and does no loading. That keeps the policy testable without a GPU, and
/// keeps the loading path (load_scene_additive_from_file + destroy_by_origin)
/// in the caller where its atomicity is already established.
///
/// Hysteresis matters: without it, a viewer standing on a region boundary would
/// load and unload every frame. A region already resident is kept until the
/// viewer is \p unload_margin beyond its radius.
class ResidencyPolicy {
public:
	/// Regions to load: those containing the viewer, or within
	/// \p load_margin of one, that are not already resident.
	std::vector<std::string> to_load(const std::vector<StreamRegion>& regions,
	                                 const glm::vec3& viewer,
	                                 const std::vector<std::string>& resident,
	                                 float load_margin = 0.0f) const;

	/// Resident regions whose centre is further than radius + \p unload_margin
	/// from the viewer, so they can be unloaded. The margin is what stops a
	/// boundary-straddling viewer from thrashing.
	std::vector<std::string> to_unload(const std::vector<StreamRegion>& regions,
	                                   const glm::vec3& viewer,
	                                   const std::vector<std::string>& resident,
	                                   float unload_margin) const;
};

} // namespace lotus
