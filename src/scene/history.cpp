#include "lotus/scene/history.hpp"

#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

namespace lotus {

SceneHistory::SceneHistory(std::size_t capacity, std::size_t byte_capacity)
        : capacity_{capacity < 2 ? 2 : capacity}
        , byte_capacity_{byte_capacity < 1024 ? 1024 : byte_capacity}
{}

std::size_t SceneHistory::bytes() const
{
	std::size_t total = 0;
	for (const std::string& entry : entries_) {
		total += entry.size();
	}
	return total;
}

void SceneHistory::trim()
{
	// Two entries is the floor: one to sit on and one to undo to.
	while (entries_.size() > capacity_ ||
	       (entries_.size() > 2 && bytes() > byte_capacity_)) {
		entries_.erase(entries_.begin());
	}
}

void SceneHistory::reset(const Scene& scene)
{
	entries_.clear();
	entries_.push_back(to_json_string(scene));
	cursor_ = 0;
	trim();
	cursor_ = entries_.size() - 1;
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
	trim();
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
