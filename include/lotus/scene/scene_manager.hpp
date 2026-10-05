#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "lotus/scene/scene.hpp"

namespace lotus {

/// Owns named scenes and tracks the active one.
class SceneManager {
public:
	/// Creates (or replaces) a scene and returns it. Does not activate it.
	Scene& create(const std::string& name);

	Scene* get(const std::string& name);
	const Scene* get(const std::string& name) const;

	bool destroy(const std::string& name);

	/// Makes \p name active, creating it if needed.
	Scene& activate(const std::string& name);

	Scene* active() { return active_; }
	const Scene* active() const { return active_; }

	const std::string& active_name() const { return active_name_; }

	std::size_t count() const { return scenes_.size(); }

private:
	std::unordered_map<std::string, std::unique_ptr<Scene>> scenes_;
	std::string active_name_;
	Scene* active_{nullptr};
};

} // namespace lotus
