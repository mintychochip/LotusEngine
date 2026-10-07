#include "lotus/application.hpp"
#include "lotus/global.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/components.hpp"
#include "lotus/scene/game_save.hpp"
#include "lotus/scene/player_controller.hpp"
#include "lotus/scene/prefab.hpp"
#include "lotus/scene/serialization.hpp"
#include "lotus/script/script.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace lotus;

namespace {

std::string trim(const std::string& text)
{
	const auto begin = text.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) {
		return {};
	}
	const auto end = text.find_last_not_of(" \t\r\n");
	return text.substr(begin, end - begin + 1);
}

std::string join_from(const std::vector<std::string>& parts, std::size_t from)
{
	std::string out;
	for (std::size_t i = from; i < parts.size(); ++i) {
		if (!out.empty()) {
			out += ' ';
		}
		out += parts[i];
	}
	return out;
}

} // namespace

void Application::on_start()
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		throw std::runtime_error(SDL_GetError());
	}
	Global::flag(LOTUS_SDL_INITIALIZED, true);
}

void Application::register_commands()
{
	console_.add_command("help", "list commands",
	                     [this](const std::vector<std::string>&) {
		                     std::string out = "commands:";
		                     for (const auto& [name, help] : console_.commands()) {
			                     out += "\n  " + name;
			                     if (!help.empty()) {
				                     out += " - " + help;
			                     }
		                     }
		                     return out;
	                     });
	console_.add_command("clear", "clear the console",
	                     [this](const std::vector<std::string>&) {
		                     console_.clear();
		                     return std::string{};
	                     });
	console_.add_command("echo", "print the arguments",
	                     [](const std::vector<std::string>& args) {
		                     return join_from(args, 0);
	                     });
	console_.add_command("quit", "stop the engine",
	                     [this](const std::vector<std::string>&) {
		                     quit_requested_ = true;
		                     return std::string{"bye"};
	                     });
	console_.add_command("scene.save", "save the active scene",
	                     [this](const std::vector<std::string>&) {
		                     std::string error;
		                     if (!save_scene_now(&error)) {
			                     return std::string{"save failed: "} + error;
		                     }
		                     return scene_status_;
	                     });
	console_.add_command("scene.reload", "reload the active scene from disk",
	                     [this](const std::vector<std::string>&) {
		                     std::string error;
		                     if (!reload_scene_now(&error)) {
			                     return std::string{"reload failed: "} + error;
		                     }
		                     return scene_status_;
	                     });
	console_.add_command("scene.list", "list scenes",
	                     [this](const std::vector<std::string>&) {
		                     std::string out;
		                     for (const std::string& name : scenes_.names()) {
			                     if (!out.empty()) {
				                     out += '\n';
			                     }
			                     out += (name == scenes_.active_name() ? "* " : "  ");
			                     out += name;
		                     }
		                     return out.empty() ? std::string{"(no scenes)"} : out;
	                     });
	console_.add_command(
	        "scene.switch", "scene.switch <name> - activate a scene, loading it "
	                        "from <scene dir>/<name>.json if not already open",
	        [this](const std::vector<std::string>& args) {
		        if (args.empty()) {
			        return std::string{"usage: scene.switch <name>"};
		        }
		        std::string error;
		        if (!switch_scene(args[0], &error)) {
			        return error;
		        }
		        return std::string{"active scene: "} + scenes_.active_name();
	        });
	console_.add_command(
	        "scene.load", "scene.load <name> - load <scene dir>/<name>.json as a "
	                      "new scene and activate it",
	        [this](const std::vector<std::string>& args) {
		        if (args.empty()) {
			        return std::string{"usage: scene.load <name>"};
		        }
		        std::string error;
		        if (!load_scene_by_name(args[0], &error)) {
			        return error;
		        }
		        return std::string{"active scene: "} + scenes_.active_name();
	        });
	console_.add_command(
	        "scene.load_async",
	        "scene.load_async <name> - load <scene dir>/<name>.json on a "
	        "worker thread; poll until ready",
	        [this](const std::vector<std::string>& args) {
		        if (args.empty()) {
			        return std::string{"usage: scene.load_async <name>"};
		        }
		        if (async_scene_load_.state() ==
		            AsyncSceneLoad::State::Loading) {
			        return std::string{"async load already in progress: "} +
			               async_scene_load_.path();
		        }
		        const std::string path = scene_file_for(args[0]);
		        if (!async_scene_load_.start(path)) {
			        return std::string{"failed to start async load"};
		        }
		        pending_async_scene_ = args[0];
		        return std::string{"loading "} + path + "...";
	        });
	console_.add_command(
	        "scene.merge", "scene.merge <path> - merge a scene file into the "
	                       "active scene without replacing it",
	        [this](const std::vector<std::string>& args) {
		        Scene* scene = scenes_.active();
		        if (scene == nullptr) {
			        return std::string{"no active scene"};
		        }
		        if (args.empty()) {
			        return std::string{"usage: scene.merge <path>"};
		        }
		        std::string error;
		        if (!load_scene_additive_from_file(*scene, args[0], &error)) {
			        return error;
		        }
		        return std::string{"merged "} + args[0] + " (" +
		               std::to_string(scene->registry().view<Name>().size()) +
		               " entities)";
	        });
	console_.add_command(
	        "game.save",
	        "game.save [path] - write player pose and scene name (default "
	        "saves/game.json)",
	        [this](const std::vector<std::string>& args) {
		        const std::string path =
		                args.empty() ? std::string{"saves/game.json"} : args[0];
		        std::string error;
		        if (!save_game_now(path, &error)) {
			        return std::string{"game.save failed: "} + error;
		        }
		        return std::string{"saved "} + path;
	        });
	console_.add_command(
	        "game.load",
	        "game.load [path] - load scene from save then apply player pose",
	        [this](const std::vector<std::string>& args) {
		        const std::string path =
		                args.empty() ? std::string{"saves/game.json"} : args[0];
		        std::string error;
		        if (!load_game_now(path, &error)) {
			        return std::string{"game.load failed: "} + error;
		        }
		        return std::string{"loaded "} + path;
	        });
	console_.add_command(
	        "scene.unload", "scene.unload <origin> - destroy every entity merged "
	                        "from that scene, leaving the rest",
	        [this](const std::vector<std::string>& args) {
		        Scene* scene = scenes_.active();
		        if (scene == nullptr) {
			        return std::string{"no active scene"};
		        }
		        if (args.empty()) {
			        return std::string{"usage: scene.unload <origin>"};
		        }
		        const std::size_t removed = scene->destroy_by_origin(args[0]);
		        if (removed == 0) {
			        return std::string{"nothing tagged '"} + args[0] + "'";
		        }
		        std::string out = "unloaded " + std::to_string(removed) +
		                          " entities from " + args[0];
		        for (const std::string& err : scene->reference_errors()) {
			        out += "\n  " + err;
		        }
		        return out;
	        });
	console_.add_command(
	        "entity.list", "list entities in the active scene",
	        [this](const std::vector<std::string>&) {
		        Scene* scene = scenes_.active();
		        if (!scene) {
			        return std::string{"no active scene"};
		        }
		        std::string out;
		        for (const Entity entity : scene->registry().view<Name>()) {
			        if (!out.empty()) {
				        out += '\n';
			        }
			        out += scene->registry().get<Name>(entity).value;
		        }
		        return out.empty() ? std::string{"(no entities)"} : out;
	        });
	console_.add_command(
	        "script.reload", "reload all scripts in the active scene",
	        [this](const std::vector<std::string>&) {
		        Scene* scene = scenes_.active();
		        if (!scene) {
			        return std::string{"no active scene"};
		        }
		        std::string error;
		        if (!scripts_.sync(*scene, &error)) {
			        return std::string{"script error: "} + error;
		        }
		        return std::string{"scripts reloaded"};
	        });
	console_.add_command("keybinds.list", "list key bindings",
	                     [this](const std::vector<std::string>&) {
		                     std::string out;
		                     for (const auto& [action, chord] : keybinds_.bindings()) {
			                     if (!out.empty()) {
				                     out += '\n';
			                     }
			                     out += action + " = " + chord.to_string();
		                     }
		                     return out;
	                     });
	console_.add_command("keybinds.save", "write keybinds.json",
	                     [this](const std::vector<std::string>&) {
		                     std::string error;
		                     if (!keybinds_.save(keybinds_path_, &error)) {
			                     return error;
		                     }
		                     return std::string{"saved "} + keybinds_path_;
	                     });
	console_.add_command("asset.list", "list registered asset keys",
	                     [this](const std::vector<std::string>&) {
		                     std::string out;
		                     for (const std::string& id : assets_.names()) {
			                     const std::string* path = assets_.find(id);
			                     if (!out.empty()) {
				                     out += '\n';
			                     }
			                     out += id + " -> " + (path ? *path : std::string{});
		                     }
		                     return out.empty() ? std::string{"(no assets)"} : out;
	                     });
	console_.add_command(
	        "asset.set", "asset.set <id> <path> - register or update a key",
	        [this](const std::vector<std::string>& args) {
		        if (args.size() < 2) {
			        return std::string{"usage: asset.set <id> <path>"};
		        }
		        assets_.set(args[0], join_from(args, 1));
		        wire_assets();
		        return std::string{"set "} + args[0];
	        });
	console_.add_command("asset.save", "write assets.json",
	                     [this](const std::vector<std::string>&) {
		                     std::string error;
		                     if (!save_assets_now(&error)) {
			                     return std::string{"save failed: "} + error;
		                     }
		                     return assets_status_;
	                     });
	console_.add_command("asset.reload", "reload assets.json from disk",
	                     [this](const std::vector<std::string>&) {
		                     std::string error;
		                     if (!load_assets_now(&error)) {
			                     return std::string{"reload failed: "} + error;
		                     }
		                     if (Scene* scene = scenes_.active()) {
			                     scene->update(0.0f);
			                     std::string script_error;
			                     if (!scripts_.sync(*scene, &script_error)) {
				                     return assets_status_ + "\nscript: " +
				                            script_error;
			                     }
		                     }
		                     return assets_status_;
	                     });
	console_.add_command(
	        "prefab.list", "list prefabs on disk",
	        [this](const std::vector<std::string>&) {
		        std::string out;
		        for (const std::string& name : prefab_names()) {
			        if (!out.empty()) {
				        out += '\n';
			        }
			        out += name;
		        }
		        return out.empty() ? std::string{"(no prefabs)"} : out;
	        });
	console_.add_command(
	        "prefab.save", "prefab.save <name> <entity> - save an entity subtree",
	        [this](const std::vector<std::string>& args) {
		        Scene* scene = scenes_.active();
		        if (!scene) {
			        return std::string{"no active scene"};
		        }
		        if (args.size() < 2) {
			        return std::string{"usage: prefab.save <name> <entity>"};
		        }
		        const Entity root = scene->find(args[1]);
		        if (root == NullEntity) {
			        return std::string{"no entity named "} + args[1];
		        }
		        std::string error;
		        if (!save_prefab_now(root, args[0], &error)) {
			        return error;
		        }
		        return std::string{"saved prefab "} + args[0];
	        });
	console_.add_command(
	        "prefab.instantiate", "prefab.instantiate <name> [parent]",
	        [this](const std::vector<std::string>& args) {
		        if (args.empty()) {
			        return std::string{"usage: prefab.instantiate <name> [parent]"};
		        }
		        Scene* scene = scenes_.active();
		        Entity parent = NullEntity;
		        if (args.size() >= 2 && scene) {
			        parent = scene->find(args[1]);
		        }
		        const Entity created = instantiate_prefab(args[0], parent);
		        if (created == NullEntity) {
			        return std::string{"could not instantiate "} + args[0];
		        }
		        return std::string{"instantiated "} + args[0];
	        });
}

