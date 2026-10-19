#include "lotus/application.hpp"
#include "lotus/application/headless.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kHeadlessUsage =
        "usage: lotus --headless [--scene NAME] [--ticks N] [--screenshot PATH]\n"
        "                       [--width W] [--height H] [--seed S]\n"
        "                       [--dump-state PATH] [--scene-dir DIR]\n"
        "                       [--assets-dir DIR] [--exec \"cmd; cmd\"]";

} // namespace

class DemoApplication : public lotus::Application {
public:
	using lotus::Application::Application;

protected:
	void on_ready(lotus::Window& /*window*/, lotus::GpuRenderer& gpu) override
	{
		gpu.lighting().direction  = {0.5f, 0.9f, 0.4f};
		gpu.lighting().intensity  = 1.0f;
		gpu.lighting().ambient    = {0.12f, 0.13f, 0.17f};

		std::string error;
		if (!load_scene_by_name("main", &error)) {
			std::cerr << "lotus: could not load scenes/main.json: " << error
			          << "\n";
			scenes().activate("main");
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
		app.run(1100, 700, "lotus");
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}
