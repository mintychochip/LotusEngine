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
///
/// Cost: a snapshot per step, so memory is O(scene size x steps) and every
/// undo re-parses a whole document. Fine for a level editor; a large world
/// would want structural diffs. \p byte_capacity bounds memory independently
/// of the step count, because 64 snapshots of a big scene is a lot more than
/// 64 snapshots of a small one.
class SceneHistory {
public:
	explicit SceneHistory(std::size_t capacity = 64,
	                      std::size_t byte_capacity = 64u * 1024u * 1024u);

	/// Clears the history and records \p scene as the baseline state.
	void reset(const Scene& scene);

	/// Records the current state of \p scene as a new step, dropping any redo
	/// steps and then the oldest entries beyond either capacity.
	void commit(const Scene& scene);

	bool can_undo() const;
	bool can_redo() const;

	/// Restores the previous/next step. Returns false when there is none.
	bool undo(Scene& scene);
	bool redo(Scene& scene);

	std::size_t size() const { return entries_.size(); }
	std::size_t cursor() const { return cursor_; }
	std::size_t capacity() const { return capacity_; }
	std::size_t byte_capacity() const { return byte_capacity_; }
	/// Bytes currently held by all snapshots.
	std::size_t bytes() const;

private:
	/// Drops the oldest entries until both the step count and the byte budget
	/// fit. Keeps at least two entries, or undo/redo stops working.
	void trim();

	std::size_t capacity_;
	std::size_t byte_capacity_;
	std::vector<std::string> entries_;
	std::size_t cursor_{0};
};

} // namespace lotus
