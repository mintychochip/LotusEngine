#include "lotus/scene/assets.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace lotus {

namespace {

bool read_json_file(const std::string& path, nlohmann::json& out,
                    std::string* error)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		if (error) {
			*error = "cannot open " + path;
		}
		return false;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	try {
		out = nlohmann::json::parse(buffer.str());
	} catch (const std::exception& ex) {
		if (error) {
			*error = std::string{"parse error: "} + ex.what();
		}
		return false;
	}
	return true;
}

bool parse_assets(const nlohmann::json& root,
                  std::unordered_map<std::string, std::string>& out,
                  std::string* error)
{
	if (!root.is_object()) {
		if (error) {
			*error = "root must be an object";
		}
		return false;
	}
	if (!root.contains("version") || !root.at("version").is_number_integer() ||
	    root.at("version").get<int>() != 1) {
		if (error) {
			*error = "unsupported or missing version";
		}
		return false;
	}
	if (!root.contains("assets") || !root.at("assets").is_object()) {
		if (error) {
			*error = "missing assets object";
		}
		return false;
	}
	std::unordered_map<std::string, std::string> parsed;
	for (const auto& [key, value] : root.at("assets").items()) {
		if (!value.is_string()) {
			if (error) {
				*error = "asset value for '" + key + "' must be a string";
			}
			return false;
		}
		parsed[key] = value.get<std::string>();
	}
	out = std::move(parsed);
	return true;
}

} // namespace

bool AssetRegistry::load(const std::string& path, std::string* error)
{
	nlohmann::json root;
	if (!read_json_file(path, root, error)) {
		return false;
	}
	std::unordered_map<std::string, std::string> parsed;
	if (!parse_assets(root, parsed, error)) {
		return false;
	}
	paths_ = std::move(parsed);
	return true;
}

bool AssetRegistry::save(const std::string& path, std::string* error) const
{
	nlohmann::json assets = nlohmann::json::object();
	for (const auto& [id, asset_path] : paths_) {
		assets[id] = asset_path;
	}
	const nlohmann::json root{{"version", 1}, {"assets", std::move(assets)}};

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot write " + path;
		}
		return false;
	}
	out << root.dump(2) << '\n';
	return true;
}

const std::string* AssetRegistry::find(const std::string& id) const
{
	const auto it = paths_.find(id);
	return it == paths_.end() ? nullptr : &it->second;
}

void AssetRegistry::set(const std::string& id, std::string path)
{
	paths_[id] = std::move(path);
}

bool AssetRegistry::erase(const std::string& id)
{
	return paths_.erase(id) != 0;
}

std::vector<std::string> AssetRegistry::names() const
{
	std::vector<std::string> out;
	out.reserve(paths_.size());
	for (const auto& [id, path] : paths_) {
		(void)path;
		out.push_back(id);
	}
	std::sort(out.begin(), out.end());
	return out;
}

std::string AssetRegistry::resolve(const std::string& id,
                                   const std::string& fallback) const
{
	if (id.empty()) {
		return fallback;
	}
	const std::string* path = find(id);
	return path != nullptr ? *path : fallback;
}

bool AssetRegistry::dangling(const std::string& id) const
{
	return !id.empty() && find(id) == nullptr;
}

} // namespace lotus
