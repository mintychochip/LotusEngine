#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {

/// Maps stable asset keys to filesystem paths. Scenes store keys in MeshSource
/// and Script so assets can move without editing every document.
class AssetRegistry {
public:
	bool load(const std::string& path, std::string* error = nullptr);
	bool save(const std::string& path, std::string* error = nullptr) const;

	const std::string* find(const std::string& id) const;
	void set(const std::string& id, std::string path);
	bool erase(const std::string& id);
	std::vector<std::string> names() const;

	std::string resolve(const std::string& id,
	                    const std::string& fallback) const;
	bool dangling(const std::string& id) const;
	std::size_t size() const { return paths_.size(); }

private:
	std::unordered_map<std::string, std::string> paths_;
};

} // namespace lotus
