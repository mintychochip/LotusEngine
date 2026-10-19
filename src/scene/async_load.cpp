#include "lotus/scene/async_load.hpp"

#include <fstream>
#include <sstream>
#include <utility>

namespace lotus {
namespace {

AsyncSceneLoad::LoadResult load_into_scene(const std::string& path)
{
	AsyncSceneLoad::LoadResult result;
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		result.error = "cannot open scene: " + path;
		return result;
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	if (!scene_from_json(result.scene, buffer.str(), &result.error)) {
		return result;
	}
	result.ok = true;
	return result;
}

} // namespace

AsyncSceneLoad::~AsyncSceneLoad()
{
	// A future from std::async blocks in its destructor, which would turn
	// destruction into a wait. Blocking here is acceptable only because it is
	// destruction, never a frame: no poll path waits. The result is discarded.
	if (future_.valid()) {
		future_.wait();
	}
}

bool AsyncSceneLoad::start(const std::string& path)
{
	if (state_.load() == State::Loading) {
		return false; // one load at a time
	}
	if (future_.valid()) {
		future_.wait(); // an earlier result was never taken; discard it
	}
	error_.clear();
	path_ = path;
	state_.store(State::Loading);

	// The worker reads and parses into a Scene it owns. It touches nothing the
	// caller can observe, so no locking is needed anywhere.
	future_ = std::async(std::launch::async,
	                     [path]() { return load_into_scene(path); });
	return true;
}

AsyncSceneLoad::State AsyncSceneLoad::poll()
{
	if (state_.load() != State::Loading || !future_.valid()) {
		return state_.load();
	}
	// Zero timeout: a poll, never a wait.
	if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
		return State::Loading;
	}

	LoadResult result = future_.get();
	if (!result.ok) {
		error_ = std::move(result.error);
		state_.store(State::Failed);
		return State::Failed;
	}
	loaded_ = std::move(result.scene);
	state_.store(State::Ready);
	return State::Ready;
}

void AsyncSceneLoad::dismiss_failure()
{
	if (state_.load() != State::Failed) {
		return;
	}
	error_.clear();
	state_.store(State::Idle);
}

bool AsyncSceneLoad::take(Scene& target)
{
	if (state_.load() != State::Ready) {
		return false;
	}
	target = std::move(loaded_);
	target.resolve_refs();
	loaded_ = Scene{"async"};
	state_.store(State::Idle);
	return true;
}

} // namespace lotus
