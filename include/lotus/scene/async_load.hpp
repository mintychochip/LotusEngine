#pragma once

#include <atomic>
#include <future>
#include <string>

#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

namespace lotus {

/// Loads a scene file off the main thread, so a large document does not stall
/// the frame loop.
///
/// The shape mirrors the renderer's occlusion fence: start the work, then poll
/// it. There is deliberately no blocking accessor -- a caller that wants the
/// result waits for poll() to say Ready, exactly as begin_frame never waits on
/// SDL_QueryGPUFence.
///
/// The parse runs on a worker and produces a whole Scene. The live scene is
/// never touched by the worker; poll() moves the finished scene in on the
/// calling thread, which is the only place it can be observed.
class AsyncSceneLoad {
public:
	enum class State {
		Idle,    ///< Nothing started, or the result was already taken.
		Loading, ///< Worker running. Not ready.
		Ready,   ///< Result available; take() commits it exactly once.
		Failed,  ///< Error is populated; nothing to take.
	};

	/// What the worker produces. \p ok is explicit rather than inferred from an
	/// empty scene, because an empty scene is a legitimate document.
	struct LoadResult {
		bool ok{false};
		Scene scene{"async"};
		std::string error;
	};

	AsyncSceneLoad() = default;
	~AsyncSceneLoad();

	AsyncSceneLoad(const AsyncSceneLoad&)            = delete;
	AsyncSceneLoad& operator=(const AsyncSceneLoad&) = delete;

	/// Starts loading \p path. Returns false if a load is already in flight.
	bool start(const std::string& path);

	/// Non-blocking. Advances Idle -> Loading -> Ready/Failed.
	State poll();

	/// Moves the loaded scene into \p target. Valid only in State::Ready, and
	/// consumes it: the state returns to Idle. Atomic with respect to \p target,
	/// because the whole scene was built before this point.
	bool take(Scene& target);

	/// Returns a failed load to Idle so the next poll does not report it again.
	void dismiss_failure();

	State state() const { return state_.load(); }
	const std::string& error() const { return error_; }
	const std::string& path() const { return path_; }

private:
	std::future<LoadResult> future_;
	Scene loaded_{"async"};
	std::atomic<State> state_{State::Idle};
	std::string error_;
	std::string path_;
};

} // namespace lotus
