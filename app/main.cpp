#include "lotus/application.hpp"
#include "lotus/application/headless.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kHeadlessUsage =
        "usage: lotus --headless [--scene NAME] [--ticks N] [--screenshot PATH]\n"
        "                       [--width W] [--height H] [--seed S]\n"
        "                       [--dump-state PATH] [--scene-dir DIR]\n"
        "                       [--assets-dir DIR] [--exec \"cmd; cmd\"]";

constexpr const char* kWindowUsage =
        "usage: lotus [--scene NAME] [--width W] [--height H]\n"
        "             [--scene-dir DIR] [--assets-dir DIR]";

} // namespace

class DemoApplication : public lotus::Application {
public:
	using lotus::Application::Application;

	void set_initial_scene(std::string name) { initial_scene_ = std::move(name); }

protected:
	void on_ready(lotus::Window& /*window*/, lotus::GpuRenderer& gpu) override
	{
		gpu.lighting().direction  = {0.5f, 0.9f, 0.4f};
		gpu.lighting().intensity  = 1.0f;
		gpu.lighting().ambient    = {0.12f, 0.13f, 0.17f};

		std::string error;
		if (!load_scene_by_name(initial_scene_, &error)) {
			std::cerr << "lotus: could not load scene '" << initial_scene_
			          << "': " << error << "\n";
			if (initial_scene_ != "main" &&
			    load_scene_by_name("main", &error)) {
				scenes().activate("main");
				return;
			}
			scenes().activate(initial_scene_);
		}
	}

	void on_update(float dt) override
	{
		lotus::Scene* scene = scenes().active();
		if (!scene) {
			return;
		}
		if (const lotus::Entity cube = scene->find("cube");
		    cube != lotus::NullEntity) {
			scene->registry().get<lotus::Transform>(cube).rotation.y +=
			        dt * 0.5f;
		}
	}

	void on_shutdown() override {}

private:
	std::string initial_scene_{"main"};
};

int main(int argc, char** argv)
{
	try {
		std::vector<std::string> args(argv + 1, argv + argc);
		const auto headless =
		        std::find(args.begin(), args.end(), "--headless");
		if (headless != args.end()) {
			args.erase(headless);
			lotus::HeadlessOptions options;
			std::string error;
			if (!lotus::parse_headless_args(args, &options, &error)) {
				std::cerr << "lotus: " << error << "\n"
				          << kHeadlessUsage << "\n";
				return 2;
			}
			DemoApplication app;
			return app.run_headless(options);
		}
		DemoApplication app;
		std::string scene = "main";
		std::string scene_dir = "scenes";
		std::string assets_dir = "assets";
		int width = 1280;
		int height = 720;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			auto value = [&](const char* flag) -> std::string {
				if (i + 1 >= args.size()) {
					throw std::runtime_error(std::string{"flag '"} + flag +
					                         "' needs a value");
				}
				return args[++i];
			};
			if (arg == "--scene") {
				scene = value("--scene");
			} else if (arg == "--scene-dir") {
				scene_dir = value("--scene-dir");
			} else if (arg == "--assets-dir") {
				assets_dir = value("--assets-dir");
			} else if (arg == "--width") {
				width = std::stoi(value("--width"));
			} else if (arg == "--height") {
				height = std::stoi(value("--height"));
			} else if (arg == "--help" || arg == "-h") {
				std::cout << kWindowUsage << "\n";
				return 0;
			} else {
				std::cerr << "lotus: unknown argument '" << arg << "'\n"
				          << kWindowUsage << "\n";
				return 2;
			}
		}
		app.set_initial_scene(scene);
		app.set_scene_dir(scene_dir);
		app.set_assets_dir(assets_dir);
		app.run(width, height, "lotus - " + scene);
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}
