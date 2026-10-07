#include "lotus/scene/game_save.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "lotus/scene/components.hpp"
#include "lotus/scene/scene.hpp"

namespace lotus {
namespace {

glm::vec3 read_vec3(const nlohmann::json& j)
{
	if (!j.is_array() || j.size() != 3) {
		return glm::vec3{0.0f};
	}
	return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

void write_vec3(nlohmann::json& j, const glm::vec3& v)
{
	j = nlohmann::json::array({v.x, v.y, v.z});
}

} // namespace

GameSaveData capture_game_state(const Scene& scene,
                                const std::string& scene_name,
                                const std::vector<std::string>& resident_regions)
{
	GameSaveData data;
	data.scene_name        = scene_name;
	data.resident_regions  = resident_regions;
	const Entity player    = scene.find(data.player_name);
	if (player == NullEntity) {
		return data;
	}
	if (const Transform* transform = scene.registry().try_get<Transform>(player)) {
		data.player_position = transform->position;
		data.player_rotation = transform->rotation;
	}
	return data;
}

void apply_game_state(Scene& scene, const GameSaveData& data)
{
	const Entity player = scene.find(data.player_name);
	if (player == NullEntity) {
		return;
	}
	if (Transform* transform = scene.registry().try_get<Transform>(player)) {
		transform->position = data.player_position;
		transform->rotation = data.player_rotation;
	}
}

bool save_game_to_file(const GameSaveData& data, const std::string& path,
                       std::string* error)
{
	nlohmann::json j;
	j["version"]      = data.version;
	j["scene_name"]   = data.scene_name;
	j["player_name"]  = data.player_name;
	write_vec3(j["player_position"], data.player_position);
	write_vec3(j["player_rotation"], data.player_rotation);
	j["resident_regions"] = data.resident_regions;

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot write " + path;
		}
		return false;
	}
	out << j.dump(2) << '\n';
	return true;
}

bool load_game_from_file(const std::string& path, GameSaveData& out,
                         std::string* error)
{
	std::ifstream in(path);
	if (!in) {
		if (error) {
			*error = "cannot open " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	try {
		const nlohmann::json j = nlohmann::json::parse(buffer.str());
		out.version = j.value("version", 1);
		if (j.contains("scene_name")) {
			out.scene_name = j.at("scene_name").get<std::string>();
		}
		if (j.contains("player_name")) {
			out.player_name = j.at("player_name").get<std::string>();
		}
		if (j.contains("player_position")) {
			out.player_position = read_vec3(j.at("player_position"));
		}
		if (j.contains("player_rotation")) {
			out.player_rotation = read_vec3(j.at("player_rotation"));
		}
		out.resident_regions.clear();
		if (j.contains("resident_regions") && j.at("resident_regions").is_array()) {
			for (const auto& entry : j.at("resident_regions")) {
				if (entry.is_string()) {
					out.resident_regions.push_back(entry.get<std::string>());
				}
			}
		}
		return true;
	} catch (const std::exception& ex) {
		if (error) {
			*error = ex.what();
		}
		return false;
	}
}

} // namespace lotus
