#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace lotus {

class Scene;

/// Player-centric snapshot separate from scene editor JSON.
struct GameSaveData {
	int version{1};
	std::string scene_name;
	std::string player_name{"player"};
	glm::vec3 player_position{0.0f};
	glm::vec3 player_rotation{0.0f};
	std::vector<std::string> resident_regions;
};

bool save_game_to_file(const GameSaveData& data, const std::string& path,
                       std::string* error = nullptr);
bool load_game_from_file(const std::string& path, GameSaveData& out,
                         std::string* error = nullptr);

/// Builds a snapshot from the live scene and residency list.
GameSaveData capture_game_state(const Scene& scene,
                                const std::string& scene_name,
                                const std::vector<std::string>& resident_regions);

/// Writes player transform when the named entity exists.
void apply_game_state(Scene& scene, const GameSaveData& data);

} // namespace lotus
