#pragma once

#include <string>

#include "lotus/application/window.hpp"
#include "lotus/console/console.hpp"
#include "lotus/input/keybinds.hpp"
#include "lotus/io/file_watcher.hpp"
#include "lotus/scene/scene_manager.hpp"
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
	Console& console() { return console_; }
	Keybinds& keybinds() { return keybinds_; }

	/// Watches \p path; when it changes, the active scene is reloaded. The
	/// reload is atomic, so a half-written file is rejected and the live
	/// scene survives.
	void watch_scene(const std::string& path);

	bool save_scene_now(std::string* error = nullptr);
	bool reload_scene_now(std::string* error = nullptr);

	const std::string& scene_path() const { return scene_path_; }
	const std::string& scene_status() const { return scene_status_; }

	/// Requests the main loop to stop (used by the `quit` console command).
	void request_quit() { quit_requested_ = true; }

protected:
	void on_start();
	/// Called once after the GPU device and editor exist.
	virtual void on_ready(Window& window, GpuRenderer& gpu) = 0;
	virtual void on_update(float dt)                       = 0;
	virtual void on_shutdown()                             = 0;

	GpuSettings settings_;

private:
	void register_commands();
	void handle_action(const std::string& action);
	void poll_hot_reload();

	SceneManager scenes_;
	Editor editor_;
	ScriptEngine scripts_;
	Console console_;
	Keybinds keybinds_;
	FileWatcher watcher_;
	std::string scene_path_;
	std::string scene_status_;
	std::string keybinds_path_{"keybinds.json"};
	bool quit_requested_{false};
};

// Legacy aliases while migrating call sites.
using GLWindow      = Window;
using GLProfile     = GpuSettings;
using GLApplication = Application;

} // namespace lotus
