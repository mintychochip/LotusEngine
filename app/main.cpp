#include "lotus/application.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"
#include "lotus/scene/serialization.hpp"
#include "lotus/script/script.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>

class DemoApplication : public lotus::Application {
public:
	using lotus::Application::Application;

protected:
	void on_ready(lotus::Window& /*window*/, lotus::GpuRenderer& /*gpu*/) override
	{
		lotus::Scene& scene = scenes().activate("main");

		// A parent square with two child squares, to exercise hierarchy.
		const lotus::Entity root = scene.create("root");
		scene.registry().emplace<lotus::Sprite>(
		        root, lotus::Sprite{{0.90f, 0.35f, 0.35f, 1.0f}, {0.5f, 0.5f}});

		const lotus::Entity left = scene.create("left");
		scene.registry().emplace<lotus::Sprite>(
		        left, lotus::Sprite{{0.35f, 0.85f, 0.45f, 1.0f}, {0.25f, 0.25f}});
		scene.registry().get<lotus::Transform>(left).position = {-0.8f, 0.0f, 0.0f};
		// Game code attaches a script; the engine runs and hot-swaps it.
		scene.registry().emplace<lotus::Script>(
		        left, lotus::Script{"../scripts/spin.lua"});
		scene.set_parent(left, root);

		const lotus::Entity right = scene.create("right");
		scene.registry().emplace<lotus::Sprite>(
		        right, lotus::Sprite{{0.35f, 0.55f, 0.95f, 1.0f}, {0.25f, 0.25f}});
		scene.registry().get<lotus::Transform>(right).position = {0.8f, 0.0f, 0.0f};
		scene.set_parent(right, root);

		// Camera is an ordinary entity component.
		const lotus::Entity camera = scene.create("camera");
		scene.registry().emplace<lotus::Camera2D>(camera);

		// Write the scene out and watch it: edit the JSON and it reloads live.
		std::error_code error;
		std::filesystem::create_directories("scenes", error);
		const std::string path = "scenes/main.json";
		std::string save_error;
		if (lotus::save_scene(scene, path, &save_error)) {
			watch_scene(path);
		} else {
			std::cout << "could not write " << path << ": " << save_error << '\n';
		}
	}

	void on_update(float dt) override
	{
		lotus::Scene* scene = scenes().active();
		if (!scene) {
			return;
		}
		// Look the entity up by name: a hot reload replaces the registry, so
		// cached handles would dangle.
		const lotus::Entity root = scene->find("root");
		if (root == lotus::NullEntity) {
			return;
		}
		scene->registry().get<lotus::Transform>(root).rotation.z += dt * 0.8f;
	}

	void on_shutdown() override {}
};

int main()
{
	try {
		DemoApplication app;
		app.run(960, 600, "lotus");
	} catch (const std::exception& ex) {
		std::cerr << "lotus: " << ex.what() << '\n';
		return 1;
	}
	return 0;
}
