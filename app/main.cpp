#include "lotus/application.hpp"
#include "lotus/gpu/renderer.hpp"
#include "lotus/scene/scene.hpp"

#include <cmath>
#include <iostream>

class DemoApplication : public lotus::Application {
public:
	using lotus::Application::Application;

protected:
	void on_ready(lotus::Window& /*window*/, lotus::GpuRenderer& gpu) override
	{
		std::cout << "gpu driver: " << gpu.driver_name() << '\n';

		lotus::Scene& scene = scenes().activate("main");

		// A parent square with two child squares, to exercise hierarchy.
		root_ = scene.create("root");
		scene.registry().emplace<lotus::Sprite>(
		        root_, lotus::Sprite{{0.90f, 0.35f, 0.35f, 1.0f}, {0.5f, 0.5f}});

		const lotus::Entity left = scene.create("left");
		scene.registry().emplace<lotus::Sprite>(
		        left, lotus::Sprite{{0.35f, 0.85f, 0.45f, 1.0f}, {0.25f, 0.25f}});
		scene.registry().get<lotus::Transform>(left).position = {-0.8f, 0.0f, 0.0f};
		scene.set_parent(left, root_);

		const lotus::Entity right = scene.create("right");
		scene.registry().emplace<lotus::Sprite>(
		        right, lotus::Sprite{{0.35f, 0.55f, 0.95f, 1.0f}, {0.25f, 0.25f}});
		scene.registry().get<lotus::Transform>(right).position = {0.8f, 0.0f, 0.0f};
		scene.set_parent(right, root_);

		// Camera is now an ordinary entity component.
		const lotus::Entity camera = scene.create("camera");
		scene.registry().emplace<lotus::Camera2D>(camera);
	}

	void on_update(float dt) override
	{
		lotus::Scene* scene = scenes().active();
		if (!scene || !scene->registry().valid(root_)) {
			return;
		}
		auto& transform = scene->registry().get<lotus::Transform>(root_);
		transform.rotation.z += dt * 0.8f;
	}

	void on_shutdown() override {}

private:
	lotus::Entity root_{lotus::NullEntity};
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