bool Application::save_prefab_now(Entity root, const std::string& name,
                                  std::string* error)
{
	Scene* scene = scenes_.active();
	if (!scene || root == NullEntity || !scene->registry().valid(root)) {
		if (error) {
			*error = "no active scene or invalid entity";
		}
		return false;
	}
	std::error_code code;
	std::filesystem::create_directories(prefab_dir_, code);
	const std::string path = prefab_dir_ + "/" + name + ".json";

	const Prefab prefab = prefab_from_scene(*scene, root);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		if (error) {
			*error = "cannot write " + path;
		}
		return false;
	}
	out << prefab.document.dump(2) << '\n';
	return true;
}

Entity Application::instantiate_prefab(const std::string& name, Entity parent)
{
	Scene* scene = scenes_.active();
	if (!scene) {
		return NullEntity;
	}
	Prefab prefab;
	std::string error;
	if (!load_prefab(prefab, prefab_dir_ + "/" + name + ".json", &error)) {
		return NullEntity;
	}
	return instantiate(prefab, *scene, parent);
}

bool Application::load_assets_now(std::string* error)
{
	assets_path_ = assets_dir_ + "/assets.json";
	std::error_code code;
	if (!std::filesystem::exists(assets_path_, code)) {
		assets_status_ = "no " + assets_path_;
		wire_assets();
		return true;
	}
	std::string failure;
	if (!assets_.load(assets_path_, &failure)) {
		assets_status_ = "load failed: " + failure;
		if (error) {
			*error = failure;
		}
		return false;
	}
	assets_status_ = "loaded " + assets_path_;
	wire_assets();
	return true;
}

