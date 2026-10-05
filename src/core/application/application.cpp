#include "lotus/application.hpp"
#include "lotus/global.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"

#include <cstdlib>
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
		console_.set_sink([](const ConsoleEntry& entry) {
			std::cout << entry.text << '\n';
		});
		console_.info(std::string{"gpu driver: "} + gpu.driver_name());

		// Keybinds: load from disk, or write the defaults so they are editable.
		std::string keybind_error;
		if (!keybinds_.load(keybinds_path_, &keybind_error)) {
			keybinds_ = default_keybinds();
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

		Uint64 last_counter = SDL_GetPerformanceCounter();
		const double freq   = static_cast<double>(SDL_GetPerformanceFrequency());

		while (!window.should_close() && !quit_requested_) {
			window.poll_events([this](const SDL_Event& event) {
				const bool captured = editor_.captures_keyboard();
				editor_.process_event(event);
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
			if (Scene* scene = scenes_.active()) {
				scene->update(dt);
				scripts_.update(*scene, dt);
				scripts_.poll(*scene);
			}

			int pixel_w = width, pixel_h = height;
			window.pixel_size(pixel_w, pixel_h);
			const float aspect =
			        pixel_h > 0 ? static_cast<float>(pixel_w) / pixel_h : 1.0f;

			gpu.begin_frame(window.handle(), 0.05f, 0.05f, 0.08f);
			if (gpu.has_frame()) {
				if (Scene* scene = scenes_.active()) {
					scene->upload(gpu);
					gpu.begin_scene_pass();
					scene->draw(gpu, aspect);
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
		std::cout << "frames rendered: " << gpu.frames_rendered() << '\n';
	} // GPU + window must be destroyed before SDL_Quit()

	SDL_Quit();
	Global::flag(LOTUS_SDL_INITIALIZED, false);
}
