#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>

#include "lotus/ecs.hpp"
#include "lotus/io/file_watcher.hpp"
#include "lotus/script/script.hpp"

struct lua_State;

namespace lotus {

class AssetRegistry;
class Scene;

/// Runs Lua scripts attached to entities through the Script component, and
/// hot-swaps them when their files change.
///
/// Loading is atomic per script: a file that fails to compile or run is
/// reported and the previously loaded version stays in effect.
class ScriptEngine {
public:
	ScriptEngine();
	~ScriptEngine();

	ScriptEngine(const ScriptEngine&)            = delete;
	ScriptEngine& operator=(const ScriptEngine&) = delete;

	/// Directory prepended to relative script paths.
	void set_root(std::string root) { root_ = std::move(root); }
	const std::string& root() const { return root_; }

	void set_assets(const AssetRegistry* assets) { assets_ = assets; }

	/// Loads (or reloads) a single script file. Returns false and fills
	/// \p error on failure, leaving any previously loaded version in place.
	bool load(const std::string& path, std::string* error = nullptr);

	/// Loads every script referenced by \p scene, then calls on_start for
	/// those entities. Call after loading or hot-reloading a scene.
	bool sync(Scene& scene, std::string* error = nullptr);

	/// Calls on_update(entity, dt) for every entity with a Script.
	void update(Scene& scene, float dt);

	/// Reloads scripts whose files changed, re-running on_start for the
	/// entities that use them. Cheap to call every frame.
	void poll(Scene& scene);

	/// Paths currently loaded.
	std::size_t loaded_count() const { return scripts_.size(); }
	bool is_loaded(const std::string& path) const;

	/// Last script error (load or runtime), or empty.
	const std::string& last_error() const { return last_error_; }

	/// Optional sink for reload/error messages (the engine does not print).
	using LogFn = std::function<void(const std::string&)>;
	void set_log(LogFn log) { log_ = std::move(log); }

private:
	std::string resolve(const std::string& path) const;
	std::string script_path_for(const Script& script) const;
	void set_current_scene(Scene* scene);
	void call_hook(Scene& scene, Entity entity, const char* hook, bool with_dt,
	               float dt);
	void start_entities(Scene& scene, const std::string& path);

	struct ScriptRef {
		int ref{0};
	};
	std::string root_;
	const AssetRegistry* assets_{nullptr};
	std::string last_error_;
	lua_State* state_{nullptr};
	std::map<std::string, ScriptRef> scripts_;
	FileWatcher watcher_;
	LogFn log_;
};

} // namespace lotus
