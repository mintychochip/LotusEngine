#include "lotus/scene/history.hpp"

#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

namespace lotus {

SceneHistory::SceneHistory(std::size_t capacity)
        : capacity_{capacity < 2 ? 2 : capacity}
{}

void SceneHistory::reset(const Scene& scene)
{
	entries_.clear();
	entries_.push_back(to_json_string(scene));
	cursor_ = 0;
}

void SceneHistory::commit(const Scene& scene)
{
	if (entries_.empty()) {
		reset(scene);
		return;
	}
	// A new edit invalidates anything that was redoable.
	entries_.resize(cursor_ + 1);
	entries_.push_back(to_json_string(scene));

	if (entries_.size() > capacity_) {
		const std::size_t drop = entries_.size() - capacity_;
		entries_.erase(entries_.begin(),
		               entries_.begin() + static_cast<std::ptrdiff_t>(drop));
	}
	cursor_ = entries_.size() - 1;
}

bool SceneHistory::can_undo() const
{
	return cursor_ > 0 && !entries_.empty();
}

bool SceneHistory::can_redo() const
{
	return !entries_.empty() && cursor_ + 1 < entries_.size();
}

bool SceneHistory::undo(Scene& scene)
{
	if (!can_undo()) {
		return false;
	}
	--cursor_;
	return scene_from_json(scene, entries_[cursor_], nullptr);
}

bool SceneHistory::redo(Scene& scene)
{
	if (!can_redo()) {
		return false;
	}
	++cursor_;
	return scene_from_json(scene, entries_[cursor_], nullptr);
}

} // namespace lotus
