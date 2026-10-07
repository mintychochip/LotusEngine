#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "lotus/scene/scene.hpp"
#include "lotus/scene/streaming.hpp"

namespace lotus {

/// Applies \c ResidencyPolicy each frame: additive loads and origin unloads.
class ResidencySystem {
public:
	void set_scene_directory(const std::string& dir) { scene_dir_ = dir; }

	bool load_catalog(const std::string& path, std::string* error = nullptr);

	void update(Scene& scene, const glm::vec3& viewer);

	const std::vector<std::string>& resident() const { return resident_; }
	const std::vector<StreamRegion>& regions() const { return regions_; }

	void set_load_margin(float margin) { load_margin_ = margin; }
	void set_unload_margin(float margin) { unload_margin_ = margin; }

private:
	std::string resolve_path(const std::string& path) const;

	std::vector<StreamRegion> regions_;
	std::vector<std::string> resident_;
	ResidencyPolicy policy_;
	std::string scene_dir_;
	float load_margin_{0.0f};
	float unload_margin_{5.0f};
};

} // namespace lotus
