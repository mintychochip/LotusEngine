#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lotus {

class Scene;

/// Snapshot-based undo/redo for a Scene.
///
/// Each step is the scene serialized with the same JSON format used for
/// saving, so undo restores exactly what a save/load round trip would.
/// Undo and redo go through the atomic loader, so a step can never leave the
/// scene half-restored.
class SceneHistory {
public:
	explicit SceneHistory(std::size_t capacity = 64);

	/// Clears the history and records \p scene as the baseline state.
	void reset(const Scene& scene);

	/// Records the current state of \p scene as a new step, dropping any redo
	/// steps and the oldest entries beyond capacity.
	void commit(const Scene& scene);

	bool can_undo() const;
	bool can_redo() const;

	/// Restores the previous/next step. Returns false when there is none.
	bool undo(Scene& scene);
	bool redo(Scene& scene);

	std::size_t size() const { return entries_.size(); }
	std::size_t cursor() const { return cursor_; }
	std::size_t capacity() const { return capacity_; }

private:
	std::size_t capacity_;
	std::vector<std::string> entries_;
	std::size_t cursor_{0};
};

} // namespace lotus
