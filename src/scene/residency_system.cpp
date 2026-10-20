#include "lotus/scene/residency_system.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "lotus/scene/prefab.hpp"

namespace lotus {

std::string ResidencySystem::resolve_path(const std::string& path) const
{
	if (path.empty()) {
		return {};
	}
	if (path.front() == '/' || (path.size() > 1 && path[1] == ':')) {
		return path;
	}
	if (scene_dir_.empty()) {
		return path;
	}
	return scene_dir_ + "/" + path;
}

bool ResidencySystem::load_catalog(const std::string& path, std::string* error)
{
	std::ifstream in(path);
	if (!in) {
		if (error) {
			*error = "cannot open regions catalog: " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	try {
		const nlohmann::json j = nlohmann::json::parse(buffer.str());
		std::vector<StreamRegion> parsed;
		const auto& list =
		        j.contains("regions") && j.at("regions").is_array()
		                ? j.at("regions")
		                : j;
		if (!list.is_array()) {
			if (error) {
				*error = "regions catalog must be an array or {regions:[]}";
			}
			return false;
		}
		for (const auto& entry : list) {
			StreamRegion region;
			if (entry.contains("name") && entry.at("name").is_string()) {
				region.name = entry.at("name").get<std::string>();
			}
			if (entry.contains("path") && entry.at("path").is_string()) {
				region.path = entry.at("path").get<std::string>();
			}
			if (entry.contains("center") && entry.at("center").is_array() &&
			    entry.at("center").size() == 3) {
				const auto& c = entry.at("center");
				region.center = {c[0].get<float>(), c[1].get<float>(),
				                 c[2].get<float>()};
			}
			if (entry.contains("radius") && entry.at("radius").is_number()) {
				region.radius = entry.at("radius").get<float>();
			}
			if (entry.contains("scenes") && entry.at("scenes").is_array()) {
				for (const auto& name : entry.at("scenes")) {
					if (name.is_string()) {
						region.scenes.push_back(name.get<std::string>());
					}
				}
			}
			if (!region.name.empty() && !region.path.empty()) {
				parsed.push_back(std::move(region));
			}
		}
		regions_ = std::move(parsed);
		return true;
	} catch (const std::exception& ex) {
		if (error) {
			*error = ex.what();
		}
		return false;
	}
}

void ResidencySystem::update(Scene& scene, const glm::vec3& viewer)
{
	const std::vector<std::string> to_load =
	        policy_.to_load(regions_, viewer, resident_, load_margin_);
	uint32_t loads_this_frame = 0;
	for (const std::string& name : to_load) {
		if (max_loads_per_frame_ > 0 &&
		    loads_this_frame >= max_loads_per_frame_) {
			break;
		}
		const StreamRegion* region = nullptr;
		for (const StreamRegion& candidate : regions_) {
			if (candidate.name == name) {
				region = &candidate;
				break;
			}
		}
		if (!region) {
			continue;
		}
		if (!region->scenes.empty() &&
		    std::find(region->scenes.begin(), region->scenes.end(),
		              scene.name()) == region->scenes.end()) {
			continue;
		}
		std::string error;
		const std::string path = resolve_path(region->path);
		if (!load_scene_additive_from_file(scene, path, &error, region->name)) {
			continue;
		}
		resident_.push_back(name);
		++loads_this_frame;
	}

	const std::vector<std::string> to_unload =
	        policy_.to_unload(regions_, viewer, resident_, unload_margin_);
	for (const std::string& name : to_unload) {
		scene.destroy_by_origin(name);
		resident_.erase(
		        std::remove(resident_.begin(), resident_.end(), name),
		        resident_.end());
	}
}

} // namespace lotus
