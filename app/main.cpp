#include "lotus/application.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"

#include <iostream>

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

int main()
{
	try {
		DemoApplication app;
		app.run(1100, 700, "lotus");
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}
