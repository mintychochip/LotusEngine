#pragma once

#include <string>
#include <vector>

#include "lotus/application/window.hpp"
#include "lotus/audio/audio_system.hpp"
#include "lotus/console/console.hpp"
#include "lotus/input/keybinds.hpp"
#include "lotus/io/file_watcher.hpp"
#include "lotus/physics/physics_world.hpp"
#include "lotus/scene/animation.hpp"
#include "lotus/scene/assets.hpp"
#include "lotus/scene/async_load.hpp"
#include "lotus/scene/residency_system.hpp"
#include "lotus/scene/scene_manager.hpp"
#include "lotus/scene/scene_pass.hpp"
#include "lotus/script/script_engine.hpp"
#include "lotus/ui/editor.hpp"

namespace lotus {

class GpuRenderer;

struct GpuSettings {
	bool debug_device{true};
};

/// Owns the window, GPU renderer, scene manager, editor, console, keybinds,
/// script engine, and hot-reload watchers, and runs the loop.
class Application {
public:
	explicit Application(GpuSettings settings = {}) : settings_{settings} {}
	virtual ~Application() = default;

	void run(int width = 800, int height = 600,
	         const std::string& title = "lotus");

	SceneManager& scenes() { return scenes_; }
	Editor& editor() { return editor_; }
	ScriptEngine& scripts() { return scripts_; }
	PhysicsWorld& physics() { return physics_; }
	Console& console() { return console_; }
	Keybinds& keybinds() { return keybinds_; }

	/// Watches \p path; when it changes, the active scene is reloaded. The
	/// reload is atomic, so a half-written file is rejected and the live
	/// scene survives.
	void watch_scene(const std::string& path);

	bool save_scene_now(std::string* error = nullptr);
	bool reload_scene_now(std::string* error = nullptr);

	/// Directory scenes are read from and written to.
	void set_scene_dir(const std::string& dir) { scene_dir_ = dir; }

	/// Makes \p name active, loading it from the scene directory when it is not
	/// already open. A switch between open scenes never re-reads from disk.
	bool switch_scene(const std::string& name, std::string* error = nullptr);
	/// Loads <scene dir>/<name>.json as a new scene and activates it.
	bool load_scene_by_name(const std::string& name,
	                        std::string* error = nullptr);

	const std::string& scene_path() const { return scene_path_; }
	const std::string& scene_status() const { return scene_status_; }

	/// Directory prefabs are read from and written to.
	void set_prefab_dir(const std::string& dir) { prefab_dir_ = dir; }

	/// Directory holding assets.json and mesh/texture files.
	void set_assets_dir(const std::string& dir) { assets_dir_ = dir; }
	const std::string& assets_dir() const { return assets_dir_; }
	AssetRegistry& assets() { return assets_; }
	const AssetRegistry& assets() const { return assets_; }
	const std::string& assets_status() const { return assets_status_; }

	bool load_assets_now(std::string* error = nullptr);
	bool save_assets_now(std::string* error = nullptr);

	bool save_game_now(const std::string& path, std::string* error = nullptr);
	bool load_game_now(const std::string& path, std::string* error = nullptr);

	/// Saves \p root and its descendants as prefabs/<name>.json. Returns false
	/// and fills \p error on failure.
	bool save_prefab_now(Entity root, const std::string& name,
	                     std::string* error = nullptr);
	/// Instantiates the named prefab into the active scene under \p parent.
	Entity instantiate_prefab(const std::string& name,
	                          Entity parent = NullEntity);
	/// Names of the prefabs currently on disk, sorted.
	std::vector<std::string> prefab_names() const;

	/// Requests the main loop to stop (used by the `quit` console command).
	void request_quit() { quit_requested_ = true; }

	/// One simulation step for the active scene: input, animation, scene
	/// update, physics, scripts, a world-matrix publish, the follow camera,
	/// and residency. No-op when no scene is active.
	void tick_simulation(float dt, bool game_input_enabled);

protected:
	void on_start();
	/// Called once after the GPU device and editor exist.
	virtual void on_ready(Window& window, GpuRenderer& gpu) = 0;
	virtual void on_update(float dt)                       = 0;
	/// Called after physics and scripts have published Transform::world, and
	/// inside the ImGui frame, so a game HUD can draw.
	virtual void on_ui(float /*dt*/) {}
	/// Called after a scene replace rebinds assets and drops physics and
	/// residency state. Cached entity handles must be resolved again here.
	virtual void on_scene_committed(Scene& /*scene*/) {}
	virtual void on_shutdown() = 0;

	GpuSettings settings_;

private:
	void register_commands();
	void handle_action(const std::string& action);
	void poll_hot_reload();
	void poll_async_scene_load();
protected:
	std::string scene_file_for(const std::string& name) const;
	void adopt_scene_path(const std::string& path);
	void bind_active_scene();
private:
	void wire_assets();

	SceneManager scenes_;
	PhysicsWorld physics_;
	AnimationLibrary animations_;
	ResidencySystem residency_;
	AsyncSceneLoad async_scene_load_;
	std::string pending_async_scene_;
	Editor editor_;
	ScriptEngine scripts_;
	AudioSystem audio_;
	Console console_;
	Keybinds keybinds_;
	FileWatcher watcher_;
	std::string scene_path_;
	std::string scene_status_;
	std::string keybinds_path_{"keybinds.json"};
	std::string prefab_dir_{"prefabs"};
	std::string scene_dir_{"scenes"};
	std::string assets_dir_{"assets"};
	AssetRegistry assets_;
	std::string assets_path_;
	std::string assets_status_;
	bool quit_requested_{false};
};

// Legacy aliases while migrating call sites.
using GLWindow      = Window;
using GLProfile     = GpuSettings;
using GLApplication = Application;

} // namespace lotus