bool Application::save_assets_now(std::string* error)
{
	std::error_code code;
	std::filesystem::create_directories(assets_dir_, code);
	assets_path_ = assets_dir_ + "/assets.json";
	std::string failure;
	if (!assets_.save(assets_path_, &failure)) {
		assets_status_ = "save failed: " + failure;
		if (error) {
			*error = failure;
		}
		return false;
	}
	assets_status_ = "saved " + assets_path_;
	return true;
}

void Application::wire_assets()
{
	scripts_.set_assets(&assets_);
	for (const std::string& name : scenes_.names()) {
		if (Scene* scene = scenes_.get(name)) {
			scene->set_assets(&assets_, assets_dir_);
		}
	}
}

std::vector<std::string> Application::prefab_names() const
{
	std::vector<std::string> out;
	std::error_code code;
	if (!std::filesystem::is_directory(prefab_dir_, code)) {
		return out;
	}
	for (const auto& entry : std::filesystem::directory_iterator(prefab_dir_, code)) {
		if (entry.is_regular_file() && entry.path().extension() == ".json") {
			out.push_back(entry.path().stem().string());
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

void Application::handle_action(const std::string& action)
{
	if (action == "editor.toggle") {
		editor_.toggle();
	} else if (action == "console.toggle") {
		editor_.toggle_console();
	} else if (action == "undo") {
		editor_.request_undo();
	} else if (action == "redo") {
		editor_.request_redo();
	} else if (action == "scene.save") {
		std::string error;
		if (!save_scene_now(&error)) {
			console_.warn(error);
		}
	} else if (action == "scene.reload") {
		std::string error;
		if (!reload_scene_now(&error)) {
			console_.warn(error);
		}
	} else if (action == "quit") {
		quit_requested_ = true;
	}
}

void Application::watch_scene(const std::string& path)
{
	scene_path_ = path;
	watcher_.watch("scene", path);
	scene_status_ = "watching " + path;
}

std::string Application::scene_file_for(const std::string& name) const
{
	return scene_dir_ + "/" + name + ".json";
}

bool Application::load_scene_by_name(const std::string& name, std::string* error)
{
	const std::string path = scene_file_for(name);

	// Parse into a scratch scene first. If the file is missing or malformed,
	// nothing is created or destroyed, so a failed load can never take out a
	// scene that was already open under this name.
	Scene scratch{name};
	std::string failure;
	if (!load_scene(scratch, path, &failure)) {
		if (error) {
			*error = "cannot load " + path + ": " + failure;
		}
		return false;
	}

	// Committed: replacing an existing scene under this name is intended here.
	Scene& scene = scenes_.create(name);
	scene = std::move(scratch);
	scene.resolve_refs();
	scene.set_assets(&assets_, assets_dir_);
	scenes_.activate(name);
	adopt_scene_path(path);
	return true;
}

bool Application::switch_scene(const std::string& name, std::string* error)
{
	if (scenes_.get(name) != nullptr) {
		// Already open: a switch is a pointer move, never a reload, so unsaved
		// changes in the scene being left stay in memory.
		scenes_.activate(name);
		adopt_scene_path(scene_file_for(name));
		return true;
	}
	return load_scene_by_name(name, error);
}

bool Application::save_game_now(const std::string& path, std::string* error)
{
	Scene* scene = scenes_.active();
	if (scene == nullptr) {
		if (error) {
			*error = "no active scene";
		}
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(
	        std::filesystem::path(path).parent_path(), ec);
	const GameSaveData data =
	        capture_game_state(*scene, scenes_.active_name(),
	                           residency_.resident());
	return save_game_to_file(data, path, error);
}

bool Application::load_game_now(const std::string& path, std::string* error)
{
	GameSaveData data;
	if (!load_game_from_file(path, data, error)) {
		return false;
	}
	if (data.scene_name.empty()) {
		if (error) {
			*error = "save missing scene_name";
		}
		return false;
	}
	std::string scene_error;
	if (!switch_scene(data.scene_name, &scene_error)) {
		if (error) {
			*error = scene_error;
		}
		return false;
	}
	Scene* scene = scenes_.active();
	if (scene == nullptr) {
		if (error) {
			*error = "no active scene after switch";
		}
		return false;
	}
	apply_game_state(*scene, data);
	update_world_transforms(*scene);
	return true;
}

void Application::adopt_scene_path(const std::string& path)
{
	scene_path_ = path;
	// Re-key the watcher, or hot reload keeps reloading the scene we left.
	watcher_.watch("scene", path);
	scene_status_ = "watching " + path;
	editor_.reset_history();
}

bool Application::save_scene_now(std::string* error)
{
	Scene* scene = scenes_.active();
	if (!scene || scene_path_.empty()) {
		if (error) {
			*error = "no active scene or scene path";
		}
		return false;
	}
	std::string failure;
	if (!save_scene(*scene, scene_path_, &failure)) {
		scene_status_ = "save failed: " + failure;
		if (error) {
			*error = failure;
		}
		return false;
	}
	scene_status_ = "saved " + scene_path_;
	editor_.clear_dirty();
	// Re-baseline so our own write does not trigger a reload.
	watcher_.watch("scene", scene_path_);
	return true;
}

bool Application::reload_scene_now(std::string* error)
{
	Scene* scene = scenes_.active();
	if (!scene || scene_path_.empty()) {
		if (error) {
			*error = "no active scene or scene path";
		}
		return false;
	}
	std::string failure;
	if (!load_scene(*scene, scene_path_, &failure)) {
		// load_scene is atomic: the live scene is untouched on failure.
		scene_status_ = "reload rejected: " + failure;
		if (error) {
			*error = failure;
		}
		return false;
	}
	scene_status_ = "reloaded " + scene_path_;
	// The scene was replaced wholesale; undo history no longer applies.
	editor_.reset_history();
	scripts_.sync(*scene);
	return true;
}

void Application::poll_async_scene_load()
{
	if (async_scene_load_.state() == AsyncSceneLoad::State::Idle) {
		return;
	}
	const AsyncSceneLoad::State state = async_scene_load_.poll();
	if (state == AsyncSceneLoad::State::Ready && !pending_async_scene_.empty()) {
		Scene loaded{pending_async_scene_};
		if (async_scene_load_.take(loaded)) {
			Scene& scene = scenes_.create(pending_async_scene_);
			scene = std::move(loaded);
			scene.set_assets(&assets_, assets_dir_);
			scenes_.activate(pending_async_scene_);
			adopt_scene_path(scene_file_for(pending_async_scene_));
			scripts_.sync(scene);
			console_.info("async load ready: " + pending_async_scene_);
		}
		pending_async_scene_.clear();
	} else if (state == AsyncSceneLoad::State::Failed) {
		console_.warn("async load failed: " + async_scene_load_.error());
		pending_async_scene_.clear();
	}
}

void Application::poll_hot_reload()
{
	for (const std::string& key : watcher_.poll()) {
		if (key != "scene") {
			continue;
		}
		std::string error;
		if (!reload_scene_now(&error)) {
			console_.warn("hot reload rejected: " + error);
		} else {
			const Scene* scene = scenes_.active();
			const std::size_t count =
			        scene ? scene->registry().view<Name>().size() : 0;
			console_.info("hot reloaded " + scene_path_ + " (" +
			              std::to_string(count) + " entities)");
		}
	}
}

void Application::run(int width, int height, const std::string& title)
{
		on_start();
	{
		std::string audio_error;
		if (!audio_.init(&audio_error)) {
			console_.warn("audio: " + audio_error);
		}
		Window window{width, height, title};
		GpuRenderer gpu;
		gpu.init(window.handle(), settings_.debug_device);
		editor_.init(window.handle(), gpu);
		if (const char* no_ui = std::getenv("LOTUS_NO_UI")) {
			if (std::atoi(no_ui) != 0) {
				editor_.set_visible(false);
			}
		}

		register_commands();
		residency_.set_scene_directory(scene_dir_);
		{
			const std::string regions_path = scene_dir_ + "/regions.json";
			std::error_code exists;
			if (std::filesystem::exists(regions_path, exists)) {
				std::string region_error;
				if (!residency_.load_catalog(regions_path, &region_error)) {
					console_.warn("regions: " + region_error);
				}
			}
		}
		std::error_code assets_dir_error;
		std::filesystem::create_directories(assets_dir_, assets_dir_error);
		{
			std::string assets_error;
			if (!load_assets_now(&assets_error)) {
				console_.warn("assets: " + assets_error);
			}
		}

		console_.set_sink([](const ConsoleEntry& entry) {
			std::cout << entry.text << '\n';
		});
		console_.info(std::string{"gpu driver: "} + gpu.driver_name());

		// Keybinds: define the defaults first (load only overrides known
		// actions), then overlay the file, writing it when absent so it is
		// hand/agent editable.
		keybinds_ = default_keybinds();
		std::string keybind_error;
		if (!keybinds_.load(keybinds_path_, &keybind_error)) {
			keybinds_.save(keybinds_path_, nullptr);
			console_.info("wrote default keybinds to " + keybinds_path_);
		} else if (!keybind_error.empty()) {
			console_.warn(keybind_error);
		}

		editor_.set_console(&console_);
		editor_.set_keybinds(&keybinds_, [this] {
			std::string error;
			if (!keybinds_.save(keybinds_path_, &error)) {
				console_.warn(error);
			} else {
				console_.info("saved " + keybinds_path_);
			}
		});

		on_ready(window, gpu);
		wire_assets();

		scripts_.set_log([this](const std::string& message) {
			console_.info(message);
		});
		if (Scene* scene = scenes_.active()) {
			std::string error;
			if (!scripts_.sync(*scene, &error)) {
				console_.warn("script load: " + error);
			}
		}

		editor_.set_scene_controls(Editor::SceneControls{
		        [this] { return scene_path_; },
		        [this] { return scene_status_; },
		        [this] { save_scene_now(); },
		        [this] { reload_scene_now(); }});

		editor_.set_prefab_controls(Editor::PrefabControls{
		        [this] { return prefab_names(); },
		        [this](Entity root, const std::string& name, std::string* error) {
			        return save_prefab_now(root, name, error);
		        },
		        [this](const std::string& name, Entity parent) {
			        return instantiate_prefab(name, parent);
		        }});

		editor_.set_physics(&physics_);
		editor_.set_asset_controls(Editor::AssetControls{
		        [this] { return &assets_; },
		        [this] { return assets_status_; },
		        [this] {
			        std::string error;
			        load_assets_now(&error);
		        },
		        [this](Scene* scene) {
			        std::vector<std::string> out;
			        if (scene == nullptr) {
				        return out;
			        }
			        for (const Entity entity :
			             scene->registry().view<MeshSource>()) {
				        const MeshSource& source =
				                scene->registry().get<MeshSource>(entity);
				        if (assets_.dangling(source.id)) {
					        out.push_back(source.id);
				        }
			        }
			        for (const Entity entity : scene->registry().view<Script>()) {
				        const Script& script =
				                scene->registry().get<Script>(entity);
				        if (assets_.dangling(script.id)) {
					        out.push_back(script.id);
				        }
			        }
			        std::sort(out.begin(), out.end());
			        out.erase(std::unique(out.begin(), out.end()), out.end());
			        return out;
		        }});

		// Scriptable console: LOTUS_CONSOLE="cmd; cmd; ..." runs at startup,
		// which makes the console usable from automation and by agents.
		if (const char* script = std::getenv("LOTUS_CONSOLE")) {
			const std::string list = script;
			std::size_t start      = 0;
			while (start <= list.size()) {
				const std::size_t end = list.find(';', start);
				const std::string command = trim(list.substr(
				        start,
				        end == std::string::npos ? std::string::npos
				                                 : end - start));
				if (!command.empty()) {
					console_.execute(command);
				}
				if (end == std::string::npos) {
					break;
				}
				start = end + 1;
			}
		}

		int max_frames = 0;
		if (const char* env = std::getenv("LOTUS_MAX_FRAMES")) {
			max_frames = std::atoi(env);
		}
		int frame = 0;
		bool game_input_enabled = true;

		Uint64 last_counter = SDL_GetPerformanceCounter();
		const double freq   = static_cast<double>(SDL_GetPerformanceFrequency());

		while (!window.should_close() && !quit_requested_) {
			window.poll_events([this, &game_input_enabled](const SDL_Event& event) {
				const bool captured = editor_.captures_keyboard();
				editor_.process_event(event);
				game_input_enabled = !editor_.captures_keyboard();
				if (!captured) {
					const std::string action = keybinds_.action_for(event);
					if (!action.empty()) {
						handle_action(action);
					}
				}
			});

			const Uint64 now = SDL_GetPerformanceCounter();
			const float dt =
			        static_cast<float>(static_cast<double>(now - last_counter) /
			                           freq);
			last_counter = now;

			on_update(dt);
			poll_hot_reload();
			poll_async_scene_load();
			if (Scene* scene = scenes_.active()) {
				apply_character_movement_input(*scene, physics_,
				                               game_input_enabled);
				animation_system_update(*scene, animations_, &assets_, dt);
				scene->update(dt);
				physics_.step(*scene, dt);
				update_follow_camera(*scene);
				scripts_.update(*scene, dt);
				scripts_.poll(*scene);

				glm::vec3 viewer{0.0f};
				if (const Camera3D* camera = scene->active_camera3d()) {
					viewer = camera->position;
				} else if (const Camera2D* camera = scene->active_camera()) {
					viewer = glm::vec3{camera->position, 0.0f};
				}
				residency_.update(*scene, viewer);
			}

			int pixel_w = width, pixel_h = height;
			window.pixel_size(pixel_w, pixel_h);
			const float aspect =
			        pixel_h > 0 ? static_cast<float>(pixel_w) / pixel_h : 1.0f;

			// A scene that carries a settings block drives the renderer's
			// lighting; one that does not leaves whatever was set globally.
			// Applied every frame so a scene switch takes effect immediately.
			if (Scene* active = scenes_.active();
			    active != nullptr && active->has_settings()) {
				const SceneSettings& s = active->settings();
				gpu.lighting().direction = s.light_direction;
				gpu.lighting().color     = s.light_color;
				gpu.lighting().intensity = s.light_intensity;
				gpu.lighting().ambient   = s.ambient;
			}
			gpu.begin_frame(window.handle(), 0.05f, 0.05f, 0.08f);
			if (gpu.has_frame()) {
				if (Scene* scene = scenes_.active()) {
					// Viewport pass: render scene to offscreen texture for the editor
					const auto vs = editor_.viewport_size();
					if (vs.width > 0 && vs.height > 0 &&
					    (vs.width != gpu.viewport_width() ||
					     vs.height != gpu.viewport_height())) {
						if (gpu.resize_viewport(vs.width, vs.height)) {
							console_.info("viewport: " + std::to_string(vs.width) +
							              "x" + std::to_string(vs.height));
						}
					}
					if (gpu.viewport_ready()) {
						const float viewport_aspect =
						        static_cast<float>(vs.width) /
						        static_cast<float>(vs.height);
						scene->upload(gpu, viewport_aspect);
						if (gpu.begin_viewport_pass()) {
							scene->draw(gpu);
							gpu.end_viewport_pass();
						}
					}

					scene->upload(gpu, aspect);
					gpu.begin_scene_pass();
					scene->draw(gpu);
					gpu.end_scene_pass();
				} else {
					gpu.begin_scene_pass();
					gpu.end_scene_pass();
				}

				editor_.begin_frame();
				editor_.draw(scenes_, dt);
				editor_.render(gpu);

				gpu.end_frame();
			} else {
				// Swapchain not ready: back off instead of spinning.
				SDL_Delay(1);
			}

			if (max_frames > 0 && ++frame >= max_frames) {
				break;
			}
		}

		editor_.shutdown();
		on_shutdown();
		audio_.shutdown();
		std::cout << "frames rendered: " << gpu.frames_rendered() << '\n';
	} // GPU + window must be destroyed before SDL_Quit()

	SDL_Quit();
	Global::flag(LOTUS_SDL_INITIALIZED, false);
}
