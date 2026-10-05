#include "lotus/scene/scene_manager.hpp"

namespace lotus {

Scene& SceneManager::create(const std::string& name)
{
	auto scene  = std::make_unique<Scene>(name);
	Scene& ref  = *scene;
	scenes_[name] = std::move(scene);
	if (active_ == nullptr) {
		active_      = &ref;
		active_name_ = name;
	}
	return ref;
}

Scene* SceneManager::get(const std::string& name)
{
	const auto it = scenes_.find(name);
	return it == scenes_.end() ? nullptr : it->second.get();
}

const Scene* SceneManager::get(const std::string& name) const
{
	const auto it = scenes_.find(name);
	return it == scenes_.end() ? nullptr : it->second.get();
}

bool SceneManager::destroy(const std::string& name)
{
	const auto it = scenes_.find(name);
	if (it == scenes_.end()) {
		return false;
	}
	const bool was_active = (it->second.get() == active_);
	scenes_.erase(it);
	if (was_active) {
		active_ = nullptr;
		active_name_.clear();
		if (!scenes_.empty()) {
			active_      = scenes_.begin()->second.get();
			active_name_ = scenes_.begin()->first;
		}
	}
	return true;
}

Scene& SceneManager::activate(const std::string& name)
{
	if (Scene* existing = get(name)) {
		active_      = existing;
		active_name_ = name;
		return *existing;
	}
	Scene& created = create(name);
	active_        = &created;
	active_name_   = name;
	return created;
}

std::vector<std::string> SceneManager::names() const
{
	std::vector<std::string> out;
	out.reserve(scenes_.size());
	for (const auto& [name, scene] : scenes_) {
		(void)scene;
		out.push_back(name);
	}
	return out;
}

} // namespace lotus
